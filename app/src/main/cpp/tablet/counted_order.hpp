#pragma once
// Included by spatial_pages.hpp after the shared scratch page store. Neither
// tree is authoritative. Single planner owner; on any mutation failure the
// caller invalidates BOTH spatial and order roots before another query.
namespace engine::tablet::spatial {
class CountedOrder {
    using Records=std::vector<Record,Alloc<Record>>;
    static constexpr unsigned depthLimit=4; // under the 4096-page cap, valid trees have at most three levels
    PageFile& file_;
    Budget& budget_;
    OrderStats& stats_;
    uint64_t sequence_=0,locator_=0;
    size_t count_=0;
    struct Position {uint64_t page;size_t slot;};
    struct Entry {Key key=0;uint64_t value=0;};
    struct Node {uint32_t kind=0,count=0;uint64_t parent=0;std::array<Entry,orderFanout> entries{};};
    static_assert(sizeof(Node)==16+orderFanout*16);
    template<class P> static bool leaf(const P& p) noexcept {return p.kind==SequenceLeaf||p.kind==LocatorLeaf;}
    static size_t fanout(bool seq) noexcept {return seq?sequenceFanout:orderFanout;}
    template<class R> static uint64_t child(const R& r,bool seq) noexcept {return seq?uint64_t(r.key):r.value;}
    static void demand(bool b){if(!b)throw Failure::Invalid;}
    const Page& read(uint64_t id,bool seq,Work& w) {
        const auto& p=file_.read(id,w);
        demand(seq?sequenceKind(p.kind):locatorKind(p.kind));demand(p.parent!=id);return p;
    }
    Node load(uint64_t id,bool seq,Work& w) {
        const auto& p=read(id,seq,w);Node n{};n.kind=p.kind;n.count=p.count;n.parent=p.parent;
        for(size_t i=0;i<n.count;++i)n.entries[i]={p.entries[i].key,p.entries[i].value};
        return n;
    }
    static Page encode(const Node& n) {
        Page p{};p.kind=n.kind;p.count=n.count;p.parent=n.parent;
        for(size_t i=0;i<n.count;++i)p.entries[i]={{},n.entries[i].key,n.entries[i].value};
        return p;
    }
    void save(uint64_t id,const Node& n){auto p=encode(n);file_.overwrite(id,p);}
    uint64_t allocate(const Node& n){auto p=encode(n);return file_.write(p);}
    template<class P> static uint64_t weight(const P& p) {
        if(p.kind==SequenceLeaf)return p.count;
        demand(p.kind==SequenceBranch);uint64_t n=0;
        for(size_t i=0;i<p.count;++i){demand(p.entries[i].value<=UINT64_MAX-n);n+=p.entries[i].value;}
        return n;
    }
    static Entry descriptor(uint64_t id,const Node& p,bool seq) {
        demand(p.count>0);return seq?Entry{Key(id),weight(p)}:Entry{p.entries[p.count-1].key,id};
    }
    static bool equal(Entry a,Entry b) noexcept {return a.key==b.key&&a.value==b.value;}
    template<class P> size_t childSlot(const P& p,uint64_t id,bool seq) {
        demand(!leaf(p));size_t i=0;for(;i<p.count&&child(p.entries[i],seq)!=id;++i){}
        demand(i<p.count);return i;
    }
    void reparent(uint64_t id,uint64_t parent,bool seq,Work& w) {
        auto p=load(id,seq,w);if(p.parent!=parent){p.parent=parent;save(id,p);}
    }
    Position locateKey(Key key,Work& w,bool insertion=false,bool query=false) {
        demand(key>0&&locator_);uint64_t id=locator_,parent=0;Key expectedMax=0;
        for(unsigned depth=0;depth<depthLimit;++depth){
            if(query)++stats_.locatorSteps;
            const auto& p=read(id,false,w);demand(p.parent==parent&&(!parent||p.entries[p.count-1].key==expectedMax));
            auto it=std::lower_bound(p.entries.begin(),p.entries.begin()+p.count,key,[](const Record& r,Key k){return r.key<k;});
            size_t i=size_t(it-p.entries.begin());
            if(leaf(p))return {id,i};
            if(i==p.count){demand(insertion);--i;}
            parent=id;expectedMax=p.entries[i].key;id=p.entries[i].value;
        }throw Failure::Invalid;
    }
    uint64_t lookupLeaf(Key key,Work& w,bool query){
        auto pos=locateKey(key,w,false,query);const auto& p=read(pos.page,false,w);
        demand(pos.slot<p.count&&p.entries[pos.slot].key==key);return p.entries[pos.slot].value;
    }
    // Missing key is allowed only for insertion. All other lookups fail closed.
    Position locateSequence(Key key,Work& w,bool query=false) {
        const auto id=lookupLeaf(key,w,query);const auto& s=read(id,true,w);demand(leaf(s));
        size_t i=0;for(;i<s.count&&s.entries[i].key!=key;++i){}demand(i<s.count);
        return {id,i};
    }
    void refresh(uint64_t id,const Node& node,bool seq,Work& w,unsigned depth) {
        demand(depth<depthLimit);
        if(!node.parent){demand(id==(seq?sequence_:locator_));return;}
        auto p=load(node.parent,seq,w);auto slot=childSlot(p,id,seq);
        const auto r=descriptor(id,node,seq);if(equal(p.entries[slot],r))return;
        p.entries[slot]=r;save(node.parent,p);refresh(node.parent,p,seq,w,depth+1);
    }
    // Insert a record at a known slot. Return the final leaf page for the new
    // key. Branch splits update only moved children's parent pointers; leaf
    // splits repair only moved locators. No document-size rank rewriting.
    uint64_t insertAt(uint64_t id,size_t slot,Entry value,bool seq,Work& w,unsigned depth,const Entry* prior=nullptr) {
        demand(depth<depthLimit);auto p=load(id,seq,w);demand(slot<=p.count);
        if(prior){demand(slot>0);p.entries[slot-1]=*prior;}
        const size_t cap=fanout(seq);
        if(p.count<cap){
            std::move_backward(p.entries.begin()+slot,p.entries.begin()+p.count,p.entries.begin()+p.count+1);
            p.entries[slot]=value;++p.count;save(id,p);refresh(id,p,seq,w,depth);return id;
        }
        std::array<Entry,orderFanout+1> entries{};
        std::copy_n(p.entries.begin(),slot,entries.begin());entries[slot]=value;
        std::copy(p.entries.begin()+slot,p.entries.begin()+p.count,entries.begin()+slot+1);
        Node right{};right.kind=p.kind;right.parent=p.parent;
        p.count=uint32_t((cap+1)/2);right.count=uint32_t(cap+1-p.count);
        std::copy_n(entries.begin(),p.count,p.entries.begin());
        std::copy_n(entries.begin()+p.count,right.count,right.entries.begin());
        save(id,p);const auto other=allocate(right);++stats_.splits;
        moved(other,right,seq,w,seq&&leaf(p)?value.key:0);
        if(!p.parent){
            auto& root=seq?sequence_:locator_;demand(root==id);
            Node parent{};parent.kind=seq?SequenceBranch:LocatorBranch;parent.count=2;
            parent.entries[0]=descriptor(id,p,seq);parent.entries[1]=descriptor(other,right,seq);
            root=allocate(parent);reparent(id,root,seq,w);reparent(other,root,seq,w);
        }else{
            auto parent=load(p.parent,seq,w);const auto at=childSlot(parent,id,seq);
            const auto left=descriptor(id,p,seq);
            insertAt(p.parent,at+1,descriptor(other,right,seq),seq,w,depth+1,&left);
        }
        return slot<p.count?id:other;
    }
    void setLocator(Key key,uint64_t page,Work& w,bool allowNew) {
        if(!locator_){demand(allowNew);Node p{};p.kind=LocatorLeaf;p.count=1;p.entries[0]={key,page};locator_=allocate(p);return;}
        auto pos=locateKey(key,w,true);auto p=load(pos.page,false,w);
        if(pos.slot<p.count&&p.entries[pos.slot].key==key){
            if(p.entries[pos.slot].value!=page){p.entries[pos.slot].value=page;save(pos.page,p);++stats_.remaps;}
        }else{demand(allowNew);insertAt(pos.page,pos.slot,{key,page},false,w,0);}
    }
    void moved(uint64_t id,const Node& p,bool seq,Work& w,Key newKey=0) {
        if(!leaf(p)){
            for(size_t i=0;i<p.count;++i)reparent(child(p.entries[i],seq),id,seq,w);
        }else if(seq){
            // Relocations are existing keys only; insert the new key once
            // after its final destination is known. Group by locator leaf,
            // avoiding a full page write for every moved ID on that leaf.
            std::array<Key,sequenceFanout> keys{};size_t n=0;
            for(size_t i=0;i<p.count;++i)if(p.entries[i].key!=newKey)keys[n++]=p.entries[i].key;
            std::sort(keys.begin(),keys.begin()+n);
            for(size_t i=0;i<n;){
                const auto pos=locateKey(keys[i],w);auto loc=load(pos.page,false,w);size_t changes=0;
                const auto end=loc.entries.begin()+loc.count;auto it=loc.entries.begin()+pos.slot;
                do{
                    it=std::lower_bound(it,end,keys[i],[](const Entry& r,Key k){return r.key<k;});
                    demand(it!=end&&it->key==keys[i]);
                    if(it->value!=id){it->value=id;++changes;}
                    ++i;++it;
                }while(i<n&&keys[i]<=loc.entries[loc.count-1].key);
                if(changes){save(pos.page,loc);stats_.remaps+=changes;}
            }
        }
    }
    void movedOne(uint64_t id,const Node& p,Entry r,bool seq,Work& w){
        if(!leaf(p))reparent(child(r,seq),id,seq,w);
        else if(seq)setLocator(r.key,id,w,false);
    }
    // Remove from a leaf or branch. Restore minimum half occupancy by merging
    // or borrowing. Recycle freed page slots; shrinking to empty releases root.
    void eraseAt(uint64_t id,size_t slot,bool seq,Work& w,unsigned depth,const Entry* prior=nullptr) {
        demand(depth<depthLimit);auto p=load(id,seq,w);demand(slot<p.count);
        if(prior){demand(slot>0);p.entries[slot-1]=*prior;}
        std::move(p.entries.begin()+slot+1,p.entries.begin()+p.count,p.entries.begin()+slot);--p.count;
        if(!p.parent){
            auto& root=seq?sequence_:locator_;demand(root==id);
            if(!p.count){demand(leaf(p));file_.release(id,p.kind);root=0;}
            else if(!leaf(p)&&p.count==1){root=child(p.entries[0],seq);reparent(root,0,seq,w);file_.release(id,p.kind);++stats_.rootCollapses;}
            else save(id,p);
            return;
        }
        const auto cap=fanout(seq);
        if(p.count>=cap/2){save(id,p);refresh(id,p,seq,w,depth);return;}
        // p is still local (the on-disk copy is the previous valid node).
        // Keep the left page on merge, so only right-side IDs are remapped.
        auto parent=load(p.parent,seq,w);const auto at=childSlot(parent,id,seq);
        demand(parent.count>=2);const size_t leftAt=at?at-1:at;
        const auto leftId=child(parent.entries[leftAt],seq),rightId=child(parent.entries[leftAt+1],seq);
        Node left=leftId==id?p:load(leftId,seq,w),right=rightId==id?p:load(rightId,seq,w);
        demand(left.kind==right.kind&&left.parent==p.parent&&right.parent==p.parent&&leftId!=rightId);
        if(size_t(left.count)+right.count<=cap){
            const auto oldLeft=left.count;std::copy_n(right.entries.begin(),right.count,left.entries.begin()+oldLeft);left.count+=right.count;
            save(leftId,left);moved(leftId,right,seq,w);file_.release(rightId,right.kind);++stats_.merges;
            const auto merged=descriptor(leftId,left,seq);
            eraseAt(p.parent,leftAt+1,seq,w,depth+1,&merged);
        }else{
            if(leftId==id){
                auto r=right.entries[0];left.entries[left.count++]=r;
                std::move(right.entries.begin()+1,right.entries.begin()+right.count,right.entries.begin());--right.count;movedOne(leftId,left,r,seq,w);
            }else{
                auto r=left.entries[--left.count];std::move_backward(right.entries.begin(),right.entries.begin()+right.count,right.entries.begin()+right.count+1);
                right.entries[0]=r;++right.count;movedOne(rightId,right,r,seq,w);
            }
            demand(left.count>=cap/2&&right.count>=cap/2);
            save(leftId,left);save(rightId,right);++stats_.borrows;
            parent.entries[leftAt]=descriptor(leftId,left,seq);parent.entries[leftAt+1]=descriptor(rightId,right,seq);
            save(p.parent,parent);refresh(p.parent,parent,seq,w,depth+1);
        }
    }
    template<class Stop> uint64_t bulk(Records& rows,bool seq,Stop& stop) {
        if(rows.empty())return 0;
        bool isLeaf=true;const auto cap=fanout(seq);Work w;
        while(true){
            const size_t groups=(rows.size()+cap-1)/cap;
            Records parents{Alloc<Record>(budget_)};parents.reserve(groups);
            size_t offset=0;
            for(size_t g=0;g<groups;++g){
                if(stop())throw Failure::Cancelled;
                Node p{};p.kind=seq?(isLeaf?SequenceLeaf:SequenceBranch):(isLeaf?LocatorLeaf:LocatorBranch);
                p.count=uint32_t(rows.size()/groups+(g<rows.size()%groups));
                for(size_t i=0;i<p.count;++i)p.entries[i]={rows[offset+i].key,rows[offset+i].value};
                offset+=p.count;
                auto id=allocate(p);
                if(!isLeaf)for(size_t i=0;i<p.count;++i)reparent(child(p.entries[i],seq),id,seq,w);
                const auto entry=descriptor(id,p,seq);parents.push_back({{},entry.key,entry.value});
            }
            if(parents.size()==1){stats_.maintenanceReads+=w.reads;stats_.maintenanceHits+=w.hits;return child(parents[0],seq);}
            rows.swap(parents);isLeaf=false;
        }
    }
    template<class Stop> void collectLocators(uint64_t id,Records& rows,Stop& stop,Work& w,unsigned depth=0) {
        demand(depth<depthLimit);if(stop())throw Failure::Cancelled;auto p=load(id,true,w);
        if(leaf(p)){for(size_t i=0;i<p.count;++i)rows.push_back({{},p.entries[i].key,id});}
        else for(size_t i=0;i<p.count;++i)collectLocators(child(p.entries[i],true),rows,stop,w,depth+1);
    }
public:
    CountedOrder(PageFile& f,Budget& b,OrderStats& s):file_(f),budget_(b),stats_(s){}
    void reset() noexcept {sequence_=locator_=0;count_=0;}
    template<class Stop> void build(Records& rows,Stop& stop) {
        reset();count_=rows.size();sequence_=bulk(rows,true,stop);
        if(!sequence_)return;
        Records loc{Alloc<Record>(budget_)};loc.reserve(count_);Work w;collectLocators(sequence_,loc,stop,w);
        demand(loc.size()==count_);std::sort(loc.begin(),loc.end(),[](const Record& a,const Record& b){return a.key<b.key;});
        demand(std::adjacent_find(loc.begin(),loc.end(),[](const Record& a,const Record& b){return a.key==b.key;})==loc.end());
        locator_=bulk(loc,false,stop);stats_.maintenanceReads+=w.reads;stats_.maintenanceHits+=w.hits;
    }
    size_t count()const noexcept{return count_;}
    void insertAfter(Key prev,Key key,Work& w) {
        demand(key>0&&prev!=key);
        if(locator_){auto pos=locateKey(key,w,true);auto p=load(pos.page,false,w);demand(pos.slot==p.count||p.entries[pos.slot].key!=key);}
        uint64_t dest;
        if(!sequence_){demand(!prev&&!count_&&!locator_);Node p{};p.kind=SequenceLeaf;p.count=1;p.entries[0]={key,0};sequence_=dest=allocate(p);}
        else{
            Position pos{};
            if(prev){pos=locateSequence(prev,w);++pos.slot;}
            else{uint64_t id=sequence_;for(unsigned depth=0;;++depth){demand(depth<depthLimit);auto p=load(id,true,w);if(leaf(p)){pos={id,0};break;}id=child(p.entries[0],true);}}
            dest=insertAt(pos.page,pos.slot,{key,0},true,w,0);
        }
        setLocator(key,dest,w,true);++count_;
    }
    void remove(Key key,Work& w) {
        demand(count_);const auto seq=locateSequence(key,w),loc=locateKey(key,w);
        eraseAt(loc.page,loc.slot,false,w,0);eraseAt(seq.page,seq.slot,true,w,0);--count_;
        demand(bool(count_)==bool(sequence_)&&bool(count_)==bool(locator_));
    }
    uint64_t leafFor(Key key,Work& w){return lookupLeaf(key,w,true);}
    struct LeafRanks {std::array<Key,sequenceFanout> ids{};size_t count=0;uint64_t prefix=0;};
    LeafRanks leafRanks(uint64_t id,Work& w,bool query=true){
        LeafRanks result;
        const auto& node=read(id,true,w);demand(leaf(node));result.count=node.count;
        for(size_t i=0;i<node.count;++i)result.ids[i]=node.entries[i].key;
        uint64_t parentId=node.parent,subtree=node.count;
        for(unsigned depth=0;depth<depthLimit;++depth){
            if(query)++stats_.rankSteps;
            demand(result.prefix<=subtree-result.count);
            if(!parentId){demand(id==sequence_&&subtree==count_);return result;}
            const auto& parent=read(parentId,true,w);const auto slot=childSlot(parent,id,true);
            demand(parent.entries[slot].value==subtree);
            for(size_t i=0;i<slot;++i){demand(parent.entries[i].value<=UINT64_MAX-result.prefix);result.prefix+=parent.entries[i].value;}
            id=parentId;parentId=parent.parent;subtree=weight(parent);demand(subtree>=result.count);
        }throw Failure::Invalid;
    }
    uint64_t rank(Key key,Work& w,bool query=true){const auto r=leafRanks(lookupLeaf(key,w,query),w,query);
        const auto it=std::find(r.ids.begin(),r.ids.begin()+r.count,key);demand(it!=r.ids.begin()+r.count);
        return r.prefix+uint64_t(it-r.ids.begin())+1;
    }
#ifndef __ANDROID__
    uint64_t sequenceRoot()const noexcept{return sequence_;}
    uint64_t locatorRoot()const noexcept{return locator_;}
#endif
};
}
