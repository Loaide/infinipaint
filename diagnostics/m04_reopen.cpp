// Host-only DLL. Use the actual v1 decoder, incremental builder and finisher.
// Nothing here is linked into the Android target. Single sequential reader.
#include "native_bridge.cpp"
#include <cstdio>
#include <stdexcept>
extern "C" int __android_log_print(int,const char*,const char*,...){return 0;}
namespace probe {
using namespace engine;
FILE* out=nullptr;
std::unique_ptr<Trace> t;
document::Brush brush;
int expected,used;
int64_t id;
double dx,dy;
bool audit;
input::RawInputStroke control;
Mesh observed;
struct Box { double x,y,X,Y; int64_t id; };
static_assert(sizeof(Box)==40);
Box box;
std::vector<Box> auditBoxes;
int parts;
uint64_t spans,quads,high;
void require(bool b){if(!b)throw std::runtime_error("replay");}
void flush(){
    if(parts){if(out)require(fwrite(&box,sizeof(box),1,out)==1);
        if(audit)auditBoxes.push_back(box);parts=0;++spans;}
}
void consume(const Mesh& mesh,bool spline){
    for(size_t i=0;i<mesh.size();i+=6){
        Vertex v=mesh[i];
        const double ax=double(v.x)+dx,ay=double(v.y)+dy;
        require(std::max(std::abs(ax),std::abs(ay))<=std::numeric_limits<float>::max()/1024.);
        v.x=float(ax);v.y=float(ay); // same post-reconstruction translation as v1
        if(!spline)continue;
        if(audit)observed.push_back(v);
        ++quads;
        const double bx=v.x+double(v.dirX)*v.len,by=v.y+double(v.dirY)*v.len;
        const double rad=std::max(v.radius,v.endRadius);
        // Radius covers the swept discs (linear centers and radii); worst v1 AA
        // at .1x. Round outwards and allow float endpoint arithmetic error.
        const double guard=8*std::numeric_limits<float>::epsilon()*
            (1+std::abs(v.x)+std::abs(v.y)+v.len+rad);
        const double pad=rad+1/MIN_ZOOM+guard;
        Box b{std::nextafter(std::min(double(v.x),bx)-pad,-INFINITY),
              std::nextafter(std::min(double(v.y),by)-pad,-INFINITY),
              std::nextafter(std::max(double(v.x),bx)+pad,INFINITY),
              std::nextafter(std::max(double(v.y),by)+pad,INFINITY),id};
        require(std::isfinite(b.x)&&std::isfinite(b.X)&&std::isfinite(b.y)&&std::isfinite(b.Y));
        if(!parts)box=b;else{
            box.x=std::min(box.x,b.x);box.y=std::min(box.y,b.y);
            box.X=std::max(box.X,b.X);box.Y=std::max(box.Y,b.Y);
        }
        if(++parts==16)flush();
    }
}
}
#define HOST extern "C" __declspec(dllexport)
HOST int startFile(const wchar_t* file){
    using namespace probe;
    t.reset();spans=quads=high=0;
    if(out)fclose(out);
    out=file?_wfopen(file,L"wb"):nullptr;
    return !file || out;
}
HOST int stopFile(uint64_t* stats){
    using namespace probe;t.reset();
    stats[0]=spans;stats[1]=quads;stats[2]=high;
    auto f=std::exchange(out,nullptr);return !f || fclose(f)==0;
}
HOST int startRecord(int64_t key,const uint8_t* h,int count,int verify){
    using namespace probe;
    try{
        // Admission, not silent truncation. Existing incremental filter/spline
        // retain O(largest admitted record) state even after mesh retirement.
        require(count>0 && count<=65536 && document::wire::readHeader({h,80},brush,dx,dy));
        auto p=input::InputProcessor::create(brush.filter);
        auto f=stroke::SplineFitter::create({stroke::Parameterization::Centripetal,1000000,.01,14});
        require(bool(p)&&bool(f));
        t=std::make_unique<Trace>(std::move(p).value(),std::move(f).value());
        t->strokeRadiusWorld=brush.radiusWorld;
        t->scratchAllowance=65536ull*640;t->meshAllowance=8u<<20;
        require(bool(t->input.begin_stroke()));
        expected=count;used=probe::parts=0;id=key;audit=verify!=0;
        observed.clear();control.clear();auditBoxes.clear();return 1;
    }catch(...){return 0;}
}
HOST int feedChunk(const uint8_t* bytes,int size){
    using namespace probe;
    try{
        std::array<input::RawInputPoint,document::wire::chunkSamples> chunk;
        require(size>0 && size%36==0 && size/36<=int(chunk.size()) && used+size/36<=expected);
        auto points=std::span(chunk).first(size/36);
        require(document::wire::readSamples({bytes,size_t(size)},t->lastAcceptedTime,points));
        for(auto p:points){
            require(appendV1(*t,p,1,0,false));
            high=std::max(high,workingBytes(*t));
            if(audit)control.push_back(p);
            consume(t->stableMesh,true);consume(t->rawMesh,false);consume(t->cleanMesh,false);
            t->stableMesh.clear();t->rawMesh.clear();t->cleanMesh.clear();
        }
        used+=int(points.size());return 1;
    }catch(...){return 0;}
}
HOST int endRecord(){
    using namespace probe;
    try{
        require(used==expected);
        auto end=finishV1(*t,{});consume(*end.spline,true);consume(*end.raw,false);consume(*end.clean,false);flush();
        if(audit){
            FinishedTrace ref;
            require(reconstructV1(std::make_shared<const document::Ink>(std::move(control),brush),ref,dx,dy));
            require(ref.spline->size()==observed.size()*6);
            for(size_t i=0;i<observed.size();++i){
                const auto& v=ref.spline->at(6*i);
                require(!memcmp(&observed[i],&v,sizeof(Vertex)));
                const auto b=auditBoxes.at(i/16);
                for(double u:{0.,.5,1.}){
                    double x=v.x+u*v.dirX*v.len,y=v.y+u*v.dirY*v.len;
                    double pad=std::lerp(double(v.radius),double(v.endRadius),u)+10.;
                    require(x-pad>=b.x && x+pad<=b.X && y-pad>=b.y && y+pad<=b.Y);
                }
            }
        }
        t.reset();return 1;
    }catch(...){return 0;}
}
