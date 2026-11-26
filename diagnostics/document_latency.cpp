// Attribution probe, not the throughput baseline. Linux/WSL thread counters are
// sampled outside each edit. Report the slowest operation, not just sorted p99.
#include "document/document.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <stdexcept>
#include <sys/resource.h>
#include <time.h>
using namespace engine::document;
using Clock=std::chrono::steady_clock;
static std::array<Clock::time_point,7> phase;
void ipDocumentPhase(int i) noexcept { phase.at(i)=Clock::now(); }
namespace {
long long cpu() {
    timespec t{};
    if(clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t)) throw std::runtime_error("thread clock");
    return t.tv_sec*1000000000LL+t.tv_nsec;
}
auto now(){return std::chrono::steady_clock::now();}
long long ns(auto duration){return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();}
void lookup(Document& d,std::mt19937& rng) {
    for(int hot=0;hot<=1;hot++) {
        std::array<Id,1024> keys{};
        std::vector<double> batches;
        for(int b=0;b<65;b++) {
            for(auto& id:keys) id=1+rng()%(hot?8:d.size());
            const auto start=now();
            for(auto id:keys) if(!d.find(id)) throw std::runtime_error("missing");
            auto elapsed=ns(now()-start);
            if(b) batches.push_back(double(elapsed)/keys.size());
        }
        std::sort(batches.begin(),batches.end());
        std::printf("size=%zu %s lookup warmed 64x1024 p50/p95 ns/op %.1f/%.1f\n",
                d.size(),hot?"hot8":"uniform",batches[32],batches[60]);
    }
}
}
int main(){
    Document d({1});d.grant(1,1000000);
    std::mt19937 rng(13792),queries(13792);
    long long maximum=0,thread=0;
    int at=0; long faults=0,major=0,involuntary=0,voluntary=0;
    std::array<long long,6> phases{};
    for(int i=1;i<=1000000;i++) {
        engine::input::RawInputStroke raw{{{double(rng()%1048576),double(rng()%1048576)},0,1,0,0}};
        Edit e{Edit::InsertAfter,d.last(),{std::make_shared<const Ink>(std::move(raw),Brush{})}};
        rusage a{},b{};
        getrusage(RUSAGE_THREAD,&a);
        auto c=cpu();auto start=now();
        auto status=d.edit(d.revision(),{&e,1});
        auto wall=ns(now()-start); auto cost=cpu()-c;
        if(status!=Status::Ok) return 1;
        if(wall>maximum) {
            getrusage(RUSAGE_THREAD,&b);
            maximum=wall;thread=cost;at=i;faults=b.ru_minflt-a.ru_minflt;major=b.ru_majflt-a.ru_majflt;
            for(size_t j=0;j<phases.size();j++) phases[j]=ns(phase[j+1]-phase[j]);
            involuntary=b.ru_nivcsw-a.ru_nivcsw;voluntary=b.ru_nvcsw-a.ru_nvcsw;
        }
        if(i==1000 || i==100000 || i==1000000) lookup(d,queries);
    }
    std::printf("worst append index=%d elapsed_ns=%lld thread_cpu_ns=%lld faults_minor/major=%ld/%ld switches_involuntary/voluntary=%ld/%ld\n",
            at,maximum,thread,faults,major,involuntary,voluntary);
    std::printf("worst phases ns vectors=%lld allocate_node=%lld splice=%lld history_slot=%lld evict=%lld publish=%lld\n",
                phases[0],phases[1],phases[2],phases[3],phases[4],phases[5]);
}
