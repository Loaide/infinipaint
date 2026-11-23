#pragma once
#include "stroke/types.hpp"
#include "input/types.hpp"
#include "core/result.hpp"
#include <vector>
#include <cstddef>
namespace engine::stroke {
// Transform from Phase 1 view-local pixels into document-local world units.
// Caller freezes this mapping for a stroke; camera changes require explicit handling upstream.
struct Affine2D {
    double m00=1, m01=0, m10=0, m11=1, tx=0, ty=0;
};
class FittedSpline {
public:
    [[nodiscard]] core::Result<StrokeSample> evaluate(std::size_t segment, double u) const noexcept;
    [[nodiscard]] core::Result<StrokeSample> evaluate(double t) const noexcept;
    [[nodiscard]] std::size_t segment_count() const noexcept { return segments_.size(); }
    [[nodiscard]] bool is_dot() const noexcept { return segments_.empty() && !source_.empty(); }
    [[nodiscard]] const std::vector<SplinePoint>& source_points() const noexcept { return source_; }
    [[nodiscard]] double approximate_length() const noexcept { return total_length_; }
    // Attribute timeline: includes stationary pressure/tilt changes between geometric knots.
    [[nodiscard]] core::Result<StrokeSample> evaluate_at_time(double timestamp) const noexcept;
    [[nodiscard]] core::Vec2 local_origin() const noexcept { return origin_; }
private:
    friend class SplineFitter;
    friend class IncrementalSplineBuilder;
    struct Segment {
        core::Vec2 a{},b{},c{},d{}; // P(u) = a + u(b + u(c + u*d)), local to origin
        std::size_t left=0, right=0;
        double length=0;
    };
    std::vector<Segment> segments_;
    std::vector<SplinePoint> source_; // lossless attribute samples after coordinate conversion
    core::Vec2 origin_{};
    double total_length_=0;
};
class SplineFitter {
public:
    struct Params {
        Parameterization parameterization = Parameterization::Centripetal;
        std::size_t max_samples = 1000000;
        double arc_tolerance = 0.01; // world units, adaptive length estimate
        unsigned max_arc_depth = 14;
    };
    [[nodiscard]] static core::Result<SplineFitter> create(Params p);
    [[nodiscard]] static core::Result<SplineFitter> create() { return create(Params{}); }
    [[nodiscard]] core::Result<FittedSpline> fit(const input::CleanInputStroke& clean, const Affine2D& view_to_world) const;
    [[nodiscard]] core::Result<FittedSpline> fit_world(const std::vector<SplinePoint>& world_points) const;
    [[nodiscard]] std::size_t max_samples() const noexcept { return params_.max_samples; }
private:
    explicit SplineFitter(Params p) : params_(p) {}
    Params params_;
};
// Append-only live stroke staging. snapshot() builds the durable curve; tail_preview()
// refits at most the last four distinct geometric knots (plus stationary samples).
// Preview is provisional and must be replaced when a new knot arrives.
class IncrementalSplineBuilder {
public:
    explicit IncrementalSplineBuilder(SplineFitter fitter) : fitter_(std::move(fitter)) {}
    [[nodiscard]] core::Result<void> append(const SplinePoint& point);
    // Copies the durable result in O(N); geometry is NOT refitted.
    [[nodiscard]] core::Result<FittedSpline> snapshot() const;
    // Evaluate just cached geometry: O(1) with no full-history copy.
    [[nodiscard]] core::Result<core::Vec2> geometry_at(std::size_t segment, double u) const noexcept;
    [[nodiscard]] std::size_t segment_count() const noexcept { return cached_.segments_.size(); }
    // A provisional, independently fitted tail. NOT geometrically identical to
    // cached segments at its truncated left boundary. The caller must replace
    // provisional geometry; it must not append the preview to stable geometry.
    [[nodiscard]] core::Result<FittedSpline> tail_preview() const;
    // First global geometric segment represented by the tail preview.
    [[nodiscard]] std::size_t tail_preview_start_segment() const noexcept {
        return knot_indices_.size()>4 ? knot_indices_.size()-4 : 0;
    }
    [[nodiscard]] std::size_t stable_segment_count() const noexcept {
        return distinct_count_ > 2 ? distinct_count_ - 2 : 0;
    }
    [[nodiscard]] std::size_t sample_count() const noexcept { return points_.size(); }
    void reset() noexcept { points_.clear(); knot_indices_.clear(); cached_ = FittedSpline{}; distinct_count_ = 0; }
private:
    SplineFitter fitter_;
    std::vector<SplinePoint> points_;
    std::size_t distinct_count_ = 0;
    std::vector<std::size_t> knot_indices_;
    FittedSpline cached_;
};
}
