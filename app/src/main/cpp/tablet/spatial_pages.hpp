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
struct OrderStats {
    std::atomic<uint64_t> sequencePages{0},locatorPages{0},freePages{0},writes{0},writeBytes{0},reused{0};
    std::atomic<uint64_t> splits{0},merges{0},borrows{0},rootCollapses{0},remaps{0};
    std::atomic<uint64_t> spliceAttempts{0},spliceBatches{0},spliceFailures{0},inserted{0},removed{0};
    std::atomic<uint64_t> spliceNs{0},spliceMaxNs{0},rankSteps{0},locatorSteps{0},maintenanceReads{0},maintenanceHits{0};
    std::atomic<uint64_t> validationChecks{0},validationRejected{0},validationNs{0},validationMaxNs{0},largeBatches{0},noCompatibleBase{0},overlayPreflight{0};
    std::atomic<uint64_t> rankCacheShifted{0},rankCacheDropped{0},rankCacheSeeded{0};
    std::array<uint64_t,32> snapshot() const noexcept {
        return {sequencePages.load(),locatorPages.load(),freePages.load(),writes.load(),writeBytes.load(),reused.load(),
            splits.load(),merges.load(),borrows.load(),rootCollapses.load(),remaps.load(),spliceAttempts.load(),spliceBatches.load(),spliceFailures.load(),
            inserted.load(),removed.load(),spliceNs.load(),spliceMaxNs.load(),rankSteps.load(),locatorSteps.load(),maintenanceReads.load(),maintenanceHits.load(),
            validationChecks.load(),validationRejected.load(),validationNs.load(),validationMaxNs.load(),largeBatches.load(),noCompatibleBase.load(),overlayPreflight.load(),rankCacheShifted.load(),rankCacheDropped.load(),rankCacheSeeded.load()};
    }
};
struct CompactionStats {
    std::atomic<uint64_t> attempts{0},success{0},failed{0},visits{0},writes{0},reused{0},released{0},splits{0},merges{0},rows{0},ns{0},maxNs{0},compactWrites{0},compactReads{0},compactHits{0};
    std::array<uint64_t,15> snapshot() const noexcept {
        return {attempts.load(),success.load(),failed.load(),visits.load(),writes.load(),reused.load(),released.load(),splits.load(),merges.load(),rows.load(),ns.load(),maxNs.load(),compactWrites.load(),compactReads.load(),compactHits.load()};
    }
};
struct Stats {
    CompactionStats compact;
    OrderStats order;
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
enum Kind : uint32_t { SpatialLeaf=1,SpatialBranch,SequenceLeaf,SequenceBranch,LocatorLeaf,LocatorBranch,FreePage };
constexpr size_t sequenceFanout=128;
inline bool sequenceKind(uint32_t k) noexcept {return k==SequenceLeaf||k==SequenceBranch;}
inline bool locatorKind(uint32_t k) noexcept {return k==LocatorLeaf||k==LocatorBranch;}
// Portable slicing-by-8 CRC32C. Scratch pages are never reopened across
// generations. Checksum covers every byte except the checksum field itself.
inline constexpr auto crcTables=[] {
    std::array<std::array<uint32_t,256>,8> t{};
    for(uint32_t i=0;i<256;++i){uint32_t c=i;for(int bit=0;bit<8;++bit)c=(c>>1)^((c&1)?0x82F63B78u:0u);t[0][i]=c;}
    for(size_t j=1;j<t.size();++j)for(size_t i=0;i<256;++i){auto c=t[j-1][i];t[j][i]=(c>>8)^t[0][c&255];}
    return t;
}();
inline uint32_t crcExtend(uint32_t c,const unsigned char* p,size_t n) noexcept {
    while(n>=8){
        c^=uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);
        c=crcTables[7][c&255]^crcTables[6][(c>>8)&255]^crcTables[5][(c>>16)&255]^crcTables[4][c>>24]^
            crcTables[3][p[4]]^crcTables[2][p[5]]^crcTables[1][p[6]]^crcTables[0][p[7]];
        p+=8;n-=8;
    }
    while(n--){c=(c>>8)^crcTables[0][(c^*p++)&255];}
    return c;
}
struct Page {
    uint32_t magic=0x49505331,kind=0,count=0,checksum=0;
    std::array<Record,orderFanout> entries{};
    uint64_t parent=0; // parent page, or next free page
    std::array<unsigned char,pageBytes-24-sizeof(entries)> padding{};
    uint32_t digest() const noexcept {
        const auto* p=reinterpret_cast<const unsigned char*>(this);
        return ~crcExtend(crcExtend(UINT32_MAX,p,12),p+16,pageBytes-16);
    }
    bool structural() const noexcept {
        if(magic!=0x49505331||kind<SpatialLeaf||kind>FreePage)return false;
        if(kind==FreePage)return count==0;
        if(!count||count>(kind<=SpatialBranch?spatialFanout:sequenceKind(kind)?sequenceFanout:orderFanout))return false;
        if(kind==SpatialLeaf&&parent!=0)return false;
        if(kind==SpatialBranch&&(parent==0||parent>16))return false;
        if(kind<=SpatialBranch)for(size_t i=0;i<count;++i)if(!entries[i].box.valid()||entries[i].key<=0||entries[i].value!=0)return false;
        if(kind>=SequenceLeaf)for(size_t i=0;i<count;++i){
            const auto& r=entries[i];
            if(r.key<=0||(kind==SequenceLeaf?r.value!=0:r.value==0))return false;
            if(locatorKind(kind)&&i&&entries[i-1].key>=r.key)return false;
        }
        return true;
    }
    bool valid() const noexcept {return structural()&&checksum==digest();}
};
static_assert(sizeof(Page)==pageBytes && offsetof(Page,entries)==16);
class PageFile {
    Stats& stats_;
    int fd_=-1;uint64_t pages_=0,clock_=0,limit_=diskLimit,free_=0;
#ifndef __ANDROID__
    int64_t failWrite_=-1;
#endif
    struct Slot {uint64_t id=0,age=0;Page page;bool dirty=false;};
    bool batchingSpatial_=false;
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
    void writeDisk(uint64_t id,Page& p,bool spatialFree=false) {
#ifndef __ANDROID__
        if(failWrite_==0) {
            (void)transfer(fd_,&p,32,off_t((id-1)*pageBytes),true);
            throw Failure::Io;
        }
        if(failWrite_>0)--failWrite_;
#endif
        if(!transfer(fd_,&p,sizeof(p),off_t((id-1)*pageBytes),true)) throw Failure::Io;
        if(p.kind>=SequenceLeaf&&!spatialFree){++stats_.order.writes;stats_.order.writeBytes+=pageBytes;}
        else ++stats_.compact.writes;
    }
    void flushSlot(Slot& slot) {
        if(slot.dirty){if(!batchingSpatial_||!slot.id)throw Failure::Invalid;writeDisk(slot.id,slot.page,true);slot.dirty=false;}
    }
    Slot& victim() noexcept {
        auto* slot=&cache_.front();for(auto& s:cache_)if(!s.id||s.age<slot->age)slot=&s;return *slot;
    }
    void store(uint64_t id,Page& p,bool spatialFree=false) {
        if(!p.structural())throw Failure::Invalid;
        p.checksum=p.digest();
        if(batchingSpatial_){
            if(p.kind>SpatialBranch&&!(p.kind==FreePage&&spatialFree))throw Failure::Invalid;
            for(auto& slot:cache_)if(slot.id==id){slot.page=p;slot.age=++clock_;slot.dirty=true;return;}
            auto& slot=victim();flushSlot(slot);slot.page=p;slot.id=id;slot.age=++clock_;slot.dirty=true;return;
        }
        writeDisk(id,p,spatialFree);
        for(auto& slot:cache_)if(slot.id==id){slot.page=p;slot.age=++clock_;break;}
    }

public:
    explicit PageFile(Stats& s,uint64_t limit=diskLimit):stats_(s),limit_(limit){}
    ~PageFile(){reset();}
    PageFile(const PageFile&)=delete;PageFile& operator=(const PageFile&)=delete;
    void reset() noexcept {
        if(fd_>=0) ::close(fd_);
        fd_=-1;pages_=clock_=free_=0;stats_.diskBytes=0;
        stats_.order.sequencePages=stats_.order.locatorPages=stats_.order.freePages=0;
#ifndef __ANDROID__
        failWrite_=-1;
#endif
        batchingSpatial_=false;for(auto& s:cache_) {s.id=0;s.age=0;s.dirty=false;}
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
        if(p.kind==FreePage)throw Failure::Invalid;
        uint64_t id;
        if(free_) {
            Work w;id=free_;const auto& old=read(id,w);
            if(old.kind!=FreePage||old.parent==id||old.parent>pages_)throw Failure::Invalid;
            free_=old.parent;--stats_.order.freePages;
            if(p.kind<=SpatialBranch){++stats_.compact.reused;stats_.compact.compactReads+=w.reads;stats_.compact.compactHits+=w.hits;}
            else {++stats_.order.reused;stats_.order.maintenanceReads+=w.reads;stats_.order.maintenanceHits+=w.hits;}
        }else{
            if(pages_>=limit_/pageBytes) throw Failure::Capacity;
            id=++pages_;
            const auto end=pages_*pageBytes;
            stats_.diskBytes=end;peak(stats_.diskPeak,end); // reserve before write
        }
        store(id,p);
        if(sequenceKind(p.kind))++stats_.order.sequencePages;
        if(locatorKind(p.kind))++stats_.order.locatorPages;
        return id;
    }
    void overwrite(uint64_t id,Page& p) {
        if(!id||id>pages_||fd_<0||p.kind==FreePage)throw Failure::Invalid;
        store(id,p);
    }
    void release(uint64_t id,uint32_t kind) {
        if(!id||id>pages_||fd_<0||(kind<SpatialLeaf||kind>=FreePage))throw Failure::Invalid;
        Page p{};p.kind=FreePage;p.parent=free_;store(id,p,kind<=SpatialBranch);free_=id;
        if(sequenceKind(kind))--stats_.order.sequencePages;else if(locatorKind(kind))--stats_.order.locatorPages;else ++stats_.compact.released;
        ++stats_.order.freePages;
    }
    // Private generation only: write-back is bounded by the existing page
    // slots. Dirty eviction writes immediately; successful compaction flushes
    // every remaining dirty page. Any exception drops both derived roots.
    void beginSpatialBatch(){if(batchingSpatial_)throw Failure::Invalid;batchingSpatial_=true;}
    template<class Stop> void finishSpatialBatch(Stop& stop){
        if(!batchingSpatial_)throw Failure::Invalid;
        for(auto& slot:cache_){if(stop())throw Failure::Cancelled;flushSlot(slot);}
        if(stop())throw Failure::Cancelled;
        batchingSpatial_=false;
    }
    const Page& read(uint64_t id,Work& work) {
        if(!id||id>pages_||fd_<0) throw Failure::Invalid;
        for(auto& s:cache_)if(s.id==id){s.age=++clock_;++work.hits;return s.page;}
        auto& slot=victim();flushSlot(slot);slot.id=0; // never publish a partial or invalid read
        ++work.reads;
        if(!transfer(fd_,&slot.page,sizeof(Page),off_t((id-1)*pageBytes),false)) throw Failure::Io;
        if(!slot.page.valid()) throw Failure::Invalid;
        slot.id=id;slot.age=++clock_;return slot.page;
    }

#ifndef __ANDROID__
    uint64_t pageCount()const noexcept{return pages_;}
    uint64_t freeHead()const noexcept{return free_;}
    void failWritesAfter(int64_t n){failWrite_=n;}
    void corrupt(uint64_t id) {assert(fd_>=0&&id&&id<=pages_);unsigned char b=0xFF;
        if(!transfer(fd_,&b,1,off_t((id-1)*pageBytes),true)) throw Failure::Io;
        for(auto& s:cache_)s.id=0;
    }
    void truncate() {assert(fd_>=0);if(::ftruncate(fd_,0)!=0)throw Failure::Io;for(auto& s:cache_)s.id=0;}
#endif
};
}
#include "counted_order.hpp"
namespace engine::tablet::spatial {
// All mutable operations and I/O are performed by the single planner owner.
// An invalidation discards both access roots; no partial index is queryable.
class Index {
    struct Overlay {Key id=0;Box box{},baseBox{};bool present=false,basePresent=false;};
    struct Node {Box box{};uint16_t left=0,right=0,entry=0;bool leaf=false;};
    struct Ranked {Key id;uint64_t rank;uint64_t leaf=0;};
    struct RankCache {Key id;uint64_t rank;};
    Stats& stats_;
    Budget budget_;
    PageFile file_;
    uint64_t diskLimit_;
    uint64_t spatialRoot_=0;
    CountedOrder order_;
    Work updateWork_{};
    Clock::time_point updateStart_{};
    uint64_t inserted_=0,removed_=0;bool updating_=false;
    size_t count_=0,overlayCount_=0,nodeCount_=0;
    Key tail_=0;
    bool ready_=false;
    std::array<Overlay,overlayLimit> overlay_{};
    std::array<uint16_t,overlayLimit> overlayOrder_{};
    std::array<Node,2*overlayLimit> nodes_{};
    // Small generation-local direct-mapped rank cache. Repair positions after
    // each validated splice; clear on any generation replacement or fault.
    std::array<RankCache,rankCacheSlots> rankCache_{};
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
    template<class Stop> uint64_t buildTree(Records& rows,Stop&& stop) {
        if(rows.empty()) return 0;
        const size_t fanout=spatialFanout;
        bool leaf=true;uint64_t level=0;
        while(true) {
            if(stop()) throw Failure::Cancelled;
            pack(rows);
            // Order rows are key-sorted at leaf creation. Branch max keys
            // preserve that order without any per-level rank recomputation.
            Records parents{Alloc<Record>(budget_)};
            parents.reserve((rows.size()-1)/fanout+1);
            for(size_t i=0;i<rows.size();i+=fanout) {
                if(stop()) throw Failure::Cancelled;
                Page p{};p.kind=leaf?SpatialLeaf:SpatialBranch;p.parent=level;
                p.count=uint32_t(std::min(fanout,rows.size()-i));
                std::copy_n(rows.begin()+i,p.count,p.entries.begin());
                Box box=p.entries[0].box;
                for(size_t j=1;j<p.count;++j) box=cover(box,p.entries[j].box);
                const auto page=file_.write(p);
                parents.push_back({box,Key(page),0});
            }
            if(parents.size()==1) return uint64_t(parents[0].key);
            rows.swap(parents);leaf=false;++level;
        }
    }
    // Private single-owner mutable maintenance of the affected packed pages.
    // Height is stored in Page::parent for spatial pages (order pages retain
    // their parent pointer). Height strictly decreases on traversal, including
    // reused page IDs; this replaces the original append-only ID invariant.
    struct SpatialNode {
        uint32_t level=0,count=0;
        std::array<Record,spatialFanout+1> rows{};
    };
    static constexpr uint64_t compactVisitLimit=8192;
    bool (*compactionStop_)(void*)=nullptr;void* compactionContext_=nullptr;
    SpatialNode loadSpatial(uint64_t id,Work& work,int expected=-1) {
        if(compactionStop_&&compactionStop_(compactionContext_))throw Failure::Cancelled;
        if(++work.pages>compactVisitLimit)throw Failure::Capacity;
        const auto& p=file_.read(id,work);
        if((p.kind!=SpatialLeaf&&p.kind!=SpatialBranch)||p.parent>16||
           (expected>=0&&p.parent!=uint64_t(expected)))throw Failure::Invalid;
        SpatialNode n{};n.level=uint32_t(p.parent);n.count=p.count;
        std::copy_n(p.entries.begin(),p.count,n.rows.begin());return n;
    }
    static Box nodeBox(const SpatialNode& n) {
        if(!n.count)throw Failure::Invalid;
        Box b=n.rows[0].box;for(size_t j=1;j<n.count;++j)b=cover(b,n.rows[j].box);return b;
    }
    uint64_t saveSpatial(uint64_t id,const SpatialNode& n) {
        if(!n.count||n.count>spatialFanout||n.level>16)throw Failure::Invalid;
        Page p{};p.kind=n.level?SpatialBranch:SpatialLeaf;p.parent=n.level;p.count=n.count;
        std::copy_n(n.rows.begin(),n.count,p.entries.begin());
        if(id){file_.overwrite(id,p);return id;}return file_.write(p);
    }
    static double area(Box b) noexcept {return (double(b.maxX)-b.minX)*(double(b.maxY)-b.minY);}
    static double margin(Box b) noexcept {return double(b.maxX)-b.minX+double(b.maxY)-b.minY;}
    static bool contains(Box outer,Box inner) noexcept {
        return outer.minX<=inner.minX&&outer.minY<=inner.minY&&outer.maxX>=inner.maxX&&outer.maxY>=inner.maxY;
    }
    struct SpatialSplit {Record left{},right{};};
    SpatialSplit insertSpatial(uint64_t page,Record row,Work& work,int expected=-1) {
        auto n=loadSpatial(page,work,expected);
        if(!n.level)n.rows[n.count++]=row;
        else {
            size_t slot=0;
            auto score=[&](Box b){const auto joined=cover(b,row.box);return std::array<double,4>{area(joined)-area(b),margin(joined)-margin(b),area(b),margin(b)};};
            for(size_t j=1;j<n.count;++j)if(score(n.rows[j].box)<score(n.rows[slot].box))slot=j;
            auto child=insertSpatial(uint64_t(n.rows[slot].key),row,work,int(n.level)-1);
            n.rows[slot]=child.left;if(child.right.key)n.rows[n.count++]=child.right;
        }
        if(n.count<=spatialFanout){saveSpatial(page,n);return {{nodeBox(n),Key(page),0},{}};}
        const Box bounds=nodeBox(n);const bool y=double(bounds.maxY)-bounds.minY>double(bounds.maxX)-bounds.minX;
        std::sort(n.rows.begin(),n.rows.begin()+n.count,[=](const Record& a,const Record& b){const auto x=center(a.box,y),v=center(b.box,y);return x!=v?x<v:a.key<b.key;});
        SpatialNode other{};other.level=n.level;other.count=n.count-n.count/2;n.count/=2;
        std::copy_n(n.rows.begin()+n.count,other.count,other.rows.begin());
        const auto newPage=saveSpatial(0,other);saveSpatial(page,n);++stats_.compact.splits;
        return {{nodeBox(n),Key(page),0},{nodeBox(other),Key(newPage),0}};
    }
    void addSpatial(Record row,Work& work) {
        if(!spatialRoot_){SpatialNode n{};n.count=1;n.rows[0]=row;spatialRoot_=saveSpatial(0,n);return;}
        const auto result=insertSpatial(spatialRoot_,row,work);
        if(result.right.key){auto left=loadSpatial(spatialRoot_,work);SpatialNode root{};root.level=left.level+1;root.count=2;root.rows[0]=result.left;root.rows[1]=result.right;spatialRoot_=saveSpatial(0,root);}
    }
    // Search only branches enclosing the *previous* exact box. Each overlay
    // entry retains that box even across repeated moves/delete/restore cycles.
    // Worst-case overlap is bounded by a fixed work ceiling, not assumed log N.
    bool eraseSpatial(uint64_t page,Key key,Box before,Work& work,Record& descriptor,int expected=-1) {
        auto n=loadSpatial(page,work,expected);
        size_t slot=0;
        if(!n.level){
            while(slot<n.count&&n.rows[slot].key!=key)++slot;
            if(slot==n.count)return false;
            if(n.rows[slot].box!=before)throw Failure::Invalid;
            std::move(n.rows.begin()+slot+1,n.rows.begin()+n.count,n.rows.begin()+slot);--n.count;
        }else{
            bool found=false;
            for(;slot<n.count;++slot){
                if(!contains(n.rows[slot].box,before))continue;
                Record child{};
                if(!eraseSpatial(uint64_t(n.rows[slot].key),key,before,work,child,int(n.level)-1))continue;
                found=true;
                if(!child.key){std::move(n.rows.begin()+slot+1,n.rows.begin()+n.count,n.rows.begin()+slot);--n.count;}
                else {
                    n.rows[slot]=child;
                    // Coalesce locally when an affected page and an adjacent
                    // sibling fit in one page. Never scan the entire tree to
                    // improve packing; the initial STR locality is retained.
                    auto current=loadSpatial(uint64_t(child.key),work,int(n.level)-1);
                    if(current.count<spatialFanout/2&&n.count>1){
                        const size_t sibling=slot?slot-1:slot+1;
                        auto other=loadSpatial(uint64_t(n.rows[sibling].key),work,int(n.level)-1);
                        if(current.count+other.count<=spatialFanout){
                            std::copy_n(other.rows.begin(),other.count,current.rows.begin()+current.count);current.count+=other.count;
                            saveSpatial(uint64_t(child.key),current);n.rows[slot].box=nodeBox(current);
                            file_.release(uint64_t(n.rows[sibling].key),other.level?SpatialBranch:SpatialLeaf);
                            std::move(n.rows.begin()+sibling+1,n.rows.begin()+n.count,n.rows.begin()+sibling);--n.count;++stats_.compact.merges;
                        }
                    }
                }
                break;
            }
            if(!found)return false;
        }
        if(!n.count){file_.release(page,n.level?SpatialBranch:SpatialLeaf);descriptor={};}
        else{saveSpatial(page,n);descriptor={nodeBox(n),Key(page),0};}
        return true;
    }
    void removeSpatial(Key key,Box before,Work& work){
        Record root{};if(!spatialRoot_||!eraseSpatial(spatialRoot_,key,before,work,root))throw Failure::Invalid;
        spatialRoot_=uint64_t(root.key);
        while(spatialRoot_){auto n=loadSpatial(spatialRoot_,work);if(!n.level||n.count!=1)break;
            const auto old=spatialRoot_;spatialRoot_=uint64_t(n.rows[0].key);file_.release(old,SpatialBranch);++stats_.compact.merges;}
    }
    template<class Stop> bool compactOverlay(Stop& stop) noexcept {
        auto& c=stats_.compact;++c.attempts;const auto start=Clock::now();Work work{};const auto writes=c.writes.load();
        compactionContext_=&stop;compactionStop_=[](void* p){return (*static_cast<Stop*>(p))();};
        bool success=false;
        try{
            if(!ready_||updating_)throw Failure::Invalid;
            file_.beginSpatialBatch();
            for(size_t i=0;i<overlayCount_;++i){if(stop())throw Failure::Cancelled;if(overlay_[i].basePresent)removeSpatial(overlay_[i].id,overlay_[i].baseBox,work);}
            for(size_t i=0;i<overlayCount_;++i){if(stop())throw Failure::Cancelled;if(overlay_[i].present)addSpatial({overlay_[i].box,overlay_[i].id,0},work);}
            file_.finishSpatialBatch(stop);
            c.rows+=overlayCount_;overlayCount_=nodeCount_=0;stats_.overlayRows=stats_.overlayActive=0;stats_.baseRows=count_;
            ++c.success;++stats_.compactions;success=true;
        }catch(const Failure& f){updateFailed(f);++c.failed;}catch(...){updateFailed(Failure::Invalid);++c.failed;}
        compactionStop_=nullptr;compactionContext_=nullptr;
        c.visits+=work.pages;c.compactWrites+=c.writes.load()-writes;c.compactReads+=work.reads;c.compactHits+=work.hits;const auto ns=elapsed(start);c.ns+=ns;peak(c.maxNs,ns);return success;
    }
    auto findOverlay(Key id) noexcept {return std::lower_bound(overlay_.begin(),overlay_.begin()+overlayCount_,id,[](const Overlay& a,Key k){return a.id<k;});}
    const Overlay* overlay(Key id) const noexcept {
        auto end=overlay_.begin()+overlayCount_;auto it=std::lower_bound(overlay_.begin(),end,id,[](const Overlay& a,Key k){return a.id<k;});
        return it!=end&&it->id==id?&*it:nullptr;
    }
    Overlay& touch(Key id, bool basePresent=false,Box baseBox={}) {
        if(id<=0) throw Failure::Invalid;
        auto it=findOverlay(id),end=overlay_.begin()+overlayCount_;
        if(it!=end&&it->id==id)return *it;
        if(overlayCount_==overlayLimit) throw Failure::OverlayFull;
        std::move_backward(it,end,end+1);++overlayCount_;*it=Overlay{id,{},baseBox,false,basePresent};return *it;
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
    template<class Stop> void queryPage(uint64_t id,Window win,std::span<Key> out,size_t& count,Work& w,Stop& stop,int expectedLevel=-1) {
        poll(w,stop);++w.pages;
        const auto& p=file_.read(id,w);const auto kind=p.kind,n=p.count;
        const auto level=int(p.parent);
        if((kind!=SpatialLeaf&&kind!=SpatialBranch)||level<0||level>16||(expectedLevel>=0&&level!=expectedLevel)||(kind==SpatialLeaf)!=(level==0)) throw Failure::Invalid;
        // Cache slots can be replaced by a recursive read. Copy only the
        // bounded child records, never retain a cache pointer across descent.
        std::array<Record,spatialFanout> rows{};std::copy_n(p.entries.begin(),n,rows.begin());
        for(size_t i=0;i<n;++i) {
            poll(w,stop);++w.tests;const auto& r=rows[i];
            if(!r.box.valid()||r.key<=0) throw Failure::Invalid;
            if(!win.hits(r.box)) continue;
            if(kind==SpatialBranch)queryPage(uint64_t(r.key),win,out,count,w,stop,level-1);
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
    RankCache& cachedRank(Key key) noexcept {
        return rankCache_[(uint64_t(key)*11400714819323198485ull)>>(64-12)];
    }
    template<class Stop> void rankCandidates(std::span<Key> ids,Work& w,Stop& stop) {
        // First resolve missing keys in ID order, reusing locator pages.
        bool missing=false;
        for(size_t i=0;i<ids.size();++i){
            poll(w,stop);const auto key=ids[i];++w.ranks;
            if(i&&key==ids[i-1])throw Failure::Invalid;
            if(const auto* o=overlay(key))if(!o->present)throw Failure::Invalid;
            auto& c=cachedRank(key);
            if(c.id==key){++w.rankHits;ranked_.push_back({key,c.rank,0});}
            else {missing=true;ranked_.push_back({key,0,order_.leafFor(key,w)}); }
        }
        // Rank once per sequence leaf. The leaf's ancestor counts supply the
        // prefix, its resident slots supply the exact within-leaf order.
        if(!missing)return;
        std::sort(ranked_.begin(),ranked_.end(),[](const Ranked& a,const Ranked& b){return a.leaf<b.leaf;});
        for(size_t i=0;i<ranked_.size();){
            if(!ranked_[i].leaf){++i;continue;}
            poll(w,stop);const auto leaf=ranked_[i].leaf;
            const auto positions=order_.leafRanks(leaf,w);
            do{
                const auto id=ranked_[i].id;
                auto at=std::find(positions.ids.begin(),positions.ids.begin()+positions.count,id);
                if(at==positions.ids.begin()+positions.count)throw Failure::Invalid;
                const auto r=positions.prefix+uint64_t(at-positions.ids.begin())+1;
                ranked_[i].rank=r;cachedRank(id)={id,r};++i;
            }while(i<ranked_.size()&&ranked_[i].leaf==leaf);
        }
    }
    void endUpdate(bool ok) noexcept {
        if(!updating_)return;
        updating_=false;auto& s=stats_.order;
        s.maintenanceReads+=updateWork_.reads;s.maintenanceHits+=updateWork_.hits;
        const auto ns=elapsed(updateStart_);s.spliceNs+=ns;peak(s.spliceMaxNs,ns);
        if(ok){++s.spliceBatches;s.inserted+=inserted_;s.removed+=removed_;}else ++s.spliceFailures;
    }
    void failure(Failure f) noexcept {
        switch(f){case Failure::Capacity:++stats_.capacity;break;case Failure::Io:++stats_.ioFaults;break;case Failure::Invalid:++stats_.invalid;break;case Failure::Cancelled:++stats_.cancelled;break;case Failure::OverlayFull:++stats_.compactions;break;}
    }
public:
    enum class Result { Ok,Unavailable,Cancelled,Overflow };
    explicit Index(Stats& stats,bool(*admits)(size_t)=nullptr,uint64_t ram=ramLimit,uint64_t disk=diskLimit):
        stats_(stats),budget_{stats,admits,ram},file_(stats,std::min(disk,diskLimit)),diskLimit_(std::min(disk,diskLimit)),order_(file_,budget_,stats.order),ranked_(Alloc<Ranked>(budget_)){}
    ~Index(){reset();}
    Index(const Index&)=delete;Index& operator=(const Index&)=delete;
    bool ready() const noexcept {return ready_;}
    size_t count() const noexcept {return count_;}
    Key tail() const noexcept {return tail_;}
    void reset() noexcept {
        ready_=false;stats_.ready=stats_.baseRows=stats_.overlayRows=stats_.overlayActive=0;
        rankCache_.fill({});
        endUpdate(false);order_.reset();file_.reset();spatialRoot_=0;count_=overlayCount_=nodeCount_=0;tail_=0;
        {std::vector<Ranked,Alloc<Ranked>> empty{Alloc<Ranked>(budget_)};ranked_.swap(empty);}
    }
    // enumerate(emit) must emit the entire validated link sequence once, with
    // no duplicates, and return true only on complete reciprocal traversal.
    template<class Enumerate,class Stop> bool rebuild(const char* directory,size_t count,Enumerate&& enumerate,Stop&& stop) noexcept {
        const auto start=Clock::now();++stats_.buildAttempts;reset();
        try {
            const auto pages=pagesFor(count,spatialFanout)+pagesFor(count,sequenceFanout)+pagesFor(count,orderFanout);
            if(count>diskLimit_/pageBytes*orderFanout||pages>diskLimit_/pageBytes)throw Failure::Capacity;
            file_.create(directory,pages*pageBytes);
            if(count) {
                Records spatial{Alloc<Record>(budget_)},order{Alloc<Record>(budget_)};
                spatial.reserve(count);order.reserve(count);
                if(!enumerate([&](Key id,Box box){
                    if(id<=0||!box.valid()||spatial.size()>=count)throw Failure::Invalid;
                    if((spatial.size()&127)==0&&stop())throw Failure::Cancelled;
                    spatial.push_back({box,id,0});order.push_back({{},id,0});tail_=id;
                })||spatial.size()!=count)throw Failure::Invalid;
                spatialRoot_=buildTree(spatial,stop);
                order_.build(order,stop);
            }else if(!enumerate([](Key,Box){throw Failure::Invalid;}))throw Failure::Invalid;
            if(stop())throw Failure::Cancelled;
            count_=count;ready_=true;stats_.baseRows=count;stats_.ready=1;++stats_.builds;
        }catch(const Failure& f){failure(f);reset();}catch(const std::bad_alloc&){failure(Failure::Capacity);reset();}catch(...){failure(Failure::Invalid);reset();}
        const auto ns=elapsed(start);stats_.buildNs+=ns;peak(stats_.buildMaxNs,ns);return ready_;
    }
    // Only called by the validated splice adapter. While mutating, no query
    // runs on the sole planner owner. Failure discards the entire generation.
    bool checkDelta(size_t oldCount,size_t patchCount) noexcept {
        if(!ready_||count_!=oldCount){++stats_.order.noCompatibleBase;return false;}
        if(patchCount>3*overlayLimit+2){++stats_.order.largeBatches;return false;}
        return true;
    }
    void classified(bool accepted,uint64_t ns) noexcept {
        auto& s=stats_.order;++s.validationChecks;if(!accepted)++s.validationRejected;
        s.validationNs+=ns;peak(s.validationMaxNs,ns);
    }
    bool alreadyOverlay(Key id) const noexcept {return overlay(id)!=nullptr;}
    template<class Stop> bool reserveOverlay(size_t additional,size_t total,Stop& stop) noexcept {
        if(additional<=overlayLimit-overlayCount_)return true;
        ++stats_.order.overlayPreflight;
        if(total>overlayLimit)return false;
        return compactOverlay(stop);
    }
    void beginUpdate() {
        if(!ready_||updating_)throw Failure::Invalid;
        updating_=true;updateStart_=Clock::now();updateWork_={};inserted_=removed_=0;++stats_.order.spliceAttempts;
    }
    void replace(Key id,Box b,Box before) {
        if(!ready_||!updating_||!b.valid())throw Failure::Invalid;
        auto& o=touch(id,true,before);o.box=b;o.present=true;
    }
    void remove(Key id,Box before) {
        if(!ready_||!updating_||!count_)throw Failure::Invalid;
        auto& o=touch(id,true,before);o.present=false;
        const auto rank=order_.rank(id,updateWork_,false);order_.remove(id,updateWork_);
        // Cached ranks are exact for this single-owner generation. A removal
        // changes only the ranks after the deleted position. Bound work by
        // the fixed cache, not by document cardinality.
        uint64_t shifted=0,dropped=0;
        for(auto& c:rankCache_)if(c.id){if(c.id==id){c={};++dropped;}else if(c.rank>rank){--c.rank;++shifted;}}
        stats_.order.rankCacheShifted+=shifted;stats_.order.rankCacheDropped+=dropped;
        --count_;++removed_;
    }
    void insertAfter(Key prev,Key id,Box b) {
        if(!ready_||!updating_||!b.valid())throw Failure::Invalid;
        auto& o=touch(id);o.box=b;o.present=true;
        const auto rank=prev?order_.rank(prev,updateWork_,false)+1:1;order_.insertAfter(prev,id,updateWork_);
        uint64_t shifted=0;
        for(auto& c:rankCache_)if(c.id&&c.rank>=rank){++c.rank;++shifted;}
        stats_.order.rankCacheShifted+=shifted;cachedRank(id)={id,rank};++stats_.order.rankCacheSeeded;
        ++count_;++inserted_;
    }
    void finishUpdate(Key tail) {
        if(!ready_||!updating_||count_!=order_.count()||bool(count_)!=bool(tail))throw Failure::Invalid;
        tail_=tail;size_t active=0;nodeCount_=0;
        for(size_t i=0;i<overlayCount_;++i)if(overlay_[i].present)overlayOrder_[active++]=uint16_t(i);
        if(active)buildOverlay(0,active);
        stats_.overlayRows=overlayCount_;stats_.overlayActive=active;++stats_.incremental;endUpdate(true);
    }
    void updateFailed(Failure f) noexcept {failure(f);reset();}
    template<class Stop> Result query(Window win,std::span<Key> out,size_t& count,Stop&& stop) noexcept {
        count=0;if(!ready_||updating_||!win.valid()){++stats_.fallbacks;return Result::Unavailable;}
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
            rankCandidates(out.first(count),w,stop);
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
    void corruptOrderRoot(){rankCache_.fill({});file_.corrupt(order_.locatorRoot());}
    void corruptSequenceRoot(){rankCache_.fill({});file_.corrupt(order_.sequenceRoot());}
    void failWritesAfter(int64_t n){file_.failWritesAfter(n);}
    void truncatePages(){file_.truncate();}
#endif
};
}
