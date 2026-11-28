#pragma once
#include "document/codec.hpp"
#include "stroke/spline.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace engine::document {
// Derived v1 span stream. The raw record remains authoritative. IDs, revision,
// verification of the record digest and commit of the sink are caller-owned.
// Each mode has its own monotonic ordinal space. A spline interval includes
// any dwell at its left knot; a single terminal packet contains the physical
// endpoint connector and final dab. Never paint each packet separately.
struct ReplaySpan {
    enum Kind {Raw,Filtered,Curve,Terminal} kind;
    size_t ordinal;
    stroke::CurveSpan curve{};
    float ax=0,ay=0,bx=0,by=0,departure=0,arrival=0,peak=0;
    float radiusWorld=0;
    [[nodiscard]] float radius(float pressure) const noexcept {return radiusWorld*(0.35f+1.15f*pressure);}
    // Raw/filtered endpoint coordinates already use the v1 float conversion.
    // The renderer passes each chord to the unchanged v1 quad generator.
    template<class F> bool chords(F&& f) const {
        if(kind==Terminal) {
            if(std::hypot(bx-ax,by-ay)>0.03f && !f(ax,ay,bx,by,departure,arrival)) return false;
            return f(bx,by,bx,by,arrival,arrival);
        }
        if(kind!=Curve) {
            if(peak>departure && !f(ax,ay,ax,ay,peak,peak)) return false;
            return f(ax,ay,bx,by,departure,arrival);
        }
        auto first=curve.at(0);float x=float(first.x),y=float(first.y);
        float start=radius(curve.departure),end=radius(curve.arrival),r=start;
        float p=radius(curve.peak);
        if(p>start && !f(x,y,x,y,p,p)) return false;
        const int steps=curve.tessellation_steps(.20*radiusWorld/1.9);
        for(int i=1;i<=steps;++i) {
            auto next=curve.at(double(i)/steps);
            float nx=float(next.x),ny=float(next.y),nr=std::lerp(start,end,float(i)/steps);
            if(!f(x,y,nx,ny,r,nr)) return false;
            x=nx;y=ny;r=nr;
        }
        return true;
    }
};
class CanonicalStream {
public:
    using Emit=bool(*)(void*,const ReplaySpan&);
    static core::Result<CanonicalStream> create(const Brush& b) {
        wire::Header h;
        if(!wire::header(b,0,0,h)) return core::Error{core::ErrorCode::InvalidArgument,"Invalid replay header"};
        auto filter=input::OneEuroFilter::create(b.filter);
        auto fitter=stroke::SplineFitter::create({stroke::Parameterization::Centripetal,1000000,.01,14});
        if(!filter) return filter.error();
        if(!fitter) return fitter.error();
        return CanonicalStream(b,std::move(filter).value(),std::move(fitter).value());
    }
    // Can be called repeatedly on arbitrary input chunks. No buffering of
    // the source history. A failed/cancelled sink poisons the entire replay.
    bool append(std::span<const input::RawInputPoint> points,void* ctx,Emit emit) {
        if(failed_||done_||!emit||points.empty()) {failed_=true;return false;}
        // Poison until this whole call succeeds (including callback exceptions).
        failed_=true;
        for(const auto& raw:points) {
            constexpr double limit=std::numeric_limits<float>::max()/1024.;
            if(samples_==1000000 || std::abs(raw.position.x)>limit || std::abs(raw.position.y)>limit ||
               !(raw.timestamp<double(INT64_MAX)/1e9)) return false;
            auto c=filter_.filter(raw);if(!c) return false;
            float r=brush_.radiusWorld*(0.35f+1.15f*raw.pressure);
            if(!std::isfinite(r)) return false;
            if(!point(raw_,ReplaySpan::Raw,raw.position,r,ctx,emit) ||
               !point(clean_,ReplaySpan::Filtered,c.value().position,r,ctx,emit)) return false;
            Relay relay{this,ctx,emit};
            const auto& v=c.value();
            if(!spline_.append({v.position,v.timestamp,v.pressure,v.tilt_x,v.tilt_y,v.speed},&relay,forward)) return false;
            lastRadius_=r;++samples_;
        }
        failed_=false;return true;
    }
    bool finish(void* ctx,Emit emit) {
        if(failed_||done_||!samples_||!emit) {failed_=true;return false;}
        done_=true;failed_=true;
        Relay relay{this,ctx,emit};
        if(!spline_.finish(&relay,forward)) return false;
        ReplaySpan terminal{ReplaySpan::Terminal,spline_.emitted(),{},clean_.x,clean_.y,raw_.x,raw_.y,clean_.peak,raw_.peak,0,brush_.radiusWorld};
        if(!emit(ctx,terminal)) return false;
        failed_=false;return true;
    }
    size_t samples() const noexcept {return samples_;}
    size_t curveSpans() const noexcept {return spline_.emitted();}
private:
    struct PointState {float x=0,y=0,peak=0;size_t ordinal=0;};
    struct Relay {CanonicalStream* self;void* ctx;Emit emit;};
    CanonicalStream(Brush b,input::OneEuroFilter filter,stroke::SplineFitter f):brush_(b),filter_(std::move(filter)),spline_(std::move(f)) {}
    static bool forward(void* v,const stroke::CurveSpan& c) {
        auto& relay=*static_cast<Relay*>(v);
        return relay.emit(relay.ctx,ReplaySpan{ReplaySpan::Curve,c.ordinal,c,0,0,0,0,0,0,0,relay.self->brush_.radiusWorld});
    }
    bool point(PointState& p,ReplaySpan::Kind kind,core::Vec2 position,float r,void* ctx,Emit emit) {
        float x=float(position.x),y=float(position.y);
        if(!std::isfinite(x)||!std::isfinite(y)) return false;
        if(!samples_) p.peak=r;
        else if(p.x!=x||p.y!=y) {
            ReplaySpan span{kind,p.ordinal,{},p.x,p.y,x,y,lastRadius_,r,p.peak,brush_.radiusWorld};
            if(!emit(ctx,span)) return false;
            ++p.ordinal;p.peak=r;
        } else p.peak=std::max(p.peak,r);
        p.x=x;p.y=y;return true;
    }
    Brush brush_;
    input::OneEuroFilter filter_;
    stroke::StreamingSpline spline_;
    PointState raw_,clean_;
    float lastRadius_=0;
    size_t samples_=0;
    bool failed_=false,done_=false;
};
static_assert(sizeof(CanonicalStream)<=2048,"fixed replay state budget");
}
