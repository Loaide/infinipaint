#include "input/input_processor.hpp"
#include <limits>
#include <algorithm>
#include <new>
#include <stdexcept>
#include <utility>
namespace engine::input {
namespace {
std::size_t growth(std::size_t capacity, std::size_t required, std::size_t limit) noexcept {
    if (capacity >= required) return capacity;
    const std::size_t doubled = capacity > limit / 2 ? limit : capacity * 2;
    return std::min(limit, std::max(required, std::max(std::size_t{1}, doubled)));
}
core::Error memory_error() { return core::Error{core::ErrorCode::AllocationFailure,"Input buffer allocation failed"}; }
}
core::Result<InputProcessor> InputProcessor::create(OneEuroParams p, std::size_t max_samples) {
    if (max_samples == 0) return core::Error{core::ErrorCode::InvalidArgument,"max_samples must be positive"};
    auto f=OneEuroFilter::create(p);
    if (!f) return f.error();
    return InputProcessor(std::move(f).value(), max_samples);
}
core::Result<void> InputProcessor::begin_stroke() {
    if (active_) return core::Error{core::ErrorCode::InvalidState,"Stroke already active"};
    filter_.reset(); raw_.clear(); clean_.clear(); active_=true;
    return {};
}
core::Result<CleanInputPoint> InputProcessor::process(const RawInputPoint& p) {
    if (!active_) return core::Error{core::ErrorCode::InvalidState,"No active stroke"};
    if (raw_.size() >= max_samples_) return core::Error{core::ErrorCode::ResourceLimit,"Active stroke sample limit exceeded"};
    OneEuroFilter candidate=filter_;
    auto r=candidate.filter(p);
    if (!r) return r.error();
    try {
        if (raw_.size()==raw_.max_size() || clean_.size()==clean_.max_size()) return memory_error();
        if (raw_.size()==raw_.capacity()) raw_.reserve(growth(raw_.capacity(), raw_.size()+1, std::min(max_samples_, raw_.max_size())));
        if (clean_.size()==clean_.capacity()) clean_.reserve(growth(clean_.capacity(), clean_.size()+1, std::min(max_samples_, clean_.max_size())));
    } catch (const std::bad_alloc&) { return memory_error(); }
      catch (const std::length_error&) { return memory_error(); }
    // After successful reserves, appending trivially copyable samples cannot throw.
    raw_.push_back(p);
    clean_.push_back(r.value());
    filter_=std::move(candidate);
    return r;
}
core::Result<CleanInputStroke> InputProcessor::process_batch(const RawInputStroke& batch) {
    if (!active_) return core::Error{core::ErrorCode::InvalidState,"No active stroke"};
    if (batch.empty()) return CleanInputStroke{};
    if (batch.size() > max_samples_ - raw_.size()) return core::Error{core::ErrorCode::ResourceLimit,"Active stroke sample limit exceeded"};
    OneEuroFilter candidate=filter_;
    core::Result<CleanInputStroke> r = CleanInputStroke{};
    try {
        r=candidate.filter_batch(batch);
        if (!r) return r.error();
        if (batch.size()>raw_.max_size()-raw_.size() || batch.size()>clean_.max_size()-clean_.size()) return memory_error();
        if (raw_.capacity() < raw_.size()+batch.size()) raw_.reserve(growth(raw_.capacity(), raw_.size()+batch.size(), std::min(max_samples_, raw_.max_size())));
        if (clean_.capacity() < clean_.size()+batch.size()) clean_.reserve(growth(clean_.capacity(), clean_.size()+batch.size(), std::min(max_samples_, clean_.max_size())));
    } catch (const std::bad_alloc&) { return memory_error(); }
      catch (const std::length_error&) { return memory_error(); }
    raw_.insert(raw_.end(),batch.begin(),batch.end());
    clean_.insert(clean_.end(),r.value().begin(),r.value().end());
    filter_=std::move(candidate);
    return r;
}
core::Result<CleanInputStroke> InputProcessor::end_stroke() {
    if (!active_) return core::Error{core::ErrorCode::InvalidState,"No active stroke"};
    if (clean_.empty()) return core::Error{core::ErrorCode::EmptyInput,"Cannot commit an empty stroke"};
    CleanInputStroke completed=std::move(clean_);
    clean_.clear(); raw_.clear(); filter_.reset(); active_=false;
    return completed;
}
void InputProcessor::cancel_stroke() noexcept {
    active_=false; filter_.reset(); raw_.clear(); clean_.clear();
}
}
