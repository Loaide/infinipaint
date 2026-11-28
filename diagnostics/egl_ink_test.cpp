// Linux host regression. All EGL and GLES symbols come from the real driver;
// only Android logging is shimmed. Run with EGL_PLATFORM=surfaceless,
// LIBGL_ALWAYS_SOFTWARE=1 and MESA_GLES_VERSION_OVERRIDE=2.0 on Mesa.
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <android/log.h>
#include <jni.h>
#include <array>
#ifdef IP_REPLAY_TEST
#include "tablet/session.hpp"
extern "C" bool ipTestCodecReplay();
extern "C" bool ipTestMoveBatch(int64_t,int64_t,double);
extern "C" bool ipTestMiddleSplice(int64_t,int64_t,int);
extern "C" void Java_com_infinipaint_demo_NativeBridge_suspend(JNIEnv*,jclass,jboolean);
extern "C" jint Java_com_infinipaint_demo_NativeBridge_command(JNIEnv*,jclass,jint,jdoubleArray,jdouble);
#endif

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <thread>
#include <chrono>
#include <cmath>
namespace engine::tablet {void hostStartPlanner();void hostForcePlannerFallback(bool);std::array<uint64_t,8> hostPlannerStats();std::array<uint64_t,39> hostSpatialStats();std::array<uint64_t,32> hostPaintOrderStats();std::array<uint64_t,5> hostSpatialFrameStats();void hostSpatialFault(int);std::array<uint64_t,34> hostSpanStats();std::array<uint64_t,15> hostCompactionStats();}
extern "C" jboolean Java_com_infinipaint_demo_NativeBridge_needsPublicationRetry(JNIEnv*,jclass);

static int observedMax=-1;
static bool planTest=false;
static unsigned planComparisons=0;
extern "C" {
void Java_com_infinipaint_demo_NativeBridge_contextCreated(JNIEnv*, jclass);
void Java_com_infinipaint_demo_NativeBridge_resizeViewport(JNIEnv*, jclass, jint, jint);
void Java_com_infinipaint_demo_NativeBridge_setCamera(JNIEnv*, jclass, jdouble, jdouble, jdouble);
void Java_com_infinipaint_demo_NativeBridge_setSmoothingPreset(JNIEnv*, jclass, jint);
jboolean Java_com_infinipaint_demo_NativeBridge_begin(JNIEnv*, jclass, jint, jint);
jboolean Java_com_infinipaint_demo_NativeBridge_sample(
    JNIEnv*, jclass, jint, jdouble, jdouble, jdouble, jfloat, jfloat, jfloat);
jboolean Java_com_infinipaint_demo_NativeBridge_end(JNIEnv*, jclass, jint);
void Java_com_infinipaint_demo_NativeBridge_clear(JNIEnv*, jclass);
void Java_com_infinipaint_demo_NativeBridge_render(JNIEnv*, jclass, jint);

int __android_log_print(int priority, const char* tag, const char* format, ...) {
    if(!std::strcmp(tag,"IP_COVERAGE") && std::strstr(format,"max_blend=")) {
        va_list a; va_start(a,format); (void)va_arg(a,int); observedMax=va_arg(a,int); va_end(a);
    }
    if (priority < ANDROID_LOG_ERROR) return 0;
    std::fprintf(stderr, "%s: ", tag);
    va_list args;
    va_start(args, format);
    const int result = std::vfprintf(stderr, format, args);
    va_end(args);
    std::fputc('\n', stderr);
    return result;
}
}

namespace {
constexpr int width = 128, height = 64;
constexpr int raw = 0, spline = 2, composite = 3;
constexpr int dotX = 23, dotY = 19, lineY = 37;
constexpr int inkThreshold = 4; // RGB levels above read-back background
using Pixel = std::array<GLubyte, 4>;
static_assert(sizeof(Pixel) == 4);
using Frame = std::array<Pixel, width * height>;

void check(bool ok, const char* message) {
    if (ok) return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(EXIT_FAILURE);
}

void eglCheck(bool ok, const char* step) {
    if (ok) return;
    std::fprintf(stderr, "FAIL: %s (EGL error 0x%04x)\n", step, unsigned(eglGetError()));
    std::exit(EXIT_FAILURE);
}

void glCheck(const char* step) {
    const GLenum error = glGetError();
    if (error == GL_NO_ERROR) return;
    std::fprintf(stderr, "FAIL: %s (GL error 0x%04x)\n", step, unsigned(error));
    std::exit(EXIT_FAILURE);
}

struct Pbuffer {
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
    EGLConfig config=nullptr;

    Pbuffer() {
        eglCheck(display != EGL_NO_DISPLAY, "eglGetDisplay");
        eglCheck(eglInitialize(display, nullptr, nullptr), "eglInitialize (try EGL_PLATFORM=surfaceless)");
        eglCheck(eglBindAPI(EGL_OPENGL_ES_API), "eglBindAPI");
        const EGLint format[] = {
            EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE
        };
        EGLint count = 0;
        eglCheck(eglChooseConfig(display, format, nullptr, 0, &count), "eglChooseConfig count");
        check(count > 0, "no candidate EGL pbuffer configs");
        std::vector<EGLConfig> configs(count);
        eglCheck(eglChooseConfig(display, format, configs.data(), count, &count), "eglChooseConfig");
        for (int i = 0; i < count; ++i) {
            const auto value = [&](EGLint attribute) {
                EGLint result = 0;
                eglCheck(eglGetConfigAttrib(display, configs[i], attribute, &result), "eglGetConfigAttrib");
                return result;
            };
            if (value(EGL_RED_SIZE) == 8 && value(EGL_GREEN_SIZE) == 8 &&
                value(EGL_BLUE_SIZE) == 8 && value(EGL_ALPHA_SIZE) == 8 &&
                value(EGL_SAMPLE_BUFFERS) == 0) {
                config = configs[i];
                break;
            }
        }
        check(config != nullptr, "no single-sample RGBA8 EGL pbuffer config");
        const EGLint size[] = {EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE};
        surface = eglCreatePbufferSurface(display, config, size);
        eglCheck(surface != EGL_NO_SURFACE, "eglCreatePbufferSurface");
        const EGLint version[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
        context = eglCreateContext(display, config, EGL_NO_CONTEXT, version);
        eglCheck(context != EGL_NO_CONTEXT, "eglCreateContext");
        eglCheck(eglMakeCurrent(display, surface, surface, context), "eglMakeCurrent");
    }

    ~Pbuffer() {
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(display, context);
        eglDestroySurface(display, surface);
        eglTerminate(display);
    }
    void recreate() {
        eglCheck(eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT),"unbind");
        eglCheck(eglDestroyContext(display,context),"destroy old context");
        const EGLint attrs[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
        context=eglCreateContext(display,config,EGL_NO_CONTEXT,attrs);
        eglCheck(context!=EGL_NO_CONTEXT && eglMakeCurrent(display,surface,surface,context),"new context");
        glDisable(GL_DITHER);
        Java_com_infinipaint_demo_NativeBridge_contextCreated(nullptr,nullptr);
        Java_com_infinipaint_demo_NativeBridge_resizeViewport(nullptr,nullptr,width,height);
    }
};

const Pixel& at(const Frame& frame, int x, int y) {
    // JNI receives top-left screen coordinates; glReadPixels starts at bottom left.
    return frame[(height - 1 - y) * width + x];
}

int rgbDelta(const Pixel& a, const Pixel& b) {
    int delta = 0;
    for (int c = 0; c < 3; ++c) delta = std::max(delta, std::abs(int(a[c]) - int(b[c])));
    return delta;
}

struct Bounds {
    int left = width, top = height, right = -1, bottom = -1;
    bool within(int l, int t, int r, int b) const {
        return left <= right && top <= bottom && left >= l && top >= t && right <= r && bottom <= b;
    }
};

Bounds inkBounds(const Frame& frame, const Pixel& background) {
    Bounds bounds;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (rgbDelta(at(frame, x, y), background) <= inkThreshold) continue;
            bounds.left = std::min(bounds.left, x);
            bounds.top = std::min(bounds.top, y);
            bounds.right = std::max(bounds.right, x);
            bounds.bottom = std::max(bounds.bottom, y);
        }
    }
    return bounds;
}

Frame render(int mode) {
    using namespace std::chrono_literals;
    using namespace engine::tablet;
    auto draw=[](int m){Java_com_infinipaint_demo_NativeBridge_render(nullptr,nullptr,m);glCheck("production render");};
    auto pixels=[](){Frame frame{};glReadPixels(0,0,width,height,GL_RGBA,GL_UNSIGNED_BYTE,frame.data());glCheck("RGBA8 glReadPixels");return frame;};
    if(!planTest) {draw(mode);return pixels();}
    auto wait=[&](bool matched) {
        const auto hits=hostPlannerStats()[1];
        const auto deadline=std::chrono::steady_clock::now()+5s;
        do {
            draw(mode);
            if(!Java_com_infinipaint_demo_NativeBridge_needsPublicationRetry(nullptr,nullptr) && (!matched || hostPlannerStats()[1]>hits)) return;
            std::this_thread::sleep_for(1ms);
        } while(std::chrono::steady_clock::now()<deadline);
        check(false,"planner did not deliver matching complete view");
    };
    hostForcePlannerFallback(true);wait(false);auto expected=pixels();
    hostForcePlannerFallback(false);wait(true);auto actual=pixels();
    check(actual==expected,"planned path differs from exact fallback RGBA");++planComparisons;return actual;
}

void clear() {
    Java_com_infinipaint_demo_NativeBridge_clear(nullptr, nullptr);
}

void begin(int id) {
    check(Java_com_infinipaint_demo_NativeBridge_begin(nullptr, nullptr, id, 2) == JNI_TRUE,
          "JNI begin");
}

void sample(int id, int x, int y, double time, float pressure) {
    // Coordinates land on pixel centers, including the cross-section center.
    check(Java_com_infinipaint_demo_NativeBridge_sample(
              nullptr, nullptr, id, x + 0.5, y + 0.5, time, pressure, 0, 0) == JNI_TRUE,
          "JNI sample");
}

void end(int id) {
    check(Java_com_infinipaint_demo_NativeBridge_end(nullptr, nullptr, id) == JNI_TRUE,
          "JNI end");
}

void beginDot(int id, float pressure) {
    begin(id);
    sample(id, dotX, dotY, 10.0, pressure);
}

void testDots(int mode, const Pixel& background) {
    for (float pressure : {0.0f, 1.0f}) {
        clear();
        beginDot(1, pressure);
        const auto live = render(mode);
        end(1);
        const auto completed = render(mode);
        const int fringeOffset = pressure == 0.0f ? 1 : 3;
        for (const Frame* frame : {&live, &completed}) {
            const int center = rgbDelta(at(*frame, dotX, dotY), background);
            const int fringe = rgbDelta(at(*frame, dotX + fringeOffset, dotY), background);
            check(center > 100 && fringe > 15 && fringe + 25 < center,
                  "dot must have a strong center and partial AA fringe");
            check(inkBounds(*frame, background).within(dotX - fringeOffset, dotY - fringeOffset,
                                                       dotX + fringeOffset, dotY + fringeOffset),
                  "dot exceeds expected pressure-dependent footprint");
        }
        for (std::size_t i = 0; i < live.size(); ++i)
            check(rgbDelta(live[i], completed[i]) <= 3, "dot changed on pen-up");
    }
}

double greenWidth(const Frame& frame, const Pixel& background, int x) {
    const int center = int(at(frame, x, lineY)[1]) - background[1];
    check(center > 100, "taper centerline missing or too faint");
    int sum = 0;
    for (int y = lineY - 4; y <= lineY + 4; ++y)
        sum += std::max(0, int(at(frame, x, y)[1]) - background[1]);
    return double(sum) / center; // effective coverage in pixels, normalized to center ink
}

void testTaper(int mode, const Pixel& background) {
    clear();
    begin(1);
    for (int i = 0; i < 5; ++i) sample(1, 20 + 22 * i, lineY, 10.0 + i, 0.25f * i);
    const auto live = render(mode);
    end(1);
    const auto completed = render(mode);
    for (const Frame* frame : {&live, &completed}) {
        const double thin = greenWidth(*frame, background, 30);
        const double mid1 = greenWidth(*frame, background, 53);
        const double mid2 = greenWidth(*frame, background, 75);
        const double thick = greenWidth(*frame, background, 98);
        std::printf("mode=%d %s width_px=%.2f/%.2f/%.2f/%.2f\n", mode,
                    frame == &live ? "live" : "completed", thin, mid1, mid2, thick);
        check(thin > 1.0 && thin < 2.7 && thick > 4.2 && thick < 6.3 &&
              mid1 > thin + 0.5 && mid2 > mid1 + 0.5 && thick > mid2 + 0.5,
              "taper must broaden measurably across four interior sections");
        for (int x = 20; x <= 108; ++x)
            check(rgbDelta(at(*frame, x, lineY), background) > 100, "taper has a centerline gap");
        check(inkBounds(*frame, background).within(17, lineY - 3, 112, lineY + 3),
              "taper exceeds expected analytic AA footprint");
    }
    const auto a = inkBounds(live, background);
    const auto b = inkBounds(completed, background);
    check(std::abs(a.left - b.left) <= 1 && std::abs(a.right - b.right) <= 1 &&
          std::abs(a.top - b.top) <= 1 && std::abs(a.bottom - b.bottom) <= 1,
          "taper bounds shifted on pen-up");
    int changed = 0, totalDelta = 0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int delta = rgbDelta(at(live, x, y), at(completed, x, y));
            if (delta <= 3) continue;
            check(x >= 103, "taper body changed on pen-up, away from the tip");
            ++changed;
            totalDelta += delta;
        }
    }
    std::printf("mode=%d pen_up changed_pixels=%d sum_max_rgb_delta=%d\n", mode, changed, totalDelta);
    check(changed <= 20 && totalDelta <= 512, "taper tip changed too much on pen-up");
}

void testCompletedOrder(const Pixel& background) {
    clear();
    beginDot(9, 1.0f);
    end(9);
    const auto thickRawFrame = render(raw);
    const auto thickCompositeFrame = render(composite);
    const Pixel thickRaw = at(thickRawFrame, dotX + 1, dotY);
    const Pixel thickComposite = at(thickCompositeFrame, dotX + 1, dotY);
    check(rgbDelta(thickRaw, background) > 100 && rgbDelta(thickRaw, thickComposite) <= 3,
          "composite must put opaque raw ink over the other modes");

    const auto overlapping = [](float first, float second) {
        clear();
        beginDot(9, first);
        end(9);
        render(composite); // First stroke is already cached when the second completes.
        beginDot(2, second);
        end(2);
        const auto frame = render(composite);
        return at(frame, dotX + 1, dotY);
    };
    // At distance one, the thick raw dot is opaque; the thin dot has
    // partial coverage. Its different-colored composite layers make
    // the two production draw orders distinguishable in actual pixels.
    const Pixel thickLast = overlapping(0.0f, 1.0f);
    const Pixel thinLast = overlapping(1.0f, 0.0f);
    std::printf("composite order probe red: thick_only=%u thick_last=%u thin_last=%u\n",
                unsigned(thickRaw[0]), unsigned(thickLast[0]), unsigned(thinLast[0]));
    check(rgbDelta(thickLast, thickRaw) <= 3 && int(thinLast[0]) - int(thickRaw[0]) >= 12,
          "overlapping completed strokes did not composite in completion order");
}

void testDwell(const Pixel& background) {
    clear(); begin(1);
    sample(1,20,lineY,10,0); sample(1,100,lineY,10,0);
    const auto before=render(spline);
    sample(1,100,lineY,10,1);
    const auto after=render(spline);
    for(int x=20;x<95;++x) for(int y=lineY-4;y<=lineY+4;++y)
        check(at(before,x,y)==at(after,x,y),"stationary pressure repainted earlier ink");
    check(rgbDelta(at(after,100,lineY+2),background)>100,"stationary deposit missing");
    end(1);
    check(render(spline)==after,"dwell changed on lift");
}
void measureJoinAA() {
    auto path=[](int step,bool ramp,bool repeat) {
        clear(); begin(1);
        auto send=[&](int x){sample(1,x,lineY,10,ramp?(x-20)/80.f:.5f);};
        for(int x=20;x<=100;x+=step) send(x);
        if(repeat) {
            for(int x=100;x>=20;x-=step) send(x);
            for(int x=20;x<=100;x+=step) send(x);
        }
        const auto live=render(raw); end(1);
        check(live==render(raw),"union changed on lift");
        return live;
    };
    auto compare=[](const Frame& a,const Frame& b) {
        int maximum=0;
        for(size_t i=0;i<a.size();++i) {
            maximum=std::max(maximum,rgbDelta(a[i],b[i]));
            check(a[i][3]==255 && b[i][3]==255,"window alpha not restored");
        }
        std::printf("equivalent path max RGB delta=%d/255\n",maximum);
        check(maximum<=2,"sample density or self-overlap changed coverage");
    };
    for(bool ramp:{false,true}) compare(path(80,ramp,false),path(1,ramp,true));
    // Preserve the original spline fixture, including both rounded end caps.
    auto curve=[](int step) {
        clear(); begin(1);
        for(int x=20;x<=100;x+=step) sample(1,x,lineY,10,.5f);
        end(1); return render(spline);
    };
    compare(curve(80),curve(1));
    auto corner=[](bool repeat) {
        clear(); begin(1);
        for(int i=0;i<(repeat?2:1);++i) {
            sample(1,40,12,10,.5);sample(1,62,48,10,.5);sample(1,45,15,10,.5);
            sample(1,62,48,10,.5);sample(1,40,12,10,.5);
        }
        end(1); return render(raw);
    };
    compare(corner(false),corner(true));
}
void comparePortableRamp() {
    const char* path=std::getenv("IP_RAMP_REFERENCE");
    if(!path) return;
    clear(); begin(1);
    sample(1,10,20,1,0); sample(1,117,31,1,1); end(1);
    const Frame frame=render(raw);
    if(observedMax) {
        FILE* f=std::fopen(path,"wb");
        check(f && std::fwrite(frame.data(),sizeof(Frame),1,f)==1,"write diagonal reference");
        std::fclose(f);
    } else {
        Frame ref;
        FILE* f=std::fopen(path,"rb");
        check(f && std::fread(ref.data(),sizeof(Frame),1,f)==1,"read diagonal reference");
        std::fclose(f);
        int max=0;
        for(size_t i=0;i<frame.size();++i) max=std::max(max,rgbDelta(frame[i],ref[i]));
        std::printf("diagonal taper fallback vs MAX max RGB delta=%d/255\n",max);
        check(max<=10,"fallback exceeds half a coverage step plus rounding");
    }
}

#ifdef IP_REPLAY_TEST
void canonicalSpanPixelOracle(Pbuffer& pbuffer) {
    using namespace engine::document;
    using namespace engine::tablet;
    // The preceding fixture intentionally staged a 1 MiB mesh budget.
    // Give this long-stroke test its own document with normal host headroom.
    Java_com_infinipaint_demo_NativeBridge_suspend(nullptr,nullptr,true);
    while(auto token=nextSave())check(acknowledge(token),"canonical fixture save ack");
    auto doc=std::make_unique<Document>(Uuid{88});
    check(doc->finishRestore(0,0)==Status::Ok&&doc->grant(1,100),"canonical fixture identity");
    check(stage(std::move(doc),96u<<20),"canonical fixture stage");activate(0,0,1);discard();
    Java_com_infinipaint_demo_NativeBridge_suspend(nullptr,nullptr,false);
    auto camera=[](double x,double y,double z){Java_com_infinipaint_demo_NativeBridge_setCamera(nullptr,nullptr,x,y,z);};
    camera(0,0,1);
    begin(99);
    double t=1;
    // Cross two persisted raw-chunk boundaries. Dwell, tied timestamps,
    // changing pressure, and integer-coordinate revisits are intentional.
    for(int i=0;i<4097;++i) {
        if(i%13!=0&&i%7!=0)t+=.003;
        const int x=-256+i/8;
        const int y=18+((i/23)%2?23-(i/11)%23:(i/11)%23);
        sample(99,x,y,t,float((i*17)%101)/100.f);
    }
    end(99);
    begin(100);sample(100,-300,32,2,.5f);sample(100,300,32,2,.9f);end(100);
    const std::array<std::array<double,3>,12> views{{
        {-256,0,1},{-128,0,1},{-64,0,1},{0,0,1},{128,0,1},{240,0,1},
        {-128,12,4},{-32,18,8},{16,18,16},{96,10,4},{-250,-20,.5},{0,0,1}}};
    std::vector<Frame> before;before.reserve(views.size()*4);
    for(auto v:views){camera(v[0],v[1],v[2]);for(int m=0;m<4;++m)before.push_back(render(m));}
    check(ipTestCodecReplay(),"multi-chunk v1 reconstruction differs at vertex bytes");
    pbuffer.recreate();size_t i=0;
    for(auto v:views){camera(v[0],v[1],v[2]);for(int m=0;m<4;++m)check(before[i++]==render(m),"multi-chunk reconstruction pixels differ");}
    camera(0,0,1);clear();
    while(auto token=nextSave())check(acknowledge(token),"canonical fixture final save ack");
    std::printf("PASS: 4097-sample canonical chunk boundary, exact vertex bytes, %zu four-mode RGBA comparisons after context recreation\n",i);
}
void refinedSpanPixelOracle(Pbuffer& pbuffer) {
    using namespace engine::document;using namespace engine::tablet;using namespace std::chrono_literals;
    if(!planTest)return;
    Java_com_infinipaint_demo_NativeBridge_suspend(nullptr,nullptr,true);
    while(auto token=nextSave())check(acknowledge(token),"span fixture save ack");
    auto doc=std::make_unique<Document>(Uuid{91});
    for(int row=0;row<2;++row){Brush b;b.radiusWorld=2.5;b.rgba=row?std::array<float,4>{.2f,.8f,.3f,.55f}:std::array<float,4>{.8f,.1f,.5f,.65f};engine::input::RawInputStroke points;
        for(int i=0;i<4097;++i)points.push_back({{double(-2048+i),31.+row*3+12*std::sin(double(i)/19)},1.+i*.003,float((i*17)%101)/100,0,0});
        Value v{std::make_shared<const Ink>(std::move(points),b),0,0};check(doc->restore(row+1,row,v,1,1,1,2)==Status::Ok,"span fixture restore");}
    check(doc->finishRestore(1,2)==Status::Ok&&doc->grant(3,100),"span fixture identity");check(stage(std::move(doc),96u<<20),"span fixture stage");activate(0,0,1);discard();Java_com_infinipaint_demo_NativeBridge_suspend(nullptr,nullptr,false);
    const auto old=hostSpanStats();unsigned comparisons=0;std::vector<Frame> frames;
    auto draw=[](int m){Java_com_infinipaint_demo_NativeBridge_render(nullptr,nullptr,m);glCheck("refined draw");Frame f{};glReadPixels(0,0,width,height,GL_RGBA,GL_UNSIGNED_BYTE,f.data());glCheck("refined RGBA");return f;};
    for(int pass=0;pass<2;++pass){if(pass)pbuffer.recreate();for(int i=0;i<24;++i){double x=-1800+i*145.25,y=4.+(i%3)*2.5,z=i%12==0?.1:i%12==1?16.:i%4==0?.75:1.+(i%4)*.75;
        if(z==16.)y=31.+12*std::sin((x+2048)/19)-2.;
        Java_com_infinipaint_demo_NativeBridge_setCamera(nullptr,nullptr,x,y,z);
        for(int m:{2,3}){hostForcePlannerFallback(true);Frame expected=draw(m);auto settle=std::chrono::steady_clock::now()+5s;while(Java_com_infinipaint_demo_NativeBridge_needsPublicationRetry(nullptr,nullptr)){check(std::chrono::steady_clock::now()<settle,"settle before reference");expected=draw(m);}hostForcePlannerFallback(false);auto count=hostSpanStats()[27];auto deadline=std::chrono::steady_clock::now()+8s;Frame actual{};
            do{actual=draw(m);if(hostSpanStats()[27]>count)break;std::this_thread::sleep_for(1ms);}while(std::chrono::steady_clock::now()<deadline);
            check(hostSpanStats()[27]>count,"actual span cut used");check(actual==expected,"refined span output differs from whole stroke");++comparisons;if(!pass)frames.push_back(actual);else check(actual==frames[comparisons-49],"refined context parity");}
    }}
    auto endStats=hostSpanStats();check(endStats[0]>old[0]&&endStats[11]>old[11]&&endStats[28]>old[28],"real span work and saved vertices");
    std::printf("PASS: %u exact refined RGBA frames, actual span rows=%llu skipped_vertices=%llu builds=%llu queries=%llu\n",comparisons,(unsigned long long)(endStats[27]-old[27]),(unsigned long long)(endStats[28]-old[28]),(unsigned long long)(endStats[0]-old[0]),(unsigned long long)(endStats[11]-old[11]));
    clear();while(auto token=nextSave())check(acknowledge(token),"span fixture final ack");
}
void compactedPixelOracle(const Pixel& background){
    using namespace engine::document;using namespace engine::tablet;
    if(!planTest)return;
    Java_com_infinipaint_demo_NativeBridge_suspend(nullptr,nullptr,true);
    auto save=[](){while(auto token=nextSave())check(acknowledge(token),"compaction fixture ack");};save();
    auto doc=std::make_unique<Document>(Uuid{93});Brush brush;brush.radiusWorld=3;brush.rgba={.3f,.8f,.4f,.65f};
    for(int i=1;i<=400;++i){engine::input::RawInputStroke rawPoint{{{double(i*32)+.5,100.5},1,.8f,0,0}};check(doc->restore(i,i-1,Value{std::make_shared<const Ink>(std::move(rawPoint),brush)},1,1,1,400)==Status::Ok,"compact restore");}
    check(doc->finishRestore(1,400)==Status::Ok&&doc->grant(401,600),"compact identity");check(stage(std::move(doc),96u<<20),"compact stage");activate(0,0,1);discard();
    Java_com_infinipaint_demo_NativeBridge_setCamera(nullptr,nullptr,0,0,1);auto blank=render(spline);check(at(blank,32,30)==background,"initial blank");auto before=hostSpatialStats();auto compact=hostCompactionStats();
    check(ipTestMoveBatch(1,128,-70),"first compaction batch");auto moved=render(spline);check(rgbDelta(at(moved,32,30),background)>8,"moved visible witness");save();
    check(ipTestMoveBatch(129,256,-70),"second compaction batch");check(render(spline)==moved,"offscreen batch leaves pixels");save();
    check(ipTestMoveBatch(257,257,-70),"trigger compact");check(render(spline)==moved,"compacted pixels match");save();
    check(hostCompactionStats()[1]==compact[1]+1&&hostSpatialStats()[7]==before[7],"actual localized compaction in planner, no rebuild");
    for(int i=0;i<24;++i){Java_com_infinipaint_demo_NativeBridge_setCamera(nullptr,nullptr,i*347.,0,i%2?1.:.75);(void)render(spline);}
    Java_com_infinipaint_demo_NativeBridge_setCamera(nullptr,nullptr,0,0,1);check(render(spline)==moved,"compaction round trip");
    std::puts("PASS: production localized compaction after 257 moved records, exact viewport RGBA and visible witness, zero rebuilds");clear();save();Java_com_infinipaint_demo_NativeBridge_suspend(nullptr,nullptr,false);
}
void spatialPixelOracle(Pbuffer& pbuffer,const Pixel& background) {
    using namespace engine::document;
    using namespace engine::tablet;
    using namespace std::chrono_literals;
    if(!planTest)return;
    const auto startComparisons=planComparisons;
    const auto startStats=hostSpatialStats();
    auto makeScene=[&](int version) {
        auto d=std::make_unique<Document>(Uuid{uint8_t(91+version)});
        constexpr int total=521;Id prev=0;
        for(int j=0;j<total;++j) {
            // Permute stable IDs independently of paint order and locality.
            const Id id=(j*127)%total+1;
            Brush brush;brush.radiusWorld=2.4f+float(j%3)*.3f;
            brush.rgba=j%2?std::array<float,4>{.9f,.1f,.2f,.45f}:std::array<float,4>{.1f,.2f,.95f,.55f};
            engine::input::RawInputStroke samples;
            if(j==0) {
                // Independent coverage witness across many spatial regions.
                brush.radiusWorld=2.8f;brush.rgba={.1f,.9f,.2f,1};
                samples={{{-4096.,32.5},1,.8f,0,0},{{4096.,32.5},2,.8f,0,0}};
            }else if(j<16) {
                // Both endpoints outside the viewport. Long crossing chords,
                // self-overlap, changing width and reversed paint order.
                const double y=5.5+double(j%8)*6.0;
                const double left=j%2?-3500.-j*120.:-200.,right=j%2?200.:3500.+j*120.;
                samples={{{left,y},1,.35f,0,0},{{-8.5,y-4.0},2,.85f,0,0},
                    {{55.5,y+4.0},3,.65f,0,0},{{right,y},4,.4f,0,0}};
                if(j%4==0)samples.push_back({{left,y},5,.75f,0,0});
            } else {
                const int k=j-16;
                // Distributed dots on both sides of STR leaf boundaries and
                // across negative world coordinates; their IDs are scattered.
                const double x=double((k%37)-18)*193.0+.5,y=double((k/37)-6)*173.0+.5;
                samples={{{x,y},1,float(j%9)/8.f,0,0}};
            }
            if(version)std::swap(brush.rgba[0],brush.rgba[2]);
            check(d->restore(id,prev,Value{std::make_shared<const Ink>(std::move(samples),brush)},1,1,1,total)==Status::Ok,"restore spatial pixel fixture");prev=id;
        }
        check(d->finishRestore(1,total)==Status::Ok&&d->grant(total+1,total+200),"spatial fixture restore");
        return d;
    };
    Java_com_infinipaint_demo_NativeBridge_suspend(nullptr,nullptr,true);
    check(stage(makeScene(0),96u<<20),"stage multi-page spatial scene");activate(0,0,1);discard();
    auto camera=[](double x,double y,double z){Java_com_infinipaint_demo_NativeBridge_setCamera(nullptr,nullptr,x,y,z);};
    auto original=render(spline);
    check(rgbDelta(at(original,60,24),background)>8,"crossing ink actually rendered");
    check(hostSpatialStats()[0]==1&&hostSpatialStats()[1]>=521&&hostSpatialStats()[9]>startStats[9],"disk-backed spatial path used, not just resident fallback");
    const auto motionStart=hostSpatialFrameStats();
    const auto faultFreeStart=hostSpatialStats();
    // A crossing stroke with endpoints far outside the viewport must never
    // develop a gap at any spatial page boundary. The resident oracle alone
    // would not catch a preexisting error in both paths, so assert coverage.
    for(int i=-8;i<=8;++i){camera(i*384.,0,1);const auto frame=render(spline);
        for(int x=0;x<width;++x)check(rgbDelta(at(frame,x,32),background)>30,"long crossing centerline gap");}
    // Trace through packed pages and prefetch boundaries, including the
    // smallest/largest supported zoom and subpixel camera offsets.
    for(int i=0;i<70;++i) {
        const double x=(i<35?i:69-i)*17.25-288.0;
        const double y=double((i*13)%21)-10.25;
        const double z=i%14==0?.1:i%13==0?16.0:i%5==0?2.0:1.0;
        camera(x,y,z);(void)render(i%4);
    }
    for(int i=0;i<36;++i){camera(double((i%12)-6)*193+.5,double((i/12)-1)*173+.5,1.0);(void)render(spline);}
    camera(0,0,1);check(render(spline)==original,"round-trip viewport parity across spatial pages");
    const auto motionEnd=hostSpatialFrameStats();const auto faultFreeEnd=hostSpatialStats();
    check(motionEnd[0]>motionStart[0]+100&&motionEnd[1]==motionStart[1]&&motionEnd[2]>motionStart[2],"indexed and reused-envelope frame path exercised without linear worker plans");
    check(faultFreeEnd[11]==faultFreeStart[11]&&faultFreeEnd[12]==faultFreeStart[12]&&faultFreeEnd[13]==faultFreeStart[13],"no hidden index fallback/fault during motion");
    pbuffer.recreate();check(render(spline)==original,"spatial context recovery");
    // Deterministic disk fault. The current generation must stop answering;
    // a full resident reference plan remains valid, never partial/blank ink.
    auto faults=hostSpatialStats()[13];hostSpatialFault(1);
    const auto deadline=std::chrono::steady_clock::now()+5s;
    while(hostSpatialStats()[13]==faults){(void)render(spline);check(std::chrono::steady_clock::now()<deadline,"page corruption handled");std::this_thread::sleep_for(1ms);}
    check(render(spline)==original&&!hostSpatialStats()[0],"corruption fallback pixel equality");
    // Switch uses same local IDs, different epoch and color order. Old
    // scratch roots and cache ranks must not survive the transition.
    check(stage(makeScene(1),96u<<20),"stage second spatial scene");activate(0,0,1);discard();
    auto switched=render(spline);check(switched!=original&&hostSpatialStats()[0]==1,"new epoch rebuild and paint colors");
    // Append overlay, remove via undo and reinsert via redo; exact pixels
    // at each revision, not just a numeric order oracle.
    Java_com_infinipaint_demo_NativeBridge_suspend(nullptr,nullptr,false);
    const auto beforeAppend=hostSpatialStats();
    begin(77);sample(77,41,26,20,1);sample(77,88,26,20.1,.6f);end(77);
    auto appended=render(spline);const auto afterAppend=hostSpatialStats();
    check(appended!=switched&&afterAppend[3]>0&&afterAppend[7]==beforeAppend[7]&&afterAppend[8]>beforeAppend[8]&&afterAppend[2]>0,"appended overlay appears over unchanged packed base");
    Java_com_infinipaint_demo_NativeBridge_suspend(nullptr,nullptr,true);
    check(Java_com_infinipaint_demo_NativeBridge_command(nullptr,nullptr,1,nullptr,0)==0,"spatial undo");
    check(render(spline)==switched,"undo removes overlay without stale pixels");
    check(Java_com_infinipaint_demo_NativeBridge_command(nullptr,nullptr,2,nullptr,0)==0,"spatial redo");
    check(render(spline)==appended&&hostSpatialStats()[7]==beforeAppend[7],"redo restores exact overlay order without rebuilding base");
    // Insert/remove in the middle of overlapping translucent ink. Check
    // every revision against the exact resident renderer and insist that the
    // counted order path handles the splices without rebuilding both roots.
    // This host fixture has no SQLite writer. Acknowledge the simulated
    // queue between edit rounds; the test is about ordering, not queue fill.
    auto drainSaves=[](){while(auto token=nextSave())check(acknowledge(token),"host simulated save acknowledgement");};
    drainSaves();
    const auto beforeMiddle=hostSpatialStats();const auto orderBefore=hostPaintOrderStats();
    check(ipTestMiddleSplice(1,128,0),"canonical middle replace splice");
    auto middle=render(spline);check(middle!=appended,"middle splice has a visible witness");
    const auto orderAfter=hostPaintOrderStats();
    check(hostSpatialStats()[7]==beforeMiddle[7]&&orderAfter[14]>orderBefore[14]&&orderAfter[15]>orderBefore[15],"middle splice actually uses counted order without rebuild");
    check(ipTestMiddleSplice(128*2-1,0,1),"canonical middle insertion");
    auto middle2=render(spline);check(middle2!=middle,"second splice visible");
    for(int i=0;i<8;++i){
        check(Java_com_infinipaint_demo_NativeBridge_command(nullptr,nullptr,1,nullptr,0)==0,"undo second middle insertion");
        check(render(spline)==middle,"undo restores exact first splice pixels");
        check(Java_com_infinipaint_demo_NativeBridge_command(nullptr,nullptr,1,nullptr,0)==0,"undo first middle replacement");
        check(render(spline)==appended,"undo restores exact original compositing");
        check(Java_com_infinipaint_demo_NativeBridge_command(nullptr,nullptr,2,nullptr,0)==0,"redo first middle splice");
        check(render(spline)==middle,"redo first splice exact");
        check(Java_com_infinipaint_demo_NativeBridge_command(nullptr,nullptr,2,nullptr,0)==0,"redo second middle splice");
        check(render(spline)==middle2,"redo second splice exact");drainSaves();
    }
    check(hostSpatialStats()[7]==beforeMiddle[7]&&hostPaintOrderStats()[13]==orderBefore[13],"all middle undo/redo splices avoid rebuild and write fault");
    camera(-20,1,1.25);(void)render(spline);camera(0,0,1);check(render(spline)==middle2,"middle splice camera round trip");
    // Corrupt sequence page (not locator) and require exact fallback.
    faults=hostSpatialStats()[13];hostSpatialFault(4);
    const auto sequenceDeadline=std::chrono::steady_clock::now()+5s;
    while(hostSpatialStats()[13]==faults){(void)render(spline);check(std::chrono::steady_clock::now()<sequenceDeadline,"sequence corruption handled");std::this_thread::sleep_for(1ms);}
    check(render(spline)==middle2&&!hostSpatialStats()[0],"counted sequence fault exact fallback");
    // Reopen-like same IDs and full root reset above, clear below.
    Java_com_infinipaint_demo_NativeBridge_suspend(nullptr,nullptr,false);
    clear();check(render(spline).front()==background,"spatial clear leaves no stale pixels");
    std::printf("PASS spatial RGBA fixture: %u extra exact comparisons, multi-page crossings, alpha, zoom, motion, context, disk fault, epoch, overlay and counted middle-splice undo/redo\n",planComparisons-startComparisons);
}
#endif
} // namespace

#ifdef IP_M04_HOST
#include "m04_host_probe.hpp"
#elif defined(IP_M04_COVERAGE)
#include "m04_coverage.hpp"
#endif

int main() {
    planTest=std::getenv("IP_PLAN_TEST")!=nullptr;
    if(planTest) {engine::tablet::hostStartPlanner();check(engine::tablet::hostPlannerStats()[0],"planner started");}
    Pbuffer pbuffer;
    const auto* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    const auto* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    check(version && renderer, "missing GL version or renderer");
    std::printf("GL_VERSION=%s GL_RENDERER=%s\n", version, renderer);
#if !defined(IP_M04_HOST) && !defined(IP_M04_COVERAGE)
    check(std::strncmp(version, "OpenGL ES 2.0", 13) == 0,
          "expected GLES 2.0 (set MESA_GLES_VERSION_OVERRIDE=2.0)");
#endif
    glDisable(GL_DITHER);

    Java_com_infinipaint_demo_NativeBridge_contextCreated(nullptr, nullptr);
    const auto* expected=std::getenv("IP_EXPECT_MAX");
    std::printf("verified context coverage path: max_blend=%d\n",observedMax);
    check(observedMax>=0 && (!expected || observedMax==std::atoi(expected)),"wrong coverage branch");
    glCheck("production contextCreated");
    Java_com_infinipaint_demo_NativeBridge_resizeViewport(nullptr, nullptr, width, height);
    Java_com_infinipaint_demo_NativeBridge_setCamera(nullptr, nullptr, 0, 0, 1);
    Java_com_infinipaint_demo_NativeBridge_setSmoothingPreset(nullptr, nullptr, 0);
#ifdef IP_M04_HOST
    return m04Probe();
#elif defined(IP_M04_COVERAGE)
    return coverageProbe();
#endif

    const auto blank = render(raw);
    GLint program = 0, linked = GL_FALSE;
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    check(program != 0, "production render did not bind a shader program");
    glGetProgramiv(GLuint(program), GL_LINK_STATUS, &linked);
    glCheck("production program link status");
    check(linked == GL_TRUE, "production shaders did not link");
    const Pixel background = blank.front();
    check(std::all_of(blank.begin(), blank.end(), [&](const Pixel& p) { return p == background; }),
          "empty production frame must be uniform");

    for (int mode : {raw, spline}) {
        testDots(mode, background);
        testTaper(mode, background);
    }
    testCompletedOrder(background);
    testDwell(background);
    measureJoinAA();
    comparePortableRamp();
    // Exercise page boundaries, live suffix uploads, mode changes and a
    // context rebuild without sealing the stroke. All four live frames must
    // be identical to fresh GPU uploads of the same immutable prefix.
    clear();begin(19);
    for(int i=0;i<300;++i) {
        sample(19,8+(i*7)%110,8+(i*13)%48,1+i*.005f,.2f+.1f*(i%8));
        if(i%37==0) (void)render(i%4);
    }
    const std::array<Frame,4> livePaged{render(0),render(1),render(2),render(3)};
    pbuffer.recreate();
    for(int i=0;i<4;++i) check(render(i)==livePaged[i],"live paged prefix differs after context rebuild");
    end(19);
    const std::array<Frame,4> sealedPaged{render(0),render(1),render(2),render(3)};
    pbuffer.recreate();
    for(int i=0;i<4;++i) check(render(i)==sealedPaged[i],"sealed paged prefix differs after context rebuild");
    std::puts("PASS: 300-sample live paged prefix and sealed four-mode context parity");
#ifdef IP_REPLAY_TEST
    clear();
    begin(12);
    sample(12,16,40,1,.4f);
    sample(12,16,40,1,.8f);
    sample(12,76,20,1,.2f);
    sample(12,82,60,1.01,.9f);
    end(12);
    const std::array<Frame,4> before{render(0),render(1),render(2),render(3)};
    check(ipTestCodecReplay(),"reconstructed vertex streams differ");
    for(int i=0;i<4;i++) check(render(i)==before[i],"canonical replay pixels differ");
    Java_com_infinipaint_demo_NativeBridge_suspend(nullptr,nullptr,true);
    auto edit=[](int op){check(Java_com_infinipaint_demo_NativeBridge_command(nullptr,nullptr,op,nullptr,0)==0,"edit rejected");};
    edit(3);check(render(2)==blank,"clear publication left stale geometry");
    edit(1);
    for(int i=0;i<4;i++) check(render(i)==before[i],"undo Clear did not restore pixels");
    edit(2);edit(1); // two changes before the next frame
    check(render(3)==before[3],"coalesced redo/undo resurrected wrong scene");
    pbuffer.recreate();
    for(int i=0;i<4;i++) check(render(i)==before[i],"new EGL context lost edited scene");
    using namespace engine::document;
    // Two documents reuse the same local IDs. Translation is applied after
    // reconstruction. Changing only their temporal order must reverse the blend.
    for(int rev=0;rev<=1;rev++) {
        auto d=std::make_unique<Document>(Uuid{uint8_t(42+rev)});
        for(int i=0;i<2;i++) {
            Brush b;b.radiusWorld=6;
            b.rgba=(i^rev)?std::array<float,4>{0,0,1,.5f}:std::array<float,4>{1,0,0,.5f};
            engine::input::RawInputStroke rawPoint{{{10.5,24.5},1,1,0,0}};
            Value v{std::make_shared<const Ink>(std::move(rawPoint),b),40,0};
            check(d->restore(i+1,i,v,1,1,1,2)==Status::Ok,"restore test record");
        }
        check(d->finishRestore(1,2)==Status::Ok && d->grant(3,20),"restore high water");
        check(engine::tablet::stage(std::move(d),1<<20),"stage document");
        engine::tablet::activate(0,0,1);engine::tablet::discard();
        auto image=render(2);
        auto pixel=at(image,50,24);
        check(at(image,10,24)==background && (rev?pixel[0]>pixel[2]:pixel[2]>pixel[0]) &&
              std::min(pixel[0],pixel[2])>30 && std::max(pixel[0],pixel[2])<200,"translation/opacity/order/switch");
        Java_com_infinipaint_demo_NativeBridge_setCamera(nullptr,nullptr,1000,2000,1);
        check(render(2)==blank,"offscreen switch projection");
        Java_com_infinipaint_demo_NativeBridge_setCamera(nullptr,nullptr,0,0,1);
        check(render(2)==image,"offscreen return");
    }
    Java_com_infinipaint_demo_NativeBridge_suspend(nullptr,nullptr,false);
    std::puts("PASS: v1 codec and reconstruction vertex bytes + 4-mode RGBA parity");
    canonicalSpanPixelOracle(pbuffer);
    refinedSpanPixelOracle(pbuffer);
    compactedPixelOracle(background);
    spatialPixelOracle(pbuffer,background);
#endif
    clear();
    check(render(composite) == blank, "JNI clear left ink in the framebuffer");
    std::puts("PASS: production GLES2 shader, pressure dots/taper, pen-up, and completed order");
    if(planTest) {
        check(planComparisons>50 && engine::tablet::hostPlannerStats()[5]==0,"planned parity coverage");
        std::printf("PASS: %u matched-plan versus exact-fallback RGBA frames\n",planComparisons);
    }
}
