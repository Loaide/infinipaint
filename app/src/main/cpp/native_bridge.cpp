
#include <jni.h>
#include <GLES2/gl2.h>
#include <android/log.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <limits>
#include <array>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "input/input_processor.hpp"
#include "stroke/spline.hpp"

namespace {

    using namespace engine;

// All stroke geometry is stored in world coordinates.
// The vertex shader converts it to screen coordinates.

    struct Vertex {
        float x, y;
        float u, v;
        float len;
        float radius, endRadius;
        float dirX, dirY;
    };

    using Mesh = std::vector<Vertex>;

    struct Trace {
        uint64_t gpuId = 0;
        input::InputProcessor input;
        stroke::IncrementalSplineBuilder spline;

        Mesh rawMesh;
        Mesh cleanMesh;
        Mesh stableMesh;
        Mesh tailMesh;
        uint64_t tailRevision = 0;

        std::vector<float> rawPoints;
        std::vector<float> cleanPoints;

        size_t stableBuilt = 0;

        double lastAcceptedTime = -1.0;
        float strokeRadiusWorld = 1.9f;
        float rawPeak = 0, cleanPeak = 0;
        float lastRadius = 0;
        double deviationSum = 0.0;
        double deviationMax = 0.0;
        double originalDeviationMax = 0.0;
        size_t deviationCount = 0;
        size_t wouldExceedLimit = 0;
        double filteredBeforeLimitSum = 0.0;

        Trace(
                input::InputProcessor i,
                stroke::SplineFitter f
        )
                : input(std::move(i)),
                  spline(std::move(f)) {}
    };

// --------------------------------------------------
// Shared application state
// --------------------------------------------------

    std::mutex stateMutex;

    std::atomic<int> smoothingPreset{1};

    // Profiling only: these counters do not alter filtering or drawing.
    using ProfileClock = std::chrono::steady_clock;
    std::atomic<int64_t> newestEventNs{0};
    std::atomic<int64_t> latestSampleEndNs{0};
    std::atomic<int64_t> sampleProcessingUs{0};
    std::atomic<uint64_t> processedSamples{0};
    std::atomic<int64_t> auditMutexWaitNs{0}, auditFilterNs{0}, auditSplineNs{0}, auditSnapshotNs{0};
    std::atomic<uint64_t> auditStageSamples{0}, auditSnapshotFrames{0};
    uint64_t frameDrawCalls = 0;
    uint64_t frameUploadBytes = 0;
    uint64_t frameIndex = 0;
    int64_t lastFrameStartNs = 0;
    double sumFrameIntervalMs = 0.0;
    uint64_t validFrameIntervals = 0;
    double sumRenderCpuMs = 0.0;
    double sumInputAgeMs = 0.0;
    double sumSampleToFrameMs = 0.0;
    uint64_t validInputAgeFrames = 0;
    uint64_t validSampleFrames = 0;
    double maxRenderCpuMs = 0.0;
    double maxInputAgeMs = 0.0;
    constexpr uint64_t PROFILE_FRAME_WINDOW = 120;
    int64_t profileNs() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                ProfileClock::now().time_since_epoch()).count();
    }


    std::unordered_map<int, std::unique_ptr<Trace>> live;

    struct FinishedTrace {
        std::shared_ptr<const input::RawInputStroke> originalRaw;
        std::shared_ptr<const Mesh> raw;
        std::shared_ptr<const Mesh> clean;
        std::shared_ptr<const Mesh> spline;
        float minX = 0, minY = 0, maxX = 0, maxY = 0;
    };
    std::vector<FinishedTrace> finished;
    // Stable IDs are assigned under stateMutex; GPU resources live only on GL thread.
    uint64_t finishedEpoch = 1;
    struct CachedGpuStroke {
        std::array<std::shared_ptr<const Mesh>, 3> source;
        std::array<GLuint, 3> vbo{};
        std::array<GLsizei, 3> count{};
        float minX=0, minY=0, maxX=0, maxY=0;
    };
    // GL thread maintains append-only cache; completed CPU meshes are copied only
    // when first received, never once per animation frame.
    std::vector<CachedGpuStroke> gpuFinished;
    size_t glFinishedCount = 0;
    uint64_t gpuEpoch = 0;
    uint64_t cacheUploadBytes = 0;
    uint64_t liveUploadBytes = 0;
    struct LiveGpuMesh {
        GLuint vbo = 0;
        size_t uploadedVertices = 0;
        size_t capacityVertices = 0;
        uint64_t revision = 0;
    };
    std::unordered_map<uint64_t, std::array<LiveGpuMesh, 4>> gpuLive;
    uint64_t nextLiveGpuId = 1;
    uint64_t liveBufferReallocations = 0;
    uint64_t liveIncrementalUpdates = 0;


// Camera coordinates are in world-space pixels.
// Screen position = (world position - camera pan) * zoom.

    double cameraPanX = 0.0;
    double cameraPanY = 0.0;
    double cameraZoom = 1.0;

    constexpr double MIN_ZOOM = 0.1;
    constexpr double MAX_ZOOM = 16.0;

// --------------------------------------------------
// OpenGL state
// --------------------------------------------------

    int width = 1;
    int height = 1;

    GLuint program = 0;
    GLuint buffer = 0;

    GLint positionLoc = -1;
    GLint localLoc = -1;
    GLint lengthLoc = -1;
    GLint radiusLoc = -1;
    GLint directionLoc = -1;

    GLint screenLoc = -1;
    GLint colorLoc = -1;

    GLint cameraPanLoc = -1;
    GLint cameraZoomLoc = -1;

// --------------------------------------------------
// Vertex shader
// --------------------------------------------------

    const char* vs = R"(
attribute vec2 aPos;
attribute vec2 aLocal;
attribute float aLength;
attribute vec2 aRadius;
attribute vec2 aDir;

uniform vec2 uScreen;
uniform vec2 uCameraPan;
uniform float uCameraZoom;

varying vec2 vLocal;
varying float vLength;
varying vec2 vRadius;

void main() {
    // Expand the segment on the GPU at the CURRENT zoom. Avoid the old
    // min-zoom world-space padding that produced gigantic overdraw when
    // zooming in on a drawing.
    float pad = max(aRadius.x,aRadius.y) + 1.0 / uCameraZoom;
    float along = aLocal.x * (aLength + 2.0 * pad) - pad;
    float across = aLocal.y * pad;
    vec2 normal = vec2(-aDir.y, aDir.x);
    vec2 worldPos = aPos + aDir * along + normal * across;
    vec2 screenPos = (worldPos - uCameraPan) * uCameraZoom;
    gl_Position = vec4(
        2.0 * screenPos.x / uScreen.x - 1.0,
        1.0 - 2.0 * screenPos.y / uScreen.y,
        0.0, 1.0
    );
    vLocal = vec2(along, across) * uCameraZoom;
    vLength = aLength * uCameraZoom;
    vRadius = aRadius * uCameraZoom;
}
)";

// --------------------------------------------------
// Fragment shader
// --------------------------------------------------

    const char* fs = R"(
precision mediump float;

uniform vec4 uColor;

varying vec2 vLocal;
varying float vLength;
varying vec2 vRadius;

void main() {

    // Distance to the union of discs swept along a linear radius ramp.
    float dr = vRadius.y-vRadius.x;
    float center = 0.0;
    if (abs(dr) >= vLength) {
        center = dr > 0.0 ? vLength : 0.0;
    } else {
        float slope = dr/vLength;
        center = clamp(vLocal.x + abs(vLocal.y)*slope/
                       sqrt(max(0.0001,1.0-slope*slope)),0.0,vLength);
    }
    float radius = vLength > 0.0 ? mix(vRadius.x,vRadius.y,center/vLength)
                                  : max(vRadius.x,vRadius.y);
    float d = length(vec2(vLocal.x-center,vLocal.y));

    float a = 1.0 - smoothstep(
        radius - 0.75,
        radius + 0.75,
        d
    );

    gl_FragColor = vec4(
        uColor.rgb * a,
        a * uColor.a
    );
}
)";

// --------------------------------------------------
// Shader compilation
// --------------------------------------------------

    GLuint compile(GLenum type, const char* source) {

        GLuint shader = glCreateShader(type);

        glShaderSource(shader, 1, &source, nullptr);
        glCompileShader(shader);

        GLint ok = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);

        if (!ok) {
            char log[1024] = {};

            glGetShaderInfoLog(
                    shader,
                    sizeof(log) - 1,
                    nullptr,
                    log
            );

            __android_log_print(
                    ANDROID_LOG_ERROR,
                    "IP90",
                    "Shader compilation failed: %s",
                    log
            );

            glDeleteShader(shader);
            return 0;
        }

        return shader;
    }

// --------------------------------------------------
// Stroke geometry
// --------------------------------------------------

    void quad(
            Mesh& mesh,
            float ax,
            float ay,
            float bx,
            float by,
            float radius, float endRadius=-1
    ) {

        float dx = bx - ax;
        float dy = by - ay;

        float len = std::hypot(dx, dy);

        if (len < 0.0001f) {
            dx = 1.0f;
            dy = 0.0f;
            len = 0.0f;
        } else {
            dx /= len;
            dy /= len;
        }

        // Six GPU-expanded vertices; constant-size topology independent of
        // camera zoom. Every fragment covers only the stroke + ~1px AA.
        auto make = [&](float u, float v) -> Vertex {
            return {ax, ay, u, v, len, radius, endRadius<0?radius:endRadius, dx, dy};
        };
        mesh.insert(mesh.end(), {
            make(0.0f, -1.0f), make(1.0f, -1.0f), make(1.0f, 1.0f),
            make(0.0f, -1.0f), make(1.0f, 1.0f), make(0.0f, 1.0f)
        });
    }

    void addPoint(
            std::vector<float>& points,
            Mesh& mesh,
            float x,
            float y,
            float radius,float& peak,float departure
    ) {

        if (!points.empty()) {

            float px = points[points.size() - 2];
            float py = points.back();

            if (px!=x || py!=y) {
                if(peak>departure) quad(mesh,px,py,px,py,peak);
                quad(
                        mesh,
                        px,
                        py,
                        x,
                        y,
                        departure,radius
                );
                peak=radius;
            } else peak=std::max(radius,peak);
        } else peak=radius;

        points.push_back(x);
        points.push_back(y);
    }

// --------------------------------------------------
// Incremental spline visualization
// --------------------------------------------------

    float brushRadius(const Trace& t,float pressure) {
        return t.strokeRadiusWorld*(0.35f+1.15f*pressure);
    }
    constexpr int maxSteps=stroke::IncrementalSplineBuilder::max_steps;

    void tessellateSegment(Mesh& mesh, const Trace& trace,
                           size_t segment) {
        auto first = trace.spline.geometry_at(segment, 0.0);
        auto last = trace.spline.geometry_at(segment, 1.0);
        if (!first || !last) return;
        float px = (float)first.value().x;
        float py = (float)first.value().y;
        const double tolerance=.20*trace.strokeRadiusWorld/1.9; // at capture zoom
        int steps=trace.spline.tessellation_steps(segment,tolerance);
        float startRadius=brushRadius(trace,trace.spline.departure_pressure(segment));
        float endRadius=brushRadius(trace,trace.spline.arrival_pressure(segment+1));
        float peak=brushRadius(trace,trace.spline.knot_pressure(segment));
        if(peak>startRadius) quad(mesh,px,py,px,py,peak);
        float radius=startRadius;
        for (int k = 1; k <= steps; ++k) {
            auto value = trace.spline.geometry_at(segment, (double)k / steps);
            if (!value) continue;
            const float x = (float)value.value().x;
            const float y = (float)value.value().y;
            float next=std::lerp(startRadius,endRadius,(float)k/steps);
            quad(mesh, px, py, x, y, radius,next);
            radius=next;
            px = x; py = y;
        }
    }

    void buildStable(Trace& trace) {
        const size_t stable = trace.spline.stable_segment_count();
        for (size_t i = trace.stableBuilt; i < stable; ++i)
            tessellateSegment(trace.stableMesh, trace, i);
        trace.stableBuilt = stable;
        // The last segment is provisional until another knot arrives. Rebuild
        // only this small tail; the long stable part is strictly append-only.
        trace.tailMesh.clear();
        for (size_t i = stable; i < trace.spline.segment_count(); ++i)
            tessellateSegment(trace.tailMesh, trace, i);
        ++trace.tailRevision;
    }

    template<class T> void reserveExtra(std::vector<T>& v, size_t extra) {
        if (extra > v.max_size()-v.size()) throw std::length_error("trace capacity");
        if (v.capacity()-v.size() < extra)
            v.reserve(std::max(v.size()+extra,v.size()+std::min(v.size(),v.max_size()-v.size())));
    }
    // Last fallible step of ingestion: reserve worst-case emission slots for
    // one new stable and one replacement tail segment, then commit the spline.
    core::Result<void> prepareSample(void* ctx, const input::CleanInputPoint& c) {
        auto& t=*static_cast<Trace*>(ctx);
        const bool moving=t.input.clean_samples().empty() ||
            t.input.clean_samples().back().position.x!=c.position.x ||
            t.input.clean_samples().back().position.y!=c.position.y;
        try {
            reserveExtra(t.rawPoints,2); reserveExtra(t.cleanPoints,2);
            reserveExtra(t.rawMesh,18); reserveExtra(t.cleanMesh,24);
            if(moving && !t.input.clean_samples().empty()) {
                constexpr size_t maxPart=(maxSteps+1)*6;
                reserveExtra(t.stableMesh,2*maxPart+12);
                if(t.tailMesh.capacity()<maxPart) t.tailMesh.reserve(maxPart);
            } else reserveExtra(t.stableMesh,12);
        } catch(const std::bad_alloc&) {
            return core::Error{core::ErrorCode::AllocationFailure,"Trace allocation failed"};
        } catch(const std::length_error&) {
            return core::Error{core::ErrorCode::ResourceLimit,"Trace capacity limit"};
        }
        return t.spline.append({c.position,c.timestamp,c.pressure,c.tilt_x,c.tilt_y,c.speed});
    }

// --------------------------------------------------
// GPU drawing
// --------------------------------------------------

    void bindAttributes() {
        glVertexAttribPointer(
                positionLoc,
                2,
                GL_FLOAT,
                GL_FALSE,
                sizeof(Vertex),
                (void*)offsetof(Vertex, x)
        );

        glVertexAttribPointer(
                localLoc,
                2,
                GL_FLOAT,
                GL_FALSE,
                sizeof(Vertex),
                (void*)offsetof(Vertex, u)
        );

        glVertexAttribPointer(
                lengthLoc,
                1,
                GL_FLOAT,
                GL_FALSE,
                sizeof(Vertex),
                (void*)offsetof(Vertex, len)
        );

        glVertexAttribPointer(
                radiusLoc,
                2,
                GL_FLOAT,
                GL_FALSE,
                sizeof(Vertex),
                (void*)offsetof(Vertex, radius)
        );
        glVertexAttribPointer(directionLoc, 2, GL_FLOAT, GL_FALSE,
                              sizeof(Vertex), (void*)offsetof(Vertex, dirX));
    }

    void drawCached(GLuint vbo, GLsizei count, float r, float g, float b) {
        if (!program || !vbo || count <= 0) return;
        glUniform4f(colorLoc, r, g, b, 1.0f);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        bindAttributes();
        glDrawArrays(GL_TRIANGLES, 0, count);
        ++frameDrawCalls;
    }

    void draw(const Mesh& mesh, float r, float g, float b) {
        if (mesh.empty() || !program || !buffer) return;
        const size_t bytes = mesh.size() * sizeof(Vertex);
        glBindBuffer(GL_ARRAY_BUFFER, buffer);
        glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)bytes, mesh.data());
        frameUploadBytes += bytes;
        liveUploadBytes += bytes;
        drawCached(buffer, (GLsizei)mesh.size(), r, g, b);
    }

    struct LiveUpload {
        size_t total = 0;
        size_t offset = 0;
        uint64_t revision = 0;
        std::vector<Vertex> data;
    };

    LiveUpload snapshotUpload(const Mesh& mesh, uint64_t id, int part, uint64_t revision = 0) {
        LiveUpload result;
        result.total = mesh.size();
        result.revision = revision;
        const auto gpu = gpuLive.find(id);
        const LiveGpuMesh* previous = gpu == gpuLive.end() ? nullptr : &gpu->second[part];
        const size_t uploaded = previous ? previous->uploadedVertices : 0;
        const size_t capacity = previous ? previous->capacityVertices : 0;
        if (part == 3 && previous && previous->revision == revision && uploaded == result.total)
            return result;
        result.offset = (part == 3 || result.total < uploaded || result.total > capacity)
                ? 0 : uploaded;
        if (result.total > result.offset)
            result.data.assign(mesh.begin() + result.offset, mesh.end());
        return result;
    }

    void drawLive(uint64_t strokeId, int part, const LiveUpload& mesh, float r, float g, float b) {
        if (mesh.total == 0 || !program) return;
        auto& cached = gpuLive[strokeId][part];
        if (!cached.vbo) glGenBuffers(1, &cached.vbo);
        if (!cached.vbo) return;
        glBindBuffer(GL_ARRAY_BUFFER, cached.vbo);
        const size_t n = mesh.total;
        if (n > cached.capacityVertices) {
            const size_t cap = std::max(n, std::max(size_t(256), cached.capacityVertices * 2));
            glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(cap * sizeof(Vertex)), nullptr, GL_DYNAMIC_DRAW);
            cached.capacityVertices = cap;
            ++liveBufferReallocations;
        }
        if (!mesh.data.empty()) {
            glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)(mesh.offset * sizeof(Vertex)),
                            (GLsizeiptr)(mesh.data.size() * sizeof(Vertex)), mesh.data.data());
            frameUploadBytes += mesh.data.size() * sizeof(Vertex);
            liveUploadBytes += mesh.data.size() * sizeof(Vertex);
            if (mesh.offset > 0) ++liveIncrementalUpdates;
        }
        cached.uploadedVertices = n;
        cached.revision = mesh.revision;
        drawCached(cached.vbo, (GLsizei)n, r, g, b);
    }

    void deleteGpuCache() {
        for (auto& item : gpuFinished)
            for (GLuint vbo : item.vbo) if (vbo) glDeleteBuffers(1, &vbo);
        gpuFinished.clear();
        glFinishedCount = 0;
    }

    void cacheFinished(const FinishedTrace& trace) {
        CachedGpuStroke item;
        item.source = {trace.raw, trace.clean, trace.spline};
        item.minX = trace.minX; item.minY = trace.minY;
        item.maxX = trace.maxX; item.maxY = trace.maxY;
        gpuFinished.push_back(std::move(item));
    }

    void drawFinishedPart(CachedGpuStroke& item, int part, float r, float g, float b) {
        const auto& vertices = *item.source[part];
        if (vertices.empty()) return;
        if (!item.vbo[part]) {
            glGenBuffers(1, &item.vbo[part]);
            if (!item.vbo[part]) return;
            glBindBuffer(GL_ARRAY_BUFFER, item.vbo[part]);
            const size_t bytes = vertices.size() * sizeof(Vertex);
            glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)bytes, vertices.data(), GL_STATIC_DRAW);
            item.count[part] = (GLsizei)vertices.size();
            frameUploadBytes += bytes;
            cacheUploadBytes += bytes;
        }
        drawCached(item.vbo[part], item.count[part], r, g, b);
    }

    struct ViewTrace {
        LiveUpload raw, clean, stable, tail;
        float lastX = 0.0f, lastY = 0.0f;
        float filteredX = 0.0f, filteredY = 0.0f;
        bool hasLast = false, hasFiltered = false;
        float radiusWorld = 1.9f;
        float filteredRadius = 1.9f;
    };

} // namespace

// ==================================================
// JNI interface
// ==================================================

extern "C" {

// --------------------------------------------------
// Camera
// --------------------------------------------------

JNIEXPORT void JNICALL
Java_com_infinipaint_demo_NativeBridge_setCamera(
        JNIEnv*,
        jclass,
        jdouble panX,
        jdouble panY,
        jdouble zoom
) {

    if (
            !std::isfinite(panX) ||
            !std::isfinite(panY) ||
            !std::isfinite(zoom) ||
            zoom <= 0.0
            ) {
        return;
    }

    std::lock_guard<std::mutex> guard(stateMutex);

    cameraPanX = panX;
    cameraPanY = panY;

    cameraZoom = std::clamp(
            (double)zoom,
            MIN_ZOOM,
            MAX_ZOOM
    );
}

JNIEXPORT void JNICALL
Java_com_infinipaint_demo_NativeBridge_setSmoothingPreset(JNIEnv*, jclass, jint preset) {
    smoothingPreset.store(std::clamp((int)preset, 0, 2), std::memory_order_relaxed);
}

// --------------------------------------------------
// Begin stroke
// --------------------------------------------------

JNIEXPORT jboolean JNICALL
Java_com_infinipaint_demo_NativeBridge_begin(
        JNIEnv*,
        jclass,
        jint id,
        jint
) {

    std::lock_guard<std::mutex> guard(stateMutex);

    // Cutoffs in world units. Three intentionally distinct presets; stronger
    // smoothing has more geometric phase lag, while raw tip preview stays live.
    input::OneEuroParams params;
    switch (smoothingPreset.load(std::memory_order_relaxed)) {
        case 0: params.min_cutoff = 8.0; params.beta = 0.025; params.dcutoff = 1.5; break;
        case 2: params.min_cutoff = 1.6; params.beta = 0.002; params.dcutoff = 1.0; break;
        default: params.min_cutoff = 3.5; params.beta = 0.009; params.dcutoff = 1.0; break;
    }
    auto inputProcessor =
            input::InputProcessor::create(params);

    auto fitter =
            stroke::SplineFitter::create();

    if (!inputProcessor || !fitter) {
        return JNI_FALSE;
    }

    auto trace = std::make_unique<Trace>(
            std::move(inputProcessor.value()),
            std::move(fitter.value())
    );

    trace->strokeRadiusWorld = (float)(1.9 / cameraZoom);
    if (!trace->input.begin_stroke()) {
        return JNI_FALSE;
    }

    trace->gpuId = nextLiveGpuId++;
    live[id] = std::move(trace);

    return JNI_TRUE;
}

// --------------------------------------------------
// Receive input sample
// --------------------------------------------------

JNIEXPORT jboolean JNICALL
Java_com_infinipaint_demo_NativeBridge_sample(
        JNIEnv*,
        jclass,
        jint id,
        jdouble x,
        jdouble y,
        jdouble time,
        jfloat pressure,
        jfloat tx,
        jfloat ty
) {

    const int64_t processingStartNs = profileNs();
    std::lock_guard<std::mutex> guard(stateMutex);
    auditMutexWaitNs.fetch_add(profileNs() - processingStartNs, std::memory_order_relaxed);

    auto it = live.find(id);

    if (it == live.end()) {
        return JNI_FALSE;
    }

    auto& trace = *it->second;

    // Convert screen-space input into world space.
    // This keeps strokes anchored when panning or zooming.

    double worldX =
            cameraPanX + x / cameraZoom;

    double worldY =
            cameraPanY + y / cameraZoom;

    if (
            !std::isfinite(worldX) ||
            !std::isfinite(worldY)
            ) {
        return JNI_FALSE;
    }

    // Original event times can tie; arrival order resolves the tie.
    if (!std::isfinite(time)) {
        __android_log_print(ANDROID_LOG_ERROR, "IP90_INPUT", "Native sample: non-finite timestamp");
        return JNI_FALSE;
    }
    if (trace.lastAcceptedTime >= 0.0 && time < trace.lastAcceptedTime) return JNI_FALSE;

    input::RawInputPoint point{
            {worldX, worldY},
            time,
            pressure,
            tx,
            ty
    };

    const size_t oldSegments=trace.spline.segment_count();
    const int64_t filterStartNs = profileNs();
    auto clean = trace.input.process(point,&trace,prepareSample);
    const int64_t commitNs=profileNs()-filterStartNs;

    if (!clean) {
        auto err = clean.error();
        __android_log_print(ANDROID_LOG_ERROR, "IP90_INPUT",
                            "Native input rejected: %.*s (code=%d, time=%.9f, last=%.9f, pressure=%.4f)",
                            (int)err.message.size(), err.message.data(), (int)err.code,
                            time, trace.lastAcceptedTime, (double)pressure);
        return JNI_FALSE;
    }
    trace.lastAcceptedTime = time;
    auditFilterNs.fetch_add(commitNs,std::memory_order_relaxed);

    // Immediate ink uses actual input positions.

    addPoint(
            trace.rawPoints,
            trace.rawMesh,
            (float)worldX,
            (float)worldY,
            brushRadius(trace,pressure),trace.rawPeak,trace.lastRadius
    );

    auto c = clean.value();
    // Diagnostic comparison uses the genuine One-Euro output, not the old
    // directional/2px fidelity limiter that hid the difference from raw.
    const double deviation = std::hypot(c.position.x - worldX, c.position.y - worldY) * cameraZoom;
    trace.originalDeviationMax = std::max(trace.originalDeviationMax, deviation);
    trace.filteredBeforeLimitSum += deviation;
    if (deviation > 2.0) ++trace.wouldExceedLimit;
    trace.deviationSum += deviation;
    trace.deviationMax = std::max(trace.deviationMax, deviation);
    ++trace.deviationCount;

    addPoint(
            trace.cleanPoints,
            trace.cleanMesh,
            (float)c.position.x,
            (float)c.position.y,
            brushRadius(trace,pressure),trace.cleanPeak,trace.lastRadius
    );

    const int64_t splineStartNs = profileNs();
    // Tessellate newly stable segments only, when the stable count advances.
    trace.lastRadius=brushRadius(trace,pressure);
    if(trace.spline.segment_count()!=oldSegments)
        buildStable(trace);
    auditSplineNs.fetch_add(profileNs() - splineStartNs, std::memory_order_relaxed);
    auditStageSamples.fetch_add(1, std::memory_order_relaxed);

    // Android MotionEvent.getEventTime() is based on uptime milliseconds.
    // Compare against steady clock as a useful estimate, not photon latency.
    const int64_t nowNs = profileNs();
    const int64_t eventNs = static_cast<int64_t>(time * 1000000000.0);
    newestEventNs.store(eventNs, std::memory_order_relaxed);
    latestSampleEndNs.store(nowNs, std::memory_order_relaxed);
    sampleProcessingUs.fetch_add((nowNs - processingStartNs) / 1000,
                                 std::memory_order_relaxed);
    processedSamples.fetch_add(1, std::memory_order_relaxed);
    return JNI_TRUE;
}

// --------------------------------------------------
// End stroke
// --------------------------------------------------

JNIEXPORT jboolean JNICALL
Java_com_infinipaint_demo_NativeBridge_end(
        JNIEnv*,
        jclass,
        jint id
) {

    std::lock_guard<std::mutex> guard(stateMutex);

    auto it = live.find(id);

    if (it == live.end()) {
        return JNI_FALSE;
    }

    auto& trace = *it->second;

    if (trace.deviationCount) {
        __android_log_print(ANDROID_LOG_INFO, "IP90_FIDELITY",
                            "samples=%zu mean_deviation_px=%.3f max_deviation_px=%.3f raw_filter_mean_px=%.3f raw_filter_max_px=%.3f would_exceed_2px=%zu (%.1f%%) zoom=%.3f",
                            trace.deviationCount, trace.deviationSum / trace.deviationCount,
                            trace.deviationMax, trace.filteredBeforeLimitSum / trace.deviationCount,
                            trace.originalDeviationMax, trace.wouldExceedLimit,
                            100.0 * trace.wouldExceedLimit / trace.deviationCount,
                            1.9 / trace.strokeRadiusWorld);
    }
    auto& splineMesh=trace.stableMesh;
    splineMesh.insert(splineMesh.end(),trace.tailMesh.begin(),trace.tailMesh.end());
    // Preserve the physical final pen position even if Android supplied
    // multiple different positions with the same timestamp.
    if (trace.rawPoints.size() >= 2 && trace.cleanPoints.size() >= 2) {
        const float rx = trace.rawPoints[trace.rawPoints.size()-2];
        const float ry = trace.rawPoints.back();
        const float cx = trace.cleanPoints[trace.cleanPoints.size()-2];
        const float cy = trace.cleanPoints.back();
        if (std::hypot(rx-cx, ry-cy) > 0.03f) {
            quad(trace.cleanMesh, cx, cy, rx, ry, trace.cleanPeak,trace.rawPeak);
            quad(splineMesh, cx, cy, rx, ry, trace.cleanPeak,trace.rawPeak);
        }
        for(auto* mesh:{&trace.rawMesh,&trace.cleanMesh,&splineMesh})
            quad(*mesh,rx,ry,rx,ry,trace.rawPeak);
    }
    const size_t rawSamples = trace.rawPoints.size() / 2;
    const size_t cleanSamples = trace.cleanPoints.size() / 2;
    __android_log_print(ANDROID_LOG_INFO, "IP_GEOMETRY",
        "raw_samples=%zu filtered_samples=%zu raw_vertices=%zu filtered_vertices=%zu spline_vertices=%zu mean_filter_deviation_px=%.4f max_filter_deviation_px=%.4f",
        rawSamples, cleanSamples, trace.rawMesh.size(), trace.cleanMesh.size(),
        splineMesh.size(), trace.deviationCount ? trace.deviationSum / trace.deviationCount : 0.0,
        trace.deviationMax);
    // World bounds include expanded quad coverage for safe camera culling.
    float minX = std::numeric_limits<float>::infinity();
    float minY = std::numeric_limits<float>::infinity();
    float maxX = -minX, maxY = -minY;
    for (const Mesh* mesh : {&trace.rawMesh, &trace.cleanMesh, &splineMesh}) {
        for (const Vertex& v : *mesh) {
            const float bx = v.x + v.dirX * v.len;
            const float by = v.y + v.dirY * v.len;
            const float pad = std::max(v.radius,v.endRadius) + 1.0f / (float)MIN_ZOOM;
            minX = std::min(minX, std::min(v.x, bx) - pad);
            minY = std::min(minY, std::min(v.y, by) - pad);
            maxX = std::max(maxX, std::max(v.x, bx) + pad);
            maxY = std::max(maxY, std::max(v.y, by) + pad);
        }
    }
    finished.push_back({
        std::make_shared<const input::RawInputStroke>(trace.input.raw_samples()),
        std::make_shared<const Mesh>(std::move(trace.rawMesh)),
        std::make_shared<const Mesh>(std::move(trace.cleanMesh)),
        std::make_shared<const Mesh>(std::move(splineMesh)),
        minX, minY, maxX, maxY
    });

    auto ended = trace.input.end_stroke();
    (void)ended;

    live.erase(it);

    return JNI_TRUE;
}

// --------------------------------------------------
// Cancel stroke
// --------------------------------------------------

JNIEXPORT void JNICALL
Java_com_infinipaint_demo_NativeBridge_cancel(
        JNIEnv*,
        jclass,
        jint id
) {

    std::lock_guard<std::mutex> guard(stateMutex);

    live.erase(id);
}

// --------------------------------------------------
// Clear canvas
// --------------------------------------------------

JNIEXPORT void JNICALL
Java_com_infinipaint_demo_NativeBridge_clear(
        JNIEnv*,
        jclass
) {

    std::lock_guard<std::mutex> guard(stateMutex);

    live.clear();
    finished.clear();
    ++finishedEpoch;
}

// --------------------------------------------------
// Initialize / resize OpenGL surface
// --------------------------------------------------

JNIEXPORT void JNICALL
Java_com_infinipaint_demo_NativeBridge_resizeViewport(
        JNIEnv*,
        jclass,
        jint w,
        jint h
) {

    width = std::max(1, (int)w);
    height = std::max(1, (int)h);

    glViewport(0, 0, width, height);
}

JNIEXPORT void JNICALL
Java_com_infinipaint_demo_NativeBridge_contextCreated(JNIEnv*, jclass) {
    // Old EGL context already owns/disposes its objects. The same numeric
    // names may identify unrelated objects in the newly current context.
    gpuFinished.clear();
    gpuLive.clear();
    glFinishedCount = 0;
    gpuEpoch = 0;
    program = 0;
    buffer = 0;

    GLuint vertexShader =
            compile(GL_VERTEX_SHADER, vs);

    GLuint fragmentShader =
            compile(GL_FRAGMENT_SHADER, fs);

    if (!vertexShader || !fragmentShader) {

        if (vertexShader) {
            glDeleteShader(vertexShader);
        }

        if (fragmentShader) {
            glDeleteShader(fragmentShader);
        }

        return;
    }

    program = glCreateProgram();

    glAttachShader(program, vertexShader);
    glAttachShader(program, fragmentShader);

    glBindAttribLocation(
            program,
            0,
            "aPos"
    );

    glLinkProgram(program);

    GLint linked = 0;

    glGetProgramiv(
            program,
            GL_LINK_STATUS,
            &linked
    );

    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    if (!linked) {

        char log[1024] = {};

        glGetProgramInfoLog(
                program,
                sizeof(log) - 1,
                nullptr,
                log
        );

        __android_log_print(
                ANDROID_LOG_ERROR,
                "IP90",
                "GL program link failed: %s",
                log
        );

        glDeleteProgram(program);
        program = 0;

        return;
    }

    positionLoc =
            glGetAttribLocation(program, "aPos");

    localLoc =
            glGetAttribLocation(program, "aLocal");

    lengthLoc =
            glGetAttribLocation(program, "aLength");

    radiusLoc =
            glGetAttribLocation(program, "aRadius");
    directionLoc = glGetAttribLocation(program, "aDir");

    screenLoc =
            glGetUniformLocation(program, "uScreen");

    colorLoc =
            glGetUniformLocation(program, "uColor");

    cameraPanLoc =
            glGetUniformLocation(program, "uCameraPan");

    cameraZoomLoc =
            glGetUniformLocation(program, "uCameraZoom");

    glGenBuffers(1, &buffer);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(12 * sizeof(Vertex)), nullptr, GL_DYNAMIC_DRAW);

    glEnable(GL_BLEND);

    glBlendFunc(
            GL_ONE,
            GL_ONE_MINUS_SRC_ALPHA
    );

    if (positionLoc >= 0) {
        glEnableVertexAttribArray(positionLoc);
    }

    if (localLoc >= 0) {
        glEnableVertexAttribArray(localLoc);
    }

    if (lengthLoc >= 0) {
        glEnableVertexAttribArray(lengthLoc);
    }

    if (radiusLoc >= 0) {
        glEnableVertexAttribArray(radiusLoc);
    }
    if (directionLoc >= 0) glEnableVertexAttribArray(directionLoc);

    __android_log_print(
            ANDROID_LOG_INFO,
            "IP90",
            "Renderer initialized with camera support"
    );
}

// --------------------------------------------------
// Render frame
// --------------------------------------------------

JNIEXPORT void JNICALL
Java_com_infinipaint_demo_NativeBridge_render(
        JNIEnv*,
        jclass,
        jint mode
) {

    const int64_t frameStartNs = profileNs();
    if (lastFrameStartNs != 0 && frameStartNs - lastFrameStartNs < 100000000LL) {
        sumFrameIntervalMs += (frameStartNs - lastFrameStartNs) / 1e6;
        ++validFrameIntervals;
    }
    lastFrameStartNs = frameStartNs;
    frameDrawCalls = 0;
    frameUploadBytes = 0;
    const int64_t eventNs = newestEventNs.load(std::memory_order_relaxed);
    const int64_t sampleEndNs = latestSampleEndNs.load(std::memory_order_relaxed);
    if (eventNs > 0 && eventNs <= frameStartNs && frameStartNs - eventNs < 50000000LL) {
        const double age = (frameStartNs - eventNs) / 1e6;
        sumInputAgeMs += age;
        maxInputAgeMs = std::max(maxInputAgeMs, age);
        ++validInputAgeFrames;
    }
    if (sampleEndNs > 0 && sampleEndNs <= frameStartNs && frameStartNs - sampleEndNs < 50000000LL) {
        sumSampleToFrameMs += (frameStartNs - sampleEndNs) / 1e6;
        ++validSampleFrames;
    }

    std::vector<FinishedTrace> newCompleted;
    uint64_t frameEpoch = 0;
    std::vector<std::pair<uint64_t, ViewTrace>> active;

    // Snapshot camera state for this frame.

    double framePanX = 0.0;
    double framePanY = 0.0;
    double frameZoom = 1.0;

    {
        const int64_t snapshotStartNs = profileNs();
        std::lock_guard<std::mutex> guard(stateMutex);

        framePanX = cameraPanX;
        framePanY = cameraPanY;
        frameZoom = cameraZoom;

        frameEpoch = finishedEpoch;
        // Epoch mismatch means clear; GL thread will retire old cache below.
        if (gpuEpoch != frameEpoch) glFinishedCount = 0;
        for (size_t i = glFinishedCount; i < finished.size(); ++i)
            newCompleted.push_back(finished[i]);
        glFinishedCount = finished.size();

        active.reserve(live.size());

        for (auto& pair : live) {

            auto& trace = *pair.second;

            ViewTrace view;
            view.radiusWorld = trace.rawPeak;
            view.filteredRadius = trace.cleanPeak;
            if (mode == 0 || mode == 3)
                view.raw = snapshotUpload(trace.rawMesh, trace.gpuId, 0);
            if (mode == 1 || mode == 3)
                view.clean = snapshotUpload(trace.cleanMesh, trace.gpuId, 1);
            if (mode == 2 || mode == 3) {
                view.stable = snapshotUpload(trace.stableMesh, trace.gpuId, 2);
                view.tail = snapshotUpload(trace.tailMesh, trace.gpuId, 3, trace.tailRevision);
            }
            if (trace.cleanPoints.size() >= 2) {
                view.filteredX = trace.cleanPoints[trace.cleanPoints.size()-2];
                view.filteredY = trace.cleanPoints.back();
                view.hasFiltered = true;
            }

            if (!trace.rawPoints.empty()) {

                view.lastX =
                        trace.rawPoints[
                                trace.rawPoints.size() - 2
                        ];

                view.lastY =
                        trace.rawPoints.back();

                view.hasLast = true;
            }

            active.emplace_back(trace.gpuId, std::move(view));
        }
        auditSnapshotNs.fetch_add(profileNs() - snapshotStartNs, std::memory_order_relaxed);
        auditSnapshotFrames.fetch_add(1, std::memory_order_relaxed);
    }

    // No input mutex is held during OpenGL calls.

    glClearColor(
            0.025f,
            0.055f,
            0.09f,
            1.0f
    );

    glClear(GL_COLOR_BUFFER_BIT);

    if (!program || !buffer) {
        return;
    }

    glUseProgram(program);

    glUniform2f(
            screenLoc,
            (float)width,
            (float)height
    );

    glUniform2f(
            cameraPanLoc,
            (float)framePanX,
            (float)framePanY
    );

    glUniform1f(
            cameraZoomLoc,
            (float)frameZoom
    );

    // Cache completed meshes on the GL thread. A clear increments the epoch.
    if (gpuEpoch != frameEpoch) {
        deleteGpuCache();
        gpuEpoch = frameEpoch;
        // This frame already captured the full replacement snapshot.
        glFinishedCount = newCompleted.size();
    }
    for (const FinishedTrace& trace : newCompleted) cacheFinished(trace);
    const double viewMinX = framePanX, viewMinY = framePanY;
    const double viewMaxX = framePanX + width/frameZoom;
    const double viewMaxY = framePanY + height/frameZoom;
    uint64_t visibleCompleted = 0;
    for (auto& cached : gpuFinished) {
        if (cached.maxX < viewMinX || cached.minX > viewMaxX ||
            cached.maxY < viewMinY || cached.minY > viewMaxY) continue;
        ++visibleCompleted;
        auto drawPart = [&](int part, float r, float g, float b) {
            drawFinishedPart(cached, part, r, g, b);
        };
        if (mode == 0) drawPart(0, 0.20f, 0.95f, 0.78f);
        else if (mode == 1) drawPart(1, 1.0f, 0.76f, 0.20f);
        else if (mode == 2) drawPart(2, 0.28f, 0.58f, 1.0f);
        else {
            drawPart(2, 0.28f, 0.58f, 1.0f);
            drawPart(1, 1.0f, 0.76f, 0.20f);
            drawPart(0, 0.20f, 0.95f, 0.78f);
        }
    }

    // Retire live buffers after a stroke ends/cancels. GL thread owns deletion.
    for (auto it = gpuLive.begin(); it != gpuLive.end();) {
        bool present = false;
        for (const auto& entry : active) if (entry.first == it->first) { present = true; break; }
        if (!present) {
            for (auto& part : it->second) if (part.vbo) glDeleteBuffers(1, &part.vbo);
            it = gpuLive.erase(it);
        } else ++it;
    }

    // Active strokes use the same mapping as completed strokes.
    for (auto& entry : active) {
        const uint64_t strokeId = entry.first;
        auto& trace = entry.second;
        auto tip=[&](bool connector,float r,float g,float b) {
            if(!trace.hasLast) return;
            Mesh dot;
            if (connector && trace.hasFiltered &&
                std::hypot(trace.lastX-trace.filteredX, trace.lastY-trace.filteredY) > 0.03f)
                quad(dot, trace.filteredX, trace.filteredY,
                     trace.lastX, trace.lastY, trace.filteredRadius,trace.radiusWorld);
            quad(dot, trace.lastX, trace.lastY,
                 trace.lastX, trace.lastY, trace.radiusWorld);

            draw(dot,r,g,b);
        };
        if(mode==2 || mode==3) {
            drawLive(strokeId,2,trace.stable,.28f,.58f,1.f);
            drawLive(strokeId,3,trace.tail,.28f,.58f,1.f);
            tip(true,.28f,.58f,1.f);
        }
        if(mode==1 || mode==3) {
            drawLive(strokeId,1,trace.clean,1.f,.76f,.20f);
            tip(true,1.f,.76f,.20f);
        }
        if(mode==0 || mode==3) {
            drawLive(strokeId,0,trace.raw,.20f,.95f,.78f);
            tip(false,.20f,.95f,.78f);
        }
    }
    const double renderCpuMs = (profileNs() - frameStartNs) / 1e6;
    sumRenderCpuMs += renderCpuMs;
    maxRenderCpuMs = std::max(maxRenderCpuMs, renderCpuMs);
    ++frameIndex;
    // Periodic low-volume profiling, never log on each stylus sample.
    static uint64_t windowUploads = 0;
    static uint64_t windowDraws = 0;
    static uint64_t windowCacheUploads = 0;
    static uint64_t windowLiveUploads = 0;
    windowUploads += frameUploadBytes;
    windowDraws += frameDrawCalls;
    windowCacheUploads += cacheUploadBytes;
    windowLiveUploads += liveUploadBytes;
    cacheUploadBytes = 0;
    liveUploadBytes = 0;
    if (frameIndex % PROFILE_FRAME_WINDOW == 0) {
        const uint64_t stageSamples = auditStageSamples.exchange(0, std::memory_order_relaxed);
        const int64_t waitNs = auditMutexWaitNs.exchange(0, std::memory_order_relaxed);
        const int64_t filterNs = auditFilterNs.exchange(0, std::memory_order_relaxed);
        const int64_t splineNs = auditSplineNs.exchange(0, std::memory_order_relaxed);
        const int64_t snapshotNs = auditSnapshotNs.exchange(0, std::memory_order_relaxed);
        const uint64_t snapshotFrames = auditSnapshotFrames.exchange(0, std::memory_order_relaxed);
        __android_log_print(ANDROID_LOG_INFO, "IP_PIPELINE",
            "samples=%llu mutex_wait_avg_us=%.2f input_reserve_spline_commit_avg_us=%.2f tessellate_avg_us=%.2f snapshot_avg_us=%.2f (CPU only; not display presentation)",
            (unsigned long long)stageSamples,
            stageSamples ? (double)waitNs / stageSamples / 1000.0 : -1.0,
            stageSamples ? (double)filterNs / stageSamples / 1000.0 : -1.0,
            stageSamples ? (double)splineNs / stageSamples / 1000.0 : -1.0,
            snapshotFrames ? (double)snapshotNs / snapshotFrames / 1000.0 : -1.0);
        const uint64_t samples = processedSamples.exchange(0, std::memory_order_relaxed);
        const int64_t processUs = sampleProcessingUs.exchange(0, std::memory_order_relaxed);
        __android_log_print(ANDROID_LOG_INFO, "IP90_PROFILE",
                            "frames=%llu frame_interval_ms=%.2f render_cpu_avg_ms=%.3f render_cpu_max_ms=%.3f "
                            "input_age_avg_ms=%.2f input_age_max_ms=%.2f sample_to_frame_avg_ms=%.2f "
                            "processing_avg_us=%.1f samples=%llu draw_calls_avg=%.1f upload_kb_avg=%.1f cache_upload_kb_avg=%.1f live_upload_kb_avg=%.1f gpu_cached=%zu completed=%zu live=%zu mode=%d",
                            (unsigned long long)PROFILE_FRAME_WINDOW,
                            validFrameIntervals ? sumFrameIntervalMs / validFrameIntervals : -1.0,
                            sumRenderCpuMs / PROFILE_FRAME_WINDOW, maxRenderCpuMs,
                            validInputAgeFrames ? sumInputAgeMs / validInputAgeFrames : -1.0,
                            validInputAgeFrames ? maxInputAgeMs : -1.0,
                            validSampleFrames ? sumSampleToFrameMs / validSampleFrames : -1.0,
                            samples ? (double)processUs / samples : -1.0,
                            (unsigned long long)samples,
                            (double)windowDraws / PROFILE_FRAME_WINDOW,
                            (double)windowUploads / PROFILE_FRAME_WINDOW / 1024.0,
                            (double)windowCacheUploads / PROFILE_FRAME_WINDOW / 1024.0,
                            (double)windowLiveUploads / PROFILE_FRAME_WINDOW / 1024.0,
                            gpuFinished.size(), gpuFinished.size(), active.size(), (int)mode);
        __android_log_print(ANDROID_LOG_INFO, "IP_VIEW_CULL",
            "total_finished=%zu visible_finished=%llu mode=%d",
            gpuFinished.size(), (unsigned long long)visibleCompleted, (int)mode);
        __android_log_print(ANDROID_LOG_INFO, "IP_GPU_INCREMENTAL",
            "live_reallocations=%llu incremental_appends=%llu live_upload_bytes_window=%llu",
            (unsigned long long)liveBufferReallocations,
            (unsigned long long)liveIncrementalUpdates,
            (unsigned long long)windowLiveUploads);
        liveBufferReallocations = liveIncrementalUpdates = 0;
        sumFrameIntervalMs = 0.0;
        validFrameIntervals = 0;
        sumRenderCpuMs = 0.0;
        sumInputAgeMs = 0.0;
        sumSampleToFrameMs = 0.0;
        validInputAgeFrames = 0;
        validSampleFrames = 0;
        maxRenderCpuMs = 0.0;
        maxInputAgeMs = 0.0;
        windowUploads = 0;
        windowDraws = 0;
        windowCacheUploads = 0;
        windowLiveUploads = 0;
    }
}

} // extern "C"
