// Host-only GL simulation. Ensures native bridge follows incremental buffer
// and lazy completed-cache contracts without requiring a tablet or GPU.
#include <jni.h>
#include <GLES2/gl2.h>
#include <android/log.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <set>
#include <map>
#include <vector>
#include <cstring>
#include <new>
#include "tablet/session.hpp"

// Single-threaded fault injection. Trigger exactly one scalar allocation.
static int failAfter=-1;
void* operator new(size_t n) {
    if(failAfter>=0 && failAfter--==0) throw std::bad_alloc{};
    if(auto* p=std::malloc(n?n:1)) return p;
    throw std::bad_alloc{};
}
[[gnu::noinline]] void operator delete(void* p) noexcept {std::free(p);}
[[gnu::noinline]] void operator delete(void* p,size_t) noexcept {std::free(p);}

extern "C" {
void Java_com_infinipaint_demo_NativeBridge_contextCreated(JNIEnv*,jclass);
void Java_com_infinipaint_demo_NativeBridge_resizeViewport(JNIEnv*,jclass,jint,jint);
void Java_com_infinipaint_demo_NativeBridge_setCamera(JNIEnv*,jclass,jdouble,jdouble,jdouble);
void Java_com_infinipaint_demo_NativeBridge_setSmoothingPreset(JNIEnv*,jclass,jint);
jboolean Java_com_infinipaint_demo_NativeBridge_begin(JNIEnv*,jclass,jint,jint);
jboolean Java_com_infinipaint_demo_NativeBridge_sample(JNIEnv*,jclass,jint,jdouble,jdouble,jdouble,jfloat,jfloat,jfloat);
jboolean Java_com_infinipaint_demo_NativeBridge_end(JNIEnv*,jclass,jint);
void Java_com_infinipaint_demo_NativeBridge_cancel(JNIEnv*,jclass,jint);
void Java_com_infinipaint_demo_NativeBridge_clear(JNIEnv*,jclass);
void Java_com_infinipaint_demo_NativeBridge_suspend(JNIEnv*,jclass,jboolean);
jint Java_com_infinipaint_demo_NativeBridge_command(JNIEnv*,jclass,jint,jdoubleArray,jdouble);
void Java_com_infinipaint_demo_NativeBridge_render(JNIEnv*,jclass,jint);
}
static unsigned nextId=1;
static uint64_t transferBytes=0, bufferAllocs=0, draws=0;
static std::set<unsigned> buffers;
static GLuint boundBuffer = 0, lastDrawBuffer = 0;
static unsigned deletedObjects = 0;
static GLsizei viewportWidth = 0, viewportHeight = 0;
static GLfloat uniformPanX, uniformPanY, uniformZoom;
static std::map<unsigned,std::vector<uint8_t>> gpuContents;
static long long lastHoldNs;
static size_t usedBytes;
static size_t clearedPixels=0,scissorPixels=0;
static unsigned maskClears=0,errorChecks=0;
static bool scissored=false;
static bool failUpload=false;
static GLenum uploadError=GL_NO_ERROR;
static GLsizei stride;
static std::vector<uint8_t> drawnVertices() {
    const auto& storage=gpuContents.at(lastDrawBuffer);
    if(usedBytes>storage.size()) std::abort();
    return {storage.begin(),storage.begin()+usedBytes};
}
static long long lastWaitNs;
extern "C" {
int __android_log_print(int,const char* tag,const char* fmt,...) {
    if(!std::strcmp(tag,"IP_EDIT")) {
        va_list a;va_start(a,fmt);std::vfprintf(stderr,fmt,a);va_end(a);std::fputc('\n',stderr);
    }
    if(!std::strcmp(tag,"IP_PEN_UP")) {
        va_list a; va_start(a,fmt); (void)va_arg(a,size_t); lastHoldNs=va_arg(a,long long);
        lastWaitNs=va_arg(a,long long); va_end(a);
    }
    return 0;
}
GLuint glCreateShader(GLenum) {return nextId++;}
void glShaderSource(GLuint,GLsizei,const GLchar*const*,const GLint*) {}
void glCompileShader(GLuint) {}
void glGetShaderiv(GLuint,GLenum,GLint* status) {*status=1;}
void glGetShaderInfoLog(GLuint,GLsizei,GLsizei*,GLchar*) {}
void glDeleteShader(GLuint) {}
GLuint glCreateProgram(void) {return nextId++;}
void glAttachShader(GLuint,GLuint) {}
void glBindAttribLocation(GLuint,GLuint,const GLchar*) {}
void glLinkProgram(GLuint) {}
void glGetProgramiv(GLuint,GLenum,GLint*status) {*status=1;}
void glGetProgramInfoLog(GLuint,GLsizei,GLsizei*,GLchar*) {}
void glDeleteProgram(GLuint) {++deletedObjects;}
GLboolean glIsProgram(GLuint) {return 1;}
GLboolean glIsBuffer(GLuint id) {return buffers.count(id)!=0;}
void glDeleteBuffers(GLsizei n,const GLuint*ids) {deletedObjects+=n;for(int i=0;i<n;i++){buffers.erase(ids[i]);gpuContents.erase(ids[i]);}}
void glGenBuffers(GLsizei n,GLuint* ids){for(int i=0;i<n;i++) {ids[i]=nextId++;buffers.insert(ids[i]);}}
void glBindBuffer(GLenum,GLuint b) {if(b && !buffers.count(b))std::abort();boundBuffer=b;}
void glBufferData(GLenum,GLsizeiptr size,const void* ptr,GLenum){
    ++bufferAllocs;
    auto& bytes=gpuContents[boundBuffer];bytes.assign((size_t)size,0);
    if(ptr){transferBytes+=size;std::memcpy(bytes.data(),ptr,(size_t)size);}
}
void glBufferSubData(GLenum,GLintptr offset,GLsizeiptr size,const void* data) {
    if(failUpload) {failUpload=false;uploadError=GL_OUT_OF_MEMORY;return;}
    transferBytes+=size;
    auto& bytes=gpuContents[boundBuffer];
    if(offset+size>(GLintptr)bytes.size())std::abort();
    std::memcpy(bytes.data()+offset,data,(size_t)size);
}
void glViewport(GLint,GLint,GLsizei w,GLsizei h) {viewportWidth=w;viewportHeight=h;}
GLint glGetAttribLocation(GLuint,const GLchar*) {return 0;}
GLint glGetUniformLocation(GLuint,const GLchar* name) {
    if (!std::strcmp(name, "uCameraPan")) return 1;
    if (!std::strcmp(name, "uCameraZoom")) return 2;
    return 0;
}
void glUseProgram(GLuint) {}
void glUniform4f(GLint,GLfloat,GLfloat,GLfloat,GLfloat) {}
void glUniform2f(GLint loc,GLfloat x,GLfloat y) {if(loc==1){uniformPanX=x;uniformPanY=y;}}
void glUniform1f(GLint loc,GLfloat z) {if(loc==2)uniformZoom=z;}
void glVertexAttribPointer(GLuint,GLint,GLenum,GLboolean,GLsizei s,const void*) {stride=s;}
void glEnableVertexAttribArray(GLuint) {}
static unsigned resolves=0;
void glDrawArrays(GLenum,GLint,GLsizei n) {
    if(!boundBuffer) {++resolves;return;}
    ++draws;lastDrawBuffer=boundBuffer;usedBytes=size_t(n)*stride;
}
void glScissor(GLint,GLint,GLsizei w,GLsizei h) {scissorPixels=size_t(w)*h;}
void glColorMask(GLboolean,GLboolean,GLboolean,GLboolean) {}
void glDisable(GLenum cap) {if(cap==GL_SCISSOR_TEST)scissored=false;}
void glBlendEquation(GLenum) {}
void glBlendFuncSeparate(GLenum,GLenum,GLenum,GLenum) {}
void glUniform1i(GLint,GLint) {}
const unsigned char* glGetString(GLenum id) {
    return (const unsigned char*)(id==GL_VERSION?"OpenGL ES 2.0":"GL_EXT_blend_minmax");
}
void glGetIntegerv(GLenum,GLint* value){*value=8;}
GLenum glGetError(){++errorChecks;GLenum result=uploadError;uploadError=GL_NO_ERROR;return result;}
void glClearColor(GLfloat,GLfloat,GLfloat,GLfloat) {}
void glClear(GLbitfield) {if(scissored){++maskClears;clearedPixels+=scissorPixels;}}
void glEnable(GLenum cap) {if(cap==GL_SCISSOR_TEST)scissored=true;}
void glBlendFunc(GLenum,GLenum) {}
}
static void check(bool ok,const char* msg){if(!ok){std::fprintf(stderr,"FAIL: %s\n",msg);std::exit(1);}}
#include <chrono>
#include <algorithm>
int main(int argc,char** argv){
    auto render=[](int mode){Java_com_infinipaint_demo_NativeBridge_render(nullptr,nullptr,mode);};
    auto sample=[](int n,double timestamp){return Java_com_infinipaint_demo_NativeBridge_sample(nullptr,nullptr,1,
        100+3*n,250+25*std::sin(n*0.13),timestamp,0.6f,0,0)==JNI_TRUE;};
    auto resize=[](int w,int h){Java_com_infinipaint_demo_NativeBridge_resizeViewport(nullptr,nullptr,w,h);};
    if(argc>1) {
        using clock=std::chrono::steady_clock;
        bool queue=std::string_view(argv[1])=="bench-queue";
        if(queue) {
            engine::tablet::setPhysicalSlice(288u<<20);
            auto d=std::make_unique<engine::document::Document>(engine::document::Uuid{3});
            check(d->grant(1,65536) && engine::tablet::stage(std::move(d),96u<<20),"bench document");
            engine::tablet::activate(0,0,1);engine::tablet::discard();
        }
        std::printf("pen-up bench save queue %s; no disk worker or concurrent GL\n",queue?"enabled":"disabled");
        Java_com_infinipaint_demo_NativeBridge_contextCreated(nullptr,nullptr);
        resize(1200,800);
        for(int n:{256,4096}) {
            std::vector<double> endUs, sampleUs, holdUs,waitUs;
            for(int rep=0;rep<101;++rep) {
                Java_com_infinipaint_demo_NativeBridge_clear(nullptr,nullptr);
                if(queue) while(auto g=engine::tablet::nextSave()) check(engine::tablet::acknowledge(g),"release queue");
                check(Java_com_infinipaint_demo_NativeBridge_begin(nullptr,nullptr,1,2),"bench begin");
                for(int i=0;i<n;++i) {
                    auto start=clock::now();
                    check(sample(i,12+i*.01),"bench sample");
                    sampleUs.push_back(std::chrono::duration<double,std::micro>(clock::now()-start).count());
                }
                if(rep==100) render(2); // live buffers exist at publication
                auto start=clock::now();
                check(Java_com_infinipaint_demo_NativeBridge_end(nullptr,nullptr,1),"bench end");
                endUs.push_back(std::chrono::duration<double,std::micro>(clock::now()-start).count());
                holdUs.push_back(lastHoldNs*.001);
                waitUs.push_back(lastWaitNs*.001);
                if(rep==100) {
                    transferBytes=0;bufferAllocs=0;draws=0;resolves=0;maskClears=errorChecks=0;clearedPixels=0;render(2);
                    std::printf("n=%d first_finished_spline payload=%llu storage_calls=%llu retained_bytes=%zu geometry_draws=%llu resolves=%u mask_clears=%u scissor_pixels=%zu error_checks=%u\n",
                        n,(unsigned long long)transferBytes,(unsigned long long)bufferAllocs,gpuContents.at(lastDrawBuffer).size(),
                        (unsigned long long)draws,resolves,maskClears,clearedPixels,errorChecks);
                    check(!bufferAllocs && transferBytes>0 && transferBytes<16000 && resolves==1 && draws==1,
                          "completed frame adopts prefix and resolves once");
                }
            }
            for(auto* v:{&sampleUs,&endUs,&holdUs,&waitUs}) {
                std::sort(v->begin(),v->end());
                std::printf("n=%d %s_us p50/p95/p99/max %.3f %.3f %.3f %.3f\n",n,
                    v==&endUs?"end":v==&holdUs?"hold":v==&waitUs?"wait":"sample",(*v)[v->size()/2],(*v)[v->size()*95/100],
                    (*v)[v->size()*99/100],v->back());
            }
        }
        return 0;
    }
    auto createContext=[&]{
        buffers.clear();gpuContents.clear();boundBuffer=0;lastDrawBuffer=0;
        const auto deletes=deletedObjects;
        Java_com_infinipaint_demo_NativeBridge_contextCreated(nullptr,nullptr);
        check(deletes==deletedObjects,"new context does not delete stale GL names");
        resize(1200,800);
    };
    createContext();
    for(int i=0;i<3;++i) {
        failAfter=i;
        check(!Java_com_infinipaint_demo_NativeBridge_begin(nullptr,nullptr,1,2),"begin allocation failure");
        failAfter=-1;
    }
    bool retried=false;
    for(int i=0;i<128 && !retried;++i) {
        check(Java_com_infinipaint_demo_NativeBridge_begin(nullptr,nullptr,1,2),"fault fixture begin");
        check(sample(0,1),"fault fixture sample");
        failAfter=0;
        auto ended=Java_com_infinipaint_demo_NativeBridge_end(nullptr,nullptr,1);
        failAfter=-1;
        if(!ended) {
            check(sample(1,2) && Java_com_infinipaint_demo_NativeBridge_end(nullptr,nullptr,1),
                  "failed completion slot leaves trace retryable");
            retried=true;
        }
    }
    check(retried,"exercised completion allocation failure");
    Java_com_infinipaint_demo_NativeBridge_clear(nullptr,nullptr);
    int injected=0;
    for(int i=0;i<28;++i) {
        check(Java_com_infinipaint_demo_NativeBridge_begin(nullptr,nullptr,1,2) && sample(0,1),"transaction setup");
        failAfter=i;
        const bool ok=sample(1,2);
        failAfter=-1;
        if(!ok) {
            check(sample(1,1.5),"failed sample rolled back time and filter");
            ++injected;
        }
        check(Java_com_infinipaint_demo_NativeBridge_end(nullptr,nullptr,1),"transaction finish");
        Java_com_infinipaint_demo_NativeBridge_clear(nullptr,nullptr);
    }
    check(injected>=8,"exercised input, spline and mesh allocation failures");
    Java_com_infinipaint_demo_NativeBridge_setCamera(nullptr,nullptr,0,0,1);
    Java_com_infinipaint_demo_NativeBridge_setSmoothingPreset(nullptr,nullptr,1);
    check(Java_com_infinipaint_demo_NativeBridge_begin(nullptr,nullptr,1,2)==JNI_TRUE,"begin stroke");
    for(int i=0;i<150;i++)check(sample(i,12.0+i*0.01),"sample 0-149 accepted");
    render(0);
    const auto firstLiveBytes=transferBytes;
    createContext();
    draws=0;transferBytes=0;render(0);
    check(draws==2 && transferBytes==firstLiveBytes,"active mesh and cursor restored after context loss");
    for(int i=150;i<180;i++)check(sample(i,12.0+i*0.01),"sample 150-179 accepted");
    render(0); // geometric growth; this allocation correctly transfers the full mesh
    for(int i=180;i<190;i++)check(sample(i,12.0+i*0.01),"sample 180-189 accepted");
    transferBytes=0;render(0);
    const auto incrementalBytes=transferBytes;
    check(incrementalBytes>0 && incrementalBytes < 2500,"later render appends only small vertex delta");
    check(sample(190,12+189*0.01),"same-timestamp different point accepted");
    render(2); // live provisional spline must be drawable
    check(Java_com_infinipaint_demo_NativeBridge_end(nullptr,nullptr,1)==JNI_TRUE,"end stroke");
    transferBytes=0;render(0);check(transferBytes>0,"finished raw uploaded once");
    auto rawGeometry=drawnVertices();
    transferBytes=0;render(0);check(transferBytes==0,"finished raw not reuploaded");
    transferBytes=0;render(1);check(transferBytes>0,"filtered mode uploaded on demand");
    auto filteredGeometry=drawnVertices();
    check(rawGeometry!=filteredGeometry,"One Euro produces non-raw geometry");
    transferBytes=0;render(1);check(transferBytes==0,"filtered mode cached");
    transferBytes=0;render(2);check(transferBytes>0,"spline mode uploaded on demand");
    auto splineGeometry=drawnVertices();
    check(splineGeometry!=filteredGeometry,"spline produces distinct tessellated geometry");
    Java_com_infinipaint_demo_NativeBridge_setCamera(nullptr,nullptr,5000,5000,1);
    draws=0;transferBytes=0;render(2);check(draws==0 && transferBytes==0,"off-screen stroke culled");
    Java_com_infinipaint_demo_NativeBridge_setCamera(nullptr,nullptr,0,0,2);
    draws=0;transferBytes=0;render(2);check(draws==1 && transferBytes==0,"zoom reuses visible geometry");
    const auto oldNextId=nextId,oldDeletes=deletedObjects;
    resize(1300,900);
    check(viewportWidth==1300 && viewportHeight==900,"resize changes viewport");
    draws=0;transferBytes=0;render(2);
    check(draws==1 && transferBytes==0 && nextId==oldNextId && deletedObjects==oldDeletes,
          "resize retains program and cached buffers");
    createContext();
    draws=0;transferBytes=0;render(2);
    check(draws==1 && transferBytes==splineGeometry.size() && gpuContents[lastDrawBuffer]==splineGeometry,
          "context loss rebuilds only the requested completed geometry");
    Java_com_infinipaint_demo_NativeBridge_clear(nullptr,nullptr);
    draws=0;render(2);check(draws==0 && buffers.size()==1,"clear retires all stroke buffers");
    // Strong preset must actually change the filtered mesh for identical input.
    Java_com_infinipaint_demo_NativeBridge_setCamera(nullptr,nullptr,0,0,1);
    Java_com_infinipaint_demo_NativeBridge_setSmoothingPreset(nullptr,nullptr,2);
    check(Java_com_infinipaint_demo_NativeBridge_begin(nullptr,nullptr,1,2)==JNI_TRUE,"begin strong smoothing");
    for(int i=0;i<190;i++)check(sample(i,12.0+i*0.01),"strong smoothing sample accepted");
    check(sample(190,12+189*0.01),"strong smoothing duplicate accepted");
    check(Java_com_infinipaint_demo_NativeBridge_end(nullptr,nullptr,1)==JNI_TRUE,"end strong smoothing");
    render(1);
    auto strongGeometry=drawnVertices();
    check(strongGeometry!=filteredGeometry,"smoothing preset changes the output");
    // Exercise JNI screen input -> world vertex -> camera uniforms (no shader execution).
    Java_com_infinipaint_demo_NativeBridge_clear(nullptr,nullptr);
    Java_com_infinipaint_demo_NativeBridge_setCamera(nullptr,nullptr,-160,580,2);
    check(Java_com_infinipaint_demo_NativeBridge_begin(nullptr,nullptr,1,2)==JNI_TRUE,"begin transformed stroke");
    check(sample(0,15) && sample(1,15.01),"transformed samples accepted");
    check(Java_com_infinipaint_demo_NativeBridge_end(nullptr,nullptr,1)==JNI_TRUE,"end transformed stroke");
    draws=0;render(0);
    float xy[2];
    check(draws==1 && gpuContents[lastDrawBuffer].size()>=sizeof(xy),"transformed raw mesh visible");
    std::memcpy(xy,gpuContents[lastDrawBuffer].data(),sizeof(xy));
    check(xy[0]==-110 && xy[1]==705,"camera converts first screen sample to world");
    check((xy[0]-uniformPanX)*uniformZoom==100 && (xy[1]-uniformPanY)*uniformZoom==250,
          "world vertex and actual projection uniforms reconstruct screen sample");
    // A fresh context/full upload is the byte oracle for each ownership path.
    for(int scenario=0;scenario<6;++scenario) {
        Java_com_infinipaint_demo_NativeBridge_clear(nullptr,nullptr);
        Java_com_infinipaint_demo_NativeBridge_setCamera(nullptr,nullptr,0,0,1);
        auto begin=[&]{check(Java_com_infinipaint_demo_NativeBridge_begin(nullptr,nullptr,1,2),"ownership begin");};
        auto feed=[&](int a,int b){for(int i=a;i<b;++i)check(sample(i,1+i*.01),"ownership sample");};
        begin(); feed(0,160);
        failUpload=scenario==3;
        render(2);
        if(scenario==0) feed(160,350); // no final live render, capacity miss
        if(scenario==1 || scenario==2) {
            if(scenario==1) Java_com_infinipaint_demo_NativeBridge_cancel(nullptr,nullptr,1);
            else Java_com_infinipaint_demo_NativeBridge_clear(nullptr,nullptr);
            begin(); feed(3,190); // same pointer, distinct generation; no intervening frame
        }
        if(scenario==4) createContext();
        check(Java_com_infinipaint_demo_NativeBridge_end(nullptr,nullptr,1),"ownership complete");
        if(scenario==5) {
            Java_com_infinipaint_demo_NativeBridge_setCamera(nullptr,nullptr,5000,5000,1);
            transferBytes=0; render(0); check(!transferBytes,"culled adoption remains lazy");
            Java_com_infinipaint_demo_NativeBridge_setCamera(nullptr,nullptr,0,0,1);
        }
        bufferAllocs=0;transferBytes=0;render(2);
        if(scenario<5)check(bufferAllocs==1,"unsafe prefix not adopted");
        else check(bufferAllocs==0,"culled adequate prefix adopted");
        auto actual=drawnVertices();
        createContext();render(2);
        check(actual==drawnVertices(),"completed bytes match clean context rebuild");
    }
    Java_com_infinipaint_demo_NativeBridge_clear(nullptr,nullptr);
    for(int i=0;i<2;i++) {
        check(Java_com_infinipaint_demo_NativeBridge_begin(nullptr,nullptr,i,2),"cache fixture begin");
        check(Java_com_infinipaint_demo_NativeBridge_sample(nullptr,nullptr,i,40+i*10,40,1,1,0,0),"cache fixture sample");
        check(Java_com_infinipaint_demo_NativeBridge_end(nullptr,nullptr,i),"cache fixture end");
    }
    render(2);Java_com_infinipaint_demo_NativeBridge_suspend(nullptr,nullptr,true);
    transferBytes=0;bufferAllocs=0;
    check(!Java_com_infinipaint_demo_NativeBridge_command(nullptr,nullptr,1,nullptr,0),"undo add");
    render(2);check(!transferBytes && !bufferAllocs,"unrelated geometry upload after undo");
    check(!Java_com_infinipaint_demo_NativeBridge_command(nullptr,nullptr,2,nullptr,0),"redo add");
    render(2);check(bufferAllocs==1,"redo uploaded more than restored record");
    std::printf("PASS bridge: incremental VBO, real One-Euro/spline/preset geometry differences, lazy uploads, culling, zoom, same-time, context restore, clear, zero unrelated undo upload; incremental_bytes=%llu\n",(unsigned long long)incrementalBytes);
}
