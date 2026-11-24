#include "input/input_processor.hpp"
#include "stroke/spline.hpp"
#include "tessellation_probe.hpp"
#include <cassert>
#include <limits>
#include <cstdio>
using namespace engine;

int main() {
    auto p=input::InputProcessor::create().value();
    stroke::SplineFitter::Params params;
    params.max_samples=4;
    stroke::IncrementalSplineBuilder b(stroke::SplineFitter::create(params).value());
    const auto accept=[](void* ctx,const input::CleanInputPoint& c) {
        return static_cast<stroke::IncrementalSplineBuilder*>(ctx)->append(
            {c.position,c.timestamp,c.pressure,c.tilt_x,c.tilt_y,c.speed});
    };
    auto sample=[&](double x,double t,float pressure) {
        return p.process({{x,0},t,pressure,0,0},&b,accept);
    };
    assert(p.begin_stroke());
    assert(sample(0,1,0));
    assert(sample(2,1,.5f));
    assert(!sample(4,0,.5f));
    assert(!sample(std::numeric_limits<double>::quiet_NaN(),1,.5f));
    assert(sample(0,1,.75f)); // direction reversal at the very same instant
    assert(sample(0,1,1));    // pressure-only endpoint
    assert(!sample(99,2,1));  // downstream spline limit, not input limit
    assert(p.raw_samples().size()==4 && p.clean_samples().size()==4 && b.sample_count()==4);
    assert(p.raw_samples().back().timestamp==1);
    assert(b.segment_count()==2 && b.stable_segment_count()==1);
    assert(b.geometry_at(0,1).value().x==2 && b.geometry_at(1,1).value().x==0);
    const auto fitted=b.snapshot().value();
    assert(fitted.source_points().size()==4);
    assert(fitted.evaluate_at_time(1).value().pressure==1);
    assert(fitted.evaluate(0,1).value().pressure==.5);
    assert(b.tail_preview().value().source_points().size()==4);
    p.cancel_stroke(); b.reset();
    assert(!sample(0,0,1));
    assert(p.begin_stroke() && sample(4,0,1).value().position.x==4);
    stroke::IncrementalSplineBuilder loop(stroke::SplineFitter::create().value());
    double t=0;
    for(auto v:std::vector<core::Vec2>{{2,2},{42,3},{40,44},{42,3},{2,2},{-40,40}}) {
        assert(loop.append({v,t++,.5f,0,0,0}));
    }
    assert(loop.append({{-40,40},t,1.f,0,0,0}));
    assert(loop.knot_pressure(loop.segment_count())==1.f);
    assert(inspectTessellation("tight-loop",loop)==0);
    for(double extent:{2000.,40000.}) {
        stroke::IncrementalSplineBuilder leap(stroke::SplineFitter::create().value());
        double time=0;
        for(auto v:std::vector<core::Vec2>{{0,0},{extent,0},{extent,extent},{0,extent},{extent,extent}})
            assert(leap.append({v,time++,.5f,0,0,0}));
        const auto hits=inspectTessellation(extent==2000.?"tablet-scale-leaps":"40k-leaps",leap);
        assert(extent==40000. ? hits>0 : hits==0);
    }
    for(auto mode:{stroke::Parameterization::Uniform,stroke::Parameterization::Chordal,
                   stroke::Parameterization::Centripetal}) {
        stroke::SplineFitter::Params fp; fp.parameterization=mode;
        const auto fitter=stroke::SplineFitter::create(fp).value();
        stroke::IncrementalSplineBuilder live(fitter);
        std::vector<stroke::SplinePoint> points;
        std::vector<core::Vec2> stable;
        for(auto v:std::vector<core::Vec2>{{0,0},{1,.01},{1,.01},{9,10},{1,.01},{-55,72},{100,-1}}) {
            points.push_back({v,1,.5f,0,0,0});
            assert(live.append(points.back()));
            const auto batch=fitter.fit_world(points).value();
            assert(batch.segment_count()==live.segment_count());
            assert(std::abs(batch.approximate_length()-live.snapshot().value().approximate_length())<1e-9);
            for(size_t k=0;k<live.segment_count();++k) for(double u:{0.,.25,.5,.75,1.}) {
                auto a=batch.evaluate(k,u).value().position;
                auto b=live.geometry_at(k,u).value();
                assert(core::Vec2::distance(a,b)<1e-10);
            }
            for(size_t k=0;k<stable.size();++k)
                assert(core::Vec2::distance(stable[k],live.geometry_at(k,.5).value())==0.);
            while(stable.size()<live.stable_segment_count())
                stable.push_back(live.geometry_at(stable.size(),.5).value());
        }
    }
    stroke::IncrementalSplineBuilder collapsed(stroke::SplineFitter::create().value());
    assert(collapsed.append({{0x1p60,0},1,.5f,0,0,0}) && collapsed.append({{0,0},2,.5f,0,0,0}));
    assert(!collapsed.append({{1,0},3,.5f,0,0,0}) && collapsed.sample_count()==2);
    for(size_t s=0;s<loop.segment_count();++s) {
        int n=loop.tessellation_steps(s,.2);
        assert(n<loop.max_steps);
        for(int k=0;k<n;++k) {
            auto a=loop.geometry_at(s,double(k)/n).value();
            auto z=loop.geometry_at(s,double(k+1)/n).value();
            for(int j=1;j<32;++j) {
                double u=j/32.;
                auto v=loop.geometry_at(s,(k+u)/n).value();
                assert(std::hypot(v.x-std::lerp(a.x,z.x,u),v.y-std::lerp(a.y,z.y,u))<=.2);
            }
        }
    }
    std::puts("PASS ordered ties, reversal, invalid/regressing samples, downstream rejection, cancellation");
}
