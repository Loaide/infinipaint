#include "tablet/frame_scheduler.hpp"
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <memory>
#include <vector>
#include <chrono>
using namespace engine::tablet;
static void check(bool b,const char* m){if(!b){std::fprintf(stderr,"FAIL %s\n",m);std::abort();}}
int main(){
    SpscRing<std::unique_ptr<int>,8> ring;
    for(int i=0;i<8;i++)check(ring.push(std::make_unique<int>(i)),"fill ring");
    auto rejected=std::make_unique<int>(99);check(!ring.push(std::move(rejected)) && rejected && *rejected==99,"full preserves owner");
    for(int i=0;i<8;i++){auto got=ring.pop();check(got && **got==i,"ordered full drain");}check(!ring.pop(),"empty");
    constexpr int n=200000;
    std::thread producer([&]{for(int i=0;i<n;i++){auto p=std::make_unique<int>(i);while(!ring.push(std::move(p)))std::this_thread::yield();}});
    for(int i=0;i<n;i++){auto v=ring.pop();while(!v){std::this_thread::yield();v=ring.pop();}check(**v==i,"concurrent ring order");}
    producer.join();
    struct Packet{std::vector<int> v;};SpscSlot<Packet> slot;Packet p{{1,2,3}},out;
    check(slot.push([&](Packet& s){s.v.swap(p.v);}),"slot move");check(p.v.empty(),"producer released");
    check(!slot.push([&](Packet&){std::abort();}),"slot backpressure");
    check(slot.pop([&](Packet& s){s.v.swap(out.v);}) && out.v==std::vector<int>({1,2,3}),"slot exact data");
    struct Snapshot {uint64_t seq=0;std::array<uint64_t,64> words{};};LatestTriple<Snapshot> latest;
    std::atomic<bool> done=false;
    std::thread writer([&]{for(uint64_t i=1;i<=n;++i){auto& s=latest.write();s.seq=i;s.words.fill(i);latest.publish();}done.store(true);});
    uint64_t previous=0;size_t reads=0;
    do {const auto& s=latest.latch();check(s.seq>=previous,"latest monotonic");for(auto w:s.words)check(w==s.seq,"no torn plan");previous=s.seq;++reads;}while(!done.load() || previous!=n);
    writer.join();check(reads>0,"read plans");
    WakeGate gate;std::atomic<int> generation=0;std::atomic<bool> finished=false;
    std::thread consumer([&]{int last=0;while(last<n){gate.wait();int now=generation.load();check(now>=last,"wake ordering");last=now;}finished=true;});
    for(int i=1;i<=n;i++){generation.store(i);gate.signal();}
    consumer.join();check(finished,"latest wake delivered");
    std::puts("PASS scheduler: 200000 ordered moves, full ownership, 200000 latest publications, coalesced wake delivery");
}
