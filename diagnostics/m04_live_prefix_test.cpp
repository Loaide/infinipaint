#include "tablet/live_prefix.hpp"
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <future>
#include <numeric>
#include <thread>

namespace {
std::atomic<size_t> charge{0};
int remaining=-1; // writer only
struct Budget {
    size_t bytes;
    explicit Budget(size_t n):bytes(n) {
        if(remaining>=0 && remaining--==0) throw std::bad_alloc{};
        charge.fetch_add(n);
    }
    ~Budget(){charge.fetch_sub(bytes);}
    Budget(const Budget&)=delete;
};
void check(bool ok,const char* msg){if(!ok){std::fprintf(stderr,"FAIL: %s\n",msg);std::exit(1);}}
using Prefix=engine::tablet::LivePrefix<uint64_t,Budget,8>;
void oracle(const Prefix::View& v,size_t start=0) {
    size_t seen=start;
    v.visit(start,[&](size_t offset,std::span<const uint64_t> values){
        check(offset==seen,"gap or overlap");
        for(auto value:values) check(value==++seen,"published prefix changed");
    });
    check(seen==v.size(),"truncated view");
}
}
int main() {
    Prefix::View old;
    {
        Prefix p;std::vector<uint64_t> source(5);std::iota(source.begin(),source.end(),uint64_t(1));
        p.reserve(source.size());p.extend(source);old=p.view();
        // Failure at every allocation in a multi-group growth. Check both
        // previously published bytes and exact charge rollback.
        const auto before=charge.load();
        int failures=0;
        for(int n=0;n<150;++n) {
            remaining=n;
            try {p.reserve(1024);remaining=-1;break;}
            catch(const std::bad_alloc&) {remaining=-1;++failures;check(charge.load()==before,"failed reserve leaked credit");oracle(p.view());}
        }
        check(failures>128,"did not exercise page/group allocation failures");
        std::promise<void> ready,done;
        auto start=ready.get_future();auto stop=done.get_future();
        std::thread reader([&]{start.wait();do {oracle(old);oracle(old,3);} while(stop.wait_for(std::chrono::microseconds(0))!=std::future_status::ready);});
        ready.set_value();
        for(size_t n=6;n<=8193;++n) {
            p.reserve(1);source.push_back(n);p.extend(source);
            if(n%127==0) oracle(p.view(),n-50);
        }
        done.set_value();reader.join();
        oracle(p.view());oracle(old);
    }
    check(charge.load()>0,"reader's old page not charged");
    old={};check(charge.load()==0,"final release leaked page/group credit");
    std::puts("PASS live prefix: 8193 values, concurrent frozen count, group rollover, allocation rollback, final-owner credit");
}
