
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
#include <deque>
#include <string_view>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "input/input_processor.hpp"
#include "document/document.hpp"
#include "document/change_queue.hpp"
#include "document/codec.hpp"
#include "document/eraser.hpp"
#include "document/working_set.hpp"
#include "tablet/session.hpp"
#include "tablet/view_intent.hpp"
#include <cstring>
#include "stroke/spline.hpp"

namespace engine::tablet {
struct AdoptAccepted final : document::Ink {
    AdoptAccepted(input::InputProcessor& p,document::Brush b):Ink(std::move(p).release_raw(),b,true) {}
};
}
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
    constexpr double MIN_ZOOM = 0.1;
    struct Bounds {
        float minX=std::numeric_limits<float>::infinity(),minY=minX,maxX=-minX,maxY=-minX;
        void include(const Mesh& mesh,size_t from) {
            // Six identical segment descriptors per quad; inspect only newly
            // emitted quads. Obsolete tail bounds are safe false positives.
            for(size_t i=from;i<mesh.size();i+=6) {
                const auto& v=mesh[i];
                const float x=v.x+v.dirX*v.len,y=v.y+v.dirY*v.len;
                const float pad=std::max(v.radius,v.endRadius)+1.f/(float)MIN_ZOOM;
                minX=std::min(minX,std::min(v.x,x)-pad); minY=std::min(minY,std::min(v.y,y)-pad);
                maxX=std::max(maxX,std::max(v.x,x)+pad); maxY=std::max(maxY,std::max(v.y,y)+pad);
            }
        }
    };
    std::atomic<uint64_t> retainedMeshes{0},activeHigh{0},switchHigh{0},gpuRequested{0},gpuHigh{0};
    std::atomic<uint64_t> historyCharge{0},queueHigh{0},canonicalCharge{0},snapshotHigh{0};
    std::atomic<uint64_t> physicalSlice{0},sceneOwners{0},candidateNodes{0},liveVboCharge{0};
    bool admits(uint64_t activeMesh,uint64_t samples,uint64_t decoding=0,uint64_t extraNodes=0) {
        const auto slice=physicalSlice.load(std::memory_order_relaxed);
        if(!slice) return true; // host renderer fixtures have no Android process
        return document::WorkingSet::forSlice(slice).admits(document::Ink::liveBytes.load(),
                retainedMeshes.load(),sceneOwners.load()+candidateNodes.load()+4+extraNodes,
                activeMesh,samples,decoding,liveVboCharge.load());
    }
    // Count allocations, not map sizes sampled at publication boundaries. swap
    // and node-handle merge transfer ownership without changing this ledger.
    template<class T> struct SceneAllocator {
        using value_type=T;
        SceneAllocator()=default;
        template<class U> SceneAllocator(const SceneAllocator<U>&) noexcept {}
        template<class U> bool operator==(const SceneAllocator<U>&) const noexcept {return true;}
        T* allocate(size_t n) {
            sceneOwners.fetch_add(n);
            try {
                if(!admits(0,0)) throw std::bad_alloc{};
                return std::allocator<T>{}.allocate(n);
            } catch(...) {sceneOwners.fetch_sub(n);throw;}
        }
        void deallocate(T* p,size_t n) noexcept {
            std::allocator<T>{}.deallocate(p,n);sceneOwners.fetch_sub(n);
        }
    };
    void peak(std::atomic<uint64_t>& counter,uint64_t value) {
        auto prev=counter.load(std::memory_order_relaxed);
        while(prev<value && !counter.compare_exchange_weak(prev,value,std::memory_order_relaxed)) {}
    }
    struct GeometryCharge {
        uint64_t bytes=0;
        // One geometry per stroke, one possible cache node. Aliasing GPU source
        // pointers retain this credit even after its Published nodes disappear.
        GeometryCharge(){sceneOwners.fetch_add(1);}
        ~GeometryCharge(){retainedMeshes.fetch_sub(bytes,std::memory_order_relaxed);sceneOwners.fetch_sub(1);}
    };
    // The base is destroyed AFTER the member vectors finish deallocation.
    struct CompletedGeometry : GeometryCharge {std::array<Mesh,3> derived;};

    struct Trace {
        tablet::ViewIntent captureView;
        uint64_t gpuId = 0;
        input::InputProcessor input;
        stroke::IncrementalSplineBuilder spline;

        Mesh rawMesh;
        Mesh cleanMesh;
        Mesh stableMesh;
        Mesh tailMesh;
        Bounds bounds;
        document::Brush brush;
        size_t meshAllowance=SIZE_MAX;
        size_t scratchAllowance=SIZE_MAX;
        std::shared_ptr<const document::Ink> sealed;
        std::shared_ptr<CompletedGeometry> completion=std::make_shared<CompletedGeometry>();
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
        std::shared_ptr<const document::Ink> record;
        std::shared_ptr<const Mesh> raw;
        std::shared_ptr<const Mesh> clean;
        std::shared_ptr<const Mesh> spline;
        float minX = 0, minY = 0, maxX = 0, maxY = 0;
        uint64_t liveId=0;
    };
    using Key=document::Id;
    struct Published {
        Key next=0;
        document::Revision geometry=0;
        std::shared_ptr<const FinishedTrace> shape;
        size_t bytes=0;
        std::array<float,4> rgba{};
    };
    uint64_t workingBytes(const Trace& t) {
        return t.input.raw_samples().capacity()*sizeof(input::RawInputPoint)+
               t.input.clean_samples().capacity()*sizeof(input::CleanInputPoint)+t.spline.retainedBytes()+
               (t.rawPoints.capacity()+t.cleanPoints.capacity())*sizeof(float)+
               (t.rawMesh.capacity()+t.cleanMesh.capacity()+t.stableMesh.capacity()+t.tailMesh.capacity())*sizeof(Vertex);
    }
    using Scene=std::map<Key,Published,std::less<Key>,SceneAllocator<std::pair<const Key,Published>>>;
    Scene finished, pendingScene;
    document::Revision sceneRevision=0; // belongs to the published head + patch, not the save worker
    size_t sceneBytes=0,sceneLimit=SIZE_MAX,canonicalLimit=SIZE_MAX;
    constexpr size_t publicationLimit=65536;
    Key sceneHead=0;
    std::unique_ptr<document::Document> model;
    document::ChangeQueue saves;
    bool persistent=false;
    bool suspended=false;
    std::atomic<int64_t> visibleRevision{0};
    // Stable IDs are assigned under stateMutex; GPU resources live only on GL thread.
    uint64_t finishedEpoch = 1;
    struct CachedGpuStroke {
        CachedGpuStroke* following=nullptr; // stable map nodes; resolved when links change
        std::array<std::shared_ptr<const Mesh>, 3> source;
        std::array<float,4> rgba{};
        std::array<GLuint, 3> vbo{};
        std::array<GLsizei, 3> count{};
        std::array<size_t,3> storage{};
        float minX=0, minY=0, maxX=0, maxY=0;
        Key next=0;
        document::Revision geometry=0;
    };
    // GL thread maintains append-only cache; completed CPU meshes are copied only
    // when first received, never once per animation frame.
    std::map<Key,CachedGpuStroke> gpuFinished;
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

    tablet::ViewMailbox desiredView;

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
    GLint resolveLoc=-1, bandLoc=-1;
    bool maxCoverage=false;

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
uniform bool uResolve;

varying vec2 vLocal;
varying float vLength;
varying vec2 vRadius;

void main() {
    if(uResolve) { gl_Position=vec4(aPos,0.0,1.0); return; }
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
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif

uniform vec4 uColor;
uniform bool uResolve;
uniform float uBand;

varying vec2 vLocal;
varying float vLength;
varying vec2 vRadius;

void main() {
    if(uResolve) { gl_FragColor=uColor; return; }

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

    if(uBand>0.0 && a<uBand-0.03125) discard;
    gl_FragColor=vec4(0.0,0.0,0.0,(uBand>0.0?uBand:a)*uColor.a);
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
            // Upper bound of geometrically grown non-mesh vectors (raw, clean,
            // two point streams, knots, attributes and Hermite segment data).
            if(t.scratchAllowance!=SIZE_MAX && t.input.raw_samples().size()+1>t.scratchAllowance/640)
                return core::Error{core::ErrorCode::ResourceLimit,"Stroke scratch budget"};
            constexpr size_t maxPart=(maxSteps+1)*6;
            const auto stableExtra=moving && !t.input.clean_samples().empty()?2*maxPart+12:12;
            if(t.meshAllowance!=SIZE_MAX) {
                auto capacity=[](const Mesh& m,size_t extra) -> uint64_t {
                    return m.capacity()-m.size()>=extra?m.capacity():m.size()+std::max(extra,m.size());
                };
                uint64_t vertices=capacity(t.rawMesh,18)+capacity(t.cleanMesh,24)+capacity(t.stableMesh,stableExtra);
                if(vertices>t.meshAllowance/sizeof(Vertex) ||
                   !admits(vertices*sizeof(Vertex),t.input.raw_samples().size()+1))
                    return core::Error{core::ErrorCode::ResourceLimit,"Document geometry budget"};
            }
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

    struct Part { GLuint vbo; GLsizei count; };
    // GL-thread only; a layer is either a completed mesh or stable/tail/tip.
    std::array<Part,3> parts{};
    size_t partCount=0;
    void drawCached(GLuint vbo, GLsizei count, float, float, float) {
        if (!program || !vbo || count <= 0) return;
        parts.at(partCount++)={vbo,count};
    }

    template<class Gather>
    void coverageLayer(const Bounds& box,double panX,double panY,double zoom,
                       float r,float g,float b,Gather gather,float opacity=1.f) {
        // Match float camera uniforms; clip in double before integer conversion.
        auto edge=[](double p,int limit){return (int)std::clamp(p,0.,double(limit));};
        int x=edge(std::floor((box.minX-(double)(float)panX)*(float)zoom)-1,width);
        int y=edge(std::floor((box.minY-(double)(float)panY)*(float)zoom)-1,height);
        int right=edge(std::ceil((box.maxX-(double)(float)panX)*(float)zoom)+1,width);
        int bottom=edge(std::ceil((box.maxY-(double)(float)panY)*(float)zoom)+1,height);
        if(right<=x || bottom<=y) return;
        partCount=0;
        gather(); // lazy upload once, even on the extension-free path
        if(!partCount) return;
        glEnable(GL_SCISSOR_TEST);
        glScissor(x,height-bottom,right-x,bottom-y);
        glColorMask(GL_FALSE,GL_FALSE,GL_FALSE,GL_TRUE);
        glClearColor(0,0,0,0);
        glClear(GL_COLOR_BUFFER_BIT);
        constexpr GLenum maxExt=0x8008;
        if(maxCoverage) glBlendEquation(maxExt);
        else glDisable(GL_BLEND);
        // Core ES2 fallback: ascending opaque threshold bands. Each band
        // overwrites alpha only where covered, so alpha=round(16*max(a))/16.
        int passes=maxCoverage?1:16;
        glUniform4f(colorLoc,r,g,b,opacity);
        for(int k=1;k<=passes;++k) {
            glUniform1f(bandLoc,maxCoverage?0.f:k/16.f);
            for(size_t i=0;i<partCount;++i) {
                glBindBuffer(GL_ARRAY_BUFFER,parts[i].vbo);
                bindAttributes();
                glDrawArrays(GL_TRIANGLES,0,parts[i].count);
                ++frameDrawCalls;
            }
        }
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
        glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
        glBlendFuncSeparate(GL_DST_ALPHA,GL_ONE_MINUS_DST_ALPHA,GL_ONE,GL_ZERO);
        glUniform1i(resolveLoc,1);
        glUniform4f(colorLoc,r,g,b,1.f);
        static constexpr GLfloat triangle[]={-1,-1,3,-1,-1,3};
        glBindBuffer(GL_ARRAY_BUFFER,0);
        glVertexAttribPointer(positionLoc,2,GL_FLOAT,GL_FALSE,0,triangle);
        glDrawArrays(GL_TRIANGLES,0,3); // scissored resolve, NOT a viewport pass
        ++frameDrawCalls;
        glUniform1i(resolveLoc,0);
        glDisable(GL_SCISSOR_TEST);
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
        size_t plannedCapacity = 0;
        uint64_t revision = 0;
        std::vector<Vertex> data;
    };

    struct FrameGrowth {
        size_t pending=0;
        void reserve(size_t n) {pending+=n;liveVboCharge.fetch_add(n);}
        ~FrameGrowth(){liveVboCharge.fetch_sub(pending);}
    };
    LiveUpload snapshotUpload(FrameGrowth& growth,const Mesh& mesh, uint64_t id, int part, uint64_t revision = 0) {
        LiveUpload result;
        result.total = mesh.size();
        result.revision = revision;
        const auto gpu = gpuLive.find(id);
        const LiveGpuMesh* previous = gpu == gpuLive.end() ? nullptr : &gpu->second[part];
        const size_t uploaded = previous ? previous->uploadedVertices : 0;
        const size_t capacity = previous ? previous->capacityVertices : 0;
        if(result.total>capacity) {
            const size_t n=result.total;
            const size_t finishRoom=part==2 ? (maxSteps+1)*6+12 : part==3 ? 0 : 12;
            result.plannedCapacity=std::max(n+std::min(finishRoom,std::max(size_t(12),n/8)),
                                            std::max(size_t(256),capacity*2));
            // Reserve under stateMutex while the active admission still owns
            // this prefix. Cancellation may run immediately after snapshot.
            growth.reserve((result.plannedCapacity-capacity)*sizeof(Vertex));
        }
        if (part == 3 && previous && previous->revision == revision && uploaded == result.total)
            return result;
        result.offset = (part == 3 || result.total < uploaded || result.total > capacity)
                ? 0 : uploaded;
        if (result.total > result.offset)
            result.data.assign(mesh.begin() + result.offset, mesh.end());
        return result;
    }

    void drawLive(FrameGrowth& frame,uint64_t strokeId, int part, const LiveUpload& mesh, float r, float g, float b) {
        if (mesh.total == 0 || !program) return;
        auto& cached = gpuLive[strokeId][part];
        if (!cached.vbo) glGenBuffers(1, &cached.vbo);
        if (!cached.vbo) return;
        glBindBuffer(GL_ARRAY_BUFFER, cached.vbo);
        const size_t n = mesh.total;
        if (n > cached.capacityVertices) {
            // Leave bounded room for the terminal tail and cursor when a
            // coalesced frame is the first upload of a long live prefix.
            const size_t cap=mesh.plannedCapacity;
            glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(cap * sizeof(Vertex)), nullptr, GL_DYNAMIC_DRAW);
            auto growth=(cap-cached.capacityVertices)*sizeof(Vertex);
            frame.pending-=growth; // ledger unchanged: GL now owns these bytes
            peak(gpuHigh,gpuRequested.fetch_add(growth)+growth);
            cached.capacityVertices = cap;
            ++liveBufferReallocations;
        }
        if (!mesh.data.empty()) {
            glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)(mesh.offset * sizeof(Vertex)),
                            (GLsizeiptr)(mesh.data.size() * sizeof(Vertex)), mesh.data.data());
            frameUploadBytes += mesh.data.size() * sizeof(Vertex);
            liveUploadBytes += mesh.data.size() * sizeof(Vertex);
            if (mesh.offset > 0) ++liveIncrementalUpdates;
            if(glGetError()!=GL_NO_ERROR) {
                // Includes failed storage allocation. Do not publish a prefix
                // length for bytes the driver did not accept. Retry from zero.
                gpuRequested.fetch_sub(cached.capacityVertices*sizeof(Vertex));
                glDeleteBuffers(1,&cached.vbo);
                liveVboCharge.fetch_sub(cached.capacityVertices*sizeof(Vertex)); cached={}; return;
            }
        }
        cached.uploadedVertices = n;
        cached.revision = mesh.revision;
        drawCached(cached.vbo, (GLsizei)n, r, g, b);
    }

    void eraseGpuNode(decltype(gpuFinished)::iterator it) {
        // All three sources alias one geometry. Keep its cache-node credit
        // alive through map-node deallocation, even for the final GL owner.
        auto pin=std::move(it->second.source[0]);
        gpuFinished.erase(it);
    }
    void deleteGpuCache() {
        while(!gpuFinished.empty()) {
            auto it=gpuFinished.begin();
            auto& item=it->second;
            for(size_t i=0;i<3;i++) if(item.vbo[i]) {
                gpuRequested.fetch_sub(item.storage[i]);glDeleteBuffers(1,&item.vbo[i]);
            }
            eraseGpuNode(it);
        }
    }

    void cacheFinished(Key id,const Published& patch) {
        auto it=gpuFinished.find(id);
        if(it!=gpuFinished.end()) {
            it->second.next=patch.next;
            it->second.rgba=patch.rgba;
            if(patch.shape && it->second.geometry==patch.geometry) return;
            for(size_t i=0;i<3;i++) if(it->second.vbo[i]) {
                gpuRequested.fetch_sub(it->second.storage[i]);glDeleteBuffers(1,&it->second.vbo[i]);
            }
            if(!patch.shape) {eraseGpuNode(it); return;}
            it->second=CachedGpuStroke{};
        }
        if(!patch.shape) return;
        auto& item=gpuFinished[id];
        const auto& trace=*patch.shape;
        item.rgba=patch.rgba;
        item.geometry=patch.geometry; item.next=patch.next;
        item.source = {trace.raw, trace.clean, trace.spline};
        item.minX = trace.minX; item.minY = trace.minY;
        item.maxX = trace.maxX; item.maxY = trace.maxY;
        auto previous=gpuLive.find(trace.liveId);
        if(previous==gpuLive.end()) return;
        auto& completed=item;
        for(size_t i=0;i<3;++i) {
            auto& prefix=previous->second[i];
            const auto n=completed.source[i]->size();
            if(prefix.vbo && prefix.uploadedVertices<=n && n<=prefix.capacityVertices &&
               prefix.capacityVertices-n<=n/2) {
                // Same logical prefix, same EGL context, single GL owner.
                // Never adopt provisional part 3; its contents are replaceable.
                completed.vbo[i]=std::exchange(prefix.vbo,0);
                completed.storage[i]=prefix.capacityVertices*sizeof(Vertex);
                // Completed geometry already reserves the adopted VBO capacity.
                liveVboCharge.fetch_sub(completed.storage[i]);
                completed.count[i]=(GLsizei)prefix.uploadedVertices;
            }
        }
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
            if(glGetError()!=GL_NO_ERROR) {
                glDeleteBuffers(1,&item.vbo[part]); item.vbo[part]=0; item.count[part]=0; return;
            }
            item.count[part] = (GLsizei)vertices.size();
            item.storage[part]=bytes;peak(gpuHigh,gpuRequested.fetch_add(bytes)+bytes);
            frameUploadBytes += bytes;
            cacheUploadBytes += bytes;
        } else if((size_t)item.count[part]<vertices.size()) {
            const size_t prefix=item.count[part];
            const size_t bytes=(vertices.size()-prefix)*sizeof(Vertex);
            glBindBuffer(GL_ARRAY_BUFFER,item.vbo[part]);
            glBufferSubData(GL_ARRAY_BUFFER,(GLintptr)(prefix*sizeof(Vertex)),(GLsizeiptr)bytes,vertices.data()+prefix);
            if(glGetError()!=GL_NO_ERROR) {
                gpuRequested.fetch_sub(item.storage[part]);item.storage[part]=0;
                glDeleteBuffers(1,&item.vbo[part]); item.vbo[part]=0; item.count[part]=0; return;
            }
            item.count[part]=(GLsizei)vertices.size();
            frameUploadBytes+=bytes;
            cacheUploadBytes+=bytes;
        }
        drawCached(item.vbo[part], item.count[part], r, g, b);
    }

    struct ViewTrace {
        Bounds bounds;
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

static bool reconstructV1(const std::shared_ptr<const document::Ink>&,FinishedTrace&,double dx=0,double dy=0,
                          size_t allowance=SIZE_MAX,size_t scratchLimit=SIZE_MAX);
static size_t meshBytes(const FinishedTrace& s) {
    return (s.raw->capacity()+s.clean->capacity()+s.spline->capacity())*sizeof(Vertex);
}
struct SceneSink final : document::Observer {
    std::shared_ptr<FinishedTrace> appended;
    size_t appendedBytes=0;
    Scene appendNodes,appendPatch;
    void reserveAppend(Key id,Key prev) {
        appendNodes.clear();appendPatch.clear();
        appendNodes.try_emplace(id);appendPatch.try_emplace(id);
        if(prev) appendPatch.try_emplace(prev);
    }
    bool worker=false;
    document::Status prepare(const document::Document& doc,std::span<const document::Touched> touched) override {
        Scene stage;
        if(doc.canonicalBytes()>canonicalLimit) return document::Status::Memory;
        if(!doc.size()) {
            Scene retired,unused;
            std::unique_lock lock(stateMutex,std::defer_lock);
            if(worker) lock.lock();
            if(persistent) {
                auto s=saves.prepare(doc,touched);
                if(s!=document::Status::Ok) return s;
            }
            finished.swap(retired);pendingScene.swap(unused);
            ++finishedEpoch;sceneHead=0;sceneBytes=0;sceneRevision=doc.revision()+1;
            return document::Status::Ok;
        }
        if(appended && touched.size()==1 && appendNodes.count(touched.front().id) &&
           appendPatch.count(touched.front().id) && (!touched.front().prev || appendPatch.count(touched.front().prev))) {
            const auto [id,prev,next,geometryChanged]=touched.front();
            if(next || appendedBytes>sceneLimit-sceneBytes) return document::Status::Memory;
            if(persistent) {
                auto status=saves.prepare(doc,touched);
                if(status!=document::Status::Ok) return status;
            }
            auto& row=appendNodes.at(id);row={0,doc.revision()+1,appended,appendedBytes,doc.find(id)->value.rgba()};
            appendPatch.at(id)=row;
            if(prev) {
                auto& predecessor=finished.at(prev);predecessor.next=id;appendPatch.at(prev)=predecessor;
                pendingScene.erase(prev);
            }
            finished.merge(appendNodes);pendingScene.merge(appendPatch);
            sceneHead=doc.first();sceneBytes+=appendedBytes;sceneRevision=doc.revision()+1;
            return document::Status::Ok;
        }
        size_t projected=sceneBytes;
        for(auto e:touched) {
            if(e.geometry && stage.try_emplace(e.id).second) {
                auto it=finished.find(e.id);
                if(it!=finished.end()) projected-=it->second.bytes;
            }
        }
        bool valid=true;
        auto include=[&](Key id,bool changed) {
            if(!id) return;
            auto& dst=stage[id];
            const auto* r=doc.find(id);
            if(!r) return;
            dst.next=r->next;
            if(changed && dst.geometry!=doc.revision()+1) {
                dst.geometry=doc.revision()+1;
                if(appended) {dst.shape=appended;dst.bytes=appendedBytes;}
                else {
                    auto shape=std::make_shared<FinishedTrace>();
                    if(!reconstructV1(r->value.ink,*shape,r->value.dx,r->value.dy,sceneLimit-projected,canonicalLimit)) {valid=false;return;}
                    dst.shape=std::move(shape);
                    dst.bytes=meshBytes(*dst.shape);
                }
                if(dst.bytes>sceneLimit-projected) {valid=false;return;}
                projected+=dst.bytes;
            } else if(!dst.shape) {dst=finished.at(id);dst.next=r->next;}
            dst.rgba=r->value.rgba();
        };
        // Subjects first: adjacent new subjects may not yet have a projection.
        for(auto e:touched) if(e.geometry) include(e.id,true);
        if(!valid) return document::Status::Memory;
        for(auto e:touched) {if(!e.geometry) include(e.id,false);include(e.prev,false);include(e.next,false);}
        Scene notifications=stage;
        std::unique_lock lock(stateMutex,std::defer_lock);
        if(worker) lock.lock();
        if(persistent) {
            if(!appended && pendingScene.size()+stage.size()>publicationLimit)
                return document::Status::QueueLimit;
            auto s=saves.prepare(doc,touched);
            if(s!=document::Status::Ok) return s;
        }
        for(auto& [id,p]:stage) finished.erase(id);
        for(auto it=stage.begin();it!=stage.end();) {
            if(!it->second.shape) it=stage.erase(it); else ++it;
        }
        finished.merge(stage);
        for(auto& [id,p]:notifications) pendingScene.erase(id);
        pendingScene.merge(notifications);
        sceneHead=doc.first();
        sceneBytes=projected;
        sceneRevision=doc.revision()+1;
        return document::Status::Ok;
    }
};
static SceneSink sceneSink;
static bool readyModel() {
#ifndef __ANDROID__
    if(!model) {
        model=std::make_unique<document::Document>(document::Uuid{1});
        model->grant(1,INT64_MAX); model->observe(&sceneSink);
    }
#endif
    return bool(model);
}

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

    desiredView.publish(panX,panY,std::clamp((double)zoom,MIN_ZOOM,MAX_ZOOM));
}

JNIEXPORT void JNICALL
Java_com_infinipaint_demo_NativeBridge_setSmoothingPreset(JNIEnv*, jclass, jint preset) {
    smoothingPreset.store(std::clamp((int)preset, 0, 2), std::memory_order_relaxed);
}

// --------------------------------------------------
// Begin stroke
// --------------------------------------------------

static jboolean beginCaptured(jint id,jint tool,tablet::ViewIntent view,int preset) {

    std::lock_guard<std::mutex> guard(stateMutex);

    // Cutoffs in world units. Three intentionally distinct presets; stronger
    // smoothing has more geometric phase lag, while raw tip preview stays live.
    input::OneEuroParams params;
    switch (preset) {
        case 0: params.min_cutoff = 8.0; params.beta = 0.025; params.dcutoff = 1.5; break;
        case 2: params.min_cutoff = 1.6; params.beta = 0.002; params.dcutoff = 1.0; break;
        default: params.min_cutoff = 3.5; params.beta = 0.009; params.dcutoff = 1.0; break;
    }
    auto inputProcessor =
            input::InputProcessor::create(params);

    auto fitter = stroke::SplineFitter::create({stroke::Parameterization::Centripetal,1000000,.01,14});

    if (!inputProcessor || !fitter) {
        return JNI_FALSE;
    }

    try {
        if(!readyModel() || suspended || !model->canAllocate() || live.count(id) ||
           (persistent && (pendingScene.size()+2>publicationLimit ||
                           model->canonicalBytes()+1024>canonicalLimit || sceneLimit-sceneBytes<54*sizeof(Vertex) ||
                           !admits(54*sizeof(Vertex),1)))) return JNI_FALSE;
        auto trace=std::make_unique<Trace>(std::move(inputProcessor.value()),std::move(fitter.value()));
        trace->captureView = view;
        trace->strokeRadiusWorld = (float)(1.9 / trace->captureView.zoom);
        trace->meshAllowance=sceneLimit-sceneBytes;
        trace->scratchAllowance=canonicalLimit;
        trace->brush.filter=params;
        trace->brush.radiusWorld=trace->strokeRadiusWorld;
        trace->brush.tool=static_cast<uint32_t>(tool);
        if (!trace->input.begin_stroke()) return JNI_FALSE;
        trace->gpuId = nextLiveGpuId++;
        if(live.empty()) sceneSink.reserveAppend(model->nextId(),model->last());
        if(persistent && !saves.admitStroke()) return JNI_FALSE;
        if(model->reserveStroke()!=document::Status::Ok) {if(persistent) saves.cancelStroke();return JNI_FALSE;}
        try {live.emplace(id,std::move(trace));}
        catch(...) {model->cancelStroke();if(persistent) saves.cancelStroke(); throw;}
        return JNI_TRUE;
    } catch(const std::bad_alloc&) { return JNI_FALSE; }
}

// --------------------------------------------------
// Receive input sample
// --------------------------------------------------

JNIEXPORT jboolean JNICALL Java_com_infinipaint_demo_NativeBridge_begin(
        JNIEnv*,jclass,jint id,jint tool) {
    return beginCaptured(id,tool,desiredView.latch(),smoothingPreset.load(std::memory_order_relaxed));
}

JNIEXPORT jboolean JNICALL Java_com_infinipaint_demo_NativeBridge_beginCaptured(
        JNIEnv*,jclass,jint id,jint tool,jdouble x,jdouble y,jdouble zoom,jint preset) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(zoom) || zoom<=0)
        return JNI_FALSE;
    return beginCaptured(id,tool,{x,y,std::clamp((double)zoom,MIN_ZOOM,MAX_ZOOM)},preset);
}

static bool appendV1(Trace&,const input::RawInputPoint&,double,int64_t,bool);

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
    const auto waitNs=profileNs()-processingStartNs;

    auto it = live.find(id);

    if (it == live.end()) {
        return JNI_FALSE;
    }

    auto& trace = *it->second;

    // Convert screen-space input into world space.
    // This keeps strokes anchored when panning or zooming.

    if(trace.sealed) return JNI_FALSE;
    if(persistent) {
        const auto& raw=trace.input.raw_samples();
        size_t capacity=raw.capacity();
        if(raw.size()==capacity) capacity=std::min<size_t>(1000000,std::max<size_t>(1,capacity*2));
        const auto bytes=capacity*sizeof(input::RawInputPoint);
        if(raw.size()==raw.capacity() &&
           !admits((trace.rawMesh.capacity()+trace.cleanMesh.capacity()+trace.stableMesh.capacity())*sizeof(Vertex),
                   raw.size()+1)) return JNI_FALSE;
        if(!model->acceptsStrokeCapacity(capacity) || model->canonicalBytes()+bytes+1024>canonicalLimit ||
           !saves.growStroke(bytes)) return JNI_FALSE;
    }
    double worldX =
            trace.captureView.x + x / trace.captureView.zoom;

    double worldY =
            trace.captureView.y + y / trace.captureView.zoom;

    if (
            !std::isfinite(worldX) ||
            !std::isfinite(worldY) ||
            std::max(std::abs(worldX),std::abs(worldY))>std::numeric_limits<float>::max()/1024.
            ) {
        return JNI_FALSE;
    }

    // Original event times can tie; arrival order resolves the tie.
    if (!std::isfinite(time) || time>=double(INT64_MAX)/1e9) {
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
    if (!appendV1(trace,point,trace.captureView.zoom,waitNs,true)) return JNI_FALSE;
    const int64_t nowNs = profileNs();
    newestEventNs.store(static_cast<int64_t>(time*1000000000.0),std::memory_order_relaxed);
    latestSampleEndNs.store(nowNs,std::memory_order_relaxed);
    sampleProcessingUs.fetch_add((nowNs-processingStartNs)/1000,std::memory_order_relaxed);
    processedSamples.fetch_add(1,std::memory_order_relaxed);
    return JNI_TRUE;
}

static bool appendV1(Trace& trace,const input::RawInputPoint& point,double zoom,int64_t waitNs,bool telemetry) {
    const auto [worldX,worldY]=point.position;
    const auto time=point.timestamp;
    const auto pressure=point.pressure;
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
    if(telemetry) {
        auditMutexWaitNs.fetch_add(waitNs,std::memory_order_relaxed);
        auditFilterNs.fetch_add(commitNs,std::memory_order_relaxed);
    }

    // Immediate ink uses actual input positions.

    const auto rawStart=trace.rawMesh.size(),cleanStart=trace.cleanMesh.size(),stableStart=trace.stableMesh.size();
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
    const double deviation = std::hypot(c.position.x - worldX, c.position.y - worldY) * zoom;
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
    if(trace.spline.segment_count()!=oldSegments) {
        buildStable(trace);
        trace.bounds.include(trace.tailMesh,0);
    }
    trace.bounds.include(trace.stableMesh,stableStart);
    trace.bounds.include(trace.rawMesh,rawStart);
    trace.bounds.include(trace.cleanMesh,cleanStart);
    if(telemetry) {
        auditSplineNs.fetch_add(profileNs() - splineStartNs, std::memory_order_relaxed);
        auditStageSamples.fetch_add(1, std::memory_order_relaxed);
    }
    return JNI_TRUE;
}

// --------------------------------------------------
// End stroke
// --------------------------------------------------
static FinishedTrace finishV1(Trace&,std::shared_ptr<const document::Ink>);

JNIEXPORT jboolean JNICALL
Java_com_infinipaint_demo_NativeBridge_end(
        JNIEnv*,
        jclass,
        jint id
) {

    const auto enter=profileNs();
    std::unique_lock<std::mutex> guard(stateMutex);
    const auto lockStart=profileNs();

    auto it = live.find(id);

    if (it == live.end()) {
        return JNI_FALSE;
    }

    auto& trace = *it->second;
    if(!trace.sealed && trace.input.raw_samples().empty()) return JNI_FALSE;
    // Allocate the ordered slot before touching live meshes. No render can
    // observe it until populated. Sample preflight reserved terminal geometry.
    try {
        auto shape=std::make_shared<FinishedTrace>();
        if(!trace.sealed) trace.sealed=std::make_shared<tablet::AdoptAccepted>(trace.input,trace.brush);
        sceneSink.appended=shape;
        sceneSink.appendedBytes=(trace.rawMesh.capacity()+trace.cleanMesh.capacity()+trace.stableMesh.capacity())*sizeof(Vertex);
        peak(activeHigh,workingBytes(trace));
        saves.committingStroke(true);
        auto result=model->commitStroke({trace.sealed});
        saves.committingStroke(false);
        sceneSink.appended.reset();
        if(result!=document::Status::Ok) {
            __android_log_print(ANDROID_LOG_ERROR,"IP_EDIT","commit rejected status=%d revision=%lld samples=%zu",
                                int(result),static_cast<long long>(model->revision()),trace.sealed->samples.size());
            return JNI_FALSE;
        }
        *shape=finishV1(trace,std::move(trace.sealed));
        visibleRevision.store(model->revision());
        historyCharge.store(model->historyBytes());canonicalCharge.store(model->canonicalBytes());peak(queueHigh,saves.bytes());
    } catch(const std::bad_alloc&) { return JNI_FALSE; }

    const auto ink=trace.completion;
    const size_t rawSamples=trace.rawPoints.size()/2, cleanSamples=trace.cleanPoints.size()/2;
    auto retired=std::move(it->second);
    live.erase(it);
    const auto held=profileNs()-lockStart;
    guard.unlock();
    __android_log_print(ANDROID_LOG_INFO, "IP_GEOMETRY",
        "raw_samples=%zu filtered_samples=%zu raw_vertices=%zu filtered_vertices=%zu spline_vertices=%zu mean_filter_deviation_px=%.4f max_filter_deviation_px=%.4f",
        rawSamples, cleanSamples, ink->derived[0].size(), ink->derived[1].size(),
        ink->derived[2].size(), trace.deviationSum/trace.deviationCount,trace.deviationMax);
    __android_log_print(ANDROID_LOG_INFO,"IP90_FIDELITY",
        "samples=%zu mean_deviation_px=%.3f max_deviation_px=%.3f would_exceed_2px=%zu zoom=%.3f",
        trace.deviationCount,trace.deviationSum/trace.deviationCount,trace.deviationMax,
        trace.wouldExceedLimit,1.9/trace.strokeRadiusWorld);
    __android_log_print(ANDROID_LOG_INFO,"IP_PEN_UP","samples=%zu lock_ns=%lld wait_ns=%lld",
                        rawSamples,(long long)held,(long long)(lockStart-enter));
    return JNI_TRUE;
}

static FinishedTrace finishV1(Trace& trace,std::shared_ptr<const document::Ink> record) {
    auto& splineMesh=trace.stableMesh;
    splineMesh.insert(splineMesh.end(),trace.tailMesh.begin(),trace.tailMesh.end());
    const auto rawStart=trace.rawMesh.size(),cleanStart=trace.cleanMesh.size(),splineStart=splineMesh.size();
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
    trace.bounds.include(trace.rawMesh,rawStart);
    trace.bounds.include(trace.cleanMesh,cleanStart);
    trace.bounds.include(splineMesh,splineStart);
    auto ink=trace.completion;
    ink->derived={std::move(trace.rawMesh),std::move(trace.cleanMesh),std::move(splineMesh)};
    for(const auto& m:ink->derived) ink->bytes+=m.capacity()*sizeof(Vertex);
    retainedMeshes.fetch_add(ink->bytes,std::memory_order_relaxed);
    const auto& b=trace.bounds;
    return {std::move(record),{ink,&ink->derived[0]},{ink,&ink->derived[1]},
                     {ink,&ink->derived[2]},b.minX,b.minY,b.maxX,b.maxY,trace.gpuId};
}

static bool reconstructV1(const std::shared_ptr<const document::Ink>& record,FinishedTrace& out,double dx,double dy,
                          size_t allowance,size_t scratchLimit) {
    if(!record || record->brush.recipe!=1 || record->samples.empty()) return false;
    auto p=input::InputProcessor::create(record->brush.filter);
    auto f=stroke::SplineFitter::create({stroke::Parameterization::Centripetal,1000000,.01,14});
    if(!p || !f) return false;
    Trace scratch(std::move(p).value(),std::move(f).value());
    scratch.strokeRadiusWorld=record->brush.radiusWorld;
    scratch.meshAllowance=allowance;
    scratch.scratchAllowance=scratchLimit;
    if(!scratch.input.begin_stroke()) return false;
    for(const auto& s:record->samples)
        if(!appendV1(scratch,s,1,0,false)) return false;
    peak(switchHigh,workingBytes(scratch)+document::Ink::liveBytes.load()+retainedMeshes.load()+gpuRequested.load());
    out=finishV1(scratch,record);
    if(dx!=0 || dy!=0) {
        Bounds b;
        // Post-reconstruction translation: do not refilter displaced samples.
        for(auto& mesh:scratch.completion->derived) {
            for(auto& v:mesh) {
                double x=double(v.x)+dx,y=double(v.y)+dy;
                if(std::max(std::abs(x),std::abs(y))>std::numeric_limits<float>::max()/1024.) return false;
                v.x=float(x);v.y=float(y);
            }
            b.include(mesh,0);
        }
        out.minX=b.minX;out.minY=b.minY;out.maxX=b.maxX;out.maxY=b.maxY;
    }
    return true;
}

JNIEXPORT jlong JNICALL Java_com_infinipaint_demo_NativeBridge_revision(JNIEnv*,jclass) {
    return visibleRevision.load();
}
JNIEXPORT jlongArray JNICALL Java_com_infinipaint_demo_NativeBridge_memory(JNIEnv* env,jclass) {
    std::array<jlong,13> a{};
    {
        std::lock_guard guard(stateMutex);
        uint64_t active=0;
        for(const auto& [id,t]:live) active+=workingBytes(*t);
        a={jlong(document::Ink::liveBytes.load()),jlong(canonicalCharge.load()),jlong(retainedMeshes.load()),
           jlong(active),jlong(activeHigh.load()),jlong(gpuRequested.load()),jlong(gpuHigh.load()),
           jlong(historyCharge.load()),jlong(saves.bytes()),jlong(queueHigh.load()),jlong(switchHigh.load()),
           jlong(snapshotHigh.load()),jlong(pendingScene.size())};
    }
    auto array=env->NewLongArray(a.size());if(array)env->SetLongArrayRegion(array,0,a.size(),a.data());return array;
}
JNIEXPORT jboolean JNICALL Java_com_infinipaint_storage_StorePort_needsIds(JNIEnv*,jclass) {
    std::lock_guard guard(stateMutex);
    return model && !model->canAllocate();
}
JNIEXPORT jboolean JNICALL Java_com_infinipaint_storage_StorePort_refill(JNIEnv*,jclass,jlong first,jlong last) {
    std::lock_guard guard(stateMutex);
    return model && model->grant(first,last);
}
JNIEXPORT void JNICALL Java_com_infinipaint_demo_NativeBridge_suspend(JNIEnv*,jclass,jboolean value) {
    std::lock_guard guard(stateMutex);suspended=value;
}
static bool hit(const FinishedTrace& ink,core::Vec2 a,core::Vec2 b,double tolerance,
                std::map<double,double>& coverage,size_t& intervalCount) {
    if(std::max(a.x,b.x)+tolerance<ink.minX || std::min(a.x,b.x)-tolerance>ink.maxX ||
       std::max(a.y,b.y)+tolerance<ink.minY || std::min(a.y,b.y)-tolerance>ink.maxY) return false;
    bool any=false;
    for(size_t i=0;i<ink.spline->size();i+=6) {
        const auto& v=(*ink.spline)[i];
        core::Vec2 p{v.x,v.y},q{v.x+v.len*v.dirX,v.y+v.len*v.dirY};
        auto before=coverage.size();
        any|=document::expose(coverage,document::clipInk(a,b,p,q,v.radius+tolerance,v.endRadius+tolerance));
        intervalCount=intervalCount-before+coverage.size();
        if(intervalCount>65536) throw std::length_error("Eraser interval budget");
        if(coverage.size()==1 && coverage.begin()->first==0 && coverage.begin()->second==1) break;
    }
    return any;
}
JNIEXPORT jint JNICALL Java_com_infinipaint_demo_NativeBridge_command(JNIEnv* env,jclass,jint op,
                                                                      jdoubleArray path,jdouble tolerance) {
    using S=document::Status;
    {
        std::lock_guard guard(stateMutex);
        if(!model || !suspended || !live.empty()) return int(S::Conflict);
    }
    // Single catalog worker; input stays suspended until main receives completion.
    sceneSink.worker=true;
    struct Reset {~Reset(){sceneSink.worker=false;}} reset;
    auto rev=model->revision();
    S result=S::Invalid;
    try {
        if(op==1) result=model->undo(rev);
        if(op==2) result=model->redo(rev);
        if(op==3) result=model->clear(rev);
        if(op==4 && path && tolerance>=0 && std::isfinite(tolerance)) {
            auto n=env->GetArrayLength(path);
            if(n<2 || n>8192 || n%2) return int(S::Invalid);
            std::vector<double> points(n);
            env->GetDoubleArrayRegion(path,0,n,points.data());
            if(env->ExceptionCheck()) return int(S::Invalid);
            std::vector<std::map<double,double>> coverage(n/2);
            std::vector<document::Edit> edits;
            double left=points[0],top=points[1],right=left,bottom=top;
            for(int i=0;i<n;i+=2) {
                if(!std::isfinite(points[i]) || !std::isfinite(points[i+1])) return int(S::Invalid);
                left=std::min(left,points[i]);right=std::max(right,points[i]);
                top=std::min(top,points[i+1]);bottom=std::max(bottom,points[i+1]);
            }
            size_t intervals=0;
            for(Key id=model->last();id;id=model->find(id)->prev) {
                auto& ink=*finished.at(id).shape;
                if(ink.maxX<left-tolerance || ink.minX>right+tolerance ||
                   ink.maxY<top-tolerance || ink.minY>bottom+tolerance) continue;
                bool exposed=false;
                for(int i=n>2?2:0;i<n;i+=2) {
                    core::Vec2 a{points[std::max(0,i-2)],points[std::max(0,i-2)+1]},b{points[i],points[i+1]};
                    auto& cover=coverage[i/2];
                    if(cover.size()==1 && cover.begin()->first==0 && cover.begin()->second==1) continue;
                    exposed|=hit(ink,a,b,tolerance,cover,intervals);
                }
                if(exposed) edits.push_back({document::Edit::Remove,id});
            }
            result=model->edit(rev,edits);
        }
        if(result==S::Ok) {
            visibleRevision.store(model->revision());historyCharge.store(model->historyBytes());
            canonicalCharge.store(model->canonicalBytes());peak(queueHigh,saves.bytes());
        }
    } catch(const std::bad_alloc&) {return int(S::Memory);}
      catch(const std::length_error&) {return int(S::Memory);}
    return int(result);
}

#ifdef IP_REPLAY_TEST
// Single-threaded Mesa harness only. Cross the codec boundary, throw away old
// derived meshes and require byte-identical vertex streams before pixel readback.
bool ipTestCodecReplay() {
    for(auto& [id,pub]:finished) {
        auto& old=*pub.shape;
        using namespace document::wire;
        const auto& src=*old.record;
        Header h;
        document::Brush b;
        double x,y;
        if(!header(src.brush,0,0,h) || !readHeader(h,b,x,y)) return false;
        input::RawInputStroke restored(src.samples.size());
        for(size_t i=0;i<restored.size();) {
            size_t n=std::min(chunkSamples,restored.size()-i);
            std::vector<uint8_t> bytes(n*sampleBytes);
            if(!samples(std::span(src.samples).subspan(i,n),bytes) ||
               !readSamples(bytes,i?restored[i-1].timestamp:-1,std::span(restored).subspan(i,n))) return false;
            i+=n;
        }
        FinishedTrace replay;
        if(!reconstructV1(std::make_shared<const document::Ink>(std::move(restored),b),replay)) return false;
        auto equal=[](const auto& a,const auto& b){
            return a->size()==b->size() && (a->empty() || !std::memcmp(a->data(),b->data(),a->size()*sizeof(Vertex)));
        };
        if(!equal(old.raw,replay.raw)||!equal(old.clean,replay.clean)||!equal(old.spline,replay.spline)) return false;
        pub.shape=std::make_shared<const FinishedTrace>(std::move(replay));
    }
    pendingScene=finished;
    ++finishedEpoch;
    return !finished.empty();
}
#endif

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

    if(live.erase(id)) {model->cancelStroke();if(persistent) saves.cancelStroke();}
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
    saves.cancelStroke();
    if(model) {model->cancelStroke();(void)model->clear(model->revision());}
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
    desiredView.viewport(width,height);

    glViewport(0, 0, width, height);
}

JNIEXPORT void JNICALL
Java_com_infinipaint_demo_NativeBridge_contextCreated(JNIEnv*, jclass) {
    // Old EGL context already owns/disposes its objects. The same numeric
    // names may identify unrelated objects in the newly current context.
    // Topology and immutable CPU sources survive EGL loss. Never reuse/delete
    // stale numeric names in the new context. No full snapshot on the input lock.
    gpuRequested.store(0);
    for(auto& [id,item]:gpuFinished) {item.vbo={};item.count={};item.storage={};}
    gpuLive.clear();
    liveVboCharge.store(0); // GL thread, no outstanding snapshot across EGL contexts
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
    resolveLoc=glGetUniformLocation(program,"uResolve");
    bandLoc=glGetUniformLocation(program,"uBand");
    GLint alphaBits=0; glGetIntegerv(GL_ALPHA_BITS,&alphaBits);
    if(alphaBits<8) {
        __android_log_print(ANDROID_LOG_ERROR,"IP_COVERAGE","RGBA8 window required; alpha bits=%d",alphaBits);
        glDeleteProgram(program); program=0; return;
    }
    const auto* ext=(const char*)glGetString(GL_EXTENSIONS);
    std::string_view names=ext?ext:"";
    while(!names.empty()) {
        auto split=names.find(' ');
        if(names.substr(0,split)=="GL_EXT_blend_minmax") break;
        names=split==names.npos?std::string_view{}:names.substr(split+1);
    }
    const char* version=(const char*)glGetString(GL_VERSION);
    maxCoverage=!names.empty() || (version && std::string_view(version).starts_with("OpenGL ES 3."));
    __android_log_print(ANDROID_LOG_INFO,"IP_COVERAGE","alpha bits=%d max_blend=%d",alphaBits,maxCoverage);
    glDisable(GL_DITHER);

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

    FrameGrowth growth;
    struct FramePublication {
        Scene patches;
        std::vector<std::pair<uint64_t, ViewTrace>> active;
        tablet::ViewIntent view;
        Key head;
        uint64_t epoch;
        document::Revision revision;
    };
    const auto frame = [&] {
        FramePublication packet;
        const int64_t snapshotStartNs = profileNs();
        std::lock_guard<std::mutex> guard(stateMutex);
        packet.view = desiredView.latch();
        packet.epoch = finishedEpoch;
        packet.revision = sceneRevision;
        // Epoch mismatch means clear; GL thread will retire old cache below.
        if(program && buffer) {
            packet.patches.swap(pendingScene);
        }
        packet.head=sceneHead;
        auto& active=packet.active;
        active.reserve(live.size());

        for (auto& pair : live) {

            auto& trace = *pair.second;

            ViewTrace view;
            view.bounds=trace.bounds;
            view.radiusWorld = trace.rawPeak;
            view.filteredRadius = trace.cleanPeak;
            if (mode == 0 || mode == 3)
                view.raw = snapshotUpload(growth,trace.rawMesh, trace.gpuId, 0);
            if (mode == 1 || mode == 3)
                view.clean = snapshotUpload(growth,trace.cleanMesh, trace.gpuId, 1);
            if (mode == 2 || mode == 3) {
                view.stable = snapshotUpload(growth,trace.stableMesh, trace.gpuId, 2);
                view.tail = snapshotUpload(growth,trace.tailMesh, trace.gpuId, 3, trace.tailRevision);
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
                float pad=std::max(trace.rawPeak,trace.cleanPeak)+1.f/(float)MIN_ZOOM;
                auto& q=view.bounds;
                q.minX=std::min(q.minX,std::min(view.lastX,view.filteredX)-pad);
                q.maxX=std::max(q.maxX,std::max(view.lastX,view.filteredX)+pad);
                q.minY=std::min(q.minY,std::min(view.lastY,view.filteredY)-pad);
                q.maxY=std::max(q.maxY,std::max(view.lastY,view.filteredY)+pad);
            }

            peak(snapshotHigh,(view.raw.data.capacity()+view.clean.data.capacity()+view.stable.data.capacity()+
                               view.tail.data.capacity())*sizeof(Vertex));
            active.emplace_back(trace.gpuId, std::move(view));
        }
        auditSnapshotNs.fetch_add(profileNs() - snapshotStartNs, std::memory_order_relaxed);
        auditSnapshotFrames.fetch_add(1, std::memory_order_relaxed);
        return packet;
    }();

    // No input mutex is held during OpenGL calls.
    const auto& newCompleted=frame.patches;
    const auto& active=frame.active;
    const auto frameHead=frame.head;
    const auto frameEpoch=frame.epoch;
    const double framePanX=frame.view.x, framePanY=frame.view.y, frameZoom=frame.view.zoom;

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
    }
    for (const auto& [id,patch] : newCompleted) cacheFinished(id,patch);
    for (const auto& [id,patch] : newCompleted) if(patch.shape) {
        auto& node=gpuFinished.at(id);
        node.following=node.next?&gpuFinished.at(node.next):nullptr;
    }
    // Cull in the same quantized viewport used by the existing float shader.
    const double viewMinX = (float)framePanX, viewMinY = (float)framePanY;
    const double viewMaxX = viewMinX + width/(double)(float)frameZoom;
    const double viewMaxY = viewMinY + height/(double)(float)frameZoom;
    uint64_t visibleCompleted = 0;
    for (auto* node=frameHead?&gpuFinished.at(frameHead):nullptr;node;node=node->following) {
        auto& cached=*node;
        if (cached.maxX < viewMinX || cached.minX > viewMaxX ||
            cached.maxY < viewMinY || cached.minY > viewMaxY) continue;
        ++visibleCompleted;
        auto drawPart = [&](int part, float r, float g, float b) {
            if(part==2) {r=cached.rgba[0];g=cached.rgba[1];b=cached.rgba[2];}
            coverageLayer({cached.minX,cached.minY,cached.maxX,cached.maxY},framePanX,framePanY,frameZoom,r,g,b,
                [&]{drawFinishedPart(cached,part,r,g,b);},cached.rgba[3]);
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
            for (auto& part : it->second) if (part.vbo) {
                gpuRequested.fetch_sub(part.capacityVertices*sizeof(Vertex));glDeleteBuffers(1,&part.vbo);
                liveVboCharge.fetch_sub(part.capacityVertices*sizeof(Vertex));
            }
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
            coverageLayer(trace.bounds,framePanX,framePanY,frameZoom,.28f,.58f,1.f,[&]{
            drawLive(growth,strokeId,2,trace.stable,.28f,.58f,1.f);
            drawLive(growth,strokeId,3,trace.tail,.28f,.58f,1.f);
            tip(true,.28f,.58f,1.f);
            });
        }
        if(mode==1 || mode==3) {
            coverageLayer(trace.bounds,framePanX,framePanY,frameZoom,1.f,.76f,.20f,[&]{
            drawLive(growth,strokeId,1,trace.clean,1.f,.76f,.20f);
            tip(true,1.f,.76f,.20f);
            });
        }
        if(mode==0 || mode==3) {
            coverageLayer(trace.bounds,framePanX,framePanY,frameZoom,.20f,.95f,.78f,[&]{
            drawLive(growth,strokeId,0,trace.raw,.20f,.95f,.78f);
            tip(false,.20f,.95f,.78f);
            });
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

namespace engine::tablet {
#ifndef __ANDROID__
std::array<uint64_t,2> hostOwnerCharges(){return {sceneOwners.load(),liveVboCharge.load()};}
const void* hostFirstRetiringMesh(){return gpuFinished.begin()->second.source[2]->data();}
#endif
void setPhysicalSlice(size_t bytes) {physicalSlice.store(bytes);}
bool admitIncoming(size_t count,size_t bytes) {
    candidateNodes.store(count);
    return admits(0,0,bytes);
}
static Scene stagedScene,stagedPublication;
static size_t stagedBytes,stagedLimit,stagedCanonicalLimit;
static std::unique_ptr<document::Document> stagedModel;
bool stage(std::unique_ptr<document::Document> candidate,size_t meshBudget) {
    Scene replacement;
    stagedLimit=meshBudget; stagedCanonicalLimit=meshBudget/8*3;
    if(candidate->canonicalBytes()>stagedCanonicalLimit) return false;
    for(Key id=candidate->first();id;) {
        const auto& r=*candidate->find(id);
        auto shape=std::make_shared<FinishedTrace>();
        if(!reconstructV1(r.value.ink,*shape,r.value.dx,r.value.dy,meshBudget,stagedCanonicalLimit)) return false;
        const auto bytes=meshBytes(*shape);
        for(const auto& mesh:{shape->raw,shape->clean,shape->spline}) {
            if(mesh->capacity()>meshBudget/sizeof(Vertex)) return false;
            meshBudget-=mesh->capacity()*sizeof(Vertex);
        }
        replacement.emplace(id,Published{r.next,r.geometry,std::move(shape),bytes,r.value.rgba()});
        id=r.next;
    }
    {
        std::lock_guard guard(stateMutex);
        if(!live.empty() || saves.size() || (model && !suspended)) return false;
    }
    stagedBytes=stagedLimit-meshBudget;
    if(!admits(0,0,0,replacement.size())) return false;
    stagedPublication=replacement; // worker only, all allocations before db.select
    stagedScene.swap(replacement);stagedModel.swap(candidate);
    return true;
}
void activate(double x,double y,double zoom) noexcept {
    Scene retired;
    {
        std::lock_guard guard(stateMutex);
        model.swap(stagedModel);
        finished.swap(stagedScene);
        retired.swap(pendingScene);
        pendingScene.swap(stagedPublication);
        sceneHead=model->first();
        sceneRevision=model->revision();
        sceneBytes=stagedBytes;sceneLimit=stagedLimit;canonicalLimit=stagedCanonicalLimit;
        desiredView.publish(x,y,zoom);
        ++finishedEpoch; persistent=true;
        model->observe(&sceneSink);
        visibleRevision.store(model->revision());
        historyCharge.store(0);canonicalCharge.store(model->canonicalBytes());
    }
}
void discard() noexcept {stagedScene.clear();stagedPublication.clear();stagedModel.reset();candidateNodes.store(0);}
std::shared_ptr<const document::ChangeSet> nextSave() {
    std::lock_guard guard(stateMutex); return saves.front();
}
bool acknowledge(const std::shared_ptr<const document::ChangeSet>& token) {
    std::lock_guard guard(stateMutex); return saves.acknowledge(token);
}
}
