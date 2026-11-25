#include "document/document.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <stdexcept>
using namespace engine::document;
using Clock=std::chrono::steady_clock;
namespace {
void require(bool b) { if(!b) throw std::logic_error("probe operation failed"); }
template<class F> void time(std::vector<double>& out,F f) {
    auto start=Clock::now(); f();
    out.push_back(std::chrono::duration<double,std::nano>(Clock::now()-start).count());
}
void report(const char* name,std::vector<double>& v,int count) {
    std::sort(v.begin(),v.end());
    std::printf("records=%d op=%s n=%zu ns p50/p95/p99/max %.0f %.0f %.0f %.0f\n",count,name,v.size(),
        v[v.size()/2],v[v.size()*95/100],v[v.size()*99/100],v.back());
}
void run(int count) {
    Document doc({1});
    require(doc.grant(1,count+1));
    std::mt19937 rng(13792);
    std::vector<double> add,lookup,del,undo,redo;
    add.reserve(count);
    for(int i=0;i<count;i++) {
        engine::input::RawInputStroke s{{{double(rng()%1048576),double(rng()%1048576)},0,1,0,0}};
        Edit e{Edit::InsertAfter,doc.last(),{std::make_shared<const Ink>(std::move(s),Brush{})}};
        time(add,[&]{require(doc.edit(doc.revision(),{&e,1})==Status::Ok);});
    }
    for(int i=0;i<1024;i++) {
        Id id=1+rng()%count;
        time(lookup,[&]{require(doc.find(id)!=nullptr);});
        Edit e{Edit::Remove,id};
        time(del,[&]{require(doc.edit(doc.revision(),{&e,1})==Status::Ok);});
        time(undo,[&]{require(doc.undo(doc.revision())==Status::Ok);});
        time(redo,[&]{require(doc.redo(doc.revision())==Status::Ok);});
        require(doc.undo(doc.revision())==Status::Ok);
    }
    report("append",add,count); report("lookup",lookup,count); report("delete",del,count);
    report("undo",undo,count); report("redo",redo,count);
    std::printf("records=%d samples=%d history_charged=%zu\n",count,count,doc.historyBytes());
}
}
int main(int argc,char**) {
    for(int n:{1000,10000,100000}) run(n);
    // Opt-in only. Caller must set a host address-space limit and verify available RAM.
    if(argc==2) run(1000000);
}
