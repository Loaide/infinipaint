#pragma once
#include "document/change_queue.hpp"
namespace engine::tablet {
void setPhysicalSlice(size_t bytes);
bool admitIncoming(size_t candidateCount,size_t sampleBytes);
// Called on the sole catalog worker, while UI edit admission is suspended.
bool stage(std::unique_ptr<document::Document> candidate,size_t meshBudget);
void activate(double x,double y,double zoom) noexcept;
void discard() noexcept;
std::shared_ptr<const document::ChangeSet> nextSave();
bool acknowledge(const std::shared_ptr<const document::ChangeSet>&);
}
