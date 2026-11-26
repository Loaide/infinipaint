#pragma once
#include "document.hpp"
#include <array>

namespace engine::document {
struct ChangedRow {
    Id id,prev,next;
    // Absent rows are deleted. Neighbors without a content change only relink.
    bool present, payloadChanged;
    Value value;
    Revision geometry=0;
};
struct ChangeSet {
    Uuid document;
    Revision base;
    Id head,tail;
    size_t count,charge;
    std::vector<ChangedRow> rows;
};

// Single owner under the document lock. The worker borrows only immutable data
// and returns the same token on acknowledgement. In-flight work remains charged.
class ChangeQueue final : public Observer {
public:
    static constexpr size_t slots=32;
    explicit ChangeQueue(size_t bytes=64u<<20):limit_(bytes) {}
    bool admitStroke() noexcept;
    bool growStroke(size_t canonicalCapacityBytes) noexcept;
    void cancelStroke() noexcept { reservation_=0; strokeEnvelope_.reset(); }
    // Enable ONLY around the admitted stroke's commit, not an unrelated edit.
    void committingStroke(bool value) noexcept { committing_=value; }
    Status prepare(const Document&,std::span<const Touched>) override;
    auto front() const noexcept { return count_?queue_[head_]:nullptr; }
    bool acknowledge(const std::shared_ptr<const ChangeSet>& token) noexcept;
    size_t bytes() const noexcept {return charged_+reservation_;}
    size_t size() const noexcept {return count_;}
private:
    static constexpr size_t envelope=sizeof(ChangeSet)+sizeof(ChangedRow)*3+512;
    std::array<std::shared_ptr<const ChangeSet>,slots> queue_{};
    size_t head_=0,count_=0,charged_=0,reservation_=0,limit_;
    bool committing_=false;
    std::shared_ptr<ChangeSet> strokeEnvelope_;
};
}
