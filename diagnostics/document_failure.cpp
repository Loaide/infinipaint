#include "document/change_queue.hpp"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <new>

static long fail=-1;
void* operator new(size_t size) {
    if(fail>=0 && fail--==0) throw std::bad_alloc();
    if(void* p=std::malloc(size?size:1)) return p;
    throw std::bad_alloc();
}
[[gnu::noinline]] void operator delete(void* p) noexcept {std::free(p);}
[[gnu::noinline]] void operator delete(void* p,size_t) noexcept {std::free(p);}

int main() {
    using namespace engine::document;
    auto v=Value{std::make_shared<const Ink>(engine::input::RawInputStroke{{{1,1},1,1,0,0}},Brush{})};
    size_t failures=0;
    for(long allocation=0;allocation<100;allocation++) {
        Document d({1});
        ChangeQueue q;
        d.observe(&q);assert(d.grant(1,10));
        Edit add{Edit::InsertAfter,0,v};
        assert(d.edit(0,{&add,1})==Status::Ok);
        assert(q.acknowledge(q.front()));
        const auto retained=d.canonicalBytes(),history=d.historyBytes();
        Edit replace[]={{Edit::Remove,1},{Edit::InsertAfter,0,v},{Edit::InsertAfter,2,v}};
        fail=allocation;
        Status status=d.edit(1,replace);
        fail=-1;
        if(status==Status::Ok) break;
        assert(status==Status::Memory && d.first()==1 && d.last()==1 && d.revision()==1 && d.size()==1);
        assert(!q.size() && d.canonicalBytes()==retained && d.historyBytes()==history && !d.find(2));
        assert(d.edit(1,replace)==Status::Ok && d.first()==2 && d.last()==3);
        failures++;
    }
    assert(failures>=5 && failures<100);
    std::printf("PASS grouped edit: %zu injected allocation failures leave membership, IDs, revision, queue and budgets retryable\n",failures);
}
