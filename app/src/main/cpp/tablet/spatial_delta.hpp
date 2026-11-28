#pragma once
#include "spatial_pages.hpp"

namespace engine::tablet::spatial {
// Canonical edits insert/remove/replace, not arbitrary reorder of surviving
// IDs. Validate the entire bounded splice against old and final reciprocal
// links before touching either access root. All surviving IDs must have the
// same relative order after skipping removed/inserted runs. Large batches or
// unsupported survivor reorder take the exact rebuild path.
template<class Scene,class BoxOf>
bool updateOrdered(Index& index,const Scene& old,const Scene& patch,Key oldHead,Key newHead,BoxOf boxOf) noexcept {
    if(!index.checkDelta(old.size(),patch.size()))return false;
    struct Classification {
        Index& index;Clock::time_point start=Clock::now();bool done=false;
        ~Classification(){if(!done)index.classified(false,elapsed(start));}
        void accept(){done=true;index.classified(true,elapsed(start));}
    } classification{index};
    using Row=typename Scene::mapped_type;
    auto oldRow=[&](Key id)->const Row*{auto it=old.find(id);return it==old.end()?nullptr:&it->second;};
    auto finalRow=[&](Key id)->const Row*{auto it=patch.find(id);if(it!=patch.end())return it->second.shape?&it->second:nullptr;return oldRow(id);};
    auto skipOld=[&](Key id,bool forward){
        size_t n=0;
        while(id&&!finalRow(id)){
            const auto* r=oldRow(id);if(!r||++n>patch.size())throw Failure::Invalid;
            id=forward?r->next:r->prev;
        }
        if(id&&!oldRow(id))throw Failure::Invalid;
        return id;
    };
    auto skipNew=[&](Key id,bool forward){
        size_t n=0;
        while(id&&!oldRow(id)){
            const auto* r=finalRow(id);if(!r||++n>patch.size())throw Failure::Invalid;
            id=forward?r->next:r->prev;
        }
        if(id&&!finalRow(id))throw Failure::Invalid;
        return id;
    };
    auto linked=[&](Key id){
        if(!id)return true;
        const auto* r=finalRow(id);if(!r)return false;
        const auto* p=finalRow(r->prev);const auto* n=finalRow(r->next);
        return (r->prev?(p&&p->next==id):id==newHead)&&(!r->next||(n&&n->prev==id));
    };
    auto survivor=[&](Key id){
        if(!id)return true;
        const auto* a=oldRow(id);const auto* b=finalRow(id);
        return a&&b&&linked(id)&&skipOld(a->prev,false)==skipNew(b->prev,false)&&skipOld(a->next,true)==skipNew(b->next,true);
    };
    bool mutating=false;
    try {
        if(!linked(newHead)|| (newHead&&finalRow(newHead)->prev))return false;
        const auto first=skipOld(oldHead,true),last=skipOld(index.tail(),false);
        if(skipNew(newHead,true)!=first||bool(first)!=bool(last))return false;
        Key tail=last;size_t tailSteps=0;
        for(Key id=last?finalRow(last)->next:newHead;id;){
            const auto* r=finalRow(id);if(!r||oldRow(id)||!linked(id)||++tailSteps>patch.size())return false;
            tail=id;id=r->next;
        }
        size_t added=0,removed=0,overlayNeeded=0;
        for(const auto& [id,r]:patch){
            if(id<=0)return false;
            const auto* before=oldRow(id);
            if(!r.shape){
                if(!before)continue; // coalesced absent tombstone
                ++removed;if(!index.alreadyOverlay(id))++overlayNeeded;
                if(!survivor(skipOld(before->prev,false))||!survivor(skipOld(before->next,true)))return false;
            }else{
                if(!linked(id)||!boxOf(r).valid())return false;
                if(before){if(!survivor(id))return false;if(boxOf(*before)!=boxOf(r)&&!index.alreadyOverlay(id))++overlayNeeded;}
                else {++added;if(!index.alreadyOverlay(id))++overlayNeeded;}
            }
        }
        // A new run starts only at head or at a surviving predecessor. Each
        // run must fill exactly one gap in the reduced old sequence.
        size_t visited=0;
        for(const auto& [id,r]:patch)if(r.shape&&!oldRow(id)&&(!r.prev||oldRow(r.prev))){
            const auto* anchor=oldRow(r.prev);
            if(r.prev&&!survivor(r.prev))return false;
            Key end=id;
            while(end&&!oldRow(end)){
                const auto* item=finalRow(end);
                if(!item||!linked(end)||++visited>added)return false;
                end=item->next;
            }
            if(end!=skipOld(anchor?anchor->next:oldHead,true)||!survivor(end))return false;
        }
        if(visited!=added||removed>old.size()||bool(newHead)!=bool(old.size()-removed+added)||bool(tail)!=bool(newHead))return false;
        // All validation precedes mutations. Only the removed/new IDs change
        // the counted sequence; surviving neighbours are not relabelled.
        classification.accept();if(!index.reserveOverlay(overlayNeeded))return false;
        index.beginUpdate();mutating=true;
        for(const auto& [id,r]:patch)if(const auto* before=oldRow(id)){
            if(!r.shape)index.remove(id);
            else if(boxOf(*before)!=boxOf(r))index.replace(id,boxOf(r));
        }
        for(const auto& [id,r]:patch)if(r.shape&&!oldRow(id)&&(!r.prev||oldRow(r.prev))){
            Key prev=r.prev;
            for(Key item=id;item&&!oldRow(item);){const auto* row=finalRow(item);index.insertAfter(prev,item,boxOf(*row));prev=item;item=row->next;}
        }
        if(index.count()!=old.size()-removed+added)throw Failure::Invalid;
        index.finishUpdate(tail);return true;
    }catch(const Failure& f){if(mutating)index.updateFailed(f);return false;}
    catch(const std::bad_alloc&){if(mutating)index.updateFailed(Failure::Capacity);return false;}
    catch(...){if(mutating)index.updateFailed(Failure::Invalid);return false;}
}
}
