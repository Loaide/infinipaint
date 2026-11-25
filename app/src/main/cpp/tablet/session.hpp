#pragma once
#include "document/change_queue.hpp"
namespace engine::tablet {
// Called on the sole catalog worker, while UI edit admission is suspended.
bool install(std::unique_ptr<document::Document> candidate,size_t meshBudget);
std::shared_ptr<const document::ChangeSet> nextSave();
bool acknowledge(const std::shared_ptr<const document::ChangeSet>&);
}
