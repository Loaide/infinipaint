#pragma once
#include "core/result.hpp"
#include "input/types.hpp"
namespace engine::input {
struct OneEuroParams {
    double min_cutoff = 1.0;
    double beta = 0.007;
    double dcutoff = 1.0;
};
class OneEuroFilter {
public:
    static core::Result<OneEuroFilter> create(OneEuroParams params = {});
    void reset() noexcept;
    [[nodiscard]] core::Result<CleanInputPoint> filter(const RawInputPoint& raw);
    [[nodiscard]] core::Result<CleanInputStroke> filter_batch(const RawInputStroke& raw);
private:
    explicit OneEuroFilter(OneEuroParams p) : params_(p) {}
    OneEuroParams params_;
    bool initialized_ = false;
    double prev_time_ = 0.0;
    core::Vec2 prev_raw_{}, prev_filtered_{}, prev_derivative_{};
};
}
