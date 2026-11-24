// Single-threaded host probe. Requested allocation traffic is not peak RSS.
#include "stroke/spline.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>

static size_t calls, bytes, largest;
void* operator new(size_t n) {
    if (void* p = std::malloc(n ? n : 1)) { ++calls; bytes += n; largest=std::max(largest,n); return p; }
    throw std::bad_alloc{};
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

int main(int argc,char**) {
    using namespace engine::stroke;
    using clock = std::chrono::steady_clock;
    for (bool stationary : {false, true}) for (size_t n : {1024,4096,16384,131072}) {
        if(n==131072 && argc==1) continue;
        IncrementalSplineBuilder b(SplineFitter::create().value());
        std::vector<double> us(n);
        calls = bytes = 0;
        size_t largeEvents=0, worstSample=0,worstRequest=0;
        double largeMax=0, otherMax=0;
        for (size_t i=0;i<n;++i) {
            const SplinePoint p{{stationary ? 0. : double(i),0.},double(i),0.5,0.,0.,0.};
            largest=0;
            auto start=clock::now();
            if (!b.append(p)) return 1;
            us[i]=std::chrono::duration<double,std::micro>(clock::now()-start).count();
            if(us[i]>std::max(largeMax,otherMax)) {worstSample=i+1;worstRequest=largest;}
            // The bounded four-knot workspace never requests 1 KiB.
            if(largest>=1024) {++largeEvents;largeMax=std::max(largeMax,us[i]);}
            else otherMax=std::max(otherMax,us[i]);
        }
        std::sort(us.begin(),us.end());
        if(argc>1 && (bytes>512*n || calls>(stationary?32u:80u))) return 2;
        std::printf("%s n=%zu allocations=%zu requested_bytes=%zu append_us p50/p95/p99/max %.3f %.3f %.3f %.3f\n",
            stationary?"dot":"line",n,calls,bytes,us[n/2],us[n*95/100],us[n*99/100],us.back());
        std::printf("  >=1KiB events=%zu max_us=%.3f other_max_us=%.3f slowest_ordinal=%zu largest_request_at_slowest=%zu\n",
            largeEvents,largeMax,otherMax,worstSample,worstRequest);
    }
}
