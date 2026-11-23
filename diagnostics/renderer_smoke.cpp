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

extern "C" {
void Java_com_infinipaint_demo_NativeBridge_surface(JNIEnv*,jclass,jint,jint);
void Java_com_infinipaint_demo_NativeBridge_setCamera(JNIEnv*,jclass,jdouble,jdouble,jdouble);
void Java_com_infinipaint_demo_NativeBridge_setSmoothingPreset(JNIEnv*,jclass,jint);
jboolean Java_com_infinipaint_demo_NativeBridge_begin(JNIEnv*,jclass,jint,jint);
jboolean Java_com_infinipaint_demo_NativeBridge_sample(JNIEnv*,jclass,jint,jdouble,jdouble,jdouble,jfloat,jfloat,jfloat);
jboolean Java_com_infinipaint_demo_NativeBridge_end(JNIEnv*,jclass,jint);
void Java_com_infinipaint_demo_NativeBridge_clear(JNIEnv*,jclass);
void Java_com_infinipaint_demo_NativeBridge_render(JNIEnv*,jclass,jint);
}
static unsigned nextId=1;
static uint64_t transferBytes=0, bufferAllocs=0, draws=0;
static std::set<unsigned> buffers;
static GLuint boundBuffer = 0, lastDrawBuffer = 0;
static std::map<unsigned,std::vector<uint8_t>> gpuContents;
extern "C" {
int __android_log_print(int,const char*,const char*,...) { return 0; }
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
void glDeleteProgram(GLuint) {}
GLboolean glIsProgram(GLuint) {return 1;}
GLboolean glIsBuffer(GLuint id) {return buffers.count(id)!=0;}
void glDeleteBuffers(GLsizei n,const GLuint*ids) {for(int i=0;i<n;i++){buffers.erase(ids[i]);gpuContents.erase(ids[i]);}}
void glGenBuffers(GLsizei n,GLuint* ids){for(int i=0;i<n;i++) {ids[i]=nextId++;buffers.insert(ids[i]);}}
void glBindBuffer(GLenum,GLuint b) {boundBuffer=b;}
void glBufferData(GLenum,GLsizeiptr size,const void* ptr,GLenum){
    ++bufferAllocs;
    auto& bytes=gpuContents[boundBuffer];bytes.assign((size_t)size,0);
    if(ptr){transferBytes+=size;std::memcpy(bytes.data(),ptr,(size_t)size);}
}
void glBufferSubData(GLenum,GLintptr offset,GLsizeiptr size,const void* data) {
    transferBytes+=size;
    auto& bytes=gpuContents[boundBuffer];
    if(offset+size>(GLintptr)bytes.size())std::abort();
    std::memcpy(bytes.data()+offset,data,(size_t)size);
}
void glViewport(GLint,GLint,GLsizei,GLsizei) {}
GLint glGetAttribLocation(GLuint,const GLchar*) {return 0;}
GLint glGetUniformLocation(GLuint,const GLchar*) {return 0;}
void glUseProgram(GLuint) {}
void glUniform4f(GLint,GLfloat,GLfloat,GLfloat,GLfloat) {}
void glUniform2f(GLint,GLfloat,GLfloat) {}
void glUniform1f(GLint,GLfloat) {}
void glVertexAttribPointer(GLuint,GLint,GLenum,GLboolean,GLsizei,const void*) {}
void glEnableVertexAttribArray(GLuint) {}
void glDrawArrays(GLenum,GLint,GLsizei) {++draws;lastDrawBuffer=boundBuffer;}
void glClearColor(GLfloat,GLfloat,GLfloat,GLfloat) {}
void glClear(GLbitfield) {}
void glEnable(GLenum) {}
void glBlendFunc(GLenum,GLenum) {}
}
static void check(bool ok,const char* msg){if(!ok){std::fprintf(stderr,"FAIL: %s\n",msg);std::exit(1);}}
int main(){
    auto render=[](int mode){Java_com_infinipaint_demo_NativeBridge_render(nullptr,nullptr,mode);};
    auto sample=[](int n,double timestamp){return Java_com_infinipaint_demo_NativeBridge_sample(nullptr,nullptr,1,
        100+3*n,250+25*std::sin(n*0.13),timestamp,0.6f,0,0)==JNI_TRUE;};
    Java_com_infinipaint_demo_NativeBridge_surface(nullptr,nullptr,1200,800);
    Java_com_infinipaint_demo_NativeBridge_setCamera(nullptr,nullptr,0,0,1);
    Java_com_infinipaint_demo_NativeBridge_setSmoothingPreset(nullptr,nullptr,1);
    check(Java_com_infinipaint_demo_NativeBridge_begin(nullptr,nullptr,1,2)==JNI_TRUE,"begin stroke");
    for(int i=0;i<150;i++)check(sample(i,12.0+i*0.01),"sample 0-149 accepted");
    render(0);
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
    auto rawGeometry=gpuContents[lastDrawBuffer];
    transferBytes=0;render(0);check(transferBytes==0,"finished raw not reuploaded");
    transferBytes=0;render(1);check(transferBytes>0,"filtered mode uploaded on demand");
    auto filteredGeometry=gpuContents[lastDrawBuffer];
    check(rawGeometry!=filteredGeometry,"One Euro produces non-raw geometry");
    transferBytes=0;render(1);check(transferBytes==0,"filtered mode cached");
    transferBytes=0;render(2);check(transferBytes>0,"spline mode uploaded on demand");
    auto splineGeometry=gpuContents[lastDrawBuffer];
    check(splineGeometry!=filteredGeometry,"spline produces distinct tessellated geometry");
    Java_com_infinipaint_demo_NativeBridge_setCamera(nullptr,nullptr,5000,5000,1);
    draws=0;transferBytes=0;render(2);check(draws==0 && transferBytes==0,"off-screen stroke culled");
    Java_com_infinipaint_demo_NativeBridge_setCamera(nullptr,nullptr,0,0,2);
    draws=0;transferBytes=0;render(2);check(draws==1 && transferBytes==0,"zoom reuses visible geometry");
    Java_com_infinipaint_demo_NativeBridge_surface(nullptr,nullptr,1200,800); // context recreation
    transferBytes=0;render(2);check(transferBytes>0,"surface rebuilds cached geometry");
    Java_com_infinipaint_demo_NativeBridge_clear(nullptr,nullptr);
    draws=0;render(2);check(draws==0,"clear retires GPU buffers");
    // Strong preset must actually change the filtered mesh for identical input.
    Java_com_infinipaint_demo_NativeBridge_setCamera(nullptr,nullptr,0,0,1);
    Java_com_infinipaint_demo_NativeBridge_setSmoothingPreset(nullptr,nullptr,2);
    check(Java_com_infinipaint_demo_NativeBridge_begin(nullptr,nullptr,1,2)==JNI_TRUE,"begin strong smoothing");
    for(int i=0;i<190;i++)check(sample(i,12.0+i*0.01),"strong smoothing sample accepted");
    check(sample(190,12+189*0.01),"strong smoothing duplicate accepted");
    check(Java_com_infinipaint_demo_NativeBridge_end(nullptr,nullptr,1)==JNI_TRUE,"end strong smoothing");
    render(1);
    auto strongGeometry=gpuContents[lastDrawBuffer];
    check(strongGeometry!=filteredGeometry,"smoothing preset changes the output");
    std::printf("PASS bridge: incremental VBO, real One-Euro/spline/preset geometry differences, lazy uploads, culling, zoom, same-time, context restore, clear; incremental_bytes=%llu\n",(unsigned long long)incrementalBytes);
}
