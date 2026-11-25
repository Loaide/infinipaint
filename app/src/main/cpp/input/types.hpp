#pragma once
#include "core/math/vec2.hpp"
#include <cstdint>
#include <vector>
namespace engine::input {
// Position and velocity use the caller's coordinate space consistently for a stroke.
// The tablet bridge maps view pixels to world units before processing; view-local
// replay callers can instead use pixels. Timestamps are monotonic seconds (ties allowed).
// One processor instance owns one active pointer/stroke; routing pointer IDs is the caller's job.
struct RawInputPoint {
    core::Vec2 position;
    double timestamp = 0.0;
    float pressure = 1.0f;
    // Tilt components are projected angles in radians, not sensor X/Y axes.
    float tilt_x = 0.0f, tilt_y = 0.0f;
};
struct CleanInputPoint {
    core::Vec2 position;
    double timestamp = 0.0;
    float pressure = 1.0f;
    // Tilt components are projected angles in radians, not sensor X/Y axes.
    float tilt_x = 0.0f, tilt_y = 0.0f;
    core::Vec2 velocity;
    double speed = 0.0;
};
using RawInputStroke = std::vector<RawInputPoint>;
using CleanInputStroke = std::vector<CleanInputPoint>;
}
