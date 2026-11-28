#pragma once
#include "document/canonical_stream.hpp"
#include <bit>
#include <cstring>
#include <limits>

namespace engine::document {
// Conservative v1 world bounds for a replay packet. Polynomial enclosure is
// deliberately conservative (interval Horner, not sampling). Also include
// the actual float chord descriptors used by the compatibility renderer.
// This is not the M05 hierarchical-coordinate certificate.
struct SpanBounds {
    double minX=std::numeric_limits<double>::infinity(),minY=minX,maxX=-minX,maxY=-minX;
    bool include(double x0,double y0,double x1,double y1,double pad) noexcept {
        if(!std::isfinite(x0)||!std::isfinite(x1)||!std::isfinite(y0)||!std::isfinite(y1)||!std::isfinite(pad)||pad<0) return false;
        constexpr auto inf=std::numeric_limits<double>::infinity();
        minX=std::min(minX,std::nextafter(std::min(x0,x1)-pad,-inf));
        minY=std::min(minY,std::nextafter(std::min(y0,y1)-pad,-inf));
        maxX=std::max(maxX,std::nextafter(std::max(x0,x1)+pad,inf));
        maxY=std::max(maxY,std::nextafter(std::max(y0,y1)+pad,inf));
        return std::isfinite(minX)&&std::isfinite(minY)&&std::isfinite(maxX)&&std::isfinite(maxY);
    }
};
inline bool spanBounds(const ReplaySpan& s,double dx,double dy,SpanBounds& bounds) noexcept {
    if(!std::isfinite(dx)||!std::isfinite(dy)) return false;
    if(s.kind==ReplaySpan::Curve) {
        // Enclose P(u)=origin+a+u(b+u(c+u*d)), 0<=u<=1.
        // Each arithmetic step rounds outward. Enclose translation separately.
        constexpr double inf=std::numeric_limits<double>::infinity();
        auto axis=[](double o,double a,double b,double c,double d,double shift) {
            constexpr double infinity=std::numeric_limits<double>::infinity();
            double lo=std::nextafter(std::min(0.,d),-infinity),hi=std::nextafter(std::max(0.,d),infinity);
            for(double term:{c,b}) {
                lo=std::min(0.,std::nextafter(term+lo,-infinity));
                hi=std::max(0.,std::nextafter(term+hi,infinity));
            }
            for(double term:{a,o,shift}) {
                lo=std::nextafter(term+lo,-infinity);hi=std::nextafter(term+hi,infinity);
            }
            return std::array<double,2>{lo,hi};
        };
        const auto& c=s.curve;
        auto x=axis(c.origin.x,c.a.x,c.b.x,c.c.x,c.d.x,dx),y=axis(c.origin.y,c.a.y,c.b.y,c.c.y,c.d.y,dy);
        const auto pad=std::nextafter(double(s.radius(std::max({c.arrival,c.departure,c.peak})))+10.,inf);
        if(!bounds.include(x[0],y[0],x[1],y[1],pad)) return false;
    }
    return s.chords([&](float ax,float ay,float bx,float by,float r0,float r1) {
        // Match compatibility quad construction, including its near-zero
        // threshold, post-reconstruction float translation, and endpoint math.
        float vx=bx-ax,vy=by-ay,len=std::hypot(vx,vy);
        if(len<0.0001f) {vx=1;vy=0;len=0;} else {vx/=len;vy/=len;}
        const double x=double(ax)+dx,y=double(ay)+dy;
        constexpr double limit=std::numeric_limits<float>::max()/1024.;
        if(!std::isfinite(x)||!std::isfinite(y)||std::abs(x)>limit||std::abs(y)>limit||!std::isfinite(len)) return false;
        ax=float(x);ay=float(y);
        // Include both rounded separate multiply/add and fused evaluation,
        // as a conservative enclosure of the v1 float endpoint.
        const float x1=ax+vx*len,y1=ay+vy*len;
        const float xf=std::fma(vx,len,ax),yf=std::fma(vy,len,ay);
        const float pad=std::max(r0,r1)+10.f;
        if(!std::isfinite(pad)||pad<0) return false;
        // Outward float rounding also covers rounding of the legacy pad addition.
        constexpr float inf=std::numeric_limits<float>::infinity();
        return bounds.include(std::nextafter(std::min({ax,x1,xf})-pad,-inf),
                              std::nextafter(std::min({ay,y1,yf})-pad,-inf),
                              std::nextafter(std::max({ax,x1,xf})+pad,inf),
                              std::nextafter(std::max({ay,y1,yf})+pad,inf),0);
    });
}

// Explicit little-endian derived format, not a schema or ABI change.
// Header: magic, version, packet bytes, first, returned, total, raw sample
// count (7 u32), followed by the exact 80-byte v1 canonical header.
// Packet: kind, ordinal (u32); five xy vectors (f64); 11 f32 fields
// (curve pressure triple, chord/terminal ax ay bx by departure arrival peak
// radiusWorld); four f64 bounds. No struct padding or native pointers.
class CanonicalRange {
public:
    static constexpr uint32_t magic=0x31535043; // CPS1 little endian
    static constexpr size_t maxSpans=128, packetBytes=164, headerBytes=28+wire::headerBytes;
    static constexpr size_t maxResult=headerBytes+maxSpans*packetBytes;
    static constexpr size_t maxScratch=256u<<10;
    static core::Result<std::unique_ptr<CanonicalRange>> create(std::span<const uint8_t> header,size_t samples,size_t first,size_t count) {
        Brush b;double dx,dy;
        if(!wire::readHeader(header,b,dx,dy)||!samples||samples>1000000||first>1000000||!count||count>maxSpans||count>1000001-first)
            return core::Error{core::ErrorCode::InvalidArgument,"Invalid canonical span request"};
        auto replay=CanonicalStream::create(b);if(!replay) return replay.error();
        try {
            auto range=std::unique_ptr<CanonicalRange>(new CanonicalRange(std::move(replay).value(),samples,first,count,dx,dy));
            std::copy(header.begin(),header.end(),range->data_.begin()+28);
            return range;
        } catch(const std::bad_alloc&) {
            return core::Error{core::ErrorCode::AllocationFailure,"Canonical range allocation failed"};
        }
    }
    // Full record validation is mandatory before finish; caller verifies
    // persisted SHA-256 and identity/revision in a coherent SQLite transaction.
    // This decoder verifies exact sample count, wire values and chunk order.
    bool append(std::span<const uint8_t> bytes) {
        if(failed_||done_||bytes.empty()||bytes.size()%wire::sampleBytes) {failed_=true;return false;}
        const auto n=bytes.size()/wire::sampleBytes;
        if(n>wire::chunkSamples||n>expected_-read_) {failed_=true;return false;}
        failed_=true;
        auto points=std::span(decoded_).first(n);
        if(!wire::readSamples(bytes,lastTime_,points)||!replay_.append(points,this,consume)) return false;
        lastTime_=points.back().timestamp;read_+=n;failed_=false;return true;
    }
    bool finish() {
        if(failed_||done_||read_!=expected_) {failed_=true;return false;}
        failed_=true;done_=true;
        if(!replay_.finish(this,consume)||first_>total_) return false;
        auto* p=data_.data();
        for(uint32_t v:{magic,1u,uint32_t(packetBytes),uint32_t(first_),uint32_t(written_),uint32_t(total_),uint32_t(read_)}) put(p,v);
        failed_=false;return true;
    }
    [[nodiscard]] std::span<const uint8_t> result() const noexcept {
        if(!done_||failed_) return {};
        return std::span(data_).first(headerBytes+written_*packetBytes);
    }
private:
    CanonicalRange(CanonicalStream replay,size_t samples,size_t first,size_t count,double dx,double dy):
        replay_(std::move(replay)),expected_(samples),first_(first),count_(count),dx_(dx),dy_(dy) {}
    template<class T> static void put(uint8_t*& p,T v) noexcept {
        using U=std::conditional_t<sizeof(T)==8,uint64_t,uint32_t>;
        U b=std::bit_cast<U>(v);for(size_t i=0;i<sizeof(T);++i){*p++=uint8_t(b);b>>=8;}
    }
    static bool consume(void* ctx,const ReplaySpan& s) {
        auto& self=*static_cast<CanonicalRange*>(ctx);
        if(s.kind!=ReplaySpan::Curve&&s.kind!=ReplaySpan::Terminal) return true;
        if(s.ordinal!=self.total_) return false;
        ++self.total_;
        if(s.ordinal<self.first_||s.ordinal-self.first_>=self.count_) return true;
        SpanBounds b;if(!spanBounds(s,self.dx_,self.dy_,b)) return false;
        auto* p=self.data_.data()+headerBytes+self.written_*packetBytes;
        put(p,uint32_t(s.kind));put(p,uint32_t(s.ordinal));
        for(auto v:{s.curve.origin,s.curve.a,s.curve.b,s.curve.c,s.curve.d}) {put(p,v.x);put(p,v.y);}
        for(float v:{s.curve.departure,s.curve.arrival,s.curve.peak,s.ax,s.ay,s.bx,s.by,s.departure,s.arrival,s.peak,s.radiusWorld}) put(p,v);
        for(double v:{b.minX,b.minY,b.maxX,b.maxY}) put(p,v);
        ++self.written_;return true;
    }
    CanonicalStream replay_;
    size_t expected_,first_,count_,read_=0,total_=0,written_=0;
    double dx_,dy_,lastTime_=-1;
    bool failed_=false,done_=false;
    std::array<input::RawInputPoint,wire::chunkSamples> decoded_{};
    std::array<uint8_t,maxResult> data_{};
};
static_assert(sizeof(CanonicalRange)+wire::chunkSamples*wire::sampleBytes<CanonicalRange::maxScratch);
}
