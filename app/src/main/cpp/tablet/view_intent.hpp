#pragma once
#include <cstdint>
#include <mutex>

namespace engine::tablet {
struct ViewIntent {
    double x=0, y=0, zoom=1;
    int width=1, height=1;
    uint64_t sequence=0, mappingGeneration=0;
};

// One replaceable value. No reader keeps the mutex while using a snapshot;
// no calls out, allocations, or document/driver locks in this port.
class ViewMailbox {
    mutable std::mutex mutex_;
    ViewIntent desired_;
public:
    void publish(double x,double y,double zoom) {
        std::lock_guard lock(mutex_);
        desired_.x=x; desired_.y=y; desired_.zoom=zoom;
        ++desired_.sequence;
    }
    void viewport(int width,int height) {
        std::lock_guard lock(mutex_);
        desired_.width=width; desired_.height=height;
        ++desired_.mappingGeneration; ++desired_.sequence;
    }
    ViewIntent latch() const {
        std::lock_guard lock(mutex_);
        return desired_;
    }
};
}
