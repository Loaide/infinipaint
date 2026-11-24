// Single-threaded host probe. Requested allocation traffic is not peak RSS.
#include "stroke/spline.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>

static size_t calls, bytes;
void* operator new(size_t n) {
    if (void* p = std::malloc(n ? n : 1)) { ++calls; bytes += n; return p; }
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
        for (size_t i=0;i<n;++i) {
            const SplinePoint p{{stationary ? 0. : double(i),0.},double(i),0.5,0.,0.,0.};
            auto start=clock::now();
            if (!b.append(p)) return 1;
            us[i]=std::chrono::duration<double,std::micro>(clock::now()-start).count();
        }
        std::sort(us.begin(),us.end());
        if(argc>1 && (bytes>2048*n || (stationary && calls>32))) return 2;
        std::printf("%s n=%zu allocations=%zu requested_bytes=%zu append_us p50/p95/p99/max %.3f %.3f %.3f %.3f\n",
            stationary?"dot":"line",n,calls,bytes,us[n/2],us[n*95/100],us[n*99/100],us.back());
    }
}
