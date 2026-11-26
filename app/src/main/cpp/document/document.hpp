#pragma once

#include "input/one_euro_filter.hpp"
#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace engine::tablet { struct AdoptAccepted; }
namespace engine::document {
using Id = std::int64_t; // Positive, scoped by UUID. Zero is the sequence sentinel.
using Uuid = std::array<std::uint8_t, 16>;
using Revision = std::int64_t;

struct Brush {
    std::uint32_t recipe = 1, brush = 1, tool = 0;
    input::OneEuroParams filter;
    float radiusWorld = 1.9f;
    std::array<float, 4> rgba{.28f,.58f,1.f,1.f};
};
// Construct off-document; ownership is transferred, not borrowed from mutable input.
struct Ink {
    const input::RawInputStroke samples;
    const Brush brush;
    Ink(input::RawInputStroke&& samples, Brush captured);
    bool valid() const noexcept { return valid_; }
    Ink(const Ink&)=delete;
    Ink& operator=(const Ink&)=delete;
    ~Ink() { liveBytes.fetch_sub(charge(),std::memory_order_relaxed); }
    // Unique payloads across model, live seal, staging, save and history.
    inline static std::atomic<uint64_t> liveBytes{0};
private:
    // Only the streaming v1 capture path can attest to per-sample validation
    // without a second O(n) pass while the pen-up mutex is held.
    friend struct engine::tablet::AdoptAccepted;
    Ink(input::RawInputStroke&& accepted, Brush captured, bool validated):
        samples(std::move(accepted)), brush(captured), valid_(validated) {
        liveBytes.fetch_add(charge(),std::memory_order_relaxed);
    }
    uint64_t charge() const noexcept {return sizeof(Ink)+samples.capacity()*sizeof(input::RawInputPoint);}
    const bool valid_;
};
struct Value {
    std::shared_ptr<const Ink> ink;
    double dx = 0, dy = 0; // Post-reconstruction world translation
    std::optional<std::array<float,4>> tint{};
    const std::array<float,4>& rgba() const noexcept {return tint?*tint:ink->brush.rgba;}
};
struct Record {
    Value value;
    Id prev = 0, next = 0;
    Revision content = 0, geometry = 0;
};
struct Edit {
    enum Kind { InsertAfter, Remove, Replace } kind;
    Id target; // InsertAfter: predecessor, 0 means head. Others: subject.
    Value value{};
};
enum class Status { Ok, Conflict, Invalid, Exhausted, HistoryLimit, Memory, Empty, QueueLimit };
// M04 derives this service from one initial traversal and revisioned splice
// batches; no rank column or spatial key becomes authoritative. It must validate
// all IDs and the exact revision before modifying the caller's candidate span.
class SparsePaintOrder {
public:
    virtual Status sort(const Uuid&,Revision,std::span<Id> candidates) const=0;
    virtual ~SparsePaintOrder()=default;
};
struct Budget { size_t groups = 128, bytes = 64u << 20; };
class Document;
// During prepare the sequence has its proposed final links/values; revision is
// still the base revision. IDs can repeat. A sink stages immutable changed data
// but cannot expose it until the caller receives Ok. Failure rolls back the model.
struct Touched { Id id, prev, next; bool geometry=true; };
struct Observer {
    virtual Status prepare(const Document&, std::span<const Touched>) = 0;
    virtual ~Observer() = default;
};

// Single serialized owner. No I/O, threads, GPU objects or implicit global document.
// Call grant only with a range already exclusively reserved by durable storage.
class Document {
public:
    explicit Document(Uuid identity, Budget budget = {});
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;
    bool grant(Id first, Id last) noexcept;
    // Build only a not-yet-published candidate in persisted order. Not an edit;
    // requires fresh history and no ID grant. Caller checks expected count/endpoints.
    Status restore(Id id, Id prev, Value, Revision content, Revision geometry,
                   Revision durable, Id reservedThrough);
    Status finishRestore(Revision durable, Id reservedThrough) noexcept;
    bool canAllocate() const noexcept {return issued_<ceiling_;}
    Id nextId() const noexcept {return canAllocate()?issued_+1:0;}
    Status reserveStroke();
    bool acceptsStrokeCapacity(size_t samples) const noexcept;
    Status commitStroke(Value);
    void cancelStroke() noexcept;
    void observe(Observer* sink) noexcept { sink_=sink; }
    Status edit(Revision expected, std::span<const Edit> group);
    Status clear(Revision expected);
    Status undo(Revision expected);
    Status redo(Revision expected);
    const Record* find(Id id) const noexcept;
    const Uuid& identity() const noexcept { return identity_; }
    Revision revision() const noexcept { return revision_; }
    Id first() const noexcept { return first_; }
    Id last() const noexcept { return last_; }
    size_t size() const noexcept { return rows_.size(); }
    size_t historyBytes() const noexcept { return historyBytes_; }
    uint64_t canonicalBytes() const noexcept { return canonicalBytes_; }
private:
    using Rows = std::map<Id, Record>;
    // Insert/remove toggles the same allocated node. Replace swaps just its value.
    struct Step { Id id; bool replace; Rows::node_type detached; Value alternate; };
    struct Group { std::vector<Step> steps; std::vector<Touched> touches; size_t charge = 0; };
    void toggle(Step&) noexcept;
    void replay(Group&, bool forwards) noexcept;
    void publish(Group&) noexcept;
    void finishGroup(Group&) noexcept;
    Status history(Revision, bool forwards);
    Uuid identity_;
    Budget budget_;
    Rows rows_;
    Id first_ = 0, last_ = 0, issued_ = 0, ceiling_ = 0;
    Revision revision_ = 0;
    std::deque<Group> groups_;
    size_t cursor_ = 0, historyBytes_ = 0;
    uint64_t canonicalBytes_=0;
    Observer* sink_ = nullptr;
    std::optional<Group> ticket_;
};
}
