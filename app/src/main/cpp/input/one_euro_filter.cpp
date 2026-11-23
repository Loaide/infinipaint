#include "input/one_euro_filter.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>
#include <new>
#include <stdexcept>
namespace engine::input {
namespace {
double alpha(double cutoff,double dt) {
    const double x = (2.0*std::numbers::pi*dt)*cutoff;
    if (std::isinf(x)) return 1.0;
    return x/(1.0+x);
}
core::Vec2 lowpass(core::Vec2 previous,core::Vec2 current,double a) {
    // Weighted form avoids overflow of (current-previous) for finite endpoints.
    return {std::lerp(previous.x,current.x,a),std::lerp(previous.y,current.y,a)};
}
core::Result<void> validate(const RawInputPoint& p) {
    if (!p.position.finite() || !std::isfinite(p.timestamp) || !std::isfinite(p.pressure)
        || !std::isfinite(p.tilt_x) || !std::isfinite(p.tilt_y))
        return core::Error{core::ErrorCode::NonFinite,"Non-finite input sample"};
    if (p.timestamp < 0.0) return core::Error{core::ErrorCode::InvalidArgument,"Negative timestamp"};
    if (p.pressure < 0.0f || p.pressure > 1.0f)
        return core::Error{core::ErrorCode::InvalidArgument,"Pressure outside [0,1]"};
    return {};
}
}
core::Result<OneEuroFilter> OneEuroFilter::create(OneEuroParams p) {
    if (!std::isfinite(p.min_cutoff) || !std::isfinite(p.beta) || !std::isfinite(p.dcutoff)
        || p.min_cutoff<=0.0 || p.beta<0.0 || p.dcutoff<=0.0)
        return core::Error{core::ErrorCode::InvalidArgument,"Invalid One-Euro parameters"};
    return OneEuroFilter(p);
}
void OneEuroFilter::reset() noexcept {
    initialized_=false; prev_time_=0.0; prev_raw_={}; prev_filtered_={}; prev_derivative_={};
}
core::Result<CleanInputPoint> OneEuroFilter::filter(const RawInputPoint& p) {
    auto valid=validate(p);
    if (!valid) return valid.error();
    if (initialized_ && p.timestamp<prev_time_)
        return core::Error{core::ErrorCode::NonMonotonicTime,"Timestamps must strictly increase"};
    CleanInputPoint out;
    out.timestamp=p.timestamp; out.pressure=p.pressure;
    out.tilt_x=p.tilt_x; out.tilt_y=p.tilt_y;
    if (!initialized_) {
        out.position=p.position;
        prev_time_=p.timestamp; prev_raw_=p.position; prev_filtered_=p.position;
        prev_derivative_={}; initialized_=true;
        return out;
    }
    const double dt=p.timestamp-prev_time_;
    if (dt == 0.0) {
        // No elapsed time: retain original timestamp and sample. Avoid fake dt,
        // infinite derivative, or a fabricated filter update.
        out.position=prev_filtered_;
        out.velocity=prev_derivative_;
        out.speed=prev_derivative_.length();
        prev_raw_=p.position;
        return out;
    }
    if (!std::isfinite(dt) || dt<0.0)
        return core::Error{core::ErrorCode::NumericalError,"Invalid elapsed time"};
    const core::Vec2 derivative=(p.position-prev_raw_)/dt;
    if (!derivative.finite())
        return core::Error{core::ErrorCode::NumericalError,"Derivative overflow"};
    const core::Vec2 filtered_derivative=lowpass(prev_derivative_,derivative,alpha(params_.dcutoff,dt));
    const double speed=filtered_derivative.length();
    if (!std::isfinite(speed))
        return core::Error{core::ErrorCode::NumericalError,"Speed overflow"};
    const double cutoff=params_.min_cutoff+params_.beta*speed;
    if (!std::isfinite(cutoff) || !filtered_derivative.finite())
        return core::Error{core::ErrorCode::NumericalError,"Filter cutoff overflow"};
    const double a = alpha(cutoff,dt);
    if (!std::isfinite(a)) return core::Error{core::ErrorCode::NumericalError,"Invalid smoothing coefficient"};
    const core::Vec2 position=lowpass(prev_filtered_,p.position,a);
    if (!position.finite()) return core::Error{core::ErrorCode::NumericalError,"Filtered position overflow"};
    out.position=position; out.velocity=filtered_derivative; out.speed=speed;
    prev_time_=p.timestamp; prev_raw_=p.position;
    prev_filtered_=position; prev_derivative_=filtered_derivative;
    return out;
}
core::Result<CleanInputStroke> OneEuroFilter::filter_batch(const RawInputStroke& raw) {
    // Strong guarantee: a failed batch never mutates the filter.
    OneEuroFilter candidate=*this;
    CleanInputStroke out;
    try { out.reserve(raw.size()); }
    catch (const std::bad_alloc&) { return core::Error{core::ErrorCode::AllocationFailure,"Filter batch allocation failed"}; }
    catch (const std::length_error&) { return core::Error{core::ErrorCode::AllocationFailure,"Filter batch length exceeded"}; }
    for (const auto& p:raw) {
        auto r=candidate.filter(p);
        if (!r) return r.error();
        out.push_back(std::move(r).value());
    }
    *this=std::move(candidate);
    return out;
}
}
