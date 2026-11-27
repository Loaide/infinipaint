#include "bench.hpp"
#include "path.inc"
#include <android/native_window_jni.h>
#include <android/trace.h>
#include <jni.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <time.h>

static int64_t ns(clockid_t c){timespec ts{};clock_gettime(c,&ts);return ts.tv_sec*1000000000LL+ts.tv_nsec;}
int64_t nowNs(){return ns(CLOCK_MONOTONIC);}
namespace {
std::unique_ptr<Backend> backend;
ANativeWindow* window=nullptr;
Scene scene;
struct Row {long long tick,begin,end,cpu,gpu,present; Metrics m;};
std::vector<Row> rows;
std::string dir,work,mode;
int w,h,seq,measured,warm,initial,visible,timer,drain;
uint64_t signature;
bool parity,live,compact,anchor,life;
int loadCount(const std::string& s, const std::string& prefix, int max){
    const auto tail=s.substr(prefix.size());
    size_t used=0; int n=std::stoi(tail,&used);
    if(used!=tail.size() || n<1 || n>max)throw std::runtime_error("invalid load");
    return n;
}
void need(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
std::string str(JNIEnv* e,jstring s){
    need(s!=nullptr,"missing required intent extra");const char* p=e->GetStringUTFChars(s,nullptr);
    need(p!=nullptr,"string allocation");std::string r=p;e->ReleaseStringUTFChars(s,p);return r;
}
void stroke(float x,float y,int chords,int id){
    Stroke s{};s.first=int(scene.items.size());s.count=chords;
    s.color[0]=id%2==0?1.f:0.f;s.color[2]=1-s.color[0];s.color[3]=.5f;
    float minX=x,minY=y,maxX=x,maxY=y;
    for(int j=0;j<chords;j++){
        // First capsule is a round dot; other chords retrace the same pressure
        // path, exercising within-stroke union rather than primitive blending.
        const int a=j==0?0:(j-1)%15,b=j==0?0:j%15;
        float ax=x+path[a][0],ay=y+path[a][1],bx=x+path[b][0],by=y+path[b][1];
        float len=std::hypot(bx-ax,by-ay),r=j==0?4.f:1.f+float(a%4);
        Instance v{ax,ay,len,r,j==0?r:1.f+float(b%4),len?(bx-ax)/len:1,len?(by-ay)/len:0};
        scene.items.push_back(v);
        for(auto& uv:corners)scene.expanded.push_back({v.x,v.y,uv[0],uv[1],v.len,v.r,v.s,v.dx,v.dy});
        minX=std::min({minX,ax,bx});minY=std::min({minY,ay,by});
        maxX=std::max({maxX,ax,bx});maxY=std::max({maxY,ay,by});
    }
    s.x=std::max(0,int(std::floor(minX))-10);s.y=std::max(0,int(std::floor(minY))-10);
    s.w=std::min(w,int(std::ceil(maxX))+10)-s.x;s.h=std::min(h,int(std::ceil(maxY))+10)-s.y;
    need(s.w>0 && s.h>0,"empty fixture bounds");scene.strokes.push_back(s);
}
void logMemory(const char* phase){
    FILE* f=fopen("/proc/self/smaps_rollup","r");
    char buf[256];
    if(f){while(fgets(buf,sizeof(buf),f))
        if(!strncmp(buf,"Rss:",4)||!strncmp(buf,"Pss:",4))BLOG("process %s %s",phase,buf);
        fclose(f);
    }else BLOG("smaps_rollup unavailable phase=%s",phase);
    f=fopen("/proc/self/status","r");
    if(f){while(fgets(buf,sizeof(buf),f))if(!strncmp(buf,"VmHWM:",6))BLOG("process %s %s",phase,buf);fclose(f);}
    if(backend)backend->sampleMemory(phase);
}
uint64_t hash(uint64_t v,const void* mem,size_t n){
    auto p=static_cast<const uint8_t*>(mem);while(n--){v^=*p++;v*=1099511628211ULL;}return v;
}
void save(){
    FILE* f=fopen((dir+"/frames.csv").c_str(),"wb");need(f,"open result");
    fprintf(f,"seq,phase,tick_ns,begin_ns,end_ns,thread_cpu_ns,sync_ns,submit_ns,present_call_ns,gpu_ns,actual_present_ns,draws,upload_bytes\n");
    for(int i=1;i<=seq;i++){
        const auto& r=rows[i];
        fprintf(f,"%d,%s,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%d,%llu\n",i,
            parity?"parity":i<=warm?"warm":i<=warm+measured?"measure":"drain",r.tick,r.begin,r.end,r.cpu,
            (long long)r.m.syncNs,(long long)r.m.submitNs,(long long)r.m.presentCallNs,r.gpu,r.present,
            r.m.draws,(unsigned long long)r.m.bytes);
    }
    need(fclose(f)==0,"write result");
}
}
void gpuResult(int i,uint64_t n){if(i>0 && size_t(i)<rows.size())rows[i].gpu=(long long)n;}
void presentResult(int i,int64_t n){if(i>0 && size_t(i)<rows.size() && n>0)rows[i].present=n;}
bool detailEnabled(){return timer>=0;}
#define JNI_FN(n) extern "C" JNIEXPORT void JNICALL Java_com_infinipaint_diag_matched_Bench_##n
JNI_FN(start)(JNIEnv* e,jclass,jobject surface,jstring api,jstring scenario,jstring dest,jint frames,jint t){
    try{
        mode=str(e,api);work=str(e,scenario);dir=str(e,dest);
        need(mode=="g2"||mode=="g3e"||mode=="g3i"||mode=="vke"||mode=="vki","api");
        need(!backend && !window,"renderer already open");
        anchor=work=="parity"||work=="reverse";
        const bool pixelLoad=work.starts_with("pixel-");
        const std::string kind=pixelLoad?work.substr(6):work;
        const bool sparseLoad=kind.starts_with("sparse-");
        const bool denseLoad=kind.starts_with("overlap-");
        life=work=="life-live"||work=="life-pixel";
        parity=anchor||pixelLoad||work=="life-pixel";
        live=work=="live"||work=="life-live";
        need(!pixelLoad||sparseLoad||denseLoad,"pixel load scene");
        need(parity||live||sparseLoad||denseLoad||work=="pan"||work=="dense2"||work=="dense16"||work=="dense64","scene");
        need(frames>=1 && frames<=60000 && t>=-1 && t<=1,"frames/timer");
        warm=(parity||life)?0:(sparseLoad||denseLoad)&&frames<1000?90:450;
        drain=(parity||life)?0:12;measured=parity?1:frames;timer=t;
        need(!life||parity||frames==8,"life suffix length");
        seq=0;rows.assign(warm+measured+drain+1,{});scene={};
        window=ANativeWindow_fromSurface(e,surface);need(window,"window");
        w=ANativeWindow_getWidth(window);h=ANativeWindow_getHeight(window);
        need(w>=256 && h>=128 && w<=4096 && h<=4096,"window bounds");
        int count=life?16:anchor?2:work=="dense2"?2:work=="dense16"?16:64;
        const int cols=(w-24)/64, gridRows=(h-24)/64;
        if(sparseLoad){count=loadCount(kind,"sparse-",1024);need(cols>0 && count<=cols*gridRows,"sparse grid does not fit");}
        if(denseLoad)count=loadCount(kind,"overlap-",2048);
        for(int i=0;i<count;i++){
            bool sparse=!life&&(work=="pan"||live);
            float x=sparseLoad?24.f+64.f*(i%cols):sparse?24+float(i%8)*(w-100)/8:40.5f;
            float y=sparseLoad?24.f+64.f*(i/cols):sparse?24+float(i/8)*(h-100)/8:24.5f;
            stroke(x,y,16,work=="reverse"?i+1:i);
        }
        initial=int(scene.items.size());
        if(life){stroke(52,64,8,2);if(parity)initial+=8;}
        else if(live)stroke(52,64,measured,2);
        visible=initial;compact=mode=="g3i"||mode=="vki";
        signature=hash(14695981039346656037ULL,scene.items.data(),scene.items.size()*sizeof(Instance));
        for(auto& s:scene.strokes)signature=hash(signature,s.color,sizeof(s.color));
        logMemory("before");auto start=nowNs();
        backend=mode[0]=='v'?openVK(window,scene,initial,compact):openGL(window,scene,initial,mode!="g2",compact);
        BLOG("start mode=%s scene=%s size=%dx%d fixture=%016llx initial=%d init_ns=%lld",mode.c_str(),work.c_str(),w,h,
             (unsigned long long)signature,initial,(long long)(nowNs()-start));
        FILE* f=fopen((dir+"/identity.txt").c_str(),"w");need(f,"identity");
        fprintf(f,"%s %s %d %d %016llx\n",mode.c_str(),work.c_str(),w,h,(unsigned long long)signature);
        need(fclose(f)==0,"identity write");
        logMemory("after_init");
    }catch(const std::exception& ex){e->ThrowNew(e->FindClass("java/lang/IllegalStateException"),ex.what());}
}
extern "C" JNIEXPORT jboolean JNICALL Java_com_infinipaint_diag_matched_Bench_step(JNIEnv* e,jclass,jlong tick){
    try{
        ++seq;auto& row=rows.at(seq);int prev=visible;
        if(live && seq>warm && seq<=warm+measured)++visible;
        Frame f{seq,visible,prev,visible-prev,work=="pan"?float(seq%16)*.25f:0.f,
                bool(timer==1 && seq<=warm+measured && seq%8==0),parity,life};
        std::vector<uint8_t> pixels;auto cpu=detailEnabled()?ns(CLOCK_THREAD_CPUTIME_ID):0;
        row.tick=tick;row.begin=nowNs();
        if(detailEnabled()){char label[48];snprintf(label,sizeof(label),"IP_MATCH frame %d",seq);ATrace_beginSection(label);}
        try{row.m=backend->frame(f,parity?&pixels:nullptr);}catch(...){if(detailEnabled())ATrace_endSection();throw;}
        if(detailEnabled())ATrace_endSection();
        row.end=nowNs();row.cpu=detailEnabled()?ns(CLOCK_THREAD_CPUTIME_ID)-cpu:0;
        if(parity){
            need(pixels.size()==size_t(w)*h*4,"wrong parity image size");
            if(anchor){
            // Anchor shared by both strokes, full coverage and alpha restored.
            auto p=pixels.data()+(size_t(24)*w+40)*4;
            bool red=work=="reverse";
            need(p[3]==255 && p[red?0:2]>120 && p[red?0:2]<145 &&
                 p[red?2:0]>55 && p[red?2:0]<80,"ordered anchor parity");
            }
            FILE* fp=fopen((dir+"/pixels.rgba").c_str(),"wb");need(fp,"open image");
            auto wrote=fwrite(pixels.data(),1,pixels.size(),fp);int err=fclose(fp);
            need(wrote==pixels.size() && !err,"write parity image");
        }
        need(row.m.bytes==uint64_t(f.added)*(compact?28:216),"unexpected per-frame upload");
        if(measured>=54000 && seq>warm && seq%1800==0)logMemory("sustained_sample");
        if(seq==warm+measured+drain){logMemory("end");save();return JNI_FALSE;}
        return JNI_TRUE;
    }catch(const std::exception& ex){e->ThrowNew(e->FindClass("java/lang/IllegalStateException"),ex.what());return JNI_FALSE;}
}
JNI_FN(close)(JNIEnv*,jclass){
    if(backend){logMemory("pre_close");backend.reset();}
    if(window){ANativeWindow_release(window);window=nullptr;scene={};rows.clear();logMemory("post_close");}
}
