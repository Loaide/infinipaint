#include "change_queue.hpp"
#include <algorithm>

namespace engine::document {
bool ChangeQueue::admitStroke() noexcept {
    if(reservation_ || count_==slots || envelope>limit_-charged_) return false;
    try {
        auto g=std::make_shared<ChangeSet>();g->rows.reserve(2);strokeEnvelope_=std::move(g);
    } catch(const std::bad_alloc&) {return false;}
    reservation_=envelope;
    return true;
}
bool ChangeQueue::growStroke(size_t n) noexcept {
    if(!reservation_ || n>limit_-charged_-envelope) return false;
    reservation_=std::max(reservation_,envelope+n);
    return true;
}
Status ChangeQueue::prepare(const Document& doc,std::span<const Touched> touched) {
    const bool usingReservation=committing_ && reservation_;
    if(count_+(reservation_ && !usingReservation)>=slots) return Status::QueueLimit;
    size_t available=limit_-charged_-(usingReservation?0:reservation_);
    if(!doc.size() && !touched.empty()) {
        const auto bytes=sizeof(ChangeSet)+128;
        if(bytes>available) return Status::QueueLimit;
        auto g=std::make_shared<ChangeSet>();
        g->document=doc.identity();g->base=doc.revision();g->head=g->tail=0;g->count=0;g->charge=bytes;
        queue_[(head_+count_)%slots]=std::move(g);charged_+=bytes;++count_;
        return Status::Ok;
    }
    if(usingReservation && touched.size()==1 && touched[0].next==0 && doc.last()==touched[0].id) {
        const auto& t=touched[0];const Record* row=doc.find(t.id);
        if(!row || row->prev!=t.prev) return Status::Invalid;
        auto n=row->value.ink->samples.capacity()*sizeof(input::RawInputPoint);
        if(!strokeEnvelope_ || n>reservation_-envelope) return Status::QueueLimit;
        auto g=std::move(strokeEnvelope_);
        g->document=doc.identity();g->base=doc.revision();g->head=doc.first();g->tail=t.id;g->count=doc.size();
        if(t.prev) g->rows.push_back({t.prev,doc.find(t.prev)->prev,t.id,true,false,{}});
        g->rows.push_back({t.id,t.prev,0,true,true,row->value,doc.revision()+1});
        g->charge=envelope+n;charged_+=g->charge;
        queue_[(head_+count_)%slots]=std::move(g);++count_;reservation_=0;
        return Status::Ok;
    }
    // Transient unique-ID planning also needs a bound independent of input group size.
    if(touched.size()>available/(3*(sizeof(ChangedRow)+sizeof(std::pair<Id,bool>)))) return Status::QueueLimit;
    // 0 relink, 1 content only, 2 geometry. Take the strongest intent per ID.
    std::vector<std::pair<Id,int>> ids;
    ids.reserve(3*touched.size());
    for(auto [id,p,n,geometry]:touched) {
        ids.emplace_back(id,geometry?2:1);
        if(p) ids.emplace_back(p,false);
        if(n) ids.emplace_back(n,false);
    }
    std::sort(ids.begin(),ids.end());
    auto g=std::make_shared<ChangeSet>();
    g->document=doc.identity(); g->base=doc.revision(); g->head=doc.first(); g->tail=doc.last(); g->count=doc.size();
    g->rows.reserve(ids.size());
    g->charge=sizeof(ChangeSet)+128+g->rows.capacity()*sizeof(ChangedRow);
    if(g->charge>available) return Status::QueueLimit;
    for(size_t i=0;i<ids.size();) {
        auto [id,changed]=ids[i++];
        while(i<ids.size() && ids[i].first==id) changed=std::max(changed,ids[i++].second);
        const Record* r=doc.find(id);
        if(r && changed) {
            size_t overhead=sizeof(Ink)+64;
            const size_t room=available-g->charge;
            if(room<overhead || r->value.ink->samples.capacity()>(room-overhead)/sizeof(input::RawInputPoint))
                return Status::QueueLimit;
            g->charge+=overhead+r->value.ink->samples.capacity()*sizeof(input::RawInputPoint);
        }
        g->rows.push_back({id,r?r->prev:0,r?r->next:0,r!=nullptr,changed!=0,r&&changed?r->value:Value{},
                           changed==2?doc.revision()+1:r?r->geometry:0});
    }
    charged_+=g->charge;
    queue_[(head_+count_)%slots]=std::move(g); ++count_;
    if(usingReservation) cancelStroke();
    return Status::Ok;
}
bool ChangeQueue::acknowledge(const std::shared_ptr<const ChangeSet>& token) noexcept {
    if(!token || token!=front()) return false;
    charged_-=token->charge; queue_[head_].reset(); head_=(head_+1)%slots; --count_;
    return true;
}
}
