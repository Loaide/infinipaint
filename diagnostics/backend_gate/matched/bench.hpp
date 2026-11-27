#pragma once
#include <android/native_window.h>
#include <android/log.h>
#include <cstdint>
#include <memory>
#include <vector>
#define BLOG(...) __android_log_print(ANDROID_LOG_INFO,"IP_MATCH",__VA_ARGS__)
// Coords in top-left screen pixels. zoom=1. Pan is subtracted from X only.
struct Instance {float x,y,len,r,s,dx,dy;};
struct Vertex {float x,y,u,v,len,r,s,dx,dy;};
static_assert(sizeof(Vertex)==36 && sizeof(Instance)==28);
inline constexpr float corners[][2]={{0,-1},{1,-1},{1,1},{0,-1},{1,1},{0,1}};
struct Stroke {int first,count;float color[4];int x,y,w,h;}; // conservative scissor incl camera excursion
struct Scene {std::vector<Instance> items;std::vector<Vertex> expanded;std::vector<Stroke> strokes;};
// initial is prefix loaded at init, suffix starts at [first,first+added).
// draw only min(stroke.count, visible-stroke.first) positive chords.
struct Frame {int seq,visible,first,added;float pan;bool timer,readback;};
struct Metrics {int draws=0;uint64_t bytes=0;int64_t syncNs=0,submitNs=0,presentCallNs=0;};
// Backends report delayed results via callbacks keyed by originating seq; <=1 per seq.
void gpuResult(int seq,uint64_t ns);
void presentResult(int seq,int64_t actualNs);
int64_t nowNs();
bool detailEnabled(); // false for minimum-attribution overhead control
class Backend {public:
    virtual ~Backend()=default;
    // Frame pixels for correctness only: full window top-left tightly packed RGBA8.
    virtual Metrics frame(const Frame&,std::vector<uint8_t>* pixels)=0;
};
// Throw std::runtime_error on unsupported/error; do not silently downshift.
std::unique_ptr<Backend> openGL(ANativeWindow*,const Scene&,int initial,bool es32,bool compact);
std::unique_ptr<Backend> openVK(ANativeWindow*,const Scene&,int initial,bool compact);
