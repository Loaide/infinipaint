#pragma once
#include "spatial_pages.hpp"

namespace engine::tablet::spatial {
// Accept only topology-preserving changes, deletions, and an append suffix.
// Arbitrary insert-after/undo splices are valid canonical operations but need
// a new rank root in this transitional implementation. The caller merges the
// authoritative delta and rebuilds from its complete reciprocal link chain.
// Row maps expose prev, next and shape (nonnull iff present); boxOf(row)
// returns the existing whole-stroke conservative bounds.
template<class Scene,class BoxOf>
bool updateOrdered(Index& index,const Scene& old,const Scene& patch,Key oldHead,Key newHead,BoxOf boxOf) noexcept {
    // Large/coalesced edits are rebuilt once. This also bounds the cost of
    // checking consecutive tombstones before touching the overlay.
    if(!index.ready()||index.count()!=old.size()||patch.size()>3*overlayLimit+2)return false;
    using Row=typename Scene::mapped_type;
    auto oldRow=[&](Key id)->const Row*{auto it=old.find(id);return it==old.end()?nullptr:&it->second;};
    auto finalRow=[&](Key id)->const Row*{auto it=patch.find(id);if(it!=patch.end())return it->second.shape?&it->second:nullptr;return oldRow(id);};
    auto skip=[&](Key id,bool forward){
        size_t n=0;
        while(id&&!finalRow(id)) {
            const auto* r=oldRow(id);
            if(!r||++n>patch.size())throw Failure::Invalid;
            id=forward?r->next:r->prev;
        }
        return id;
    };
    auto linked=[&](Key id){
        if(!id)return true;
        const auto* r=finalRow(id);if(!r)return false;
        const auto* p=finalRow(r->prev);const auto* n=finalRow(r->next);
        return (r->prev?(p&&p->next==id):id==newHead)&&(!r->next||(n&&n->prev==id));
    };
    try {
        const Key first=skip(oldHead,true),last=skip(index.tail(),false);
        if(first&&first!=newHead)return false;
        if(bool(first)!=bool(last)||!linked(newHead))return false;
        if(newHead&&finalRow(newHead)->prev)return false;
        const Key appendHead=last?finalRow(last)->next:newHead;
        size_t added=0,removed=0;
        for(const auto& [id,r]:patch) {
            if(id<=0)return false;
            const auto* before=oldRow(id);
            if(!r.shape) {
                // Coalescing can include a tombstone for an ID that was
                // never latched. It does not change the old sequence.
                if(!before)continue;
                ++removed;
                Key p=skip(before->prev,false),n=skip(before->next,true);
                if(!linked(p)||!linked(n))return false;
                if(p&&finalRow(p)->next!=(n?n:appendHead))return false;
                if(n&&finalRow(n)->prev!=p)return false;
                continue;
            }
            if(!linked(id)||!boxOf(r).valid())return false;
            if(!before){++added;continue;}
            if(r.prev!=skip(before->prev,false))return false;
            Key next=skip(before->next,true);
            if(r.next!=(next?next:appendHead))return false;
        }
        size_t visited=0;Key prior=last;
        for(Key id=appendHead;id;) {
            const auto* r=finalRow(id);
            if(oldRow(id)||!r||++visited>added||r->prev!=prior||!linked(id))return false;
            prior=id;id=r->next;
        }
        if(visited!=added||removed>old.size()||bool(newHead)!=bool(old.size()-removed+added))return false;
        // No mutation until the entire splice classification passes. A
        // bounded overlay overflow invalidates both roots before rebuild.
        for(const auto& [id,r]:patch) if(const auto* before=oldRow(id)) {
            if(!r.shape)index.remove(id);
            else if(boxOf(*before)!=boxOf(r))index.replace(id,boxOf(r));
        }
        for(Key id=appendHead;id;) {const auto* r=finalRow(id);index.append(id,boxOf(*r));id=r->next;}
        if(index.count()!=old.size()-removed+added)throw Failure::Invalid;
        index.finishUpdate(prior);return true;
    }catch(const Failure& f){index.updateFailed(f);return false;}
    catch(...){index.updateFailed(Failure::Invalid);return false;}
}
}
