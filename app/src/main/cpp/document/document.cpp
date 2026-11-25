#include "document.hpp"
#include "codec.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

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
           std::isfinite(v.dx) && std::isfinite(v.dy);
}
}
Ink::Ink(input::RawInputStroke&& s,Brush b):samples(std::move(s)),brush(b),valid_(wire::validInk(*this)) {}
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
        if(!rows_.emplace(id,Record{std::move(v),prev,0,content,geometry}).second) return Status::Invalid;
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
        std::swap(rows_.at(s.id).value,s.alternate);
    } else if (s.detached) {
        const auto [p,n]=std::pair{s.detached.mapped().prev,s.detached.mapped().next};
        rows_.insert(std::move(s.detached));
        (p ? rows_.at(p).next : first_) = s.id;
        (n ? rows_.at(n).prev : last_) = s.id;
    } else {
        s.detached=rows_.extract(s.id);
        const auto& r=s.detached.mapped();
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
    for (auto& s:g.steps) {
        auto it=rows_.find(s.id);
        if(it!=rows_.end()) it->second.geometry=it->second.content=revision_;
    }
}
Status Document::edit(Revision expected,std::span<const Edit> edits) {
    if(expected!=revision_) return Status::Conflict;
    if(edits.empty()) return Status::Ok;
    if(revision_==maximum) return Status::Exhausted;
    if(!budget_.groups || budget_.bytes<sizeof(Group) ||
       edits.size()>(budget_.bytes-sizeof(Group))/(sizeof(Step)+sizeof(Touched))) return Status::HistoryLimit;
    Group g;
    Id candidate=issued_;
    auto abort=[&](Status why){ replay(g,false); return why; };
    try {
        g.steps.reserve(edits.size());
        g.touches.reserve(edits.size());
        g.charge=sizeof(Group)+g.steps.capacity()*sizeof(Step)+g.touches.capacity()*sizeof(Touched);
        for(const auto& e:edits) {
            const auto* r=find(e.target);
            Step s{e.target,e.kind==Edit::Replace,{},{}};
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
            } else {
                if(!r) return abort(Status::Invalid);
                if(s.replace) {
                    if(!valid(e.value)) return abort(Status::Invalid);
                    if(r->value.ink==e.value.ink && r->value.dx==e.value.dx && r->value.dy==e.value.dy) continue;
                    if(!charge(retained(e.value))) return abort(Status::HistoryLimit);
                    s.alternate=e.value;
                } else if(e.kind!=Edit::Remove) return abort(Status::Invalid);
                if(!charge(retained(r->value))) return abort(Status::HistoryLimit);
            }
            const auto& slot=s.detached?s.detached.mapped():*r;
            g.touches.push_back({s.id,slot.prev,slot.next});
            g.steps.push_back(std::move(s));
            toggle(g.steps.back());
        }
        if(g.steps.empty()) return Status::Ok;
        // Allocate destination before abandoning redo. Everything below is nonthrowing.
        groups_.emplace_back();
        if(sink_) {
            Status result;
            try { result=sink_->prepare(*this,g.touches); }
            catch(...) { groups_.pop_back(); throw; }
            if(result!=Status::Ok) { groups_.pop_back(); return abort(result); }
        }
    } catch(const std::bad_alloc&) { return abort(Status::Memory); }
      catch(const std::length_error&) { return abort(Status::Memory); }
      catch(...) { replay(g,false); throw; }
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
    historyBytes_+=committed.charge;
    cursor_=groups_.size(); issued_=candidate; publish(committed);
    return Status::Ok;
}
Status Document::clear(Revision expected) {
    if(expected!=revision_) return Status::Conflict;
    if(rows_.size()>budget_.bytes/(sizeof(Step)+overhead)) return Status::HistoryLimit;
    try {
        std::vector<Edit> edits;
        edits.reserve(size());
        for(Id id=first_;id;id=rows_.at(id).next) edits.push_back({Edit::Remove,id});
        return edit(expected,edits);
    } catch(const std::bad_alloc&) { return Status::Memory; }
}
Status Document::history(Revision expected,bool forwards) {
    if(expected!=revision_) return Status::Conflict;
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
