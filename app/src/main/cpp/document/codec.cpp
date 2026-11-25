#include "codec.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace engine::document::wire {
namespace {
static_assert(std::numeric_limits<double>::is_iec559 && std::numeric_limits<float>::is_iec559);
template<class T> using Bits = std::conditional_t<sizeof(T)==8,uint64_t,uint32_t>;
template<class T> void put(uint8_t*& p,T value) noexcept {
    auto bits=std::bit_cast<Bits<T>>(value);
    for(size_t i=0;i<sizeof(T);++i) { *p++=uint8_t(bits); bits>>=8; }
}
template<class T> T take(const uint8_t*& p) noexcept {
    Bits<T> bits=0;
    for(size_t i=0;i<sizeof(T);++i) bits|=Bits<T>(*p++)<<(8*i);
    return std::bit_cast<T>(bits);
}
bool valid(const Brush& b,double x,double y) noexcept {
    return b.recipe==1 && b.brush==1 && b.radiusWorld>0 && std::isfinite(b.radiusWorld) &&
        std::isfinite(x) && std::isfinite(y) && std::isfinite(b.filter.min_cutoff) &&
        b.filter.min_cutoff>0 && std::isfinite(b.filter.dcutoff) && b.filter.dcutoff>0 &&
        std::isfinite(b.filter.beta) && b.filter.beta>=0 &&
        std::all_of(b.rgba.begin(),b.rgba.end(),[](float f){return f>=0 && f<=1;});
}
bool valid(const input::RawInputPoint& p,double prev) noexcept {
    constexpr double limit=std::numeric_limits<float>::max()/1024.;
    return std::abs(p.position.x)<=limit && std::abs(p.position.y)<=limit &&
           p.timestamp>=0 && p.timestamp>=prev && p.timestamp<double(INT64_MAX)/1e9 &&
           p.pressure>=0 && p.pressure<=1 && std::isfinite(p.tilt_x) && std::isfinite(p.tilt_y);
}
input::RawInputPoint point(const uint8_t*& p) noexcept {
    return {{take<double>(p),take<double>(p)},take<double>(p),take<float>(p),take<float>(p),take<float>(p)};
}
}
bool header(const Brush& b,double dx,double dy,Header& out) noexcept {
    if(!valid(b,dx,dy)) return false;
    auto p=out.data();
    put(p,uint32_t{1}); put(p,uint32_t{1}); // header version, world-unit version
    put(p,b.recipe); put(p,b.brush); put(p,b.tool); put(p,b.radiusWorld);
    put(p,b.filter.min_cutoff); put(p,b.filter.beta); put(p,b.filter.dcutoff);
    for(float f:b.rgba) put(p,f);
    put(p,dx); put(p,dy);
    return true;
}
bool readHeader(std::span<const uint8_t> in,Brush& b,double& dx,double& dy) noexcept {
    if(in.size()!=headerBytes) return false;
    auto p=in.data();
    if(take<uint32_t>(p)!=1 || take<uint32_t>(p)!=1) return false;
    Brush candidate;
    candidate.recipe=take<uint32_t>(p); candidate.brush=take<uint32_t>(p); candidate.tool=take<uint32_t>(p);
    candidate.radiusWorld=take<float>(p);
    candidate.filter={take<double>(p),take<double>(p),take<double>(p)};
    for(float& f:candidate.rgba) f=take<float>(p);
    const auto x=take<double>(p), y=take<double>(p);
    if(!valid(candidate,x,y)) return false;
    b=candidate; dx=x; dy=y; return true;
}
bool samples(std::span<const input::RawInputPoint> src,std::span<uint8_t> out) noexcept {
    if(src.empty() || src.size()>chunkSamples || out.size()!=sampleBytes*src.size()) return false;
    double prev=-1;
    for(const auto& s:src) { if(!valid(s,prev)) return false; prev=s.timestamp; }
    auto p=out.data();
    for(const auto& s:src) {
        put(p,s.position.x); put(p,s.position.y); put(p,s.timestamp);
        put(p,s.pressure); put(p,s.tilt_x); put(p,s.tilt_y);
    }
    return true;
}
bool readSamples(std::span<const uint8_t> in,double prev,std::span<input::RawInputPoint> out) noexcept {
    if(out.empty() || out.size()>chunkSamples || in.size()!=out.size()*sampleBytes) return false;
    auto p=in.data();
    for(size_t i=0;i<out.size();++i) {
        auto s=point(p);
        if(!valid(s,prev)) return false;
        prev=s.timestamp;
    }
    p=in.data(); for(auto& s:out) s=point(p);
    return true;
}
}
