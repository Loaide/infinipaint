// Deterministic correctness, fault and capacity tests for the production
// spatial pages and ordered-delta adapter. No GLES/Android dependencies.
#include "tablet/spatial_delta.hpp"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <numeric>
#include <random>
#include <string_view>
#include <sys/resource.h>
#include <csignal>
using namespace engine::tablet::spatial;
static void check(bool ok,const char* why){if(!ok){std::fprintf(stderr,"FAIL: %s\n",why);std::exit(1);}}
struct Row {Key prev=0,next=0;bool shape=true;Box box{};};
using Scene=std::map<Key,Row>;
static Box boxOf(const Row& r){return r.box;}
static auto noStop=[](){return false;};
struct Fixture {
    Stats stats;
    std::unique_ptr<Index> index;
    Scene scene;std::vector<Key> order;
    std::vector<Key> output=std::vector<Key>(65536);
    uint64_t comparisons=0,fastEdits=0,rebuilds=0;
    explicit Fixture(uint64_t ram=ramLimit,uint64_t disk=diskLimit):index(std::make_unique<Index>(stats,nullptr,ram,disk)){}
    Key head()const{return order.empty()?0:order.front();}
    void links(){for(size_t i=0;i<order.size();++i){auto& r=scene.at(order[i]);r.prev=i?order[i-1]:0;r.next=i+1<order.size()?order[i+1]:0;}}
    bool rebuild(const char* directory="/tmp") {
        const bool ok=index->rebuild(directory,order.size(),[&](auto emit){for(Key id:order)emit(id,scene.at(id).box);return true;},noStop);
        if(ok)++rebuilds;
        return ok;
    }
    void compare(Window w) {
        std::vector<Key> expected;
        for(Key id:order)if(w.hits(scene.at(id).box))expected.push_back(id);
        size_t count=999;
        auto result=index->query(w,output,count,noStop);
        if(expected.size()>output.size())check(result==Index::Result::Overflow&&count==0,"overflow must not publish partial IDs");
        else check(result==Index::Result::Ok&&count==expected.size()&&std::equal(expected.begin(),expected.end(),output.begin()),"exact ordered oracle");
        ++comparisons;
    }
    void transition(Scene prior,Key priorHead) {
        links();Scene patch;
        for(const auto&[id,r]:prior)if(!scene.contains(id))patch[id]={0,0,false,{}};
        for(const auto&[id,r]:scene) {
            auto old=prior.find(id);
            if(old==prior.end()||old->second.prev!=r.prev||old->second.next!=r.next||old->second.box!=r.box)patch[id]=r;
        }
        if(updateOrdered(*index,prior,patch,priorHead,head(),boxOf))++fastEdits;
        else check(rebuild(),"rebuild after structural/overlay change");
        check(index->count()==scene.size()&&index->tail()==(order.empty()?0:order.back()),"index order metadata");
        check(stats.diskBytes<=diskLimit&&stats.dynamicBytes<=ramLimit&&stats.overlayRows<=overlayLimit,"bounded ownership");
    }
};
static Box randomBox(std::mt19937_64& rng){
    float x=float(int(rng()%40000)-20000)/4,y=float(int(rng()%40000)-20000)/4;
    const float w=rng()%9==0?4000.f:float(rng()%300)/16,h=rng()%11==0?4000.f:float(rng()%300)/16;
    return {x,y,x+w,y+h};
}
static void randomized(uint64_t seed,size_t edits){
    Fixture f;std::mt19937_64 rng(seed);Key next=1;
    // Initial paint order is deliberately unrelated to stable numeric IDs.
    for(size_t i=0;i<777;++i){f.order.push_back(next);f.scene[next++].box=randomBox(rng);}
    std::shuffle(f.order.begin(),f.order.end(),rng);f.links();check(f.rebuild(),"initial STR and B+ pages");
    f.compare({-1e20,-1e20,1e20,1e20});
    std::vector<std::pair<Key,Box>> removed;
    for(size_t i=0;i<edits;++i){
        auto old=f.scene;auto head=f.head();auto op=rng()%12;
        if(f.order.empty())op=0;
        if(op<=3){ // append, including coalesced suffix
            size_t n=1+rng()%4;
            while(n--){auto id=next++;f.scene[id].box=randomBox(rng);f.order.push_back(id);}
        }else if(op<=5){ // deletion of one or several consecutive strokes
            auto start=size_t(rng()%f.order.size()),n=std::min<size_t>(1+rng()%5,f.order.size()-start);
            for(size_t j=0;j<n;++j){auto id=f.order[start+j];removed.push_back({id,f.scene.at(id).box});f.scene.erase(id);}
            f.order.erase(f.order.begin()+start,f.order.begin()+start+n);
        }else if(op<=8){ // move existing geometry
            auto id=f.order[rng()%f.order.size()];f.scene[id].box=randomBox(rng);
        }else if(op==9){ // middle insert / restored ID
            Key id;Box b;
            if(!removed.empty()&&(rng()&1)){id=removed.back().first;b=removed.back().second;removed.pop_back();}
            else{id=next++;b=randomBox(rng);}
            f.order.insert(f.order.begin()+rng()%(f.order.size()+1),id);f.scene[id].box=b;
        }else if(op==10){ // coalesced deletion + append in same revision
            auto id=f.order.back();removed.push_back({id,f.scene.at(id).box});f.scene.erase(id);f.order.pop_back();
            f.order.push_back(next);f.scene[next++].box=randomBox(rng);
        }else{ // topology unchanged, style-only analogue
        }
        f.transition(std::move(old),head);
        auto b=randomBox(rng);f.compare({b.minX,b.minY,b.maxX,b.maxY});
        // Exactly touching zero-area query and next representable coordinate.
        if(!f.order.empty()){
            auto e=f.scene.at(f.order[rng()%f.order.size()]).box;
            f.compare({e.maxX,e.maxY,e.maxX,e.maxY});
            const auto x=std::nextafter(double(e.maxX),std::numeric_limits<double>::infinity());
            f.compare({x,e.minY,x,e.maxY});
        }
        if(i%37==0)f.compare({-1e20,-1e20,1e20,1e20});
    }
    check(f.fastEdits>edits/2,"incremental changes must not degenerate to rebuilds");
    check(f.stats.invalid==0&&f.stats.ioFaults==0&&f.stats.capacity==0,"no hidden failures");
    std::printf("seed=%llu edits=%zu ordered_oracle=%llu fast=%llu rebuilds=%llu compactions=%llu peak_disk=%llu peak_dynamic=%llu\n",
        (unsigned long long)seed,edits,(unsigned long long)f.comparisons,(unsigned long long)f.fastEdits,
        (unsigned long long)f.rebuilds,(unsigned long long)f.stats.compactions.load(),(unsigned long long)f.stats.diskPeak.load(),(unsigned long long)f.stats.dynamicPeak.load());
    f.index->reset();check(f.stats.diskBytes==0&&f.stats.dynamicBytes==0,"all dynamic charges released after reset");
}
static bool buildPlain(Index& index,size_t n,const char* dir="/tmp"){
    return index.rebuild(dir,n,[&](auto emit){for(size_t i=0;i<n;++i){float x=float(i*4);emit(Key(i+1),{x,0,x+2,2});}return true;},noStop);
}
static int allocFailAt=-1,allocCalls=0;
static bool testAdmission(size_t){return allocCalls++!=allocFailAt;}
static void faults(){
    Stats s;auto i=std::make_unique<Index>(s);std::array<Key,1024> out{};size_t count=1;
    check(buildPlain(*i,700),"fault fixture build");
    i->corruptSpatialRoot();check(i->query({-1,-1,100,3},out,count,noStop)==Index::Result::Unavailable&&count==0&&!i->ready(),"checksum rejects corrupted root");
    check(s.invalid==1&&s.diskBytes==0&&s.dynamicBytes==0,"corruption charge cleanup");
    check(buildPlain(*i,700),"rebuild after checksum fault");
    i->corruptOrderRoot();check(i->query({-1,-1,100,3},out,count,noStop)==Index::Result::Unavailable&&s.invalid==2&&count==0&&!i->ready(),"order page fault rejects entire plan");
    check(buildPlain(*i,700),"rebuild after order fault");i->truncatePages();
    check(i->query({-1,-1,100,3},out,count,noStop)==Index::Result::Unavailable&&s.ioFaults==1&&count==0&&!i->ready(),"short read rejects whole query");
    check(!buildPlain(*i,700,"/this/path/must/not/exist")&&s.ioFaults==2&&s.dynamicBytes==0,"unavailable scratch directory");
    check(buildPlain(*i,700),"rebuild after IO fault");
    auto stop=[](){return true;};
    check(i->query({-1,-1,1e10,10},out,count,stop)==Index::Result::Cancelled&&count==0&&i->ready(),"cancel does not invalidate roots");
    unsigned polls=0;auto during=[&]{return ++polls>4;};
    check(i->query({-1,-1,1e10,10},out,count,during)==Index::Result::Cancelled&&count==0&&i->ready(),"mid-query cancellation");
    std::array<Key,7> tiny{};
    check(i->query({-1,-1,1e10,10},tiny,count,noStop)==Index::Result::Overflow&&count==0&&i->ready(),"output overflow retains valid roots, zero visible result");
    polls=0;check(!i->rebuild("/tmp",700,[&](auto emit){for(int n=0;n<700;++n)emit(n+1,{0,0,1,1});return true;},during)&&s.dynamicBytes==0&&s.diskBytes==0,"cancelled rebuild releases all charges");
    check(!i->rebuild("/tmp",2,[](auto emit){emit(1,{0,0,1,1});emit(1,{0,0,1,1});return true;},noStop),"duplicate IDs reject rebuild");
    check(!i->rebuild("/tmp",1,[](auto emit){emit(1,{0,0,NAN,1});return true;},noStop),"nonfinite bounds reject rebuild");
    check(!i->rebuild("/tmp",2,[](auto emit){emit(1,{0,0,1,1});return true;},noStop),"incomplete enumeration rejects rebuild");
    check(buildPlain(*i,700),"rebuild after validation faults");
    check(i->query({0,0,0,0},out,count,noStop)==Index::Result::Ok&&count==1&&out[0]==1,"inclusive query edge");
    i->reset();check(s.diskBytes==0&&s.dynamicBytes==0,"final fault fixture cleanup");
    Stats low;auto small=std::make_unique<Index>(low,nullptr,1024,64ull<<20);
    check(!buildPlain(*small,700)&&low.capacity==1&&low.dynamicBytes==0&&low.diskBytes==0,"RAM admission rejects full build");
    Stats disk;auto smallDisk=std::make_unique<Index>(disk,nullptr,ramLimit,2*pageBytes);
    check(!buildPlain(*smallDisk,700)&&disk.capacity==1&&disk.dynamicBytes==0&&disk.diskBytes==0,"disk admission rejects full build before writes");
    // Sweep each charged dynamic allocation, including parent vectors after
    // the file has been written. A failed generation must leave no credit.
    int failures=0;for(int n=0;n<32;++n){
        Stats a;auto candidate=std::make_unique<Index>(a,testAdmission);allocCalls=0;allocFailAt=n;
        if(buildPlain(*candidate,4097)){check(failures==n,"complete allocation sweep");break;}
        ++failures;check(!candidate->ready()&&a.dynamicBytes==0&&a.diskBytes==0&&a.capacity==1,"allocation failure rollback");
    }
    check(failures>=4&&failures<32,"multiple charged allocation sites swept");allocFailAt=-1;
    // Actual kernel write failure after a partial generation. Process-local
    // file size limit; restored immediately, never touches canonical files.
    struct rlimit saved{},limited{};check(getrlimit(RLIMIT_FSIZE,&saved)==0,"file size limit read");
    limited=saved;limited.rlim_cur=pageBytes+32;
    auto oldSignal=std::signal(SIGXFSZ,SIG_IGN);check(oldSignal!=SIG_ERR&&setrlimit(RLIMIT_FSIZE,&limited)==0,"file write fault setup");
    const auto priorIo=s.ioFaults.load();bool writeOk=buildPlain(*i,700);
    check(setrlimit(RLIMIT_FSIZE,&saved)==0&&std::signal(SIGXFSZ,oldSignal)!=SIG_ERR,"restore process limits");
    check(!writeOk&&s.ioFaults==priorIo+1&&s.diskBytes==0&&s.dynamicBytes==0,"partial write fault releases roots and charges");
    check(buildPlain(*i,700),"recovery after partial write");i->reset();
    std::printf("PASS %d allocation failure points and real partial-write fault\n",failures);
    // Overlay cap forces paired-root rebuild; deletion masks old base IDs.
    Fixture f;for(int n=1;n<=400;++n){f.order.push_back(n);f.scene[n].box={float(n),0,float(n+1),1};}f.links();check(f.rebuild(),"overlay base");
    for(int n=1;n<=257;++n){auto old=f.scene;auto head=f.head();f.scene.erase(n);f.order.erase(f.order.begin());f.transition(std::move(old),head);}
    check(f.stats.compactions==1&&f.fastEdits==256,"exact overlay compaction breakpoint");f.compare({-1,-1,10000,10000});
    // An incomplete delta must not be treated as a valid append/deletion.
    Scene incomplete{{f.order[10],{0,0,false,{}}}};
    check(!updateOrdered(*f.index,f.scene,incomplete,f.head(),f.head(),boxOf),"reject missing reciprocal deletion patch");
    f.compare({-1,-1,10000,10000});
    std::puts("PASS fault injection, cancellation, capacity, overlay compaction, incomplete delta");
}
static void dense(){
    Stats s;auto index=std::make_unique<Index>(s);std::vector<Key> out(65536);size_t count=0;
    check(index->rebuild("/tmp",65537,[](auto emit){for(Key id=65537;id;--id)emit(id,{0,0,1,1});return true;},noStop),"dense overflow base");
    check(index->query({0,0,1,1},out,count,noStop)==Index::Result::Overflow&&count==0&&index->ready(),"actual planner-limit overflow never truncates");
    std::vector<Key> bigger(65537);
    check(index->query({0,0,1,1},bigger,count,noStop)==Index::Result::Ok&&count==65537,"dense complete result when capacity sufficient");
    for(size_t i=0;i<count;++i)check(bigger[i]==Key(count-i),"dense exact reversed paint order");
    index->reset();check(s.diskBytes==0&&s.dynamicBytes==0,"dense cleanup");
    std::puts("PASS dense 65,537 overlaps: strict overflow and complete ordered control");
}
static void scale(size_t n){
    Stats s;auto index=std::make_unique<Index>(s);std::vector<Key> ids(1024);size_t count=0;
    // Fixed 256 visible records, with keys distributed over the full ID
    // space and paint order unrelated to spatial order. Positions of the
    // first 256 records are identical for all N, remaining rows are distant.
    const auto start=Clock::now();
    check(index->rebuild("/tmp",n,[&](auto emit){
        for(size_t j=0;j<n;++j){size_t i=(j*7919)%n; // gcd(7919, 1000/10000/100000)==1
            float x=i<256?float(i%16)*4:1000+float(i%1000)*16,y=i<256?float(i/16)*4:1000+float(i/1000)*16;
            emit(Key((i*3571)%n+1),{x,y,x+2,y+2});}return true;
    },noStop),"scale build");
    auto build=elapsed(start);std::vector<Key> expected;
    for(size_t j=0;j<n;++j){size_t i=(j*7919)%n;if(i<256)expected.push_back(Key((i*3571)%n+1));}
    std::vector<uint64_t> times;auto before=s.snapshot();
    for(int q=0;q<300;++q){auto t=Clock::now();check(index->query({0,0,62,62},ids,count,noStop)==Index::Result::Ok,"scale query");times.push_back(elapsed(t));check(count==256&&std::equal(expected.begin(),expected.end(),ids.begin()),"scale exact order");}
    auto after=s.snapshot();const auto cold=times.front();std::sort(times.begin(),times.end());
    std::printf("N=%zu k=%zu build_ms=%.3f first_query_us=%.3f p50_us=%.3f p95_us=%.3f max_us=%.3f spatial_pages/query=%.2f bounds/query=%.2f reads/query=%.3f rank_lookups/query=%.2f disk=%llu dynamic_peak=%llu\n",
        n,count,build/1e6,cold/1e3,times[150]/1e3,times[284]/1e3,times.back()/1e3,(after[17]-before[17])/300.,(after[18]-before[18])/300.,(after[15]-before[15])/300.,(after[22]-before[22])/300.,(unsigned long long)s.diskBytes.load(),(unsigned long long)s.dynamicPeak.load());
    check(after[18]-before[18]<n*300,"fixed k sparse bounds pruning");
    index->reset();check(s.dynamicBytes==0&&s.diskBytes==0,"scale retirement");
}
int main(int argc,char**argv){
    if(argc>1&&std::string_view(argv[1])=="scale"){scale(1000);scale(10000);scale(100000);return 0;}
    if(argc>1&&std::string_view(argv[1])=="faults"){faults();dense();return 0;}
    const uint64_t seed=argc>1?std::strtoull(argv[1],nullptr,10):13792;
    randomized(seed,argc>2?std::strtoull(argv[2],nullptr,10):1200);
    if(argc==1){faults();dense();scale(1000);scale(10000);scale(100000);}
}
