#pragma once
#include "input/one_euro_filter.hpp"
#include <cstddef>
namespace engine::input {
class InputProcessor {
public:
    static core::Result<InputProcessor> create(OneEuroParams params = {}, std::size_t max_samples = 1000000);
    [[nodiscard]] core::Result<void> begin_stroke();
    [[nodiscard]] core::Result<CleanInputPoint> process(const RawInputPoint& raw);
    // Called with reserved input slots and a candidate filter result. Failure
    // leaves histories and filter unchanged. Success commits without allocation.
    using BeforeCommit = core::Result<void>(*)(void*,const CleanInputPoint&);
    [[nodiscard]] core::Result<CleanInputPoint> process(const RawInputPoint&, void*, BeforeCommit);
    [[nodiscard]] core::Result<CleanInputStroke> process_batch(const RawInputStroke& raw);
    [[nodiscard]] core::Result<CleanInputStroke> end_stroke();
    // For a retiring processor only. Moves the authoritative history without
    // iterating it. Remaining scratch storage dies with the live trace.
    [[nodiscard]] RawInputStroke release_raw() && noexcept { return std::move(raw_); }
    void cancel_stroke() noexcept;
    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] const RawInputStroke& raw_samples() const noexcept { return raw_; }
    [[nodiscard]] const CleanInputStroke& clean_samples() const noexcept { return clean_; }
    [[nodiscard]] std::size_t max_samples() const noexcept { return max_samples_; }
private:
    explicit InputProcessor(OneEuroFilter f, std::size_t max_samples) : filter_(std::move(f)), max_samples_(max_samples) {}
    OneEuroFilter filter_;
    std::size_t max_samples_;
    bool active_ = false;
    RawInputStroke raw_;
    CleanInputStroke clean_;
};
}
