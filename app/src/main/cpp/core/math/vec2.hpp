#pragma once
#include <cmath>
#include <limits>
namespace engine::core {
struct Vec2 {
    double x = 0.0, y = 0.0;
    constexpr Vec2() = default;
    constexpr Vec2(double a, double b) : x(a), y(b) {}
    constexpr Vec2 operator+(Vec2 b) const { return {x+b.x,y+b.y}; }
    constexpr Vec2 operator-(Vec2 b) const { return {x-b.x,y-b.y}; }
    constexpr Vec2 operator*(double s) const { return {x*s,y*s}; }
    constexpr Vec2 operator/(double s) const { return {x/s,y/s}; }
    constexpr Vec2& operator+=(Vec2 b) { x+=b.x; y+=b.y; return *this; }
    constexpr Vec2& operator-=(Vec2 b) { x-=b.x; y-=b.y; return *this; }
    [[nodiscard]] bool finite() const { return std::isfinite(x)&&std::isfinite(y); }
    [[nodiscard]] double length() const { return std::hypot(x,y); }
    [[nodiscard]] double length_sq() const { return x*x+y*y; }
    [[nodiscard]] Vec2 normalized() const {
        const double n=length();
        if (!(n>0.0) || !std::isfinite(n)) return {};
        return {x/n,y/n};
    }
    static double distance(Vec2 a,Vec2 b) { return (a-b).length(); }
};
inline constexpr Vec2 operator*(double s,Vec2 v) { return v*s; }
}
