// Independent whole-tree structural oracle for the worker-owned counted B+
// tree and ID locator. Tests the actual scratch page implementation.
#include "tablet/spatial_delta.hpp"
#include <map>
#include <random>
#include <set>
#include <string_view>
#include <cstdio>
#include <cstdlib>
#include <pthread.h>
using namespace engine::tablet::spatial;
static void check(bool ok,const char* why){if(!ok){std::fprintf(stderr,"FAIL: %s\n",why);std::exit(1);}}
static auto noStop=[](){return false;};
struct Direct {
    Stats stats;
    Budget budget{stats};
    PageFile file{stats};
    CountedOrder order{file,budget,stats.order};
    std::vector<Key> oracle;
    uint64_t checks=0;
    void build(size_t n){
        file.create("/tmp",diskLimit);oracle.clear();
        std::vector<Record,Alloc<Record>> rows{Alloc<Record>(budget)};rows.reserve(n);
        for(size_t i=0;i<n;++i){auto id=Key(i+2);oracle.push_back(id);rows.push_back({{},id,0});}
        order.build(rows,noStop);
    }
    void insert(size_t pos,Key id){Work w;order.insertAfter(pos?oracle[pos-1]:0,id,w);oracle.insert(oracle.begin()+pos,id);}
    void erase(size_t pos){Work w;order.remove(oracle[pos],w);oracle.erase(oracle.begin()+pos);}
    void ranks(std::mt19937_64& rng,size_t n){
        if(oracle.empty())return;
        Work w;for(size_t i=0;i<n;++i){auto pos=size_t(rng()%oracle.size());check(order.rank(oracle[pos],w)==pos+1,"exact prefix rank");++checks;}
    }
    struct Summary{uint64_t size;Key max;};
    void audit(){
        std::set<uint64_t> visited;std::map<Key,uint64_t> seqLoc,actualLoc;
        std::vector<Key> seq;
        int depths[2]={-1,-1};uint64_t seqPages=0,locPages=0;Work w;
        auto walk=[&](auto&& self,uint64_t id,uint64_t parent,bool sequence,int depth)->Summary{
            check(depth<=3&&visited.insert(id).second,"unique bounded tree page");
            const Page p=file.read(id,w);
            check(p.valid()&&p.parent==parent,"page checksum and exact parent locator");
            check(sequence?sequenceKind(p.kind):locatorKind(p.kind),"page kind belongs to root");
            const size_t cap=sequence?sequenceFanout:orderFanout;
            check(p.count<=cap&&p.count>0&&(!parent||p.count>=cap/2),"nonroot minimum occupancy");
            if(sequence)++seqPages;else ++locPages;
            bool leaf=p.kind==SequenceLeaf||p.kind==LocatorLeaf;
            if(leaf){
                int& expected=depths[sequence?0:1];if(expected<0)expected=depth;
                check(depth==expected,"balanced leaf depth");
                for(size_t i=0;i<p.count;++i){auto r=p.entries[i];
                    if(sequence){seq.push_back(r.key);check(seqLoc.emplace(r.key,id).second,"unique sequence ID");}
                    else check(actualLoc.emplace(r.key,r.value).second,"unique locator ID");
                }
                return {p.count,p.entries[p.count-1].key};
            }
            check(parent||p.count>=2,"collapsed unary root");
            Summary total{0,0};
            for(size_t i=0;i<p.count;++i){const auto r=p.entries[i];auto sub=self(self,sequence?uint64_t(r.key):r.value,id,sequence,depth+1);
                if(sequence)check(r.value==sub.size,"exact subtree count");
                else check(r.key==sub.max&&r.key>total.max,"exact locator separator");
                total.size+=sub.size;total.max=sub.max;
            }
            return total;
        };
        check(order.count()==oracle.size(),"sequence cardinality");
        if(!oracle.empty()){
            check(walk(walk,order.sequenceRoot(),0,true,0).size==oracle.size(),"sequence root count");
            check(walk(walk,order.locatorRoot(),0,false,0).size==oracle.size(),"locator root count");
            check(seq==oracle&&actualLoc==seqLoc,"full ordered oracle and exact ID leaf map");
        }else check(!order.sequenceRoot()&&!order.locatorRoot(),"empty roots");
        uint64_t free=0;
        for(auto id=file.freeHead();id;){check(visited.insert(id).second,"free list disjoint/acyclic");
            auto p=file.read(id,w);check(p.kind==FreePage&&p.count==0,"free page encoding");id=p.parent;++free;
        }
        check(visited.size()==file.pageCount()&&seqPages==stats.order.sequencePages&&locPages==stats.order.locatorPages&&free==stats.order.freePages,"all allocated pages accounted once");
        check(stats.diskBytes==file.pageCount()*pageBytes&&stats.diskBytes<=diskLimit,"disk charged capacity");
        check(stats.dynamicBytes==0,"no retained dynamic tree allocations");
        ++checks;
    }
};
static void randomTest(uint64_t seed,size_t operations){
    auto d=std::make_unique<Direct>();d->build(480);std::mt19937_64 rng(seed);Key next=1000000;
    std::vector<Key> erased;d->audit();
    for(size_t i=0;i<operations;++i){
        bool add=d->oracle.empty()||(d->oracle.size()<950&&(rng()%100<51));
        if(add){Key id=next++;if(!erased.empty()&&rng()%3==0){id=erased.back();erased.pop_back();}d->insert(size_t(rng()%(d->oracle.size()+1)),id);}
        else {auto pos=size_t(rng()%d->oracle.size());erased.push_back(d->oracle[pos]);d->erase(pos);}
        d->ranks(rng,5);if(i%89==0)d->audit();
    }
    d->audit();
    // Remove every row in randomized order, then rebuild solely by edits.
    while(!d->oracle.empty()){d->erase(size_t(rng()%d->oracle.size()));if(d->oracle.size()%63==0)d->audit();}
    const auto high=d->stats.diskBytes.load();for(int i=0;i<350;++i)d->insert(size_t(rng()%(d->oracle.size()+1)),next++);
    d->audit();d->ranks(rng,500);check(d->stats.diskBytes<=high,"freed slots reused after complete shrink");
    std::printf("seed=%llu ops=%zu checks=%llu splits=%llu merges=%llu borrows=%llu reused=%llu peak_disk=%llu\n",
        (unsigned long long)seed,operations,(unsigned long long)d->checks,(unsigned long long)d->stats.order.splits.load(),
        (unsigned long long)d->stats.order.merges.load(),(unsigned long long)d->stats.order.borrows.load(),
        (unsigned long long)d->stats.order.reused.load(),(unsigned long long)d->stats.diskPeak.load());
}
static void cascades(){
    for(size_t n:{sequenceFanout*sequenceFanout,orderFanout*orderFanout}){
        auto d=std::make_unique<Direct>();d->build(n);d->audit();std::mt19937_64 rng(n);
        const auto splits=d->stats.order.splits.load(),collapse=d->stats.order.rootCollapses.load();
        d->insert(0,1);d->audit();d->ranks(rng,1000);
        check(d->stats.order.splits>=splits+2,"cascading branch split");
        d->erase(0);d->audit();d->ranks(rng,1000);
        check(d->stats.order.rootCollapses>collapse,"cascading merge shrinks root");
        std::printf("PASS cascading split/merge at N=%zu, exact all-page audit\n",n);
    }
    auto d=std::make_unique<Direct>();d->build(256);d->audit();
    uint64_t plateau=0;
    for(int i=0;i<160;++i){d->insert(0,1);d->audit();d->erase(0);d->audit();
        if(!i)plateau=d->stats.diskBytes;else check(d->stats.diskBytes==plateau,"split/merge cycles reuse disk slots");}
    check(d->stats.order.reused>0&&d->stats.order.rootCollapses>0,"actual page reclamation");
    std::printf("PASS 160 forced split/merge cycles; disk plateau=%llu bytes\n",(unsigned long long)plateau);
    d->build(400);for(int i=0;i<100;++i)d->erase(0);d->audit();check(d->stats.order.borrows>0,"underfull node borrows from sibling");
    std::puts("PASS borrow path");
}
struct Row{Key prev=0,next=0;bool shape=true;Box box{0,0,1,1};};using Scene=std::map<Key,Row>;
static Box boxOf(const Row& r){return r.box;}
static Scene scene(std::vector<Key> ids){Scene s;for(size_t i=0;i<ids.size();++i)s[ids[i]]={i?ids[i-1]:0,i+1<ids.size()?ids[i+1]:0,true,{0,0,1,1}};return s;}
static Scene delta(const Scene& a,const Scene& b){Scene p;for(auto&[id,r]:a)if(!b.contains(id))p[id]={0,0,false,{}};
    for(auto&[id,r]:b){auto it=a.find(id);if(it==a.end()||it->second.prev!=r.prev||it->second.next!=r.next||it->second.box!=r.box)p[id]=r;}return p;}
static bool build(Index& i,const std::vector<Key>& ids){return i.rebuild("/tmp",ids.size(),[&](auto emit){for(auto id:ids)emit(id,{0,0,1,1});return true;},noStop);}
static void compare(Index& i,const std::vector<Key>& ids){std::vector<Key> out(ids.size()+2);size_t n=0;check(i.query({0,0,1,1},out,n,noStop)==Index::Result::Ok&&n==ids.size()&&std::equal(ids.begin(),ids.end(),out.begin()),"paired exact visibility order");}
static void writeFaults(){
    // Split and merge cases exercise overwrite, allocation and reclaimed-page
    // writes. Faults persist a real partial 32-byte write at each site.
    for(size_t size:{size_t(256),size_t(8192)})for(bool removal:{false,true}){
        std::vector<Key> base;for(size_t i=0;i<size;++i)base.push_back(Key(2+(i*(size==256?1:127))%size));
        auto expanded=base;expanded.insert(expanded.begin(),1);
        auto a=scene(base),b=scene(expanded);const auto& from=removal?expanded:base;const auto& to=removal?base:expanded;
        const auto& old=removal?b:a;const auto& now=removal?a:b;auto patch=delta(old,now);
        Stats s;auto idx=std::make_unique<Index>(s);
        auto setup=[&]{check(build(*idx,base),"fault base");if(removal)check(updateOrdered(*idx,a,delta(a,b),2,1,boxOf),"fault merge setup");};
        setup();const auto before=s.order.writes.load();check(updateOrdered(*idx,old,patch,from.front(),to.front(),boxOf),"fault control splice");const auto writes=s.order.writes.load()-before;compare(*idx,to);
        check(writes>4&&writes<1024,"bounded split/merge write count");
        for(uint64_t n=0;n<writes;++n){setup();const auto io=s.ioFaults.load();idx->failWritesAfter(int64_t(n));
            check(!updateOrdered(*idx,old,patch,from.front(),to.front(),boxOf)&&!idx->ready(),"partial update invalidates BOTH roots");
            check(s.ioFaults==io+1&&s.diskBytes==0&&s.dynamicBytes==0&&s.order.freePages==0&&s.order.sequencePages==0&&s.order.locatorPages==0,"fault releases complete derived generation");
            std::array<Key,8> out{};size_t nout=99;check(idx->query({0,0,1,1},out,nout,noStop)==Index::Result::Unavailable&&nout==0,"no partially ordered plan");
            check(build(*idx,to),"rebuild canonical target after write fault");compare(*idx,to);
        }
        std::printf("PASS N=%zu %llu partial-write sites in %s, fail closed then exact rebuild\n",size,(unsigned long long)writes,removal?"merge":"split");
    }
    std::vector<Key> v;for(Key i=2;i<=129;++i)v.push_back(i);auto next=v;next.insert(next.begin(),1);
    Stats s;auto idx=std::make_unique<Index>(s,nullptr,ramLimit,5*pageBytes);
    check(build(*idx,v),"exact disk ceiling base");check(!updateOrdered(*idx,scene(v),delta(scene(v),scene(next)),2,1,boxOf)&&!idx->ready()&&s.capacity==1&&s.diskBytes==0,"growth denied without partial plan");
    std::puts("PASS exact disk-capacity failure during split");
}
static void deltaCases(){
    Stats s;auto idx=std::make_unique<Index>(s);std::vector<Key> a{10,20,30,40,50},b{1,10,2,3,30,4,50,5};
    check(build(*idx,a),"multi-gap base");auto old=scene(a),now=scene(b);auto patch=delta(old,now);patch[99]={0,0,false,{}};
    auto invalid=patch;invalid.erase(30);auto writes=s.order.writes.load();
    check(!updateOrdered(*idx,old,invalid,10,1,boxOf)&&s.order.writes==writes,"missing reciprocal edit rejected before writes");compare(*idx,a);
    invalid=patch;invalid[100]={101,101,true,{0,0,1,1}};invalid[101]={100,100,true,{0,0,1,1}};
    check(!updateOrdered(*idx,old,invalid,10,1,boxOf)&&s.order.writes==writes,"disconnected reciprocal cycle rejected before writes");compare(*idx,a);
    check(updateOrdered(*idx,old,patch,10,1,boxOf),"multiple arbitrary splices in one batch");compare(*idx,b);
    check(s.builds==1&&s.order.inserted==5&&s.order.removed==2,"only actual changed IDs modified");
    auto reorder=b;std::swap(reorder[1],reorder[4]);auto rscene=scene(reorder);
    writes=s.order.writes;check(!updateOrdered(*idx,now,delta(now,rscene),1,reorder.front(),boxOf)&&s.order.writes==writes,"unsupported survivor reorder fails before mutation");compare(*idx,b);
    check(updateOrdered(*idx,now,delta(now,{}),1,0,boxOf),"remove all through splice");compare(*idx,{});
    check(updateOrdered(*idx,Scene{},old,0,10,boxOf),"insert into empty generation");compare(*idx,a);
    auto moved=old;moved[30].box={4,4,5,5};writes=s.order.writes;check(updateOrdered(*idx,old,delta(old,moved),10,10,boxOf)&&s.order.writes==writes,"geometry-only edit performs no rank writes");
    check(build(*idx,a),"sequence corruption base");idx->corruptSequenceRoot();std::array<Key,16> out{};size_t count=99;
    check(idx->query({0,0,1,1},out,count,noStop)==Index::Result::Unavailable&&count==0&&!idx->ready(),"sequence checksum fault rejects entire plan");
    const unsigned char v[]="123456789";check(~crcExtend(UINT32_MAX,v,9)==0xe3069283u,"standard CRC32C test vector");
    Page p{};p.kind=SequenceLeaf;p.count=1;p.entries[0].key=1;p.checksum=p.digest();check(p.valid(),"valid checked page");p.padding[400]^=1;check(!p.valid(),"padding corruption detected");
    std::puts("PASS multi-gap splices, invalid deltas, empty transition, geometry-only edit, CRC32C and sequence corruption");
}
// Coalesced multi-gap batches include disjoint removals, arbitrary insertions
// and restored IDs, all checked against an independent vector and link oracle.
static void multigap(uint64_t seed){
    Stats s;auto idx=std::make_unique<Index>(s);std::mt19937_64 rng(seed);
    std::vector<Key> ids;for(Key id=1;id<=350;++id)ids.push_back(id);std::shuffle(ids.begin(),ids.end(),rng);
    auto old=scene(ids);check(build(*idx,ids),"multigap base");Key next=1000;std::vector<Key> gone;size_t fast=0;
    for(int n=0;n<600;++n){
        auto v=ids;auto h=v.empty()?0:v.front();
        const int rem=int(rng()%7),add=int(rng()%7);std::vector<Key> removedNow;
        for(int j=0;j<rem&&!v.empty();++j){size_t pos=rng()%v.size();removedNow.push_back(v[pos]);v.erase(v.begin()+pos); }
        for(int j=0;j<add;++j){Key id=next++;if(!gone.empty()&&rng()%3==0){id=gone.back();gone.pop_back();}v.insert(v.begin()+rng()%(v.size()+1),id);}
        gone.insert(gone.end(),removedNow.begin(),removedNow.end());
        auto now=scene(v);auto patch=delta(old,now);
        if(updateOrdered(*idx,old,patch,h,v.empty()?0:v.front(),boxOf))++fast;
        else check(build(*idx,v),"multigap rebuild");
        compare(*idx,v);ids=std::move(v);old=std::move(now);
    }
    check(fast>570&&s.invalid==0&&s.ioFaults==0&&s.order.spliceFailures==0&&s.order.validationRejected==0,"bounded multi-gap edits incremental except overlay preflight");
    std::printf("seed=%llu multi_gap_batches=600 exact_oracle=600 incremental=%zu classification_reject=%llu compactions=%llu\n",
        (unsigned long long)seed,fast,(unsigned long long)s.order.validationRejected.load(),(unsigned long long)s.compactions.load());
}
static void scaleEdits(size_t n){
    Stats s;auto idx=std::make_unique<Index>(s);std::vector<Key> ids;ids.reserve(n);
    for(size_t j=0;j<n;++j)ids.push_back(Key((j*7919)%n+2));
    auto old=scene(ids);
    // All records exist, but only 256 fixed IDs are visible. Unrelated
    // insertions at arbitrary paint positions use the same overlay ID.
    for(auto&[id,r]:old)if(id>257)r.box={1000,1000,1001,1001};
    check(idx->rebuild("/tmp",n,[&](auto emit){for(auto id:ids)emit(id,old.at(id).box);return true;},noStop),"scale edit build");
    std::vector<uint64_t> timings,writes,queries;std::vector<Key> out(1024);std::mt19937_64 rng(n);Key newId=1;
    const auto buildCount=s.builds.load();size_t count=0;
    auto apply=[&](const Scene& p,Key head,Key newHead){
        const auto start=Clock::now();const auto w=s.order.writes.load();check(updateOrdered(*idx,old,p,head,newHead,boxOf),"middle splice must remain incremental");
        timings.push_back(elapsed(start));writes.push_back(s.order.writes-w);
        for(const auto&[id,r]:p){if(r.shape)old[id]=r;else old.erase(id);}
    };
    for(int iteration=0;iteration<100;++iteration){
        size_t pos=1+rng()%(n-1);Key prev=ids[pos-1],next=ids[pos];Scene add{{prev,old.at(prev)},{next,old.at(next)},{newId,{prev,next,true,{0,0,1,1}}}};
        add[prev].next=newId;add[next].prev=newId;apply(add,ids.front(),ids.front());
        auto t=Clock::now();check(idx->query({0,0,1,1},out,count,noStop)==Index::Result::Ok,"post-splice sparse query");queries.push_back(elapsed(t));
        std::vector<Key> expected;for(size_t j=0;j<n;++j){if(j==pos)expected.push_back(newId);if(ids[j]<=257)expected.push_back(ids[j]);}
        check(count==257&&std::equal(expected.begin(),expected.end(),out.begin()),"post-splice exact sparse oracle");
        Scene remove{{prev,old.at(prev)},{next,old.at(next)},{newId,{0,0,false,{}}}};remove[prev].next=next;remove[next].prev=prev;apply(remove,ids.front(),ids.front());
        check(idx->query({0,0,1,1},out,count,noStop)==Index::Result::Ok,"post-delete sparse query");expected.erase(std::find(expected.begin(),expected.end(),newId));
        check(count==256&&std::equal(expected.begin(),expected.end(),out.begin()),"post-delete exact sparse oracle");
    }
    check(s.builds==buildCount&&s.order.spliceBatches==200&&s.order.inserted==100&&s.order.removed==100&&s.order.rankCacheShifted>0&&s.order.rankCacheDropped>0,"no rebuild for middle edit workload, actual rank cache repair");
    std::sort(timings.begin(),timings.end());std::sort(writes.begin(),writes.end());std::sort(queries.begin(),queries.end());
    std::printf("N=%zu middle_splices=200 rebuilds=0 splice_p50_us=%.3f p95_us=%.3f max_us=%.3f writes_p50=%llu writes_p95=%llu writes_max=%llu post_insert_query_p50_us=%.3f p95_us=%.3f max_us=%.3f disk_peak=%llu\n",
        n,timings[100]/1e3,timings[189]/1e3,timings.back()/1e3,(unsigned long long)writes[100],(unsigned long long)writes[189],(unsigned long long)writes.back(),
        queries[50]/1e3,queries[94]/1e3,queries.back()/1e3,(unsigned long long)s.diskPeak.load());
}
static void semanticFaults(){
    auto d=std::make_unique<Direct>();d->build(700);Work w;
    auto root=d->order.sequenceRoot();auto p=d->file.read(root,w);++p.entries[0].value;d->file.overwrite(root,p);
    bool rejected=false;try{(void)d->order.rank(2,w);}catch(const Failure& f){rejected=f==Failure::Invalid;}check(rejected,"checksummed but wrong subtree count rejected");
    d->build(700);auto loc=d->order.locatorRoot();p=d->file.read(loc,w);++p.entries[0].key;d->file.overwrite(loc,p);
    rejected=false;try{(void)d->order.rank(2,w);}catch(const Failure& f){rejected=f==Failure::Invalid;}check(rejected,"checksummed but wrong locator separator rejected");
    // Verify portable CRC against a separate bitwise implementation over an
    // entire patterned page, not only the standard nine-byte vector.
    std::array<unsigned char,pageBytes> bytes{};std::mt19937_64 rng(13792);for(auto& b:bytes)b=uint8_t(rng());
    uint32_t c=UINT32_MAX;for(auto b:bytes){c^=b;for(int bit=0;bit<8;++bit)c=(c>>1)^((c&1)?0x82F63B78u:0u);}
    check(crcExtend(UINT32_MAX,bytes.data(),bytes.size())==c,"CRC full-page independent bitwise oracle");
    std::puts("PASS semantic parent-count and separator faults, full-page CRC oracle");
}
static void* smallStack(void*){cascades();return nullptr;}
int main(int argc,char** argv){
    if(argc>1&&std::string_view(argv[1])=="faults"){writeFaults();deltaCases();semanticFaults();return 0;}
    if(argc>1&&std::string_view(argv[1])=="scale"){scaleEdits(1000);scaleEdits(10000);scaleEdits(100000);return 0;}
    if(argc>1&&std::string_view(argv[1])=="multi"){multigap(argc>2?std::strtoull(argv[2],nullptr,10):13792);return 0;}
    if(argc>1&&std::string_view(argv[1])=="cascades"){
        pthread_attr_t attr;check(!pthread_attr_init(&attr)&&!pthread_attr_setstacksize(&attr,1024*1024),"1MiB worker stack");pthread_t thread;
        check(!pthread_create(&thread,&attr,smallStack,nullptr)&&!pthread_attr_destroy(&attr)&&!pthread_join(thread,nullptr),"cascades on bounded worker stack");return 0;
    }
    randomTest(argc>1?std::strtoull(argv[1],nullptr,10):13792,argc>2?std::strtoull(argv[2],nullptr,10):2500);
}
