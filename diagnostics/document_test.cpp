#include "document/codec.hpp"
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
}
int main() {
    codec(); edits(); oracle();
    std::puts("PASS M03 v1 codec, reversible splices, history, ID ranges, seeded 10000-step ordered oracle");
}
