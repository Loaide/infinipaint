#include "tablet/session.hpp"
#include "document/codec.hpp"
#include <jni.h>
#include <stdexcept>

namespace {
using namespace engine;
using namespace document;
// Confined to the single Java executor (not Activity lifetime).
std::unique_ptr<Document> candidate;
std::shared_ptr<const ChangeSet> saving;
Revision revision;
Id ceiling;
size_t expected;
struct Incoming {
    Brush brush;
    double dx,dy;
    Id id,prev;
    Revision content,geometry;
    input::RawInputStroke samples;
    size_t used;
};
std::unique_ptr<Incoming> incoming;
void require(bool ok) {if(!ok) throw std::invalid_argument("Invalid or stale document transfer");}
template<class F> auto guarded(JNIEnv* env,F f) -> decltype(f()) {
    try{return f();}
    catch(const std::exception& e) {
        if(!env->ExceptionCheck()) env->ThrowNew(env->FindClass("java/lang/IllegalStateException"),e.what());
        return {};
    }
}
template<size_t N> jlongArray longs(JNIEnv* e,const std::array<jlong,N>& values) {
    auto a=e->NewLongArray(N); if(a) e->SetLongArrayRegion(a,0,N,values.data());
    return a;
}
template<class T> jbyteArray bytes(JNIEnv* e,const T& data) {
    auto a=e->NewByteArray(static_cast<jsize>(data.size()));
    if(a) e->SetByteArrayRegion(a,0,static_cast<jsize>(data.size()),reinterpret_cast<const jbyte*>(data.data()));
    return a;
}
Uuid uuid(jlong hi,jlong lo) {
    Uuid result{};
    for(size_t i=0;i<8;i++) { result[7-i]=uint64_t(hi)>>(8*i); result[15-i]=uint64_t(lo)>>(8*i); }
    return result;
}
}
#define STORE(name) Java_com_infinipaint_storage_StorePort_##name
extern "C" {
JNIEXPORT void JNICALL STORE(begin)(JNIEnv* e,jclass,jlong hi,jlong lo,jlong rev,jlong high,jlong count,jlong slice) {
    guarded(e,[&]{require(rev>=0 && high>=0 && count>=0 && uint64_t(count)<=SIZE_MAX);
        require(slice>0 && slice<=(jlong(384)<<20));tablet::setPhysicalSlice(size_t(slice));
        require(tablet::admitIncoming(size_t(count),0));
        candidate=std::make_unique<Document>(uuid(hi,lo)); incoming.reset();
        revision=rev;ceiling=high;expected=size_t(count);return true;});
}
JNIEXPORT void JNICALL STORE(stroke)(JNIEnv* e,jclass,jlong id,jlong prev,jlong content,jlong geometry,jint count,jbyteArray header) {
    guarded(e,[&]{require(candidate && !incoming && count>0 && count<=1000000 && header &&
                           e->GetArrayLength(header)==wire::headerBytes);
        auto s=std::make_unique<Incoming>(); wire::Header h;
        require(tablet::admitIncoming(expected,size_t(count)*sizeof(input::RawInputPoint)));
        e->GetByteArrayRegion(header,0,h.size(),reinterpret_cast<jbyte*>(h.data()));
        require(!e->ExceptionCheck() && wire::readHeader(h,s->brush,s->dx,s->dy));
        s->id=id;s->prev=prev;s->content=content;s->geometry=geometry;s->used=0;
        s->samples.resize(count); incoming=std::move(s);return true;});
}
JNIEXPORT void JNICALL STORE(chunk)(JNIEnv* e,jclass,jbyteArray array) {
    guarded(e,[&]{require(incoming && array);
        auto n=e->GetArrayLength(array);
        require(n>0 && n<=int(wire::chunkSamples*wire::sampleBytes) && n%wire::sampleBytes==0);
        auto count=n/wire::sampleBytes; auto& s=*incoming;
        require(count<=s.samples.size()-s.used);
        std::vector<uint8_t> buf(n);
        e->GetByteArrayRegion(array,0,n,reinterpret_cast<jbyte*>(buf.data()));
        require(!e->ExceptionCheck() && wire::readSamples(buf,s.used?s.samples[s.used-1].timestamp:-1,
                                                          std::span(s.samples).subspan(s.used,count)));
        s.used+=count;return true;});
}
JNIEXPORT void JNICALL STORE(endStroke)(JNIEnv* e,jclass) {
    guarded(e,[&]{require(incoming && incoming->used==incoming->samples.size()); auto s=std::move(incoming);
        Value v{std::make_shared<const Ink>(std::move(s->samples),s->brush),s->dx,s->dy};
        require(candidate->restore(s->id,s->prev,std::move(v),s->content,s->geometry,revision,ceiling)==Status::Ok);
        return true;});
}
JNIEXPORT void JNICALL STORE(stage)(JNIEnv* e,jclass,jlong first,jlong last,jlong budget) {
    guarded(e,[&]{require(candidate && !incoming && candidate->size()==expected && budget>0 &&
                          uint64_t(budget)<=SIZE_MAX && candidate->finishRestore(revision,ceiling)==Status::Ok &&
                          candidate->grant(first,last));
        require(tablet::stage(std::move(candidate),size_t(budget)));return true;});
}
JNIEXPORT void JNICALL STORE(activate)(JNIEnv*,jclass,jdouble x,jdouble y,jdouble zoom) {tablet::activate(x,y,zoom);}
JNIEXPORT void JNICALL STORE(discard)(JNIEnv*,jclass) {incoming.reset();candidate.reset();tablet::discard();}
JNIEXPORT jlongArray JNICALL STORE(delta)(JNIEnv* e,jclass,jlong hi,jlong lo) {
    return guarded(e,[&]() -> jlongArray {
        saving=tablet::nextSave(); if(!saving) return nullptr;
        require(uuid(hi,lo)==saving->document);
        return longs(e,std::array<jlong,5>{saving->base,saving->head,saving->tail,
                                           jlong(saving->count),jlong(saving->rows.size())});
    });
}
JNIEXPORT jlongArray JNICALL STORE(row)(JNIEnv* e,jclass,jint index) {
    return guarded(e,[&]{require(bool(saving));
        const auto& r=saving->rows.at(index);
        return longs(e,std::array<jlong,6>{r.id,r.prev,r.next,r.payloadChanged,
                    r.value.ink?jlong(r.value.ink->samples.size()):0,r.geometry});});
}
JNIEXPORT jbyteArray JNICALL STORE(header)(JNIEnv* e,jclass,jint index) {
    return guarded(e,[&]{require(bool(saving));
        const auto& v=saving->rows.at(index).value;
        require(bool(v.ink));auto brush=v.ink->brush;brush.rgba=v.rgba();
        wire::Header h;require(wire::header(brush,v.dx,v.dy,h));return bytes(e,h);});
}
JNIEXPORT jbyteArray JNICALL STORE(data)(JNIEnv* e,jclass,jint index,jint chunk) {
    return guarded(e,[&]{require(saving && chunk>=0 && chunk<550);
        const auto& v=saving->rows.at(index).value;size_t start=chunk*wire::chunkSamples;
        require(v.ink && start<v.ink->samples.size());
        auto points=std::span(v.ink->samples).subspan(start);
        points=points.first(std::min(points.size(),wire::chunkSamples));
        std::vector<uint8_t> b(points.size()*wire::sampleBytes);
        require(wire::samples(points,b));return bytes(e,b);});
}
JNIEXPORT void JNICALL STORE(ack)(JNIEnv* e,jclass) {
    guarded(e,[&]{require(tablet::acknowledge(saving));saving.reset();return true;});
}
}
