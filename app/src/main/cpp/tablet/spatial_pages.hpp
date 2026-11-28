#pragma once
// Transitional, worker-owned access service over the validated resident scene.
// A private unlinked file holds both packed spatial pages and ID->paint rank
// pages. No page/overlay entry is authoritative, no geometry is split/clipped.
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <span>
#include <unistd.h>
#include <sys/statvfs.h>
#include <vector>

namespace engine::tablet::spatial {
using Key=int64_t;
constexpr size_t pageBytes=16384, spatialFanout=64, orderFanout=256;
constexpr size_t overlayLimit=256, cacheSlots=32, rankCacheSlots=4096;
static_assert(rankCacheSlots==4096); // high 12 bits of multiplicative hash
constexpr uint64_t diskLimit=64ull<<20, ramLimit=32ull<<20;
struct Box {
    float minX=0,minY=0,maxX=0,maxY=0;
    bool valid() const noexcept {return std::isfinite(minX)&&std::isfinite(minY)&&std::isfinite(maxX)&&std::isfinite(maxY)&&minX<=maxX&&minY<=maxY;}
    bool operator==(const Box&) const = default;
};
struct Window {
    double minX,minY,maxX,maxY;
    bool valid() const noexcept {return std::isfinite(minX)&&std::isfinite(minY)&&std::isfinite(maxX)&&std::isfinite(maxY)&&minX<=maxX&&minY<=maxY;}
    bool contains(Window b) const noexcept {return valid()&&b.valid()&&b.minX>=minX&&b.minY>=minY&&b.maxX<=maxX&&b.maxY<=maxY;}
    bool hits(Box b) const noexcept {return !(b.maxX<minX||b.minX>maxX||b.maxY<minY||b.minY>maxY);}
};
inline Box cover(Box a,Box b) noexcept {return {std::min(a.minX,b.minX),std::min(a.minY,b.minY),std::max(a.maxX,b.maxX),std::max(a.maxY,b.maxY)};}
inline double center(Box b,bool y) noexcept {return y?(double(b.minY)+b.maxY)*.5:(double(b.minX)+b.maxX)*.5;}
inline void peak(std::atomic<uint64_t>& target,uint64_t v) noexcept {
    auto p=target.load(std::memory_order_relaxed);while(p<v&&!target.compare_exchange_weak(p,v,std::memory_order_relaxed)){}
}
using Clock=std::chrono::steady_clock;
inline uint64_t elapsed(Clock::time_point t) noexcept {return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-t).count());}
struct Stats {
    std::atomic<uint64_t> ready{0},baseRows{0},overlayRows{0},diskBytes{0},diskPeak{0},dynamicBytes{0},dynamicPeak{0};
    std::atomic<uint64_t> builds{0},incremental{0},queries{0},cancelled{0},fallbacks{0},ioFaults{0},invalid{0},capacity{0},compactions{0};
    std::atomic<uint64_t> reads{0},cacheHits{0},spatialPages{0},boundsTests{0},overlayNodes{0},shadowed{0},candidates{0},rankLookups{0};
    std::atomic<uint64_t> buildNs{0},buildMaxNs{0},spatialNs{0},spatialMaxNs{0},orderNs{0},orderMaxNs{0},queryNs{0},queryMaxNs{0};
    std::atomic<uint64_t> buildAttempts{0},queryAttempts{0},rankCacheHits{0},overlayActive{0},successfulQueryNs{0},successfulSpatialNs{0},successfulOrderNs{0};
    // Fixed, bounded histogram; bucket edges are upper bounds in microseconds.
    inline static constexpr std::array<uint64_t,16> latencyEdges{25,50,100,200,400,800,1600,3200,6400,12800,25600,51200,102400,204800,409600,819200};
    std::array<std::atomic<uint64_t>,17> latency{};
    std::atomic<uint64_t> successfulMaxNs{0};
    void success(uint64_t total,uint64_t spatial,uint64_t order) noexcept {
        successfulQueryNs+=total;successfulSpatialNs+=spatial;successfulOrderNs+=order;peak(successfulMaxNs,total);
        const auto us=(total+999)/1000;
        auto it=std::lower_bound(latencyEdges.begin(),latencyEdges.end(),us);
        ++latency[size_t(it-latencyEdges.begin())];
    }
    std::array<uint64_t,39> snapshot() const noexcept {
        return {ready.load(),baseRows.load(),overlayRows.load(),diskBytes.load(),diskPeak.load(),dynamicBytes.load(),dynamicPeak.load(),
            builds.load(),incremental.load(),queries.load(),cancelled.load(),fallbacks.load(),ioFaults.load(),invalid.load(),capacity.load(),
            reads.load(),cacheHits.load(),spatialPages.load(),boundsTests.load(),overlayNodes.load(),shadowed.load(),candidates.load(),rankLookups.load(),
            buildNs.load(),buildMaxNs.load(),spatialNs.load(),spatialMaxNs.load(),orderNs.load(),orderMaxNs.load(),queryNs.load(),queryMaxNs.load(),compactions.load(),buildAttempts.load(),queryAttempts.load(),rankCacheHits.load(),overlayActive.load(),successfulQueryNs.load(),successfulSpatialNs.load(),successfulOrderNs.load()};
    }
};
struct Work {
    uint64_t reads=0,hits=0,pages=0,tests=0,nodes=0,shadowed=0,candidates=0,ranks=0,rankHits=0,steps=0;
    void record(Stats& s) const noexcept {
        s.reads+=reads;s.cacheHits+=hits;s.spatialPages+=pages;s.boundsTests+=tests;s.overlayNodes+=nodes;
        s.shadowed+=shadowed;s.candidates+=candidates;s.rankLookups+=ranks;s.rankCacheHits+=rankHits;
    }
};
enum class Failure { Capacity, Io, Invalid, Cancelled, OverlayFull };
struct Budget {
    Stats& stats;
    bool (*admits)(size_t)=nullptr;
    uint64_t limit=ramLimit;
    void charge(size_t n) {
        const auto bytes=stats.dynamicBytes.load();
        if(n>limit||bytes>limit-n) throw std::bad_alloc{};
        // Reserve before asking the shared ledger; other lanes can observe
        // this credit even while the allocator has not obtained memory yet.
        const auto reserved=stats.dynamicBytes.fetch_add(n)+n;
        if(admits&&!admits(0)) {stats.dynamicBytes.fetch_sub(n);throw std::bad_alloc{};}
        peak(stats.dynamicPeak,reserved);
    }
    void release(size_t n) noexcept {stats.dynamicBytes.fetch_sub(n);}
};
template<class T> struct Alloc {
    using value_type=T;
    Budget* budget=nullptr;
    explicit Alloc(Budget& b):budget(&b){}
    template<class U> Alloc(const Alloc<U>& a) noexcept:budget(a.budget){}
    template<class U> bool operator==(const Alloc<U>& a) const noexcept {return budget==a.budget;}
    T* allocate(size_t n) {
        if(n>SIZE_MAX/sizeof(T)) throw std::bad_alloc{};
        budget->charge(n*sizeof(T));try{return std::allocator<T>{}.allocate(n);}catch(...){budget->release(n*sizeof(T));throw;}
    }
    void deallocate(T* p,size_t n) noexcept {std::allocator<T>{}.deallocate(p,n);budget->release(n*sizeof(T));}
};
struct Record {Box box{};Key key=0;uint64_t value=0;};
static_assert(sizeof(Record)==32);
enum Kind : uint32_t { SpatialLeaf=1,SpatialBranch,OrderLeaf,OrderBranch };
struct Page {
    uint32_t magic=0x49505331,kind=0,count=0,checksum=0;
    std::array<Record,orderFanout> entries{};
    std::array<unsigned char,pageBytes-16-sizeof(entries)> padding{};
    uint32_t digest() const noexcept {
        // FNV-1a integrity check for ephemeral native-format scratch pages.
        // Not an authentication mechanism, nor a claim of SQLite integrity.
        const auto* p=reinterpret_cast<const unsigned char*>(this);uint32_t h=2166136261u;
        for(size_t i=0;i<pageBytes;++i) if(i<12||i>=16) h=(h^p[i])*16777619u;
        return h;
    }
    bool valid() const noexcept {
        return magic==0x49505331&&kind>=SpatialLeaf&&kind<=OrderBranch&&count>0&&
            count<=(kind<=SpatialBranch?spatialFanout:orderFanout)&&checksum==digest();
    }
};
static_assert(sizeof(Page)==pageBytes && offsetof(Page,entries)==16);
class PageFile {
    Stats& stats_;
    int fd_=-1;uint64_t pages_=0,clock_=0,limit_=diskLimit;
    struct Slot {uint64_t id=0,age=0;Page page;};
    std::array<Slot,cacheSlots> cache_{};
    static bool transfer(int fd,void* data,size_t bytes,off_t offset,bool writing) noexcept {
        auto* p=static_cast<unsigned char*>(data);
        while(bytes) {
            auto n=writing?::pwrite(fd,p,bytes,offset) : ::pread(fd,p,bytes,offset);
            if(n<0&&errno==EINTR) continue;
            if(n<=0) return false;
            p+=n;bytes-=size_t(n);offset+=n;
        }
        return true;
    }
public:
    explicit PageFile(Stats& s,uint64_t limit=diskLimit):stats_(s),limit_(limit){}
    ~PageFile(){reset();}
    PageFile(const PageFile&)=delete;PageFile& operator=(const PageFile&)=delete;
    void reset() noexcept {
        if(fd_>=0) ::close(fd_);
        fd_=-1;pages_=clock_=0;stats_.diskBytes=0;
        for(auto& s:cache_) {s.id=0;s.age=0;}
    }
    void create(const char* directory,uint64_t required) {
        reset();char name[4096];
        if(!directory||!*directory) throw Failure::Io;
        const auto n=std::snprintf(name,sizeof(name),"%s/ip-spatial-XXXXXX",directory);
        if(n<=0||size_t(n)>=sizeof(name)) throw Failure::Io;
        fd_=::mkstemp(name);
        if(fd_<0) throw Failure::Io;
        // Never reuse a named file after process death or document switch.
        if(::unlink(name)!=0) {reset();(void)::unlink(name);throw Failure::Io;}
        if(::fcntl(fd_,F_SETFD,FD_CLOEXEC)<0) {reset();throw Failure::Io;}
        struct statvfs v{};
        if(::fstatvfs(fd_,&v)!=0 || !v.f_frsize) {reset();throw Failure::Io;}
        constexpr uint64_t keepFree=32ull<<20;
        if(uint64_t(v.f_bavail)<(required+keepFree+v.f_frsize-1)/v.f_frsize) {reset();throw Failure::Capacity;}
    }
    uint64_t write(Page& p) {
        if(fd_<0) throw Failure::Io;
        if(pages_>=limit_/pageBytes) throw Failure::Capacity;
        p.checksum=p.digest();
        const auto end=(pages_+1)*pageBytes;
        stats_.diskBytes=end;peak(stats_.diskPeak,end); // reserve before write
        if(!transfer(fd_,&p,sizeof(p),off_t(pages_*pageBytes),true)) throw Failure::Io;
        return ++pages_;
    }
    const Page& read(uint64_t id,Work& work) {
        if(!id||id>pages_||fd_<0) throw Failure::Invalid;
        Slot* victim=&cache_.front();
        for(auto& s:cache_) {
            if(s.id==id) {s.age=++clock_;++work.hits;return s.page;}
            if(!s.id||s.age<victim->age) victim=&s;
        }
        victim->id=0; // never publish a partial or invalid read
        ++work.reads;
        if(!transfer(fd_,&victim->page,sizeof(Page),off_t((id-1)*pageBytes),false)) throw Failure::Io;
        if(!victim->page.valid()) throw Failure::Invalid;
        victim->id=id;victim->age=++clock_;return victim->page;
    }
#ifndef __ANDROID__
    void corrupt(uint64_t id) {assert(fd_>=0&&id&&id<=pages_);unsigned char b=0xFF;
        if(!transfer(fd_,&b,1,off_t((id-1)*pageBytes),true)) throw Failure::Io;
        for(auto& s:cache_)s.id=0;
    }
    void truncate() {assert(fd_>=0);if(::ftruncate(fd_,0)!=0)throw Failure::Io;for(auto& s:cache_)s.id=0;}
#endif
};
// All mutable operations and I/O are performed by the single planner owner.
// An invalidation discards both access roots; no partial index is queryable.
class Index {
    struct Overlay {Key id=0;Box box{};uint64_t rank=0;bool present=false;};
    struct Node {Box box{};uint16_t left=0,right=0,entry=0;bool leaf=false;};
    struct Ranked {Key id;uint64_t rank;};
    Stats& stats_;
    Budget budget_;
    PageFile file_;
    uint64_t diskLimit_;
    uint64_t spatialRoot_=0,orderRoot_=0,maxRank_=0;
    size_t count_=0,overlayCount_=0,nodeCount_=0;
    Key tail_=0;
    bool ready_=false;
    std::array<Overlay,overlayLimit> overlay_{};
    std::array<uint16_t,overlayLimit> overlayOrder_{};
    std::array<Node,2*overlayLimit> nodes_{};
    // Small generation-local direct-mapped cache. It never contains new
    // overlay ranks, and is cleared on every base root replacement/fault.
    std::array<Ranked,rankCacheSlots> rankCache_{};
    std::vector<Ranked,Alloc<Ranked>> ranked_;
    using Records=std::vector<Record,Alloc<Record>>;
    static uint64_t pagesFor(size_t n,size_t fanout) noexcept {
        uint64_t pages=0;while(n) {n=(n-1)/fanout+1;pages+=n;if(n==1)break;}return pages;
    }
    static void pack(Records& rows) {
        auto cmp=[](bool y){return [=](const Record& a,const Record& b){const auto x=center(a.box,y),v=center(b.box,y);return x!=v?x<v:a.key<b.key;};};
        const auto pages=(rows.size()-1)/spatialFanout+1;
        const auto slices=size_t(std::ceil(std::sqrt(double(pages))));
        const auto stripe=((pages-1)/slices+1)*spatialFanout;
        std::sort(rows.begin(),rows.end(),cmp(false));
        for(size_t i=0;i<rows.size();i+=stripe) std::sort(rows.begin()+i,rows.begin()+std::min(i+stripe,rows.size()),cmp(true));
    }
    template<class Stop> uint64_t buildTree(Records& rows,bool spatial,Stop&& stop) {
        if(rows.empty()) return 0;
        const size_t fanout=spatial?spatialFanout:orderFanout;
        bool leaf=true;
        while(true) {
            if(stop()) throw Failure::Cancelled;
            if(spatial) pack(rows);
            // Order rows are key-sorted at leaf creation. Branch max keys
            // preserve that order without any per-level rank recomputation.
            Records parents{Alloc<Record>(budget_)};
            parents.reserve((rows.size()-1)/fanout+1);
            for(size_t i=0;i<rows.size();i+=fanout) {
                if(stop()) throw Failure::Cancelled;
                Page p{};p.kind=spatial?(leaf?SpatialLeaf:SpatialBranch):(leaf?OrderLeaf:OrderBranch);
                p.count=uint32_t(std::min(fanout,rows.size()-i));
                std::copy_n(rows.begin()+i,p.count,p.entries.begin());
                Box box=p.entries[0].box;
                if(spatial) for(size_t j=1;j<p.count;++j) box=cover(box,p.entries[j].box);
                const auto page=file_.write(p);
                parents.push_back(spatial?Record{box,Key(page),0}:Record{{},p.entries[p.count-1].key,page});
            }
            if(parents.size()==1) return spatial?uint64_t(parents[0].key):parents[0].value;
            rows.swap(parents);leaf=false;
        }
    }
    auto findOverlay(Key id) noexcept {return std::lower_bound(overlay_.begin(),overlay_.begin()+overlayCount_,id,[](const Overlay& a,Key k){return a.id<k;});}
    const Overlay* overlay(Key id) const noexcept {
        auto end=overlay_.begin()+overlayCount_;auto it=std::lower_bound(overlay_.begin(),end,id,[](const Overlay& a,Key k){return a.id<k;});
        return it!=end&&it->id==id?&*it:nullptr;
    }
    Overlay& touch(Key id) {
        if(id<=0) throw Failure::Invalid;
        auto it=findOverlay(id),end=overlay_.begin()+overlayCount_;
        if(it!=end&&it->id==id)return *it;
        if(overlayCount_==overlayLimit) throw Failure::OverlayFull;
        std::move_backward(it,end,end+1);++overlayCount_;*it=Overlay{id};return *it;
    }
    uint16_t buildOverlay(size_t lo,size_t hi) noexcept {
        assert(lo<hi&&nodeCount_<nodes_.size());const auto i=uint16_t(nodeCount_++);
        Box b=overlay_[overlayOrder_[lo]].box;
        for(size_t j=lo+1;j<hi;++j)b=cover(b,overlay_[overlayOrder_[j]].box);
        nodes_[i]={b,0,0,overlayOrder_[lo],hi-lo==1};
        if(hi-lo==1)return i;
        const bool y=double(b.maxY)-b.minY>double(b.maxX)-b.minX;
        auto mid=lo+(hi-lo)/2;
        std::nth_element(overlayOrder_.begin()+lo,overlayOrder_.begin()+mid,overlayOrder_.begin()+hi,[&](uint16_t a,uint16_t v){
            const auto ca=center(overlay_[a].box,y),cv=center(overlay_[v].box,y);return ca!=cv?ca<cv:overlay_[a].id<overlay_[v].id;});
        nodes_[i].left=buildOverlay(lo,mid);nodes_[i].right=buildOverlay(mid,hi);return i;
    }
    template<class Stop> static void poll(Work& w,Stop& stop) {if((++w.steps&127)==0&&stop()) throw Failure::Cancelled;}
    template<class Stop> void queryPage(uint64_t id,Window win,std::span<Key> out,size_t& count,Work& w,Stop& stop) {
        poll(w,stop);++w.pages;
        const auto& p=file_.read(id,w);const auto kind=p.kind,n=p.count;
        if(kind!=SpatialLeaf&&kind!=SpatialBranch) throw Failure::Invalid;
        // Cache slots can be replaced by a recursive read. Copy only the
        // bounded child records, never retain a cache pointer across descent.
        std::array<Record,spatialFanout> rows{};std::copy_n(p.entries.begin(),n,rows.begin());
        for(size_t i=0;i<n;++i) {
            poll(w,stop);++w.tests;const auto& r=rows[i];
            if(!r.box.valid()||r.key<=0||(kind==SpatialBranch&&uint64_t(r.key)>=id)) throw Failure::Invalid;
            if(!win.hits(r.box)) continue;
            if(kind==SpatialBranch)queryPage(uint64_t(r.key),win,out,count,w,stop);
            else if(overlay(r.key))++w.shadowed;
            else {if(count==out.size())throw Failure::Capacity;out[count++]=r.key;++w.candidates;}
        }
    }
    template<class Stop> void queryOverlay(uint16_t id,Window win,std::span<Key> out,size_t& count,Work& w,Stop& stop) {
        poll(w,stop);++w.nodes;const auto& n=nodes_[id];
        if(!win.hits(n.box))return;
        if(n.leaf) {if(count==out.size())throw Failure::Capacity;out[count++]=overlay_[n.entry].id;++w.candidates;}
        else {queryOverlay(n.left,win,out,count,w,stop);queryOverlay(n.right,win,out,count,w,stop);}
    }
    template<class Stop> uint64_t rank(Key key,Work& w,Stop& stop) {
        ++w.ranks;
        if(const auto* o=overlay(key)) {if(!o->present)throw Failure::Invalid;if(o->rank)return o->rank;}
        const auto slot=(uint64_t(key)*11400714819323198485ull)>>(64-12);
        auto& cached=rankCache_[slot];
        if(cached.id==key){++w.rankHits;return cached.rank;}
        auto id=orderRoot_;
        while(id) {
            poll(w,stop);const auto& p=file_.read(id,w);
            if(p.kind!=OrderLeaf&&p.kind!=OrderBranch)throw Failure::Invalid;
            const auto end=p.entries.begin()+p.count;
            auto it=std::lower_bound(p.entries.begin(),end,key,[](const Record& r,Key k){return r.key<k;});
            if(it==end)throw Failure::Invalid;
            if(p.kind==OrderLeaf) {if(it->key!=key||!it->value||it->value>maxRank_)throw Failure::Invalid;cached={key,it->value};return it->value;}
            if(!it->value||it->value>=id)throw Failure::Invalid;
            id=it->value;
        }
        throw Failure::Invalid;
    }
    void failure(Failure f) noexcept {
        switch(f){case Failure::Capacity:++stats_.capacity;break;case Failure::Io:++stats_.ioFaults;break;case Failure::Invalid:++stats_.invalid;break;case Failure::Cancelled:++stats_.cancelled;break;case Failure::OverlayFull:++stats_.compactions;break;}
    }
public:
    enum class Result { Ok,Unavailable,Cancelled,Overflow };
    explicit Index(Stats& stats,bool(*admits)(size_t)=nullptr,uint64_t ram=ramLimit,uint64_t disk=diskLimit):
        stats_(stats),budget_{stats,admits,ram},file_(stats,disk),diskLimit_(disk),ranked_(Alloc<Ranked>(budget_)){}
    ~Index(){reset();}
    Index(const Index&)=delete;Index& operator=(const Index&)=delete;
    bool ready() const noexcept {return ready_;}
    size_t count() const noexcept {return count_;}
    Key tail() const noexcept {return tail_;}
    void reset() noexcept {
        ready_=false;stats_.ready=stats_.baseRows=stats_.overlayRows=stats_.overlayActive=0;
        rankCache_.fill({});
        file_.reset();spatialRoot_=orderRoot_=maxRank_=0;count_=overlayCount_=nodeCount_=0;tail_=0;
        {std::vector<Ranked,Alloc<Ranked>> empty{Alloc<Ranked>(budget_)};ranked_.swap(empty);}
    }
    // enumerate(emit) must emit the entire validated link sequence once, with
    // no duplicates, and return true only on complete reciprocal traversal.
    template<class Enumerate,class Stop> bool rebuild(const char* directory,size_t count,Enumerate&& enumerate,Stop&& stop) noexcept {
        const auto start=Clock::now();++stats_.buildAttempts;reset();
        try {
            if(count>diskLimit_/pageBytes*orderFanout||pagesFor(count,spatialFanout)+pagesFor(count,orderFanout)>diskLimit_/pageBytes)throw Failure::Capacity;
            if(count) {
                Records spatial{Alloc<Record>(budget_)},order{Alloc<Record>(budget_)};
                spatial.reserve(count);order.reserve(count);
                if(!enumerate([&](Key id,Box box){
                    if(id<=0||!box.valid()||spatial.size()>=count)throw Failure::Invalid;
                    if((spatial.size()&127)==0&&stop())throw Failure::Cancelled;
                    spatial.push_back({box,id,0});order.push_back({{},id,spatial.size()});tail_=id;
                })||spatial.size()!=count)throw Failure::Invalid;
                std::sort(order.begin(),order.end(),[](const Record& a,const Record& b){return a.key<b.key;});
                if(std::adjacent_find(order.begin(),order.end(),[](const Record& a,const Record& b){return a.key==b.key;})!=order.end())throw Failure::Invalid;
                file_.create(directory,(pagesFor(count,spatialFanout)+pagesFor(count,orderFanout))*pageBytes);spatialRoot_=buildTree(spatial,true,stop);orderRoot_=buildTree(order,false,stop);
            }
            if(stop())throw Failure::Cancelled;
            count_=maxRank_=count;ready_=true;stats_.baseRows=count;stats_.ready=1;++stats_.builds;
        }catch(const Failure& f){failure(f);reset();}catch(const std::bad_alloc&){failure(Failure::Capacity);reset();}catch(...){failure(Failure::Invalid);reset();}
        const auto ns=elapsed(start);stats_.buildNs+=ns;peak(stats_.buildMaxNs,ns);return ready_;
    }
    // Called only after the caller verifies the new sequence is the old
    // sequence minus deletions plus an append-only suffix. Middle splices
    // require a fresh pair of roots in this transitional access service.
    void replace(Key id,Box b) {
        if(!ready_||!b.valid())throw Failure::Invalid;
        auto& o=touch(id);o.box=b;o.present=true;
    }
    void remove(Key id) {
        if(!ready_||!count_)throw Failure::Invalid;
        auto& o=touch(id);o.present=false;o.rank=0;--count_;
    }
    void append(Key id,Box b) {
        if(!ready_||!b.valid()||maxRank_==UINT64_MAX)throw Failure::Invalid;
        auto& o=touch(id);o={id,b,++maxRank_,true};++count_;tail_=id;
    }
    void finishUpdate(Key tail) noexcept {
        assert(ready_);tail_=tail;size_t active=0;nodeCount_=0;
        for(size_t i=0;i<overlayCount_;++i)if(overlay_[i].present)overlayOrder_[active++]=uint16_t(i);
        if(active)buildOverlay(0,active);
        stats_.overlayRows=overlayCount_;stats_.overlayActive=active;++stats_.incremental;
    }
    void updateFailed(Failure f) noexcept {failure(f);reset();}
    template<class Stop> Result query(Window win,std::span<Key> out,size_t& count,Stop&& stop) noexcept {
        count=0;if(!ready_||!win.valid()){++stats_.fallbacks;return Result::Unavailable;}
        ++stats_.queryAttempts;const auto start=Clock::now();auto phase=start;Work w;uint64_t spatialTime=0,orderTime=0;bool orderPhase=false;
        Result result=Result::Unavailable;
        try {
            if(stop())throw Failure::Cancelled;
            if(spatialRoot_)queryPage(spatialRoot_,win,out,count,w,stop);
            if(nodeCount_)queryOverlay(0,win,out,count,w,stop);
            spatialTime=elapsed(phase);phase=Clock::now();orderPhase=true;
            ranked_.clear();ranked_.reserve(count);
            // Batch rank lookups in ID order to reuse adjacent leaf pages;
            // the final ordering is by exact paint rank, never by spatial ID.
            std::sort(out.begin(),out.begin()+count);
            for(size_t i=0;i<count;++i){if(i&&out[i]==out[i-1])throw Failure::Invalid;ranked_.push_back({out[i],rank(out[i],w,stop)});}
            std::sort(ranked_.begin(),ranked_.end(),[](const Ranked& a,const Ranked& b){return a.rank<b.rank;});
            for(size_t i=0;i<count;++i){poll(w,stop);if(i&&ranked_[i-1].rank==ranked_[i].rank)throw Failure::Invalid;out[i]=ranked_[i].id;}
            if(stop())throw Failure::Cancelled;
            result=Result::Ok;++stats_.queries;
        }catch(const Failure& f){failure(f);if(f==Failure::Cancelled)result=Result::Cancelled;
            else if(f==Failure::Capacity)result=Result::Overflow;else reset();}
        catch(const std::bad_alloc&){failure(Failure::Capacity);result=Result::Unavailable;}
        catch(...){failure(Failure::Invalid);reset();}
        if(orderPhase)orderTime=elapsed(phase);else spatialTime=elapsed(phase);
        w.record(stats_);stats_.spatialNs+=spatialTime;peak(stats_.spatialMaxNs,spatialTime);stats_.orderNs+=orderTime;peak(stats_.orderMaxNs,orderTime);
        const auto ns=elapsed(start);stats_.queryNs+=ns;peak(stats_.queryMaxNs,ns);
        if(result==Result::Ok)stats_.success(ns,spatialTime,orderTime);
        if(result!=Result::Ok){count=0;if(result!=Result::Cancelled)++stats_.fallbacks;}
        return result;
    }
#ifndef __ANDROID__
    bool hasBase()const noexcept{return spatialRoot_!=0;}
    void corruptSpatialRoot(){file_.corrupt(spatialRoot_);}
    void corruptOrderRoot(){rankCache_.fill({});file_.corrupt(orderRoot_);}
    void truncatePages(){file_.truncate();}
#endif
};
}
