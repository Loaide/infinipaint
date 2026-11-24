// Linux host regression. All EGL and GLES symbols come from the real driver;
// only Android logging is shimmed. Run with EGL_PLATFORM=surfaceless,
// LIBGL_ALWAYS_SOFTWARE=1 and MESA_GLES_VERSION_OVERRIDE=2.0 on Mesa.
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <android/log.h>
#include <jni.h>

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

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
        EGLConfig config = nullptr;
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
} // namespace

int main() {
    const Pbuffer pbuffer;
    const auto* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    const auto* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    check(version && renderer, "missing GL version or renderer");
    std::printf("GL_VERSION=%s GL_RENDERER=%s\n", version, renderer);
    check(std::strncmp(version, "OpenGL ES 2.0", 13) == 0,
          "expected GLES 2.0 (set MESA_GLES_VERSION_OVERRIDE=2.0)");
    glDisable(GL_DITHER);

    Java_com_infinipaint_demo_NativeBridge_contextCreated(nullptr, nullptr);
    glCheck("production contextCreated");
    Java_com_infinipaint_demo_NativeBridge_resizeViewport(nullptr, nullptr, width, height);
    Java_com_infinipaint_demo_NativeBridge_setCamera(nullptr, nullptr, 0, 0, 1);
    Java_com_infinipaint_demo_NativeBridge_setSmoothingPreset(nullptr, nullptr, 0);

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
    clear();
    check(render(composite) == blank, "JNI clear left ink in the framebuffer");
    std::puts("PASS: production GLES2 shader, pressure dots/taper, pen-up, and completed order");
}
