#pragma once
#include "stroke/spline.hpp"
#include <cstdio>

// Capture-space approximation, no GPU rounding or later zoom. Report both a
// rigorous parameter-interpolation bound and a sampled error (not a proof).
inline size_t inspectTessellation(const char* name,const engine::stroke::IncrementalSplineBuilder& b) {
    size_t capped=0;
    double worst=0,bound=0;
    for(size_t s=0;s<b.segment_count();++s) {
        int n=b.tessellation_steps(s,.20);
        capped+=n==b.max_steps;
        bound=std::max(bound,b.linear_error_bound(s)/(n*n));
        for(int k=0;k<n;++k) {
            auto a=b.geometry_at(s,double(k)/n).value(),z=b.geometry_at(s,double(k+1)/n).value();
            for(int j=1;j<32;++j) {
                double u=j/32.;
                auto v=b.geometry_at(s,(k+u)/n).value();
                worst=std::max(worst,std::hypot(v.x-std::lerp(a.x,z.x,u),v.y-std::lerp(a.y,z.y,u)));
            }
        }
    }
    std::printf("tessellation %s segments=%zu at_cap=%zu max_bound_px=%.6f sampled_max_px=%.6f\n",
        name,b.segment_count(),capped,bound,worst);
    if(worst>bound+1e-8 || (!capped && bound>.20000001)) std::abort();
    return capped;
}
