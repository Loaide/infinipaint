#pragma once
#include "core/math/vec2.hpp"
namespace engine::stroke {
// All stored positions are in document-local world units, NOT view pixels.
struct SplinePoint {
    core::Vec2 position{};
    double timestamp = 0.0;
    float pressure = 1.0f;
    float tilt_x = 0.0f, tilt_y = 0.0f;
    double speed = 0.0; // VIEW-LOCAL pixels/second, not world units/second
};
enum class Parameterization { Uniform, Centripetal, Chordal };
struct StrokeSample {
    core::Vec2 position{}; // document-local world units
    core::Vec2 tangent{};  // derivative with respect to segment-local u
    double timestamp = 0.0;
    double pressure = 1.0;
    double tilt_x = 0.0, tilt_y = 0.0;
    double speed = 0.0; // VIEW-LOCAL pixels/second, not world units/second
};
}
