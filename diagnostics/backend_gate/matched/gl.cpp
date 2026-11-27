#include "bench.hpp"
#include "v1_shaders.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
namespace reference {
struct Vertex {float x,y,u,v,len,radius,endRadius,dirX,dirY;};
static_assert(sizeof(Vertex)==sizeof(::Vertex));
struct Bounds {float minX,minY,maxX,maxY;};
constexpr GLint positionLoc=0,localLoc=1,lengthLoc=2,radiusLoc=3,directionLoc=4;
int width,height;
GLuint program;
GLint resolveLoc,bandLoc,colorLoc;
bool maxCoverage=true;
uint64_t frameDrawCalls;
#include "v1_draw.inc"
}

constexpr std::array<const char*, 5> attributes{"aPos", "aLocal", "aLength", "aRadius", "aDir"};
constexpr GLfloat resolveTriangle[]{-1, -1, 3, -1, -1, 3};
constexpr size_t slots = 32;
constexpr uint64_t maxAge = slots;

bool has(const char* list, std::string_view name) {
    std::string_view rest = list ? list : "";
    while (!rest.empty()) {
        const auto end = rest.find(' ');
        if (rest.substr(0, end) == name) return true;
        if (end == rest.npos) break;
        rest.remove_prefix(end + 1);
    }
    return false;
}

template<class T> T proc(const char* name) {
    return reinterpret_cast<T>(eglGetProcAddress(name));
}

void eglCheck(bool ok, const char* where) {
    if (!ok) throw std::runtime_error(std::string(where) + ": EGL error " +
                                      std::to_string(eglGetError()));
}

void glCheck(const char* where) {
    const GLenum err = glGetError();
    if (err != GL_NO_ERROR)
        throw std::runtime_error(std::string(where) + ": GL error " + std::to_string(err));
}

GLuint compile(GLenum type, const char* source) {
    const GLuint id = glCreateShader(type);
    if (!id) throw std::runtime_error("glCreateShader");
    glShaderSource(id, 1, &source, nullptr);
    glCompileShader(id);
    GLint ok = 0;
    glGetShaderiv(id, GL_COMPILE_STATUS, &ok);
    if (ok) return id;
    char text[2048]{};
    glGetShaderInfoLog(id, sizeof(text), nullptr, text);
    glDeleteShader(id);
    throw std::runtime_error(std::string("v1 shader: ") + text);
}

struct GpuSlot {
    GLuint query[2]{};
    uint64_t issued = 0;
    int seq = 0;
    bool busy = false, valid = false;
};

struct PresentSlot {
    EGLuint64KHR id = 0;
    uint64_t issued = 0;
    int seq = 0;
    bool busy = false;
};

class Gl final : public Backend {
    const Scene& scene;
    const int initial;
    const bool es32, compact;
    const size_t chordBytes;
    int resident, width = 0, height = 0;
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
    bool current = false;

    GLuint program = 0, mesh = 0, staticMesh = 0, coverageVao = 0, resolveVao = 0;
    std::vector<GLuint> refBuffers;
    GLint screen = -1, pan = -1, zoom = -1, color = -1, resolve = -1, band = -1;

    PFNGLGENQUERIESEXTPROC genQueries = nullptr;
    PFNGLDELETEQUERIESEXTPROC deleteQueries = nullptr;
    PFNGLQUERYCOUNTEREXTPROC queryCounter = nullptr;
    PFNGLGETQUERYOBJECTUIVEXTPROC queryStatus = nullptr;
    PFNGLGETQUERYOBJECTUI64VEXTPROC queryValue = nullptr;
    bool timerAvailable = false;
    PFNEGLGETNEXTFRAMEIDANDROIDPROC nextFrameId = nullptr;
    PFNEGLGETFRAMETIMESTAMPSANDROIDPROC frameTimestamps = nullptr;
    bool presentAvailable = false;
    std::array<GpuSlot, slots> gpu{};
    std::array<PresentSlot, slots> presents{};
    uint64_t frameIndex = 0, timerDropped = 0, presentDropped = 0, disjoints = 0;

    EGLint configValue(EGLConfig config, EGLint key) const {
        EGLint value = 0;
        eglCheck(eglGetConfigAttrib(display, config, key, &value), "eglGetConfigAttrib");
        return value;
    }

    void initEgl(ANativeWindow* window) {
        if (!window) throw std::runtime_error("missing GL window");
        display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        eglCheck(display != EGL_NO_DISPLAY, "eglGetDisplay");
        EGLint eglMajor = 0, eglMinor = 0;
        eglCheck(eglInitialize(display, &eglMajor, &eglMinor), "eglInitialize");
        eglCheck(eglBindAPI(EGL_OPENGL_ES_API), "eglBindAPI");
        const char* ext = eglQueryString(display, EGL_EXTENSIONS);
        if (es32 && !(eglMajor > 1 || (eglMajor == 1 && eglMinor >= 5) ||
                      has(ext, "EGL_KHR_create_context")))
            throw std::runtime_error("EGL cannot request an ES 3.2 context");
        const EGLint renderable = es32 ? EGL_OPENGL_ES3_BIT_KHR : EGL_OPENGL_ES2_BIT;
        const EGLint required[]{EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, renderable,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
            EGL_SAMPLE_BUFFERS, 0, EGL_SAMPLES, 0, EGL_NONE};
        EGLint count = 0;
        eglCheck(eglChooseConfig(display, required, nullptr, 0, &count), "eglChooseConfig count");
        if (!count) throw std::runtime_error("no eligible window config");
        std::vector<EGLConfig> configs(count);
        eglCheck(eglChooseConfig(display, required, configs.data(), count, &count),
                 "eglChooseConfig list");
        EGLConfig chosen = nullptr;
        for (int i = 0; i < count; ++i) {
            const auto c = configs[i];
            if (configValue(c, EGL_RED_SIZE) != 8 || configValue(c, EGL_GREEN_SIZE) != 8 ||
                configValue(c, EGL_BLUE_SIZE) != 8 || configValue(c, EGL_ALPHA_SIZE) != 8 ||
                configValue(c, EGL_SAMPLES) != 0 || configValue(c, EGL_SAMPLE_BUFFERS) != 0 ||
                configValue(c, EGL_MIN_SWAP_INTERVAL) > 1 ||
                configValue(c, EGL_MAX_SWAP_INTERVAL) < 1) continue;
            // Prefer no depth/stencil; neither is part of the matched workload.
            if (!chosen) chosen = c;
            if (!configValue(c, EGL_DEPTH_SIZE) && !configValue(c, EGL_STENCIL_SIZE)) {
                chosen = c;
                break;
            }
        }
        if (!chosen) throw std::runtime_error("no exact RGBA8888 non-MSAA window config");
        const EGLint es2Attrs[]{EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
        const EGLint es32Attrs[]{EGL_CONTEXT_MAJOR_VERSION_KHR, 3,
                                 EGL_CONTEXT_MINOR_VERSION_KHR, 2, EGL_NONE};
        context = eglCreateContext(display, chosen, EGL_NO_CONTEXT, es32 ? es32Attrs : es2Attrs);
        eglCheck(context != EGL_NO_CONTEXT, "eglCreateContext");
        const EGLint visual = configValue(chosen, EGL_NATIVE_VISUAL_ID);
        // Don't change the runner's requested buffer size.
        const EGLint linear[]{EGL_GL_COLORSPACE_KHR, EGL_GL_COLORSPACE_LINEAR_KHR, EGL_NONE};
        const bool canSpecifyLinear = has(ext, "EGL_KHR_gl_colorspace") ||
                                      eglMajor > 1 || (eglMajor == 1 && eglMinor >= 5);
        surface = eglCreateWindowSurface(display, chosen, window, canSpecifyLinear ? linear : nullptr);
        eglCheck(surface != EGL_NO_SURFACE, "eglCreateWindowSurface");
        eglCheck(eglMakeCurrent(display, surface, surface, context), "eglMakeCurrent");
        current = true;
        eglCheck(eglSwapInterval(display, 1), "eglSwapInterval");
        EGLint w = 0, h = 0, colorspace = EGL_GL_COLORSPACE_LINEAR_KHR, renderBuffer = 0;
        eglCheck(eglQuerySurface(display, surface, EGL_WIDTH, &w), "eglQuerySurface width");
        eglCheck(eglQuerySurface(display, surface, EGL_HEIGHT, &h), "eglQuerySurface height");
        eglCheck(eglQuerySurface(display, surface, EGL_RENDER_BUFFER, &renderBuffer),
                 "eglQuerySurface render buffer");
        if (canSpecifyLinear)
            eglCheck(eglQuerySurface(display, surface, EGL_GL_COLORSPACE_KHR, &colorspace),
                     "eglQuerySurface colorspace");
        if (w <= 0 || h <= 0 || uint64_t(w) * h > std::numeric_limits<size_t>::max()/4 ||
            colorspace != EGL_GL_COLORSPACE_LINEAR_KHR || renderBuffer != EGL_BACK_BUFFER)
            throw std::runtime_error("unexpected GL surface size/colorspace/render buffer");
        width = w;
        height = h;
        BLOG("gl config=%d EGL=%d.%d request=%s visual=%d rgba=8/8/8/8 depth=%d stencil=%d "
             "samples=0 swap=1 surface=%dx%d linear=%s render_buffer=back",
             configValue(chosen, EGL_CONFIG_ID), eglMajor, eglMinor, es32 ? "3.2" : "2.0", visual,
             configValue(chosen, EGL_DEPTH_SIZE), configValue(chosen, EGL_STENCIL_SIZE), w, h,
             canSpecifyLinear ? "verified" : "default");
    }

    static void attrib(GLuint index, GLint count, GLsizei stride, size_t offset) {
        glVertexAttribPointer(index, count, GL_FLOAT, GL_FALSE, stride,
                              reinterpret_cast<const void*>(offset));
    }

    void bindExpanded() const {
        glBindBuffer(GL_ARRAY_BUFFER, mesh);
        attrib(0, 2, sizeof(Vertex), offsetof(Vertex, x));
        attrib(1, 2, sizeof(Vertex), offsetof(Vertex, u));
        attrib(2, 1, sizeof(Vertex), offsetof(Vertex, len));
        attrib(3, 2, sizeof(Vertex), offsetof(Vertex, r));
        attrib(4, 2, sizeof(Vertex), offsetof(Vertex, dx));
    }

    void bindInstances(int first) const {
        const size_t base = static_cast<size_t>(first) * sizeof(Instance);
        glBindBuffer(GL_ARRAY_BUFFER, mesh);
        attrib(0, 2, sizeof(Instance), base + offsetof(Instance, x));
        attrib(2, 1, sizeof(Instance), base + offsetof(Instance, len));
        attrib(3, 2, sizeof(Instance), base + offsetof(Instance, r));
        attrib(4, 2, sizeof(Instance), base + offsetof(Instance, dx));
    }

    void upload(int first, int count) const {
        if (!count) return;
        if(!es32){
            for(size_t i=0;i<scene.strokes.size();i++){
                const auto& s=scene.strokes[i];
                int lo=std::max(first,s.first),hi=std::min(first+count,s.first+s.count);
                if(hi<=lo)continue;
                glBindBuffer(GL_ARRAY_BUFFER,refBuffers[i]);
                glBufferSubData(GL_ARRAY_BUFFER,size_t(lo-s.first)*chordBytes,size_t(hi-lo)*chordBytes,
                                scene.expanded.data()+6*lo);
            }
            return;
        }
        const void* source = compact ? static_cast<const void*>(scene.items.data() + first)
                                     : static_cast<const void*>(scene.expanded.data() + size_t(first)*6);
        glBindBuffer(GL_ARRAY_BUFFER, mesh);
        glBufferSubData(GL_ARRAY_BUFFER, size_t(first)*chordBytes, size_t(count)*chordBytes, source);
    }

    void initGl() {
        const auto* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
        const auto* ext = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
        if (!version) throw std::runtime_error("missing GL version");
        if (es32) {
            GLint major = 0, minor = 0, encoding = 0;
            glGetIntegerv(GL_MAJOR_VERSION, &major);
            glGetIntegerv(GL_MINOR_VERSION, &minor);
            if (major != 3 || minor != 2) throw std::runtime_error("actual GLES version is not 3.2");
            glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_BACK,
                GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING, &encoding);
            if (encoding != GL_LINEAR) throw std::runtime_error("nonlinear GL default framebuffer");
        }
        // v1 accepts its ES2 extension OR the same MAX enum when the driver
        // returns a higher context. No ES3 vertex path for an ES2 request.
        if (!has(ext, "GL_EXT_blend_minmax") && !std::string_view(version).starts_with("OpenGL ES 3."))
            throw std::runtime_error("v1 MAX blend unavailable (band fallback not matched)");
        GLint red = 0, green = 0, blue = 0, alpha = 0, samples = 0;
        glGetIntegerv(GL_RED_BITS, &red);
        glGetIntegerv(GL_GREEN_BITS, &green);
        glGetIntegerv(GL_BLUE_BITS, &blue);
        glGetIntegerv(GL_ALPHA_BITS, &alpha);
        glGetIntegerv(GL_SAMPLES, &samples);
        if (red != 8 || green != 8 || blue != 8 || alpha != 8 || samples != 0)
            throw std::runtime_error("default framebuffer not RGBA8888 non-MSAA");

        program = glCreateProgram();
        if (!program) throw std::runtime_error("glCreateProgram");
        auto attach = [this](GLenum type, const char* source) {
            const auto shader = compile(type, source);
            glAttachShader(program, shader);
            glDeleteShader(shader);
        };
        attach(GL_VERTEX_SHADER, v1vs);
        attach(GL_FRAGMENT_SHADER, v1fs);
        for (GLuint i = 0; i < attributes.size(); ++i) glBindAttribLocation(program, i, attributes[i]);
        glLinkProgram(program);
        GLint linked = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (!linked) {
            char text[2048]{};
            glGetProgramInfoLog(program, sizeof(text), nullptr, text);
            throw std::runtime_error(std::string("v1 program: ") + text);
        }
        auto uniform = [this](const char* name) {
            const auto loc = glGetUniformLocation(program, name);
            if (loc < 0) throw std::runtime_error(std::string("missing v1 uniform ") + name);
            return loc;
        };
        screen = uniform("uScreen"); pan = uniform("uCameraPan"); zoom = uniform("uCameraZoom");
        color = uniform("uColor"); resolve = uniform("uResolve"); band = uniform("uBand");

        const size_t capacity = scene.items.size()*chordBytes;
        if(!es32){
            refBuffers.resize(scene.strokes.size());
            glGenBuffers(GLsizei(refBuffers.size()),refBuffers.data());
            for(size_t i=0;i<refBuffers.size();i++){
                glBindBuffer(GL_ARRAY_BUFFER,refBuffers[i]);
                glBufferData(GL_ARRAY_BUFFER,scene.strokes[i].count*chordBytes,nullptr,GL_DYNAMIC_DRAW);
            }
            reference::program=program;reference::colorLoc=color;reference::resolveLoc=resolve;
            reference::bandLoc=band;reference::width=width;reference::height=height;
        }else{
            glGenBuffers(1, &mesh);glBindBuffer(GL_ARRAY_BUFFER, mesh);
            glBufferData(GL_ARRAY_BUFFER, capacity, nullptr, GL_DYNAMIC_DRAW);
        }
        upload(0, initial);

        glGenBuffers(1, &staticMesh);
        glBindBuffer(GL_ARRAY_BUFFER, staticMesh);
        const size_t staticBytes = sizeof(resolveTriangle) + (compact ? sizeof(corners) : 0);
        glBufferData(GL_ARRAY_BUFFER, staticBytes, nullptr, GL_STATIC_DRAW);
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(resolveTriangle), resolveTriangle);
        if (compact)
            glBufferSubData(GL_ARRAY_BUFFER, sizeof(resolveTriangle), sizeof(corners), corners);

        if (es32) {
            glGenVertexArrays(1, &resolveVao);
            glBindVertexArray(resolveVao);
            glEnableVertexAttribArray(0);
            attrib(0, 2, 0, 0);
            glGenVertexArrays(1, &coverageVao);
            glBindVertexArray(coverageVao);
        }
        for (GLuint i = 0; i < attributes.size(); ++i) glEnableVertexAttribArray(i);
        if (compact) {
            glBindBuffer(GL_ARRAY_BUFFER, staticMesh);
            attrib(1, 2, 0, sizeof(resolveTriangle));
            for (GLuint i : {0u, 2u, 3u, 4u}) glVertexAttribDivisor(i, 1);
            bindInstances(0);
        } else if(es32) {
            bindExpanded();
        }
        glDisable(GL_DITHER);
        glViewport(0, 0, width, height);
        glUseProgram(program);
        glUniform2f(screen, width, height);
        glUniform1f(zoom, 1.f);
        glUniform1f(band, 0.f);
        glUniform1i(resolve, 0);
        glCheck("initializing matched GL pipeline");
        BLOG("gl actual=%s layout=%s max_blend=1 mesh_capacity_bytes=%zu initial_upload_bytes=%zu "
             "static_buffer_bytes=%zu color_bytes_per_image=%llu (logical allocations, not GPU RSS)",
             version, compact ? "instance" : "expanded", capacity, size_t(initial)*chordBytes,
             staticBytes, static_cast<unsigned long long>(uint64_t(width)*height*4));
        if (auto* file = std::fopen("/proc/self/status", "r")) {
            char line[256]{};
            while (std::fgets(line, sizeof(line), file)) {
                int kib = 0;
                if (std::sscanf(line, "VmRSS: %d kB", &kib) == 1) {
                    BLOG("gl process_rss_init_kib=%d", kib);
                    break;
                }
            }
            std::fclose(file);
        }
    }

    void initInstrumentation() {
        GLint bits = 0;
        const auto* glExt = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
        if (has(glExt, "GL_EXT_disjoint_timer_query")) {
            genQueries = proc<PFNGLGENQUERIESEXTPROC>("glGenQueriesEXT");
            deleteQueries = proc<PFNGLDELETEQUERIESEXTPROC>("glDeleteQueriesEXT");
            queryCounter = proc<PFNGLQUERYCOUNTEREXTPROC>("glQueryCounterEXT");
            queryStatus = proc<PFNGLGETQUERYOBJECTUIVEXTPROC>("glGetQueryObjectuivEXT");
            queryValue = proc<PFNGLGETQUERYOBJECTUI64VEXTPROC>("glGetQueryObjectui64vEXT");
            const auto getQuery = proc<PFNGLGETQUERYIVEXTPROC>("glGetQueryivEXT");
            if (genQueries && deleteQueries && queryCounter && queryStatus && queryValue && getQuery) {
                getQuery(GL_TIMESTAMP_EXT, GL_QUERY_COUNTER_BITS_EXT, &bits);
                if (glGetError() == GL_NO_ERROR && bits > 0) {
                    for (auto& s : gpu) genQueries(2, s.query);
                    timerAvailable = glGetError() == GL_NO_ERROR &&
                        std::all_of(gpu.begin(), gpu.end(), [](const auto& s) {
                            return s.query[0] && s.query[1];
                        });
                }
            }
        }
        const char* eglExt = eglQueryString(display, EGL_EXTENSIONS);
        if (has(eglExt, "EGL_ANDROID_get_frame_timestamps")) {
            nextFrameId = proc<PFNEGLGETNEXTFRAMEIDANDROIDPROC>("eglGetNextFrameIdANDROID");
            frameTimestamps = proc<PFNEGLGETFRAMETIMESTAMPSANDROIDPROC>("eglGetFrameTimestampsANDROID");
            const auto supports = proc<PFNEGLGETFRAMETIMESTAMPSUPPORTEDANDROIDPROC>(
                "eglGetFrameTimestampSupportedANDROID");
            if (nextFrameId && frameTimestamps && supports &&
                eglSurfaceAttrib(display, surface, EGL_TIMESTAMPS_ANDROID, EGL_TRUE))
                presentAvailable = supports(display, surface, EGL_DISPLAY_PRESENT_TIME_ANDROID);
            if (!presentAvailable) (void)eglGetError();
        }
        BLOG("gl instrumentation gpu_timestamp_pairs=%d counter_bits=%d display_present_android=%d slots=%zu",
             timerAvailable, bits, presentAvailable, slots);
    }

    void pollGpu() {
        if (!timerAvailable || std::none_of(gpu.begin(), gpu.end(), [](const auto& s){return s.busy;})) return;
        struct Result { int seq; uint64_t ns; };
        std::array<Result, slots> results{};
        size_t n = 0;
        for (auto& s : gpu) {
            if (!s.busy) continue;
            if (frameIndex - s.issued >= maxAge) s.valid = false;
            GLuint available = 0;
            queryStatus(s.query[1], GL_QUERY_RESULT_AVAILABLE_EXT, &available);
            if (!available) continue;
            if (s.valid) {
                queryStatus(s.query[0], GL_QUERY_RESULT_AVAILABLE_EXT, &available);
                if (!available) continue;
                GLuint64 first = 0, last = 0;
                queryValue(s.query[0], GL_QUERY_RESULT_EXT, &first);
                queryValue(s.query[1], GL_QUERY_RESULT_EXT, &last);
                if (last > first) results[n++] = {s.seq, last - first};
                else ++timerDropped;
            } else ++timerDropped;
            s.busy = false;
        }
        // Read after harvesting the available pairs, before publishing any.
        // The disjoint flag covers work since its last read, including pending pairs.
        GLint disjoint = 0;
        glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);
        glCheck("polling matched GPU timestamps");
        if (disjoint) {
            ++disjoints;
            timerDropped += n;
            for (auto& s : gpu) if (s.busy) s.valid = false;
            return;
        }
        for (size_t i = 0; i < n; ++i) gpuResult(results[i].seq, results[i].ns);
    }

    void pollPresent() {
        if (!presentAvailable) return;
        constexpr EGLint name = EGL_DISPLAY_PRESENT_TIME_ANDROID;
        for (auto& s : presents) {
            if (!s.busy) continue;
            if (frameIndex - s.issued < maxAge) {
                EGLnsecsANDROID stamp = EGL_TIMESTAMP_INVALID_ANDROID;
                if (frameTimestamps(display, surface, s.id, 1, &name, &stamp)) {
                    if (stamp == EGL_TIMESTAMP_PENDING_ANDROID) continue;
                    if (stamp > 0) {
                        presentResult(s.seq, stamp);
                        s.busy = false;
                        continue;
                    }
                } else {
                    (void)eglGetError(); // Missing/aged out; never substitute swap-return time.
                }
            }
            ++presentDropped;
            s.busy = false;
        }
    }

    void draw(const Frame& f, Metrics& m) {
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(.025f, .055f, .09f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        glUniform2f(pan, f.pan, 0.f);
        if(!es32){
            glEnable(GL_BLEND);reference::frameDrawCalls=0;
            for(size_t i=0;i<scene.strokes.size();i++){
                const auto& s=scene.strokes[i];int n=std::min(s.count,f.visible-s.first);
                if(n<=0)continue;
                // Inverse of v1's floor/ceil +/-1 expansion: matched screen
                // scissor for this fixed visible set, also during the pan.
                reference::Bounds b{s.x+1.f+f.pan,s.y+1.f,s.x+s.w-1.f+f.pan,s.y+s.h-1.f};
                reference::coverageLayer(b,f.pan,0,1,s.color[0],s.color[1],s.color[2],
                    [&]{reference::drawCached(refBuffers[i],6*n,0,0,0);},s.color[3]);
            }
            m.draws=int(reference::frameDrawCalls);return;
        }
        for (const auto& s : scene.strokes) {
            const int count = std::min(s.count, f.visible - s.first);
            if (count <= 0 || s.w <= 0 || s.h <= 0) continue;
            const int x = std::clamp(s.x, 0, width), y = std::clamp(s.y, 0, height);
            const int right = std::clamp<int64_t>(int64_t(s.x) + s.w, 0, width);
            const int bottom = std::clamp<int64_t>(int64_t(s.y) + s.h, 0, height);
            if (right <= x || bottom <= y) continue;
            glEnable(GL_SCISSOR_TEST);
            glScissor(x, height - bottom, right - x, bottom - y);
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
            glClearColor(0, 0, 0, 0);
            glClear(GL_COLOR_BUFFER_BIT);
            glEnable(GL_BLEND);
            glBlendEquation(GL_MAX); // GL_MAX_EXT has the same value.
            glUniform4fv(color, 1, s.color);
            if (es32) glBindVertexArray(coverageVao);
            if (compact) {
                bindInstances(s.first);
                glDrawArraysInstanced(GL_TRIANGLES, 0, 6, count);
            } else {
                if (!es32) bindExpanded();
                glDrawArrays(GL_TRIANGLES, 6*s.first, 6*count);
            }
            ++m.draws;

            glBlendEquation(GL_FUNC_ADD);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glBlendFuncSeparate(GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_ONE, GL_ZERO);
            glUniform1i(resolve, 1);
            glUniform4f(color, s.color[0], s.color[1], s.color[2], 1.f);
            if (es32) glBindVertexArray(resolveVao);
            else {
                glBindBuffer(GL_ARRAY_BUFFER, staticMesh);
                attrib(0, 2, 0, 0);
            }
            glDrawArrays(GL_TRIANGLES, 0, 3);
            ++m.draws;
            glUniform1i(resolve, 0);
            glDisable(GL_SCISSOR_TEST);
        }
    }

    void read(std::vector<uint8_t>& pixels) const {
        const size_t row = size_t(width)*4;
        pixels.resize(row*height);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        glCheck("parity readback");
        for (int y = 0; y < height/2; ++y) {
            auto* a = pixels.data() + size_t(y)*row;
            auto* b = pixels.data() + size_t(height - 1 - y)*row;
            std::swap_ranges(a, a + row, b);
        }
    }

public:
    Gl(const Scene& s, int prefix, bool high, bool packed)
        : scene(s), initial(prefix), es32(high), compact(packed),
          chordBytes(packed ? sizeof(Instance) : 6*sizeof(Vertex)), resident(prefix) {}

    void init(ANativeWindow* window) {
        if (compact && !es32) throw std::runtime_error("compact layout requires ES 3.2");
        if (initial < 0 || size_t(initial) > scene.items.size() ||
            scene.items.size() > size_t(std::numeric_limits<int>::max()/6) ||
            scene.items.size() > size_t(std::numeric_limits<GLsizeiptr>::max())/chordBytes ||
            (!compact && scene.expanded.size() != 6*scene.items.size()))
            throw std::runtime_error("invalid GL scene geometry/prefix");
        for (const auto& s : scene.strokes)
            if (s.first < 0 || s.count < 0 || size_t(s.first) + s.count > scene.items.size())
                throw std::runtime_error("invalid GL stroke range");
        initEgl(window);
        initGl();
        initInstrumentation();
    }

    ~Gl() override {
        if (current) {
            for (const auto& s : gpu) {
                if (s.busy) ++timerDropped;
                if (deleteQueries && (s.query[0] || s.query[1])) deleteQueries(2, s.query);
            }
            for (const auto& s : presents) if (s.busy) ++presentDropped;
            BLOG("gl instrumentation final gpu_dropped=%llu present_dropped=%llu disjoints=%llu",
                 static_cast<unsigned long long>(timerDropped), static_cast<unsigned long long>(presentDropped),
                 static_cast<unsigned long long>(disjoints));
            if (coverageVao) glDeleteVertexArrays(1, &coverageVao);
            if (resolveVao) glDeleteVertexArrays(1, &resolveVao);
            if (mesh) glDeleteBuffers(1, &mesh);
            if (!refBuffers.empty()) glDeleteBuffers(GLsizei(refBuffers.size()),refBuffers.data());
            if (staticMesh) glDeleteBuffers(1, &staticMesh);
            if (program) glDeleteProgram(program);
            eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        }
        if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
        if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
        if (display != EGL_NO_DISPLAY) eglTerminate(display);
    }

    Metrics frame(const Frame& f, std::vector<uint8_t>* pixels) override {
        if ((f.readback && (f.timer || !pixels)) || f.added < 0 || f.visible < 0 ||
            size_t(f.visible) > scene.items.size() ||
            (f.added && (f.first < initial || f.first > resident ||
                         size_t(f.first) + f.added > scene.items.size())))
            throw std::runtime_error("invalid GL matched frame or timed readback");
        if (f.visible > (f.added ? std::max(resident, f.first + f.added) : resident))
            throw std::runtime_error("frame refers to unuploaded GL geometry");

        ++frameIndex;
        pollGpu();
        pollPresent();
        GpuSlot* timing = nullptr;
        if (f.timer && timerAvailable) {
            auto it = std::find_if(gpu.begin(), gpu.end(), [](const auto& s) { return !s.busy; });
            if (it != gpu.end()) timing = &*it;
            else ++timerDropped;
        }
        Metrics m{};
        const auto submitStart = nowNs();
        if (timing) queryCounter(timing->query[0], GL_TIMESTAMP_EXT);
        if (f.added) {
            upload(f.first, f.added);
            resident = std::max(resident, f.first + f.added);
            m.bytes = size_t(f.added)*chordBytes;
        }
        draw(f, m);
        if (timing) {
            queryCounter(timing->query[1], GL_TIMESTAMP_EXT);
            timing->busy = timing->valid = true;
            timing->seq = f.seq;
            timing->issued = frameIndex;
        }
        m.submitNs = nowNs() - submitStart;
        if (f.readback) read(*pixels);
        glCheck("matched GL submission");

        PresentSlot* p = nullptr;
        if (presentAvailable && !f.readback) {
            auto it = std::find_if(presents.begin(), presents.end(), [](const auto& s) { return !s.busy; });
            if (it != presents.end() && nextFrameId(display, surface, &it->id)) p = &*it;
            else {
                (void)eglGetError();
                ++presentDropped;
            }
        }
        const auto presentStart = nowNs();
        const bool swapped = eglSwapBuffers(display, surface);
        m.presentCallNs = nowNs() - presentStart;
        eglCheck(swapped, "eglSwapBuffers");
        if (p) {
            p->busy = true;
            p->seq = f.seq;
            p->issued = frameIndex;
        }
        return m; // No explicit GPU wait on the timed path (syncNs remains zero).
    }
};

} // namespace

std::unique_ptr<Backend> openGL(ANativeWindow* window, const Scene& scene, int initial,
                                bool es32, bool compact) {
    auto out = std::make_unique<Gl>(scene, initial, es32, compact);
    out->init(window);
    return out;
}
