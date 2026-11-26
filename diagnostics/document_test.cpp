#include "document/codec.hpp"
#include "document/change_queue.hpp"
#include "document/eraser.hpp"
#include <cassert>
#include <cstdio>
#include <random>
#include <algorithm>
#include <bit>

using namespace engine::document;
namespace {
Value dot() {
    engine::input::RawInputStroke s{{{0,0},0,1,0,0}};
    return {std::make_shared<const Ink>(std::move(s),Brush{})};
}
void codec() {
    Brush b; b.tool=2;
    wire::Header h;
    assert(wire::header(b,1,-0.,h));
    assert(h[0]==1 && h[4]==1 && h[16]==2 && h.back()==128);
    Brush restored; double dx,dy;
    assert(wire::readHeader(h,restored,dx,dy) && dx==1 && std::signbit(dy));
    wire::Header again;
    assert(wire::header(restored,dx,dy,again) && h==again);
    h[8]=2; assert(!wire::readHeader(h,restored,dx,dy));
    engine::input::RawInputStroke samples{{{1,-0.},1,0,1,-1},{{1,-0.},1,.9f,2,-2}};
    auto out=samples;
    std::array<uint8_t,72> bytes{},roundtrip{};
    assert(wire::samples(samples,bytes) && wire::readSamples(bytes,-1,out));
    assert(wire::samples(out,roundtrip) && bytes==roundtrip);
    assert(!wire::readSamples(bytes,2,out));
    std::fill(bytes.begin()+60,bytes.begin()+64,255); // second pressure NaN
    assert(!wire::readSamples(bytes,1,out));
    assert(!wire::readSamples(std::span(bytes).first(71),1,out));
}
void edits() {
    const auto v=dot();
    Document d({1});
    assert(d.grant(500,1000));
    Edit add{Edit::InsertAfter,0,v};
    assert(d.edit(0,{&add,1})==Status::Ok);
    Edit group[]={{Edit::Remove,500},{Edit::InsertAfter,0,v},{Edit::Replace,999,v}};
    assert(d.edit(1,group)==Status::Invalid && d.revision()==1 && d.find(500));
    Edit split[]={{Edit::Remove,500},{Edit::InsertAfter,0,v},{Edit::InsertAfter,501,v}};
    assert(d.edit(1,split)==Status::Ok && d.first()==501 && d.last()==502);
    assert(d.undo(2)==Status::Ok && d.first()==500 && d.last()==500);
    assert(d.redo(3)==Status::Ok && !d.find(500) && d.find(502)->prev==501);
    auto translated=v; translated.dx=-11;
    Edit move{Edit::Replace,501,translated};
    assert(d.edit(4,{&move,1})==Status::Ok && d.find(501)->geometry==5 && d.find(502)->geometry==4);
    assert(d.clear(5)==Status::Ok && !d.first());
    assert(d.undo(6)==Status::Ok && d.find(501)->value.dx==-11);
    assert(d.undo(7)==Status::Ok && d.find(501)->value.dx==0);
    assert(d.edit(8,{&add,1})==Status::Ok && d.first()==503 && d.size()==3);
    assert(d.redo(9)==Status::Empty);
    Document bounded({2},{1,1024});
    assert(bounded.grant(1,2));
    assert(bounded.edit(0,{&add,1})==Status::Ok);
    assert(bounded.edit(1,{&add,1})==Status::Ok);
    assert(bounded.edit(2,{&add,1})==Status::Exhausted);
    assert(bounded.undo(2)==Status::Ok && bounded.undo(3)==Status::Empty);
    assert(!bounded.grant(2,4) && bounded.grant(4,10));
}
void oracle() {
    Document d({1},{10000,64u<<20});
    assert(d.grant(1,10000));
    std::vector<std::vector<Id>> states(1);
    size_t cursor=0; Id issued=0;
    std::mt19937 rng(13792);
    auto v=dot();
    for(int i=0;i<10000;i++) {
        auto snapshot=states[cursor];
        auto kind=rng()%6;
        if(kind==0 && cursor) {
            assert(d.undo(d.revision())==Status::Ok); --cursor;
        } else if(kind==1 && cursor+1<states.size()) {
            assert(d.redo(d.revision())==Status::Ok); ++cursor;
        } else {
            if(kind==2 && !snapshot.empty()) {
                assert(d.clear(d.revision())==Status::Ok); snapshot.clear();
            } else if(kind==3 && !snapshot.empty()) {
                size_t pos=rng()%snapshot.size();
                Edit e{Edit::Remove,snapshot[pos]};
                assert(d.edit(d.revision(),{&e,1})==Status::Ok); snapshot.erase(snapshot.begin()+pos);
            } else {
                size_t pos=rng()%(snapshot.size()+1);
                Edit e{Edit::InsertAfter,pos?snapshot[pos-1]:0,v};
                assert(d.edit(d.revision(),{&e,1})==Status::Ok);
                snapshot.insert(snapshot.begin()+pos,++issued);
            }
            states.resize(++cursor); states.push_back(std::move(snapshot));
        }
        Id prev=0, id=d.first();
        for(auto expected:states[cursor]) {
            assert(id==expected);
            auto r=d.find(id);
            assert(r && r->prev==prev && r->value.ink==v.ink); prev=id; id=r->next;
        }
        assert(!id && prev==d.last() && d.size()==states[cursor].size());
    }
}

void saturation() {
    Document d({2});
    ChangeQueue q;
    d.observe(&q);
    assert(d.grant(1,1000));
    Edit e{Edit::InsertAfter,0,dot()};
    assert(q.admitStroke() && !q.admitStroke() && !q.growStroke(SIZE_MAX));
    assert(q.growStroke(sizeof(engine::input::RawInputPoint)));
    for(int i=0;i<31;i++) assert(d.edit(d.revision(),{&e,1})==Status::Ok);
    assert(d.edit(d.revision(),{&e,1})==Status::QueueLimit && d.size()==31 && q.size()==31);
    q.committingStroke(true);
    assert(d.edit(d.revision(),{&e,1})==Status::Ok && q.size()==32);
    q.committingStroke(false);
    auto stale=q.front();
    assert(stale->base==0 && !q.admitStroke());
    assert(d.undo(d.revision())==Status::QueueLimit && d.size()==32);
    assert(q.acknowledge(stale) && !q.acknowledge(stale));
    assert(d.undo(d.revision())==Status::Ok && d.size()==31);
    while(auto token=q.front()) assert(q.acknowledge(token));
    assert(!q.bytes() && q.admitStroke()); q.cancelStroke(); assert(!q.bytes());
}
void clipping() {
    // Two separated dots crossed in one motion; exact same topmost footprint is
    // not erased through. A miss, taper and zero-length eraser remain defined.
    std::map<double,double> seen;
    auto sweep=[&](double x,double radius) {
        return clipInk({0,0},{20,0},{x,0},{x,0},radius,radius);
    };
    assert(expose(seen,sweep(15,1)) && expose(seen,sweep(5,1)) && !expose(seen,sweep(15,.5)));
    auto taper=clipInk({5,-10},{5,10},{0,0},{10,0},1,3);
    assert(taper.lo<.41 && taper.hi>.59 && taper.lo>.38 && taper.hi<.62);
    assert(clipInk({0,0},{0,0},{0,0},{2,0},1,1).hi==1);
    assert(clipInk({0,5},{2,5},{0,0},{2,0},1,1).hi==0);
}
void contentAndLimits() {
    Document d({3},{2,4096});
    ChangeQueue q;d.observe(&q);assert(d.grant(1,100));
    auto value=dot();
    Edit a{Edit::InsertAfter,0,value};
    assert(d.edit(0,{&a,1})==Status::Ok);q.acknowledge(q.front());
    auto styled=value;styled.tint=std::array<float,4>{1,0,0,.4f};
    Edit r{Edit::Replace,1,styled};
    assert(d.edit(1,{&r,1})==Status::Ok && d.find(1)->geometry==1 && d.find(1)->content==2);
    assert(q.front()->rows[0].geometry==1 && q.front()->rows[0].value.rgba()[3]==.4f);
    assert(d.undo(2)==Status::Ok && d.find(1)->geometry==1 && d.find(1)->value.rgba()==value.rgba());
    assert(d.redo(3)==Status::Ok && d.find(1)->geometry==1 && d.find(1)->value.rgba()==styled.rgba());
    while(q.front()) q.acknowledge(q.front());
    for(int i=0;i<16;i++) assert(d.edit(d.revision(),{&a,1})==Status::Ok);
    auto rev=d.revision();auto last=d.last(),first=d.first();auto bytes=d.canonicalBytes();
    assert(d.clear(rev)==Status::HistoryLimit && rev==d.revision() && d.size()==17 &&
           last==d.last() && first==d.first() && bytes==d.canonicalBytes());
}
void tickets() {
    Document d({7});ChangeQueue q;
    d.observe(&q);assert(d.grant(1,8));
    auto v=dot();
    assert(d.reserveStroke()==Status::Ok && d.reserveStroke()==Status::Conflict);
    assert(d.revision()==0 && d.size()==0 && !d.acceptsStrokeCapacity(SIZE_MAX));
    assert(d.clear(0)==Status::Conflict);
    d.cancelStroke();assert(d.commitStroke(v)==Status::Conflict);
    assert(d.reserveStroke()==Status::Ok && q.admitStroke() && q.growStroke(sizeof(engine::input::RawInputPoint)));
    q.committingStroke(true);
    assert(d.commitStroke(v)==Status::Ok && d.commitStroke(v)==Status::Conflict);
    q.committingStroke(false);
    assert(d.first()==2 && d.last()==2 && d.revision()==1 && q.front()->tail==2);
    assert(d.undo(1)==Status::Ok && d.reserveStroke()==Status::Ok);
    d.cancelStroke();assert(d.redo(2)==Status::Ok && d.first()==2);
}
}
int main() {
    codec(); edits(); oracle(); saturation(); clipping();contentAndLimits();tickets();
    std::puts("PASS M03 v1 codec, reversible splices, history, ID ranges, seeded 10000-step ordered oracle, queue reserve/saturation/retry");
}
