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

static int observedMax=-1;
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
    Java_com_infinipaint_demo_NativeBridge_render(nullptr, nullptr, mode);
    glCheck("production render");
    Frame frame{};
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, frame.data());
    glCheck("RGBA8 glReadPixels");
    return frame;
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
} // namespace

#ifdef IP_M04_HOST
#include "m04_host_probe.hpp"
#elif defined(IP_M04_COVERAGE)
#include "m04_coverage.hpp"
#endif

int main() {
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
#endif
    clear();
    check(render(composite) == blank, "JNI clear left ink in the framebuffer");
    std::puts("PASS: production GLES2 shader, pressure dots/taper, pen-up, and completed order");
}
