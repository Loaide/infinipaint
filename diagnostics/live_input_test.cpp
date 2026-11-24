#include "input/input_processor.hpp"
#include "stroke/spline.hpp"
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
    std::puts("PASS ordered ties, reversal, invalid/regressing samples, downstream rejection, cancellation");
}
