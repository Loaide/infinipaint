#pragma once
// Rebuildable per-record vector sidecar. The canonical record and the current
// paired scene roots remain authoritative. A file is never accepted for an
// unverified source identity. The page format is explicit little endian.
#include "document/canonical_range.hpp"
#include "document/source_digest.hpp"
#include "tablet/spatial_pages.hpp"
#include <sys/stat.h>
#include <string_view>
namespace engine::tablet::spans {
using document::ReplaySpan;using document::SpanBounds;using document::Digest;
using spatial::Failure;using spatial::Clock;using spatial::elapsed;using spatial::peak;
constexpr size_t pageBytes=16384,capacity=64,packetBytes=172,descriptorBytes=64,headerBytes=80;
constexpr size_t scratchCeiling=256u<<10;
constexpr uint64_t diskCeiling=64ull<<20;
constexpr uint32_t magic=0x32535049,format=1;
constexpr uint32_t maxSpans=1000001,maxVertices=maxSpans*6*98;
static_assert(headerBytes+capacity*packetBytes<pageBytes);
struct Key {
    document::Uuid doc{};document::Id id=0;document::Revision geometry=0;
    Digest digest{},mesh{};
    bool operator==(const Key&)const=default;
};
struct Range {uint32_t first=0,count=0;};
struct Stats {
    std::atomic<uint64_t> builds{0},buildFailures{0},buildCancelled{0},hits{0},misses{0},invalid{0},io{0},capacityFailures{0},evictions{0},
        readPages{0},writePages{0},queries{0},queryFailures{0},selectedSpans{0},testedSpans{0},directReads{0},rawSamples{0},hashSamples{0},
        bytes{0},peakBytes{0},buildNs{0},buildMaxNs{0},queryNs{0},queryMaxNs{0},promotions{0},fallbackRows{0},plannedRows{0},renderedRows{0},verticesSaved{0},
        incompatible{0},cancelled{0},limitSkips{0},refineNs{0},refineMaxNs{0};
    std::array<uint64_t,34> snapshot()const noexcept{return {builds.load(),buildFailures.load(),buildCancelled.load(),hits.load(),misses.load(),invalid.load(),io.load(),capacityFailures.load(),evictions.load(),readPages.load(),writePages.load(),queries.load(),queryFailures.load(),selectedSpans.load(),testedSpans.load(),directReads.load(),rawSamples.load(),hashSamples.load(),bytes.load(),peakBytes.load(),buildNs.load(),buildMaxNs.load(),queryNs.load(),queryMaxNs.load(),promotions.load(),fallbackRows.load(),plannedRows.load(),renderedRows.load(),verticesSaved.load(),incompatible.load(),cancelled.load(),limitSkips.load(),refineNs.load(),refineMaxNs.load()};}
};
struct TimeCharge {
    std::atomic<uint64_t>& sum;std::atomic<uint64_t>& maximum;Clock::time_point start=Clock::now();
    ~TimeCharge(){const auto ns=elapsed(start);sum+=ns;peak(maximum,ns);}
};
inline void demand(bool b){if(!b)throw Failure::Invalid;}
inline void stopped(bool b){if(b)throw Failure::Cancelled;}
inline void note(Stats& s,Failure f) noexcept {
    switch(f){case Failure::Io:++s.io;break;case Failure::Invalid:++s.invalid;break;case Failure::Capacity:case Failure::OverlayFull:++s.capacityFailures;break;case Failure::Cancelled:++s.cancelled;break;}
}
using Page=std::array<uint8_t,pageBytes>;
struct Writer {
    uint8_t* p;
    template<class T>void put(T v) noexcept {using U=std::conditional_t<sizeof(T)==8,uint64_t,uint32_t>;auto b=std::bit_cast<U>(v);for(size_t i=0;i<sizeof(T);++i){*p++=uint8_t(b);b>>=8;}}
    template<size_t N>void put(const std::array<uint8_t,N>& b) noexcept{std::copy(b.begin(),b.end(),p);p+=N;}
};
struct Reader {
    const uint8_t* p;
    template<class T>T get()noexcept{using U=std::conditional_t<sizeof(T)==8,uint64_t,uint32_t>;U b=0;for(size_t i=0;i<sizeof(T);++i)b|=U(*p++)<<(8*i);return std::bit_cast<T>(b);}
    template<size_t N>std::array<uint8_t,N> bytes()noexcept{std::array<uint8_t,N>b;std::copy_n(p,N,b.begin());p+=N;return b;}
};
inline uint32_t crc(const Page& p) noexcept{return ~spatial::crcExtend(spatial::crcExtend(UINT32_MAX,p.data(),12),p.data()+16,p.size()-16);}
inline uint32_t seal(Page& p) noexcept{const auto v=crc(p);Writer{p.data()+12}.put(v);return v;}
inline uint32_t checked(const Page& p,uint32_t kind) {Reader r{p.data()};demand(r.get<uint32_t>()==magic&&r.get<uint32_t>()==format&&r.get<uint32_t>()==kind);auto v=r.get<uint32_t>();demand(v==crc(p));return v;}
inline bool valid(SpanBounds b) noexcept{return std::isfinite(b.minX)&&std::isfinite(b.minY)&&std::isfinite(b.maxX)&&std::isfinite(b.maxY)&&b.minX<=b.maxX&&b.minY<=b.maxY;}
inline bool equal(SpanBounds a,SpanBounds b)noexcept{return a.minX==b.minX&&a.minY==b.minY&&a.maxX==b.maxX&&a.maxY==b.maxY;}
inline SpanBounds join(SpanBounds a,SpanBounds b) noexcept{return {std::min(a.minX,b.minX),std::min(a.minY,b.minY),std::max(a.maxX,b.maxX),std::max(a.maxY,b.maxY)};}
inline bool hits(SpanBounds a,spatial::Window w)noexcept{return !(a.maxX<w.minX||a.minX>w.maxX||a.maxY<w.minY||a.minY>w.maxY);}
inline void putBox(Writer& w,SpanBounds b){for(auto v:{b.minX,b.minY,b.maxX,b.maxY})w.put(v);}
inline SpanBounds getBox(Reader& r){auto a=r.get<double>(),b=r.get<double>(),c=r.get<double>(),d=r.get<double>();SpanBounds box{a,b,c,d};demand(valid(box));return box;}
struct Packet {ReplaySpan span{};SpanBounds box{};Range vertices{};};
inline void putPacket(uint8_t* p,const Packet& a) {
    Writer w{p};const auto& s=a.span;w.put(uint32_t(s.kind));w.put(uint32_t(s.ordinal));
    for(auto v:{s.curve.origin,s.curve.a,s.curve.b,s.curve.c,s.curve.d}){w.put(v.x);w.put(v.y);}
    for(float v:{s.curve.departure,s.curve.arrival,s.curve.peak,s.ax,s.ay,s.bx,s.by,s.departure,s.arrival,s.peak,s.radiusWorld})w.put(v);
    putBox(w,a.box);w.put(a.vertices.first);w.put(a.vertices.count);
}
inline Packet getPacket(const uint8_t* p){
    Reader r{p};Packet a{};auto& s=a.span;const auto kind=r.get<uint32_t>();demand(kind==ReplaySpan::Curve||kind==ReplaySpan::Terminal);
    s.kind=ReplaySpan::Kind(kind);s.ordinal=r.get<uint32_t>();demand(s.ordinal<maxSpans);s.curve.ordinal=s.ordinal;
    for(auto* v:{&s.curve.origin,&s.curve.a,&s.curve.b,&s.curve.c,&s.curve.d}){v->x=r.get<double>();v->y=r.get<double>();demand(std::isfinite(v->x)&&std::isfinite(v->y));}
    for(auto* v:{&s.curve.departure,&s.curve.arrival,&s.curve.peak,&s.ax,&s.ay,&s.bx,&s.by,&s.departure,&s.arrival,&s.peak,&s.radiusWorld}){*v=r.get<float>();demand(std::isfinite(*v));}
    demand(s.radiusWorld>0&&s.curve.departure>=0&&s.curve.departure<=1&&s.curve.arrival>=0&&s.curve.arrival<=1&&s.curve.peak>=0&&s.curve.peak<=1&&s.departure>=0&&s.arrival>=0&&s.peak>=0);
    a.box=getBox(r);a.vertices={r.get<uint32_t>(),r.get<uint32_t>()};demand(a.vertices.first%6==0&&a.vertices.count&&a.vertices.count%6==0&&a.vertices.count<=6*98&&a.vertices.first<=maxVertices-a.vertices.count);return a;
}
struct Desc {SpanBounds box{};uint32_t page=0,checksum=0,first=0,count=0,vertex=0,vertices=0,level=0;};
inline void putDesc(uint8_t* p,const Desc& d){Writer w{p};putBox(w,d.box);for(auto v:{d.page,d.checksum,d.first,d.count,d.vertex,d.vertices,d.level,0u})w.put(v);}
inline Desc getDesc(const uint8_t* p){Reader r{p};Desc d{};d.box=getBox(r);d.page=r.get<uint32_t>();d.checksum=r.get<uint32_t>();d.first=r.get<uint32_t>();d.count=r.get<uint32_t>();d.vertex=r.get<uint32_t>();d.vertices=r.get<uint32_t>();d.level=r.get<uint32_t>();demand(r.get<uint32_t>()==0&&d.page>0&&d.count&&d.count<=maxSpans&&d.first<=maxSpans-d.count&&d.vertices&&d.vertices<=maxVertices&&d.vertex<=maxVertices-d.vertices&&d.vertex%6==0&&d.vertices%6==0&&d.level<=4);return d;}
inline bool same(const Desc&a,const Desc&b)noexcept{return equal(a.box,b.box)&&a.page==b.page&&a.checksum==b.checksum&&a.first==b.first&&a.count==b.count&&a.vertex==b.vertex&&a.vertices==b.vertices&&a.level==b.level;}
inline Desc combine(Desc a,const Desc& b){demand(a.level==b.level&&a.first+a.count==b.first&&a.vertex+a.vertices==b.vertex);a.box=join(a.box,b.box);demand(a.count<=maxSpans-b.count&&a.vertices<=maxVertices-b.vertices);a.count+=b.count;a.vertices+=b.vertices;return a;}
struct Manifest {Key key{};document::Revision durable=0;document::wire::Header canonical{};uint32_t samples=0,pages=0;Desc root{};};
// Header page: checksum, identity, canonical wire header, root descriptor.
inline Page manifestPage(const Manifest& m){Page p{};Writer w{p.data()};for(auto v:{magic,format,0u,0u})w.put(v);w.put(m.key.doc);w.put(m.key.id);w.put(m.key.geometry);w.put(m.key.digest);w.put(m.key.mesh);w.put(m.durable);w.put(m.canonical);w.put(m.samples);w.put(m.pages);putDesc(w.p,m.root);seal(p);return p;}
inline Manifest readManifest(const Page& p){checked(p,0);Reader r{p.data()+16};Manifest m{};m.key.doc=r.bytes<16>();m.key.id=r.get<int64_t>();m.key.geometry=r.get<int64_t>();m.key.digest=r.bytes<32>();m.key.mesh=r.bytes<32>();m.durable=r.get<int64_t>();m.canonical=r.bytes<80>();m.samples=r.get<uint32_t>();m.pages=r.get<uint32_t>();m.root=getDesc(r.p);
    document::Brush b;double dx,dy;demand(m.key.id>0&&m.key.geometry>0&&(m.durable==0||m.durable>=m.key.geometry)&&m.samples&&m.samples<=1000000&&m.pages>=2&&m.pages<=diskCeiling/pageBytes&&m.root.page==m.pages-1&&m.root.first==0&&m.root.vertex==0&&m.root.count<=m.samples+1&&document::wire::readHeader(m.canonical,b,dx,dy));return m;}
// Every page is verified against its parent's descriptor, not just a free-
// standing checksum. Metadata and payload must describe one contiguous cut.
inline Desc pageDesc(const Page& p,uint32_t page){const auto kind=Reader{p.data()+8}.get<uint32_t>();demand(kind==1||kind==2);const auto c=checked(p,kind);auto d=getDesc(p.data()+16);demand(d.page==page&&d.checksum==0&&((kind==1)==(d.level==0)));d.checksum=c;return d;}
inline void validateData(const Page& p,const Desc& d){
    demand(d.level==0&&d.count<=capacity&&d.first%capacity==0&&d.page==d.first/capacity+1);SpanBounds box{};uint32_t vertex=d.vertex;
    for(uint32_t i=0;i<d.count;++i){auto a=getPacket(p.data()+headerBytes+i*packetBytes);demand(a.span.ordinal==d.first+i&&a.vertices.first==vertex);box=join(box,a.box);vertex+=a.vertices.count;}
    demand(vertex==d.vertex+d.vertices&&equal(box,d.box));
}
inline uint32_t childCount(const Page& p,const Desc& d){
    demand(d.level>0);const auto n=Reader{p.data()+headerBytes}.get<uint32_t>();demand(n&&n<=capacity);Desc aggregate{};
    for(uint32_t i=0;i<n;++i){auto c=getDesc(p.data()+headerBytes+4+i*descriptorBytes);demand(c.page<d.page&&c.level+1==d.level);if(i==0)aggregate=c;else aggregate=combine(aggregate,c);}
    demand(aggregate.first==d.first&&aggregate.count==d.count&&aggregate.vertex==d.vertex&&aggregate.vertices==d.vertices&&equal(aggregate.box,d.box));return n;
}
inline void setPageHeader(Page& p,Desc d){Writer w{p.data()};for(auto v:{magic,format,d.level?2u:1u,0u})w.put(v);d.checksum=0;putDesc(w.p,d);seal(p);}
class File {
    int fd_=-1;
public:
    File()=default;explicit File(int fd):fd_(fd){}~File(){close();}File(const File&)=delete;File&operator=(const File&)=delete;
    File(File&& f)noexcept:fd_(std::exchange(f.fd_,-1)){}File&operator=(File&& f)noexcept{if(this!=&f){close();fd_=std::exchange(f.fd_,-1);}return *this;}
    void close()noexcept{if(fd_>=0)::close(fd_);fd_=-1;}
    explicit operator bool()const noexcept{return fd_>=0;}
    uint64_t size()const{struct stat s{};if(::fstat(fd_,&s)||!S_ISREG(s.st_mode)||s.st_size<0)throw Failure::Io;return uint64_t(s.st_size);}
    void transfer(Page& p,uint32_t page,bool write){auto* data=p.data();size_t n=p.size();off_t off=off_t(uint64_t(page)*pageBytes);while(n){auto bytes=write?::pwrite(fd_,data,n,off) : ::pread(fd_,data,n,off);if(bytes<0&&errno==EINTR)continue;if(bytes<=0)throw Failure::Io;data+=bytes;n-=size_t(bytes);off+=bytes;}}
    void sync(){if(::fsync(fd_))throw Failure::Io;}
};
inline Page read(File& f,uint32_t id,Stats& s){Page p;f.transfer(p,id,false);++s.readPages;return p;}
inline void write(File& f,uint32_t id,Page& p,Stats& s){f.transfer(p,id,true);++s.writePages;}
// Match is called for each actual chord; it may strengthen the bounds using
// the exact resident v1 descriptor and must reject any geometry mismatch.
// The build's entire result is discarded on cancellation or sink failure.
template<class Stop,class Match,class Admit> Manifest build(File& f,const Key& key,document::Revision durable,const document::Ink& ink,double dx,double dy,uint32_t vertexCount,Stats& stats,Stop& stop,Match& match,Admit& admit){
    Manifest m{};m.key=key;m.durable=durable;m.samples=uint32_t(ink.samples.size());demand(ink.valid()&&document::wire::header(ink.brush,dx,dy,m.canonical));
    auto stream=document::CanonicalStream::create(ink.brush);demand(bool(stream));
    // Page zero is reserved but not a valid manifest until all pages exist.
    Page zero{};admit(pageBytes);write(f,0,zero,stats);m.pages=1;
    struct Sink {
        File& f;Stats& stats;Stop& stop;Match& match;Admit& admit;Manifest& m;double dx,dy;
        Page data{};Desc d{};uint32_t ordinal=0,vertex=0;bool mismatch=false;
        void flush(){if(!d.count)return;stopped(stop());d.page=m.pages;d.level=0;setPageHeader(data,d);admit(uint64_t(m.pages+1)*pageBytes);write(f,m.pages++,data,stats);data={};d={};}
        static bool emit(void* ptr,const ReplaySpan& s){auto& self=*static_cast<Sink*>(ptr);if(s.kind!=ReplaySpan::Curve&&s.kind!=ReplaySpan::Terminal)return true;stopped(self.stop());demand(s.ordinal==self.ordinal&&self.ordinal<maxSpans);
            Packet a{};a.span=s;a.vertices.first=self.vertex;
            if(!s.chords([&](float ax,float ay,float bx,float by,float r0,float r1){if(self.vertex>maxVertices-6||!self.match(self.vertex,ax,ay,bx,by,r0,r1,a.box)){self.mismatch=true;return false;}self.vertex+=6;return true;}))return false;
            SpanBounds curve{};if(!document::spanBounds(s,self.dx,self.dy,curve))return false;a.box=join(a.box,curve);demand(valid(a.box));a.vertices.count=self.vertex-a.vertices.first;demand(a.vertices.count&&a.vertices.count<=6*98);
            if(!self.d.count){self.d.first=self.ordinal;self.d.vertex=a.vertices.first;self.d.box=a.box;}else self.d.box=join(self.d.box,a.box);
            putPacket(self.data.data()+headerBytes+self.d.count*packetBytes,a);++self.d.count;self.d.vertices+=a.vertices.count;++self.ordinal;if(self.d.count==capacity)self.flush();return true;
        }
    } sink{f,stats,stop,match,admit,m,dx,dy};
    auto points=std::span(ink.samples);auto& decoder=stream.value();while(!points.empty()){stopped(stop());auto n=std::min<size_t>(points.size(),64);if(!decoder.append(points.first(n),&sink,Sink::emit)){if(sink.mismatch)++stats.incompatible;throw Failure::Invalid;}stats.rawSamples+=n;points=points.subspan(n);}
    if(!decoder.finish(&sink,Sink::emit)){if(sink.mismatch)++stats.incompatible;throw Failure::Invalid;}sink.flush();demand(sink.vertex==vertexCount&&sink.ordinal&&m.pages>1);
    // Bottom-up packed interval hierarchy: one bounded page buffer, no
    // descriptor vector proportional to stroke length. Spatial union of
    // consecutive intervals; direct ordinal seeks use the same hierarchy.
    uint32_t begin=1,end=m.pages;
    while(end-begin>1){const uint32_t next=m.pages;for(uint32_t i=begin;i<end;){stopped(stop());Page out{};Desc parent{};uint32_t n=0;
        for(;n<capacity&&i<end;++n,++i){stopped(stop());auto p=read(f,i,stats);auto child=pageDesc(p,i);if(!n)parent=child;else parent=combine(parent,child);putDesc(out.data()+headerBytes+4+n*descriptorBytes,child);}
        Writer{out.data()+headerBytes}.put(n);++parent.level;parent.page=m.pages;setPageHeader(out,parent);admit(uint64_t(m.pages+1)*pageBytes);write(f,m.pages++,out,stats);
    }begin=next;end=m.pages;}
    m.root=pageDesc(read(f,begin,stats),begin);demand(m.root.count==sink.ordinal&&m.root.vertices==vertexCount);stopped(stop());auto h=manifestPage(m);write(f,0,h,stats);return m;
}
inline Manifest open(File& f,const Key& key,document::Revision durable,uint32_t vertices,Stats& s){auto m=readManifest(read(f,0,s));demand(m.key==key&&m.root.vertices==vertices&&m.durable<=durable&&f.size()==uint64_t(m.pages)*pageBytes);return m;}
struct QueryWork {uint32_t pages=0,tested=0,hits=0;};
// Bounded, ordinal-ordered traversal. Every selected packet is delivered
// once; consumer can preserve its coefficients or aggregate a conservative
// vertex range without altering internal caps or whole-stroke compositing.
template<class Stop,class Emit> void visit(File& f,const Desc& d,spatial::Window* window,uint32_t first,uint32_t end,Stats& stats,QueryWork& work,uint32_t limit,Stop& stop,Emit& emit,uint32_t total){
    stopped(stop());if(work.pages==limit)throw Failure::Capacity;++work.pages;
    const auto p=read(f,d.page,stats);const auto actual=pageDesc(p,d.page);demand(same(actual,d));
    if(d.level){const auto n=childCount(p,d);for(uint32_t i=0;i<n;++i){const auto c=getDesc(p.data()+headerBytes+4+i*descriptorBytes);if(window?!hits(c.box,*window):(c.first>=end||c.first+c.count<=first))continue;visit(f,c,window,first,end,stats,work,limit,stop,emit,total);}}
    else {validateData(p,d);for(uint32_t i=0;i<d.count;++i){stopped(stop());auto a=getPacket(p.data()+headerBytes+i*packetBytes);demand((a.span.kind==ReplaySpan::Terminal)==(a.span.ordinal+1==total));++work.tested;if(window?!hits(a.box,*window):(a.span.ordinal<first||a.span.ordinal>=end))continue;++work.hits;emit(a);}}
}
template<class Stop,class Emit> QueryWork query(File& f,const Manifest& m,spatial::Window w,uint32_t limit,Stats& stats,Stop& stop,Emit& emit){demand(w.valid());QueryWork work{};visit(f,m.root,&w,0,0,stats,work,limit,stop,emit,m.root.count);return work;}
template<class Stop,class Emit> QueryWork direct(File& f,const Manifest& m,uint32_t first,uint32_t count,Stats& stats,Stop& stop,Emit& emit){demand(count&&count<=128&&first<=m.root.count&&count<=m.root.count-first);QueryWork work{};visit(f,m.root,nullptr,first,first+count,stats,work,20,stop,emit,m.root.count);demand(work.hits==count);++stats.directReads;return work;}
}
