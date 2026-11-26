#include "document.hpp"
#include "codec.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#ifdef IP_DOCUMENT_PHASE_PROBE
extern void ipDocumentPhase(int) noexcept;
#define PHASE(index) ipDocumentPhase(index)
#else
#define PHASE(index) ((void)0)
#endif

namespace engine::document {
namespace {
constexpr auto maximum = std::numeric_limits<Id>::max();
// Conservative estimate of a map node, its allocator metadata and immutable owner.
constexpr size_t overhead = sizeof(Record) + sizeof(Ink) + 16 * sizeof(void*);
size_t retained(const Value& v) noexcept {
    if (!v.ink) return 0;
    auto cap = v.ink->samples.capacity();
    if (cap > (SIZE_MAX-overhead)/sizeof(input::RawInputPoint)) return SIZE_MAX;
    return overhead + cap*sizeof(input::RawInputPoint);
}
bool valid(const Value& v) noexcept {
    return v.ink && v.ink->valid() &&
           std::isfinite(v.dx) && std::isfinite(v.dy) &&
           std::all_of(v.rgba().begin(),v.rgba().end(),[](float f){return f>=0 && f<=1;});
}
}
Ink::Ink(input::RawInputStroke&& s,Brush b):samples(std::move(s)),brush(b),valid_(wire::validInk(*this)) {
    liveBytes.fetch_add(charge(),std::memory_order_relaxed);
}
Document::Document(Uuid identity, Budget budget):identity_(identity),budget_(budget) {
    if (std::all_of(identity.begin(),identity.end(),[](auto b){return !b;}))
        throw std::invalid_argument("nil document identity");
}
bool Document::grant(Id first, Id last) noexcept {
    if (first<=ceiling_ || last<first || issued_!=ceiling_) return false;
    issued_=first-1; ceiling_=last; return true;
}
Status Document::restore(Id id,Id prev,Value v,Revision content,Revision geometry,Revision durable,Id reservedThrough) {
    if(sink_ || ceiling_ || revision_ || !groups_.empty() || id<=0 || id>reservedThrough ||
       prev!=last_ || geometry<=0 || geometry>content || content>durable || !valid(v))
        return Status::Invalid;
    try {
        const auto bytes=retained(v);
        if(!rows_.emplace(id,Record{std::move(v),prev,0,content,geometry}).second) return Status::Invalid;
        canonicalBytes_+=bytes;
        (last_?rows_.at(last_).next:first_)=id; last_=id;
        return Status::Ok;
    } catch(const std::bad_alloc&) {return Status::Memory;}
}
Status Document::finishRestore(Revision durable,Id reservedThrough) noexcept {
    if(sink_ || ceiling_ || revision_ || !groups_.empty() || durable<0 || reservedThrough<0 ||
       (!rows_.empty() && rows_.rbegin()->first>reservedThrough)) return Status::Invalid;
    revision_=durable; issued_=ceiling_=reservedThrough; return Status::Ok;
}
const Record* Document::find(Id id) const noexcept {
    auto it=rows_.find(id); return it==rows_.end()?nullptr:&it->second;
}
void Document::toggle(Step& s) noexcept {
    if (s.replace) {
        canonicalBytes_-=retained(rows_.at(s.id).value);
        canonicalBytes_+=retained(s.alternate);
        std::swap(rows_.at(s.id).value,s.alternate);
    } else if (s.detached) {
        canonicalBytes_+=retained(s.detached.mapped().value);
        const auto [p,n]=std::pair{s.detached.mapped().prev,s.detached.mapped().next};
        rows_.insert(std::move(s.detached));
        (p ? rows_.at(p).next : first_) = s.id;
        (n ? rows_.at(n).prev : last_) = s.id;
    } else {
        s.detached=rows_.extract(s.id);
        const auto& r=s.detached.mapped();
        canonicalBytes_-=retained(r.value);
        (r.prev ? rows_.at(r.prev).next : first_) = r.next;
        (r.next ? rows_.at(r.next).prev : last_) = r.prev;
    }
}
void Document::replay(Group& g,bool forwards) noexcept {
    if (forwards) for(auto& s:g.steps) toggle(s);
    else for(auto it=g.steps.rbegin();it!=g.steps.rend();++it) toggle(*it);
}
void Document::publish(Group& g) noexcept {
    ++revision_;
    for (auto& s:g.touches) {
        auto it=rows_.find(s.id);
        if(it!=rows_.end()) {
            it->second.content=revision_;
            if(s.geometry) it->second.geometry=revision_;
        }
    }
}
Status Document::edit(Revision expected,std::span<const Edit> edits) {
    if(expected!=revision_ || ticket_) return Status::Conflict;
    if(edits.empty()) return Status::Ok;
    if(revision_==maximum) return Status::Exhausted;
    if(!budget_.groups || budget_.bytes<sizeof(Group) ||
       edits.size()>(budget_.bytes-sizeof(Group))/(sizeof(Step)+sizeof(Touched))) return Status::HistoryLimit;
    Group g;
    Id candidate=issued_;
    auto abort=[&](Status why){ replay(g,false); return why; };
    try {
        PHASE(0);
        g.steps.reserve(edits.size());
        g.touches.reserve(edits.size());
        g.charge=sizeof(Group)+g.steps.capacity()*sizeof(Step)+g.touches.capacity()*sizeof(Touched);
        PHASE(1);
        for(const auto& e:edits) {
            const auto* r=find(e.target);
            Step s{e.target,e.kind==Edit::Replace,{},{}};bool geometry=true;
            auto charge=[&](size_t n){
                if(g.charge>budget_.bytes || n>budget_.bytes-g.charge) return false;
                g.charge+=n; return true;
            };
            if(e.kind==Edit::InsertAfter) {
                if((e.target && !r) || !valid(e.value)) return abort(Status::Invalid);
                if(candidate==ceiling_) return abort(Status::Exhausted);
                if(!charge(retained(e.value))) return abort(Status::HistoryLimit);
                Rows staged;
                s.id=++candidate;
                staged.emplace(s.id,Record{e.value,e.target,r?r->next:first_});
                s.detached=staged.extract(s.id);
                PHASE(2);
            } else {
                if(!r) return abort(Status::Invalid);
                if(s.replace) {
                    if(!valid(e.value)) return abort(Status::Invalid);
                    geometry=r->value.ink!=e.value.ink || r->value.dx!=e.value.dx || r->value.dy!=e.value.dy;
                    if(!geometry && r->value.rgba()==e.value.rgba()) continue;
                    if(!charge(retained(e.value))) return abort(Status::HistoryLimit);
                    s.alternate=e.value;
                } else if(e.kind!=Edit::Remove) return abort(Status::Invalid);
                if(!charge(retained(r->value))) return abort(Status::HistoryLimit);
            }
            const auto& slot=s.detached?s.detached.mapped():*r;
            g.touches.push_back({s.id,slot.prev,slot.next,geometry});
            g.steps.push_back(std::move(s));
            toggle(g.steps.back());
            PHASE(3);
        }
        if(g.steps.empty()) return Status::Ok;
        // Allocate destination before abandoning redo. Everything below is nonthrowing.
        groups_.emplace_back();
        PHASE(4);
        if(sink_) {
            Status result;
            try { result=sink_->prepare(*this,g.touches); }
            catch(...) { groups_.pop_back(); throw; }
            if(result!=Status::Ok) { groups_.pop_back(); return abort(result); }
        }
    } catch(const std::bad_alloc&) { return abort(Status::Memory); }
      catch(const std::length_error&) { return abort(Status::Memory); }
      catch(...) { replay(g,false); throw; }
    finishGroup(g);issued_=candidate;
    PHASE(6);
    return Status::Ok;
}
void Document::finishGroup(Group& g) noexcept {
    while(groups_.size()>cursor_+1) {
        historyBytes_-=groups_[groups_.size()-2].charge;
        groups_.pop_back();
    }
    groups_.back()=std::move(g);
    auto& committed=groups_.back();
    while(groups_.size()>budget_.groups || historyBytes_>budget_.bytes-committed.charge) {
        historyBytes_-=groups_.front().charge;
        groups_.pop_front();
    }
    PHASE(5);
    historyBytes_+=committed.charge;
    cursor_=groups_.size(); publish(committed);
}
Status Document::reserveStroke() {
    if(ticket_) return Status::Conflict;
    if(!canAllocate() || revision_==maximum) return Status::Exhausted;
    constexpr size_t base=sizeof(Group)+sizeof(Step)+sizeof(Touched);
    if(!budget_.groups || budget_.bytes<base+overhead+sizeof(input::RawInputPoint)) return Status::HistoryLimit;
    try {
        Group g;
        Rows staged;
        Id id=issued_+1;
        staged.emplace(id,Record{{},last_,0});
        g.steps.push_back({id,false,staged.extract(id),{}});
        g.touches.push_back({id,last_,0});
        g.charge=base;
        groups_.emplace_back(); // placeholder removed on cancel; no history is evicted yet
        ticket_.emplace(std::move(g));issued_=id;
        return Status::Ok;
    } catch(const std::bad_alloc&) {return Status::Memory;}
}
bool Document::acceptsStrokeCapacity(size_t n) const noexcept {
    return ticket_ && n<=1000000 && n<=(budget_.bytes-ticket_->charge-overhead)/sizeof(input::RawInputPoint);
}
Status Document::commitStroke(Value value) {
    if(!ticket_) return Status::Conflict;
    if(!valid(value)) return Status::Invalid;
    if(!acceptsStrokeCapacity(value.ink->samples.capacity())) return Status::HistoryLimit;
    auto& g=*ticket_;auto& step=g.steps.front();
    const auto cost=retained(value);
    step.detached.mapped().value=std::move(value);
    toggle(step);
    Status result=Status::Ok;
    try {if(sink_) result=sink_->prepare(*this,g.touches);}
    catch(const std::bad_alloc&) {result=Status::Memory;}
    catch(const std::length_error&) {result=Status::Memory;}
    catch(...) {toggle(step);throw;}
    if(result!=Status::Ok) {toggle(step);return result;}
    g.charge+=cost;finishGroup(g);ticket_.reset();
    return Status::Ok;
}
void Document::cancelStroke() noexcept {
    if(ticket_) {ticket_.reset();groups_.pop_back();}
}
Status Document::clear(Revision expected) {
    if(expected!=revision_ || ticket_) return Status::Conflict;
    if(rows_.size()>budget_.bytes/(sizeof(Step)+overhead)) return Status::HistoryLimit;
    try {
        std::vector<Edit> edits;
        edits.reserve(size());
        for(Id id=first_;id;id=rows_.at(id).next) edits.push_back({Edit::Remove,id});
        return edit(expected,edits);
    } catch(const std::bad_alloc&) { return Status::Memory; }
}
Status Document::history(Revision expected,bool forwards) {
    if(expected!=revision_ || ticket_) return Status::Conflict;
    if(forwards ? cursor_==groups_.size() : !cursor_) return Status::Empty;
    if(revision_==maximum) return Status::Exhausted;
    Group& g=groups_[forwards?cursor_:cursor_-1];
    replay(g,forwards);
    if(sink_) {
        Status s;
        try { s=sink_->prepare(*this,g.touches); }
        catch(const std::bad_alloc&) { s=Status::Memory; }
        catch(const std::length_error&) { s=Status::Memory; }
        catch(...) { replay(g,!forwards); throw; }
        if(s!=Status::Ok) {replay(g,!forwards);return s;}
    }
    publish(g);
    if(forwards) ++cursor_; else --cursor_;
    return Status::Ok;
}
Status Document::undo(Revision r){return history(r,false);}
Status Document::redo(Revision r){return history(r,true);}
}
