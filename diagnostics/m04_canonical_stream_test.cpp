#include "document/canonical_range.hpp"
#include "input/input_processor.hpp"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <random>
#include <vector>
#include <thread>

using namespace engine;
using namespace engine::document;
static bool rejectAlloc=false;
void* operator new(size_t n) {
    if(rejectAlloc) throw std::bad_alloc();
    if(auto* p=std::malloc(n?n:1)) return p;
    throw std::bad_alloc();
}
[[gnu::noinline]] void operator delete(void* p) noexcept {std::free(p);}
[[gnu::noinline]] void operator delete(void* p,size_t) noexcept {std::free(p);}
using Chord=std::array<float,6>;
using Mesh=std::vector<Chord>;
static void require(bool b,const char* msg) {if(!b){std::fprintf(stderr,"FAIL %s\n",msg);std::abort();}}
static bool same(const Mesh& a,const Mesh& b) {
    return a.size()==b.size() && (a.empty()||!std::memcmp(a.data(),b.data(),a.size()*sizeof(Chord)));
}
static float radius(const Brush& b,float p){return b.radiusWorld*(0.35f+1.15f*p);}
struct Expected {
    std::array<Mesh,3> modes;
    stroke::IncrementalSplineBuilder spline;
    explicit Expected(stroke::SplineFitter f):spline(std::move(f)){}
};
static Expected oracle(const input::RawInputStroke& raw,const Brush& b) {
    Expected out(stroke::SplineFitter::create().value());
    auto filter=input::InputProcessor::create(b.filter).value();require(bool(filter.begin_stroke()),"oracle begin");
    // Independent full-history reference. v1 input float conversion, dwell
    // pressure and terminal behavior deliberately follow existing production.
    std::array<std::vector<core::Vec2>,2> pts;
    std::array<float,2> peaks{};
    float last=0;
    for(auto& p:raw) {
        auto clean=filter.process(p);require(bool(clean),"oracle filter");auto c=clean.value();
        require(bool(out.spline.append({c.position,c.timestamp,c.pressure,c.tilt_x,c.tilt_y,c.speed})),"oracle spline");
        std::array<core::Vec2,2> xy{p.position,c.position};
        const auto r=radius(b,p.pressure);
        for(size_t m=0;m<2;++m) {
            const auto x=float(xy[m].x),y=float(xy[m].y);
            if(!pts[m].empty()) {
                auto px=float(pts[m].back().x),py=float(pts[m].back().y);
                if(px!=x||py!=y) {
                    if(peaks[m]>last) out.modes[m].push_back({px,py,px,py,peaks[m],peaks[m]});
                    out.modes[m].push_back({px,py,x,y,last,r});peaks[m]=r;
                } else peaks[m]=std::max(peaks[m],r);
            } else peaks[m]=r;
            pts[m].push_back({x,y});
        }
        last=r;
    }
    for(size_t s=0;s<out.spline.segment_count();++s) {
        const auto first=out.spline.geometry_at(s,0.).value();float x=float(first.x),y=float(first.y);
        float r0=radius(b,out.spline.departure_pressure(s)),r1=radius(b,out.spline.arrival_pressure(s+1)),p=radius(b,out.spline.knot_pressure(s)),r=r0;
        if(p>r0) out.modes[2].push_back({x,y,x,y,p,p});
        auto steps=out.spline.tessellation_steps(s,.20*b.radiusWorld/1.9);
        for(int k=1;k<=steps;++k) {
            auto point=out.spline.geometry_at(s,double(k)/steps).value();float nx=float(point.x),ny=float(point.y),nr=std::lerp(r0,r1,float(k)/steps);
            out.modes[2].push_back({x,y,nx,ny,r,nr});x=nx;y=ny;r=nr;
        }
    }
    const auto rx=float(pts[0].back().x),ry=float(pts[0].back().y),cx=float(pts[1].back().x),cy=float(pts[1].back().y);
    if(std::hypot(rx-cx,ry-cy)>.03f) for(size_t m=1;m<3;++m) out.modes[m].push_back({cx,cy,rx,ry,peaks[1],peaks[0]});
    for(auto& m:out.modes)m.push_back({rx,ry,rx,ry,peaks[0],peaks[0]});
    return out;
}
struct Actual {
    std::array<Mesh,3> modes;
    std::vector<ReplaySpan> spans;
    static bool emit(void* p,const ReplaySpan& s) {
        auto& a=*static_cast<Actual*>(p);
        auto add=[&](size_t i,auto... v){a.modes[i].push_back({v...});return true;};
        if(s.kind==ReplaySpan::Terminal) {
            a.spans.push_back(s);add(0,s.bx,s.by,s.bx,s.by,s.arrival,s.arrival);
            for(size_t i=1;i<3;++i) require(s.chords([&](auto... v){return add(i,v...);}),"terminal");
        } else {
            if(s.kind==ReplaySpan::Curve)a.spans.push_back(s);
            require(s.chords([&](auto... v){return add(size_t(s.kind),v...);}),"chords");
        }
        return true;
    }
};
static uint64_t chordChecks=0,curveChecks=0,boundsChecks=0,packetChecks=0,rawChecks=0;
static Actual compare(const input::RawInputStroke& raw,const Brush& b,const Expected& expected,size_t chunk) {
    auto stream=CanonicalStream::create(b).value();Actual actual;
    for(size_t i=0;i<raw.size();i+=chunk) require(stream.append(std::span(raw).subspan(i,std::min(chunk,raw.size()-i)),&actual,Actual::emit),"stream append");
    require(stream.finish(&actual,Actual::emit),"stream finish");require(stream.samples()==raw.size(),"sample count");
    for(size_t m=0;m<3;++m) {require(same(actual.modes[m],expected.modes[m]),"v1 chord stream differs");chordChecks+=actual.modes[m].size();}
    require(actual.spans.size()==expected.spline.segment_count()+1,"curve and terminal span count");
    for(size_t i=0;i+1<actual.spans.size();++i) {
        const auto& s=actual.spans[i];const auto& c=s.curve;
        require(s.ordinal==i&&s.kind==ReplaySpan::Curve,"span order");
        require(c.arrival==expected.spline.arrival_pressure(i+1)&&c.departure==expected.spline.departure_pressure(i)&&c.peak==expected.spline.knot_pressure(i),"pressure boundary");
        require(c.linear_error_bound()==expected.spline.linear_error_bound(i),"error bound parity");
        SpanBounds box;const double dx=137.25,dy=-509.5;
        require(spanBounds(s,dx,dy,box),"span bounds");
        for(int k=0;k<=16;++k) {
            double u=double(k)/16;const auto p=c.at(u),q=expected.spline.geometry_at(i,u).value();
            require(!std::memcmp(&p,&q,sizeof(p)),"bit-exact curve evaluation");++curveChecks;
            const double pad=radius(b,std::max({c.arrival,c.departure,c.peak}))+10.;
            require(p.x+dx-pad>=box.minX&&p.x+dx+pad<=box.maxX&&p.y+dy-pad>=box.minY&&p.y+dy+pad<=box.maxY,"cubic footprint enclosure");++boundsChecks;
        }
        require(s.chords([&](float ax,float ay,float bx,float by,float a,float d){
            float vx=bx-ax,vy=by-ay,len=std::hypot(vx,vy);
            if(len<.0001f){vx=1;vy=0;len=0;}else{vx/=len;vy/=len;}
            ax=float(double(ax)+dx);ay=float(double(ay)+dy);float x=ax+vx*len,y=ay+vy*len,pad=std::max(a,d)+10.f;
            require(std::min(ax,x)-pad>=box.minX&&std::max(ax,x)+pad<=box.maxX&&std::min(ay,y)-pad>=box.minY&&std::max(ay,y)+pad<=box.maxY,"legacy float footprint enclosure");++boundsChecks;return true;
        }),"bounds chords");
    }
    require(actual.spans.back().kind==ReplaySpan::Terminal&&actual.spans.back().ordinal==actual.spans.size()-1,"terminal identity");
    SpanBounds terminal;require(spanBounds(actual.spans.back(),137.25,-509.5,terminal),"terminal bounds");
    return actual;
}
static uint32_t u32(std::span<const uint8_t> a,size_t p) {uint32_t out=0;for(size_t i=0;i<4;++i)out|=uint32_t(a[p+i])<<(i*8);return out;}
static std::vector<uint8_t> getRange(const input::RawInputStroke& raw,const Brush& b,size_t first,size_t count,size_t chunk) {
    wire::Header h;require(wire::header(b,137.25,-509.5,h),"encode header");
    auto rr=CanonicalRange::create(h,raw.size(),first,count);require(bool(rr),"range create");auto r=std::move(rr).value();
    std::array<uint8_t,wire::chunkSamples*wire::sampleBytes> bytes{};
    for(size_t i=0;i<raw.size();i+=chunk) {
        const auto n=std::min(chunk,raw.size()-i);auto dst=std::span(bytes).first(n*wire::sampleBytes);
        require(wire::samples(std::span(raw).subspan(i,n),dst),"wire encode");
        require(r->append(dst),"range append");require(r->result().empty(),"no unverified prefix exposed");
    }
    require(r->finish(),"range seal");auto data=r->result();
    require(data.size()>=CanonicalRange::headerBytes&&u32(data,0)==CanonicalRange::magic&&u32(data,4)==1&&u32(data,8)==CanonicalRange::packetBytes&&u32(data,12)==first&&u32(data,24)==raw.size(),"packet header");
    require(data.size()==CanonicalRange::headerBytes+u32(data,16)*CanonicalRange::packetBytes,"packet extent");
    require(!std::memcmp(data.data()+28,h.data(),h.size()),"exact source header");
    for(size_t i=0;i<u32(data,16);++i) {
        auto offset=CanonicalRange::headerBytes+i*CanonicalRange::packetBytes;
        require(u32(data,offset+4)==first+i,"packet ordinal");++packetChecks;
    }
    return {data.begin(),data.end()};
}
static input::RawInputStroke generate(uint32_t seed,size_t n,double base=0) {
    std::mt19937 rng(seed);input::RawInputStroke raw;raw.reserve(n);
    double x=base,y=-base,t=1.;
    for(size_t i=0;i<n;++i) {
        if(i%23<17){x+=(int(rng()%2001)-1000)*.021;y+=(int(rng()%2001)-1000)*.023;}
        if(i%7!=0 && i%23<17)t+=double(1+rng()%18)*.0005;
        raw.push_back({{x,y},t,float(rng()%1001)/1000.f,float(int(rng()%31)-15),float(int(rng()%31)-15)});
    }
    return raw;
}
static void random(uint32_t seed,int runs) {
    std::mt19937 rng(seed);
    for(int i=0;i<runs;++i) {
        auto n=size_t(1+rng()%650);if(i%10==0)n=1821+rng()%2100;
        auto raw=generate(rng(),n,i%5==0?1099511627776.:i%7==0?1048576.:0.);
        Brush b;b.radiusWorld=std::array<float,5>{.119f,.4f,1.9f,6.f,19.f}[i%5];b.filter.beta=i%3==0?0:.007;
        auto expected=oracle(raw,b);
        auto actual=compare(raw,b,expected,1+rng()%wire::chunkSamples);
        // Overlapping independently requested pages must contain identical
        // serialized packets despite different raw input partitions.
        const auto total=actual.spans.size();size_t first=rng()%total;
        auto p=getRange(raw,b,first,128,wire::chunkSamples);
        auto q=getRange(raw,b,first,128,1+rng()%127);
        require(p==q,"range partition independence");
        size_t k=std::min<size_t>(u32(p,16)-1,5);
        auto r=getRange(raw,b,first+k,1,37);
        require(!std::memcmp(p.data()+CanonicalRange::headerBytes+k*CanonicalRange::packetBytes,r.data()+CanonicalRange::headerBytes,CanonicalRange::packetBytes),"overlapping range parity");
        require(u32(p,20)==total,"total spans");rawChecks+=raw.size();
    }
    std::printf("PASS seed=%u strokes=%d raw=%llu exact_chords=%llu exact_curve_evaluations=%llu bounds=%llu packets=%llu\n",seed,runs,(unsigned long long)rawChecks,(unsigned long long)chordChecks,(unsigned long long)curveChecks,(unsigned long long)boundsChecks,(unsigned long long)packetChecks);
}
static bool countSpan(void* p,const ReplaySpan&){++*static_cast<size_t*>(p);return true;}
static bool rejectSpan(void*,const ReplaySpan&){return false;}
static void faults() {
    Brush b;wire::Header h;require(wire::header(b,0,0,h),"header");
    auto raw=generate(1,17);auto replay=CanonicalStream::create(b).value();
    size_t count=0;require(!replay.append(raw,&count,rejectSpan),"cancel span");require(!replay.finish(&count,countSpan)&&!replay.append(raw,&count,countSpan),"poisoned after cancellation");
    replay=CanonicalStream::create(b).value();require(replay.append(std::span(raw).first(1),&count,countSpan),"one sample");
    auto bad=raw[0];bad.timestamp=0;require(!replay.append({&bad,1},&count,countSpan),"regressing time");require(!replay.finish(&count,countSpan),"bad sample poisons");
    replay=CanonicalStream::create(b).value();require(replay.append(std::span(raw).first(1),&count,countSpan),"one point");
    require(!replay.finish(&count,rejectSpan)&&!replay.finish(&count,countSpan),"terminal rejection");
    rejectAlloc=true;auto oom=CanonicalRange::create(h,17,0,1);rejectAlloc=false;require(!oom&&oom.error().code==core::ErrorCode::AllocationFailure,"allocation failure");
    auto rr=CanonicalRange::create(h,raw.size(),0,128);require(bool(rr),"range");auto r=std::move(rr).value();
    std::array<uint8_t,17*wire::sampleBytes> bytes{};require(wire::samples(raw,bytes),"encode");
    require(!r->append(std::span(bytes).first(1))&&r->result().empty()&&!r->finish(),"partial sample rejected");
    r=std::move(CanonicalRange::create(h,raw.size()+1,0,128)).value();require(r->append(bytes)&&!r->finish()&&r->result().empty(),"truncated record");
    r=std::move(CanonicalRange::create(h,raw.size()-1,0,128)).value();require(!r->append(bytes)&&r->result().empty(),"excess samples");
    r=std::move(CanonicalRange::create(h,raw.size(),1000,1)).value();require(r->append(bytes)&&!r->finish()&&r->result().empty(),"out-of-range first");
    r=std::move(CanonicalRange::create(h,raw.size(),0,1)).value();auto broken=bytes;
    // first sample pressure NaN, little endian offset 24
    broken[24]=0;broken[25]=0;broken[26]=192;broken[27]=127;
    require(!r->append(broken)&&r->result().empty()&&!r->finish(),"bad wire value");
    require(!CanonicalRange::create(h,0,0,1)&&!CanonicalRange::create(h,1,0,129)&&!CanonicalRange::create(h,1,1000001,1),"range limits");
    auto noAlloc=std::move(CanonicalRange::create(h,raw.size(),0,128)).value();
    rejectAlloc=true;
    require(noAlloc->append(bytes)&&noAlloc->finish()&&!noAlloc->result().empty(),"range needs no post-admission allocation");
    rejectAlloc=false;
    h[0]=0;require(!CanonicalRange::create(h,1,0,1),"version rejection");
    // Complete stationary stroke: exactly one terminal, even with dwell.
    input::RawInputStroke dot{{{1,2},1,.2f,0,0},{{1,2},1,.9f,0,0},{{1,2},1,.1f,0,0}};
    auto exp=oracle(dot,b);compare(dot,b,exp,1);
    auto terminal=getRange(dot,b,0,128,1);require(u32(terminal,16)==1&&u32(terminal,20)==1,"dot terminal only");
    auto empty=getRange(dot,b,1,1,1);require(u32(empty,16)==0,"end sentinel");
    std::puts("PASS failure cases: invalid input, cancellation/poison, terminal rejection, allocation, malformed/truncated/excess samples, range limits, bad version, dot and end sentinel");
}
static void scale() {
    using Clock=std::chrono::steady_clock;
    for(bool moving:{false,true}) {
        auto replay=CanonicalStream::create(Brush{}).value();size_t output=0;
        std::array<input::RawInputPoint,wire::chunkSamples> raw{};
        const auto start=Clock::now();
        size_t n=moving?100000:1000000;
        // Decoder and fitter allocate nothing, even across a million samples.
        rejectAlloc=true;
        for(size_t i=0;i<n;) {
            size_t count=std::min(raw.size(),n-i);
            for(size_t k=0;k<count;++k)raw[k]={{moving?double(i+k)*.1:1.,moving?double((i+k)%31)*.01:2.},1.+(i+k)*.001,float((i+k)%11)/10.f,0,0};
            require(replay.append(std::span(raw).first(count),&output,countSpan),"bounded scale append");i+=count;
        }
        require(replay.finish(&output,countSpan),"bounded scale finish");rejectAlloc=false;
        require(replay.samples()==n&&replay.curveSpans()==(moving?n-1:0),"scale count");
        auto ms=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
        std::printf("PASS allocation-disabled samples=%zu moving=%d spans=%zu emissions=%zu replay_ms=%.3f state=%zu range_state=%zu range_max_result=%zu\n",n,moving,replay.curveSpans(),output,ms,sizeof(CanonicalStream),sizeof(CanonicalRange),CanonicalRange::maxResult);
    }
}
int main(int argc,char** argv) {
    if(argc>1&&std::string_view(argv[1])=="faults"){faults();return 0;}
    if(argc>1&&std::string_view(argv[1])=="scale"){scale();return 0;}
    random(argc>1?uint32_t(std::strtoul(argv[1],nullptr,10)):13792,argc>2?std::atoi(argv[2]):20);
}
