
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
#include <thread>
#include "tablet/frame_scheduler.hpp"
#include "tablet/spatial_delta.hpp"
#include "tablet/span_cache.hpp"

#include "input/input_processor.hpp"
#include "document/document.hpp"
#include "document/change_queue.hpp"
#include "document/codec.hpp"
#include "document/canonical_stream.hpp"
#include "document/eraser.hpp"
#include "document/working_set.hpp"
#include "tablet/session.hpp"
#include "tablet/view_intent.hpp"
#include "tablet/live_prefix.hpp"
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
    std::atomic<uint64_t> historyCharge{0},queueHigh{0},canonicalCharge{0};
    std::atomic<uint64_t> physicalSlice{0},sceneOwners{0},candidateNodes{0},liveVboCharge{0};
    std::atomic<uint64_t> livePublicationBytes{0},livePublicationHigh{0},publicationRetries{0},plannerFixedCharge{0};
    tablet::spatial::Stats spatialStats;
    tablet::spans::Stats spanStats;
    // Restore/edit worker reconstruction only, not active ink callbacks.
    struct ReplayCounters {
        std::atomic<uint64_t> attempts{0},success{0},failed{0},samples{0},spans{0},chords{0},ns{0},maxNs{0},peakMesh{0};
    } replayCounters;
    bool admits(uint64_t activeMesh,uint64_t samples,uint64_t decoding=0,uint64_t extraNodes=0) {
        const auto slice=physicalSlice.load(std::memory_order_relaxed);
        if(!slice) return true; // host renderer fixtures have no Android process
        return document::WorkingSet::forSlice(slice).admits(document::Ink::liveBytes.load(),
                retainedMeshes.load(),sceneOwners.load()+candidateNodes.load()+4+extraNodes,
                activeMesh,samples,decoding+livePublicationBytes.load()+plannerFixedCharge.load()+spatialStats.dynamicBytes.load(),liveVboCharge.load());
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

    // Charges every retained mirror page/directory/tail until its final owner
    // releases storage, including conservative 2x payload GPU-growth credit.
    // Actual VBO reservations are also counted, intentionally double-reserving
    // that headroom. This protects a parked old latch after cancel/seal and
    // before a late upload; full cross-lane byte credits remain future work.
    struct LivePublicationCharge {
        size_t bytes;
        explicit LivePublicationCharge(size_t n):bytes(n) {
            if(!admits(0,0,n)) throw std::bad_alloc{};
            peak(livePublicationHigh,livePublicationBytes.fetch_add(n)+n);
        }
        ~LivePublicationCharge(){livePublicationBytes.fetch_sub(bytes);}
        LivePublicationCharge(const LivePublicationCharge&)=delete;
    };
    using LivePrefix=tablet::LivePrefix<Vertex,LivePublicationCharge>;
    struct LiveTail : LivePublicationCharge {
        std::array<Vertex,(stroke::IncrementalSplineBuilder::max_steps+1)*6> values;
        size_t count=0;
        LiveTail():LivePublicationCharge(sizeof(LiveTail)+64+2*sizeof(values)) {}
    };
    struct LiveMirror {
        std::array<LivePrefix,3> prefix;
        std::shared_ptr<const LiveTail> tail;
        std::shared_ptr<LiveTail> candidate;
        std::array<std::shared_ptr<LiveTail>,3> tailPool;
        void prepareTail() {
            // A slot with only the pool reference is no longer reachable from
            // any publication. Reuse it without touching any reader's bytes.
            for(auto& slot:tailPool) {
                if(!slot) slot=std::make_shared<LiveTail>();
                if(slot.use_count()==1) {candidate=slot;return;}
            }
            // A renderer may be releasing an older handle outside the port.
            // One bounded transient candidate is allowed and charged.
            candidate=std::make_shared<LiveTail>();
        }
    };
    struct LiveMeshView {
        LivePrefix::View prefix;
        std::shared_ptr<const LiveTail> tail;
        size_t size() const {return tail?tail->count:prefix.size();}
        template<class F> void visit(size_t offset,F&& f) const {
            if(tail) {if(offset<tail->count) f(offset,std::span<const Vertex>(tail->values.data()+offset,tail->count-offset));}
            else prefix.visit(offset,std::forward<F>(f));
        }
    };
    struct LiveView {
        uint64_t id=0,tailRevision=0;
        Bounds bounds;
        std::array<LiveMeshView,4> parts;
        float lastX=0,lastY=0,filteredX=0,filteredY=0;
        bool hasLast=false,hasFiltered=false;
        float radiusWorld=1.9f,filteredRadius=1.9f;
    };

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
        std::unique_ptr<LiveMirror> mirror; // live JNI only; replay scratch stays v1

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
        double dx=0,dy=0;
    };
    using Key=document::Id;
    struct Published {
        Key next=0;
        document::Revision geometry=0;
        std::shared_ptr<const FinishedTrace> shape;
        size_t bytes=0;
        std::array<float,4> rgba{};
        Key prev=0; // reciprocal canonical link for sparse delta validation
    };
    uint64_t workingBytes(const Trace& t) {
        return t.input.raw_samples().capacity()*sizeof(input::RawInputPoint)+
               t.input.clean_samples().capacity()*sizeof(input::CleanInputPoint)+t.spline.retainedBytes()+
               (t.rawPoints.capacity()+t.cleanPoints.capacity())*sizeof(float)+
               (t.rawMesh.capacity()+t.cleanMesh.capacity()+t.stableMesh.capacity()+t.tailMesh.capacity())*sizeof(Vertex);
    }
    using Scene=std::map<Key,Published,std::less<Key>,SceneAllocator<std::pair<const Key,Published>>>;
    Scene finished, pendingScene;
    // Writers serialize on stateMutex. Only publication metadata and pending
    // patch ownership cross this separate port. GL never takes stateMutex.
    // Lock order: state -> publication -> view; GL try-locks publication.
    std::mutex publicationMutex;
    LiveView publishedLive;
    uint64_t livePublicationSequence=0;
    struct RenderLatch {
        LiveView active;
        tablet::ViewIntent view;
        Key head=0;
        uint64_t epoch=0, liveSequence=0;
        document::Revision revision=0;
        document::Uuid doc{};
    };
    RenderLatch renderLatch; // GL-owned; keep prior coherent latch on contention
    bool publicationRetry=false; // GL-owned, consumed by onDrawFrame
    size_t pendingCount() {
        std::lock_guard guard(publicationMutex);
        return pendingScene.size();
    }
    void retireLivePublication() {
        LiveView retired;
        {std::lock_guard guard(publicationMutex); std::swap(retired,publishedLive);++livePublicationSequence;}
    }
    // Durable watermark is distinct from the visible root. Only an acknowledged
    // SQLite commit (or fully validated activation) may advance it.
    std::atomic<uint64_t> durableEpoch{0};
    std::atomic<document::Revision> durableRevision{0};
    document::Uuid sceneDocument{};
    void setDurable(uint64_t epoch,document::Revision revision) noexcept {
        durableEpoch.store(0,std::memory_order_release);
        durableRevision.store(revision,std::memory_order_relaxed);
        durableEpoch.store(epoch,std::memory_order_release);
    }
    document::Revision durableFor(uint64_t epoch) noexcept {
        if(durableEpoch.load(std::memory_order_acquire)!=epoch)return 0;
        auto r=durableRevision.load(std::memory_order_acquire);
        return durableEpoch.load(std::memory_order_acquire)==epoch?r:0;
    }
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

    // Worker-owned packed spatial pages, edit BVH, counted paint sequence and
    // ID locator; only completed, compatible plans leave the worker. The resident scene
    // remains the validated authority and exact fallback until canonical
    // streaming and geometry residency are integrated.
    struct PlanStamp {
        uint64_t epoch=0;
        document::Revision revision=0;
        Key head=0;
        tablet::ViewIntent view;
        bool sameScene(const PlanStamp& other) const noexcept {
            return epoch==other.epoch && revision==other.revision && head==other.head &&
                view.mappingGeneration==other.view.mappingGeneration && view.width==other.view.width && view.height==other.view.height;
        }
        bool same(const PlanStamp& other) const noexcept {return sameScene(other)&&view.sequence==other.view.sequence;}
        tablet::spatial::Window window() const noexcept {
            const double x=(float)view.x,y=(float)view.y,z=(float)view.zoom;
            return {x,y,x+view.width/z,y+view.height/z};
        }
    };
    struct SceneDelta {
        Scene rows;
        document::Uuid doc{};
        uint64_t epoch=0;
        document::Revision revision=0;
        Key head=0;
    };
    constexpr size_t visiblePlanLimit=65536;
    constexpr size_t spanCutLimit=64;
    struct SpanCut {
        size_t index=0;document::Revision geometry=0;
        tablet::spans::Range vertices{};
    };
    struct VisiblePlan {
        PlanStamp stamp;
        tablet::spatial::Window envelope{};
        std::array<Key,visiblePlanLimit> ids;
        size_t count=0;
        std::array<SpanCut,spanCutLimit> cuts{};size_t cutCount=0;
        bool valid=false,indexed=false;
    };
    struct FramePlanner {
        tablet::SpscSlot<SceneDelta> deltas;
        tablet::SpscRing<LiveView,8> retiredLive;
        tablet::LatestTriple<VisiblePlan> plans;
        // Worker scratch is charged with the fixed planner reservation. A
        // coarse complete plan is published before any cold span I/O.
        VisiblePlan workingPlan;
        tablet::spans::Cache spanCache{spanStats};
        struct SpanMemo {
            std::weak_ptr<const FinishedTrace> shape;
            tablet::spans::Key key{};uint64_t epoch=0;bool valid=false,disabled=false;
        };
        std::array<SpanMemo,256> spanMemos{};
        bool spanInitAttempted=false;
        void publishPlan(const VisiblePlan& plan) noexcept {
            auto& out=plans.write();out.stamp=plan.stamp;out.envelope=plan.envelope;
            out.count=plan.count;out.valid=plan.valid;out.indexed=plan.indexed;out.cutCount=plan.cutCount;
            std::copy_n(plan.ids.begin(),plan.count,out.ids.begin());std::copy_n(plan.cuts.begin(),plan.cutCount,out.cuts.begin());
            plans.publish();++built;
        }
        tablet::WakeGate wake;
        tablet::spatial::Index index{spatialStats,[](size_t n){return admits(0,0,n);}};
        std::mutex cacheDirMutex;
#ifdef __ANDROID__
        std::array<char,4096> cacheDir{};
#else
        std::array<char,4096> cacheDir{'/','t','m','p','\0'};
#endif
        std::atomic<uint64_t> envelopeReuse{0},indexedPlans{0},linearPlans{0},linearTests{0};
        std::atomic<uint64_t> indexedFrames{0},linearFrames{0},envelopeFrames{0},planIdsTested{0},residentFallbackRows{0};
        std::atomic<bool> enabled{false},stopping{false};
        bool attempted=false; // GL owner, one startup attempt per process
        std::thread worker;
        std::atomic<uint64_t> hits{0},fallbacks{0},cancelled{0},built{0},backpressure{0},invalid{0},retired{0};
#ifndef __ANDROID__
        std::atomic<bool> testPark{false},testParked{false};
        bool testForceFallback=false; // GL test owner only
        std::atomic<int> testSpatialFault{0};
        std::binary_semaphore testRelease{0};
#endif
        void configureCache(const char* path) noexcept {
            if(!path)return;
            std::lock_guard lock(cacheDirMutex);
            if(std::strlen(path)<cacheDir.size())std::memcpy(cacheDir.data(),path,std::strlen(path)+1);
        }
        void start() noexcept {
            if(attempted) return;
            attempted=true;
            // Fixed triple buffer/ring charge plus existing map-node charges.
            // If the reserved plan storage cannot fit, retain exact v1 path.
            if(!admits(0,0,sizeof(FramePlanner)+tablet::spans::scratchCeiling)) return;
            plannerFixedCharge.store(sizeof(FramePlanner)+tablet::spans::scratchCeiling);
            try {worker=std::thread([this]{run();});enabled.store(true,std::memory_order_release);}
            catch(...) {plannerFixedCharge.store(0);}
        }
        ~FramePlanner() {
            if(worker.joinable()) {
                stopping.store(true);wake.signal();
#ifndef __ANDROID__
                if(testPark.exchange(false)) testRelease.release();
#endif
                worker.join();
            }
        }
        void request() noexcept {if(enabled.load(std::memory_order_acquire)) wake.signal();}
        void submit(Scene& rows,const RenderLatch& frame) noexcept {
            // canPush was checked before consuming the publication. Only GL
            // produces these batches, and the worker can only free capacity.
            // Swap with the preconstructed slot; no map allocation here.
            if(!deltas.push([&](SceneDelta& delta) noexcept {
                assert(delta.rows.empty());delta.rows.swap(rows);
                delta.epoch=frame.epoch;delta.revision=frame.revision;delta.head=frame.head;delta.doc=frame.doc;
            })) std::terminate();
            wake.signal();
        }
        void retire(LiveView&& view) noexcept {
            if(!retiredLive.push(std::move(view))) std::terminate();
            wake.signal();
        }
        void drainRetired() noexcept {
            for(size_t i=0;i<8;++i) {auto old=retiredLive.pop();if(!old)break;old.reset();++retired;}
        }
        bool obsolete(const PlanStamp& stamp,tablet::spatial::Window envelope) const noexcept {
            const auto view=desiredView.latch();
            PlanStamp wanted{stamp.epoch,stamp.revision,stamp.head,view};
            return stopping.load() || deltas.hasData() || !stamp.sameScene(wanted) || !envelope.contains(wanted.window());
        }
        static tablet::spatial::Box boxOf(const Published& p) noexcept {
            return {p.shape->minX,p.shape->minY,p.shape->maxX,p.shape->maxY};
        }
        void rebuildIndex(const Scene& scene,Key head) noexcept {
            std::array<char,4096> path{};
            {std::lock_guard lock(cacheDirMutex);path=cacheDir;}
            auto stop=[&]{drainRetired();return stopping.load()||deltas.hasData();};
            index.rebuild(path.data(),scene.size(),[&](auto emit){
                size_t visited=0;Key prev=0,id=head;
                while(id) {
                    auto it=scene.find(id);
                    if(it==scene.end()||!it->second.shape||it->second.prev!=prev||++visited>scene.size())return false;
                    emit(id,boxOf(it->second));prev=id;id=it->second.next;
                }
                return visited==scene.size();
            },stop);
        }
        // Match the exact resident v1 descriptors, including translation,
        // endpoint ordering and all six vertices. No mesh reconstruction on GL.
        static bool matchChord(const FinishedTrace& shape,uint32_t first,float ax,float ay,float bx,float by,float a,float b,document::SpanBounds& bounds) noexcept {
            if(first>shape.spline->size()||shape.spline->size()-first<6)return false;
            float dx=bx-ax,dy=by-ay,len=std::hypot(dx,dy);
            if(len<.0001f){dx=1;dy=0;len=0;}else{dx/=len;dy/=len;}
            if(shape.dx!=0||shape.dy!=0){ax=float(double(ax)+shape.dx);ay=float(double(ay)+shape.dy);}
            static constexpr std::array<std::array<float,2>,6> corners{{{0,-1},{1,-1},{1,1},{0,-1},{1,1},{0,1}}};
            static_assert(sizeof(Vertex)==9*sizeof(float));
            for(size_t j=0;j<6;++j){const Vertex v{ax,ay,corners[j][0],corners[j][1],len,a,b<0?a:b,dx,dy};
                if(std::memcmp(&v,&(*shape.spline)[first+j],sizeof(v)))return false;}
            // Enclose the entire expanded quad (not just the analytic disc)
            // at the minimum supported zoom, plus generous floating-point
            // and screen-transform margin. Outside this v1 domain use exact
            // whole-stroke submission. M05 owns wider precision guarantees.
            constexpr double domain=16777216.;
            const double magnitude=std::max({std::abs(double(ax)),std::abs(double(ay)),double(len),double(a),double(b),1.});
            if(!std::isfinite(magnitude)||magnitude>domain)return false;
            const double pad=2*(std::max(double(a),double(b))+10.)+32.+64*std::numeric_limits<float>::epsilon()*magnitude;
            return bounds.include(ax,ay,double(ax)+double(dx)*len,double(ay)+double(dy)*len,pad);
        }
        template<class Stop> bool identifySpan(SpanMemo& memo,const Published& row,const document::Uuid& doc,Key id,uint64_t epoch,Stop& stop) {
            using namespace tablet::spans;
            document::wire::Header header{};const auto& shape=*row.shape;const auto& ink=*shape.record;
            if(!document::wire::header(ink.brush,shape.dx,shape.dy,header))return false;
            document::SourceDigest source,mesh;source.update(header);
            std::array<uint8_t,64*document::wire::sampleBytes> chunk{};auto points=std::span(ink.samples);
            while(!points.empty()) {if(stop())return false;const auto n=std::min<size_t>(64,points.size());auto bytes=std::span(chunk).first(n*document::wire::sampleBytes);
                if(!document::wire::samples(points.first(n),bytes))return false;
                source.update(bytes);spanStats.hashSamples+=n;points=points.subspan(n);}
            // Derived identity also binds every resident vertex. It prevents
            // reusing ranges across any live/replay/compiler discrepancy.
            static_assert(std::endian::native==std::endian::little);
            auto data=std::span(reinterpret_cast<const uint8_t*>(shape.spline->data()),shape.spline->size()*sizeof(Vertex));
            while(!data.empty()){if(stop())return false;const auto n=std::min<size_t>(65536,data.size());mesh.update(data.first(n));data=data.subspan(n);}
            memo.shape=row.shape;memo.epoch=epoch;memo.key={doc,id,row.geometry,source.finish(),mesh.finish()};memo.valid=true;memo.disabled=false;return true;
        }
        template<class Stop> void refineSpans(const Scene& scene,const document::Uuid& doc,VisiblePlan& plan,Stop& stop) noexcept {
            namespace sp=tablet::spans;
            sp::TimeCharge elapsedRefine{spanStats.refineNs,spanStats.refineMaxNs};
            if(!spanInitAttempted){spanInitAttempted=true;std::array<char,4096> path{};{std::lock_guard lock(cacheDirMutex);path=cacheDir;}
#ifdef __ANDROID__
                spanCache.init(path.data());
#else
                spanCache.init(path.data(),true);
#endif
            }
            if(!spanCache.ready())return;
            constexpr double domain=16777216.;const auto& w=plan.envelope;
            if(std::max({std::abs(w.minX),std::abs(w.minY),std::abs(w.maxX),std::abs(w.maxY)})>domain)return;
            bool coldUsed=false;uint32_t pagesLeft=256;size_t considered=0;
            for(size_t i=0;i<plan.count;++i){
                if(stop()){++spanStats.cancelled;plan.cutCount=0;return;}
                auto it=scene.find(plan.ids[i]);if(it==scene.end()||!it->second.shape)continue;
                const auto& row=it->second;const auto& shape=*row.shape;
                if(!shape.record||!shape.spline||shape.spline->size()<6*256||shape.spline->size()>sp::maxVertices)continue;
                if(std::max({std::abs(double(shape.minX)),std::abs(double(shape.minY)),std::abs(double(shape.maxX)),std::abs(double(shape.maxY))})>domain)continue;
                if(considered++==spanCutLimit||!pagesLeft){++spanStats.limitSkips;break;}
                auto& memo=spanMemos[uint64_t(plan.ids[i])%spanMemos.size()];
                const bool same=memo.valid&&memo.epoch==plan.stamp.epoch&&memo.key.id==plan.ids[i]&&memo.key.geometry==row.geometry&&memo.shape.lock()==row.shape;
                if(!same){if(coldUsed){++spanStats.fallbackRows;continue;}coldUsed=true;memo={};if(!identifySpan(memo,row,doc,plan.ids[i],plan.stamp.epoch,stop)){++spanStats.fallbackRows;continue;}}
                if(memo.disabled){++spanStats.fallbackRows;continue;}
                const auto durable=durableFor(plan.stamp.epoch);
                document::wire::Header header{};if(!document::wire::header(shape.record->brush,shape.dx,shape.dy,header))continue;
                auto ready=spanCache.lookup(memo.key,durable,uint32_t(shape.spline->size()),header,uint32_t(shape.record->samples.size()));
                if(!ready){if(same&&coldUsed){++spanStats.fallbackRows;continue;}coldUsed=true;
                    auto match=[&](uint32_t first,float ax,float ay,float bx,float by,float a,float b,document::SpanBounds& bnd){return matchChord(shape,first,ax,ay,bx,by,a,b,bnd);};
                    const auto failed=spanStats.buildFailures.load();
                    ready=spanCache.create(memo.key,durable>=plan.stamp.revision?plan.stamp.revision:0,*shape.record,shape.dx,shape.dy,uint32_t(shape.spline->size()),stop,match);
                    if(!ready){if(spanStats.buildFailures.load()!=failed)memo.disabled=true;++spanStats.fallbackRows;continue;}
                }else if(!ready->manifest.durable&&durable>=plan.stamp.revision&&!spanCache.promote(*ready,plan.stamp.revision)){++spanStats.fallbackRows;continue;}
                // Only a complete cut is published. On overflow, I/O fault,
                // cancellation or mismatch the existing whole-stroke VBO is
                // used, not a truncated range. No GL thread disk access.
                sp::Range range{};bool any=false;
                auto emit=[&](const sp::Packet& p){if(!any){range=p.vertices;any=true;}else range.count=p.vertices.first+p.vertices.count-range.first;};
                const auto before=spanStats.readPages.load();sp::TimeCharge elapsedQuery{spanStats.queryNs,spanStats.queryMaxNs};
                try{
                    auto work=sp::query(ready->file,ready->manifest,w,pagesLeft,spanStats,stop,emit);
                    pagesLeft-=work.pages;spanStats.testedSpans+=work.tested;spanStats.selectedSpans+=work.hits;++spanStats.queries;
                    if(range.count<shape.spline->size()){plan.cuts[plan.cutCount++]={i,row.geometry,range};++spanStats.plannedRows;}
                }catch(const tablet::spatial::Failure& f){sp::note(spanStats,f);++spanStats.queryFailures;++spanStats.fallbackRows;
                    pagesLeft-=std::min<uint64_t>(pagesLeft,spanStats.readPages.load()-before);
                    if(f==tablet::spatial::Failure::Io||f==tablet::spatial::Failure::Invalid)spanCache.invalidate(ready->slot);
                    if(f==tablet::spatial::Failure::Cancelled){plan.cutCount=0;return;}
                }catch(...){++spanStats.invalid;++spanStats.queryFailures;++spanStats.fallbackRows;}
            }
        }
        void run() noexcept {
            Scene scene; // sole planner owner, existing SceneAllocator ledger
            SceneDelta delta;
            auto takeDelta=[&]() noexcept {return deltas.pop([&](SceneDelta& slot) noexcept {
                assert(delta.rows.empty());delta.rows.swap(slot.rows);
                delta.epoch=slot.epoch;delta.revision=slot.revision;delta.head=slot.head;delta.doc=slot.doc;
            });};
            PlanStamp stamp,last;document::Uuid doc{};
            bool haveLast=false,lastValid=false;
            tablet::spatial::Window lastEnvelope{};
            while(true) {
                wake.wait();
                if(stopping.load()) break;
#ifndef __ANDROID__
                if(testPark.load()) {testParked.store(true);testRelease.acquire();testParked.store(false);}
                if(stopping.load()) break;
#endif
                if(takeDelta()) {
                    const bool sameEpoch=stamp.epoch==delta.epoch;
                    const bool incremented=sameEpoch&&tablet::spatial::updateOrdered(index,scene,delta.rows,stamp.head,delta.head,boxOf,[&]{drainRetired();return stopping.load()||deltas.hasData();});
                    if(!sameEpoch) scene.clear();
                    for(auto it=delta.rows.begin();it!=delta.rows.end();) {
                        scene.erase(it->first);
                        if(!it->second.shape) it=delta.rows.erase(it);else ++it;
                    }
                    scene.merge(delta.rows);
                    stamp.epoch=delta.epoch;stamp.revision=delta.revision;stamp.head=delta.head;doc=delta.doc;
                    if(!incremented)rebuildIndex(scene,stamp.head);
                    // Context rebuild can resend the same revision; never
                    // retain a plan from before consuming a delta batch.
                    haveLast=false;
                }
                drainRetired();
#ifndef __ANDROID__
                if(const int fault=testSpatialFault.exchange(0)) {
                    if(index.ready()&&index.hasBase()) {if(fault==1)index.corruptSpatialRoot();else if(fault==2)index.truncatePages();else if(fault==4)index.corruptSequenceRoot();else index.corruptOrderRoot();}
                    haveLast=false;
                }
#endif
                stamp.view=desiredView.latch();
                const auto exact=stamp.window();
                if(haveLast&&stamp.same(last))continue;
                if(haveLast&&lastValid&&stamp.sameScene(last)&&lastEnvelope.contains(exact)) {last.view=stamp.view;++envelopeReuse;continue;}
                auto& plan=workingPlan;plan.stamp=stamp;plan.count=0;plan.cutCount=0;plan.valid=false;plan.indexed=false;
                // Prefetch a conservative 20% margin on each side. This is
                // an ID superset, not a tile image. GL rechecks exact bounds
                // in the actual viewport and composites whole strokes once.
                const double dx=(exact.maxX-exact.minX)*.20,dy=(exact.maxY-exact.minY)*.20;
                plan.envelope={exact.minX-dx,exact.minY-dy,exact.maxX+dx,exact.maxY+dy};
                auto stop=[&]{drainRetired();return obsolete(stamp,plan.envelope);};
                using Result=tablet::spatial::Index::Result;
                auto result=index.query(plan.envelope,plan.ids,plan.count,stop);
                if(result==Result::Overflow) {
                    plan.envelope=exact;result=index.query(exact,plan.ids,plan.count,stop);
                }
                if(result==Result::Cancelled||stop()){++cancelled;continue;}
                if(result==Result::Ok) {plan.valid=true;plan.indexed=true;}
                else {
                    // Never truncate visible IDs or reuse incomplete pages.
                    // The resident reference provides exact pixels on any
                    // cache/disk/admission fault. It remains a scalability
                    // limitation, and is explicitly counted.
                    plan.envelope=exact;plan.count=0;
                    size_t visited=0;Key id=stamp.head,prev=0;bool bad=false,aborted=false;
                    while(id) {
                        if((visited&127)==0&&stop()){aborted=true;break;}
                        auto it=scene.find(id);
                        if(it==scene.end()||++visited>scene.size()||!it->second.shape||it->second.prev!=prev){bad=true;break;}
                        if(exact.hits(boxOf(it->second))) {
                            if(plan.count==plan.ids.size()){bad=true;break;}
                            plan.ids[plan.count++]=id;
                        }
                        prev=id;id=it->second.next;
                    }
                    linearTests+=visited;
                    if(aborted||stop()){++cancelled;continue;}
                    plan.valid=!bad&&visited==scene.size();
                }
                if(stop()){++cancelled;continue;}
                if(!plan.valid)++invalid;else if(plan.indexed)++indexedPlans;else ++linearPlans;
                last=stamp;lastEnvelope=plan.envelope;lastValid=plan.valid;haveLast=true;publishPlan(plan);
                if(plan.valid&&plan.indexed&&plan.count){
                    refineSpans(scene,doc,plan,stop);
                    if(plan.cutCount&&!stop())publishPlan(plan);
                }
            }
            // All CPU references in this lane are released on its own thread.
            if(takeDelta()) delta.rows.clear();
            index.reset();spanCache.close();drainRetired();
        }
        const VisiblePlan* latch(const RenderLatch& frame,int w,int h) noexcept {
            if(!enabled.load(std::memory_order_acquire)) return nullptr;
            const auto& plan=plans.latch();
#ifndef __ANDROID__
            if(testForceFallback) {++fallbacks;return nullptr;}
#endif
            PlanStamp wanted{frame.epoch,frame.revision,frame.head,frame.view};
            if(!plan.valid || !plan.stamp.sameScene(wanted) || !plan.envelope.contains(wanted.window()) || frame.view.width!=w || frame.view.height!=h) {
                ++fallbacks;return nullptr;
            }
            return &plan;
        }
    };
    FramePlanner planner;


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
            if(t.mirror) {
                auto& m=*t.mirror;
                m.prefix[0].reserve(18); m.prefix[1].reserve(24); m.prefix[2].reserve(stableExtra);
                m.candidate.reset();
                if(moving && !t.input.clean_samples().empty()) m.prepareTail();
                const auto bytes=(t.rawMesh.capacity()+t.cleanMesh.capacity()+t.stableMesh.capacity())*sizeof(Vertex);
                if(!admits(bytes,t.input.raw_samples().size()+1))
                    return core::Error{core::ErrorCode::ResourceLimit,"Live publication budget"};
            }
        } catch(const std::bad_alloc&) {
            return core::Error{core::ErrorCode::AllocationFailure,"Trace allocation failed"};
        } catch(const std::length_error&) {
            return core::Error{core::ErrorCode::ResourceLimit,"Trace capacity limit"};
        }
        return t.spline.append({c.position,c.timestamp,c.pressure,c.tilt_x,c.tilt_y,c.speed});
    }

    // All allocations were reserved by prepareSample, before accepted input.
    // Build the next small handle packet outside the publication lock.
    void publishLive(Trace& t) noexcept {
        auto& m=*t.mirror;
        m.prefix[0].extend(t.rawMesh);m.prefix[1].extend(t.cleanMesh);m.prefix[2].extend(t.stableMesh);
        if(m.candidate) {
            assert(t.tailMesh.size()<=m.candidate->values.size());
            std::copy(t.tailMesh.begin(),t.tailMesh.end(),m.candidate->values.begin());
            m.candidate->count=t.tailMesh.size();m.tail=std::move(m.candidate);
        }
        LiveView next;
        next.id=t.gpuId;next.tailRevision=t.tailRevision;next.bounds=t.bounds;
        for(size_t i=0;i<3;++i) next.parts[i].prefix=m.prefix[i].view();
        next.parts[3].tail=m.tail;
        next.radiusWorld=t.rawPeak;next.filteredRadius=t.cleanPeak;
        if(t.cleanPoints.size()>=2) {
            next.filteredX=t.cleanPoints[t.cleanPoints.size()-2];next.filteredY=t.cleanPoints.back();next.hasFiltered=true;
        }
        if(t.rawPoints.size()>=2) {
            next.lastX=t.rawPoints[t.rawPoints.size()-2];next.lastY=t.rawPoints.back();next.hasLast=true;
            float pad=std::max(t.rawPeak,t.cleanPeak)+1.f/(float)MIN_ZOOM;
            auto& q=next.bounds;
            q.minX=std::min(q.minX,std::min(next.lastX,next.filteredX)-pad);
            q.maxX=std::max(q.maxX,std::max(next.lastX,next.filteredX)+pad);
            q.minY=std::min(q.minY,std::min(next.lastY,next.filteredY)-pad);
            q.maxY=std::max(q.maxY,std::max(next.lastY,next.filteredY)+pad);
        }
        // Swap, then release obsolete handles outside the port.
        {std::lock_guard guard(publicationMutex);std::swap(next,publishedLive);++livePublicationSequence;}
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

    struct Part { GLuint vbo; GLsizei count; GLint first=0; };
    // GL-thread only; a layer is either a completed mesh or stable/tail/tip.
    std::array<Part,3> parts{};
    size_t partCount=0;
    void drawCached(GLuint vbo, GLsizei count, float, float, float,GLint first=0) {
        if (!program || !vbo || count <= 0) return;
        parts.at(partCount++)={vbo,count,first};
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
                glDrawArrays(GL_TRIANGLES,parts[i].first,parts[i].count);
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
        LiveMeshView data;
    };

    struct FrameGrowth {
        size_t pending=0;
        void reserve(size_t n) {pending+=n;liveVboCharge.fetch_add(n);}
        ~FrameGrowth(){liveVboCharge.fetch_sub(pending);}
    };
    LiveUpload snapshotUpload(FrameGrowth& growth,const LiveMeshView& mesh, uint64_t id, int part, uint64_t revision = 0) {
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
            // Reserve while the latched immutable prefix is still pinned.
            // No mutable trace or input-state lock is needed.
            growth.reserve((result.plannedCapacity-capacity)*sizeof(Vertex));
        }
        if (part == 3 && previous && previous->revision == revision && uploaded == result.total) {
            result.offset=result.total;
            return result;
        }
        result.offset = (part == 3 || result.total < uploaded || result.total > capacity)
                ? 0 : uploaded;
        if (result.total > result.offset) result.data=mesh;
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
        if (mesh.total>mesh.offset) {
            mesh.data.visit(mesh.offset,[&](size_t offset,std::span<const Vertex> vertices) {
                glBufferSubData(GL_ARRAY_BUFFER,(GLintptr)(offset*sizeof(Vertex)),
                                (GLsizeiptr)vertices.size_bytes(),vertices.data());
                frameUploadBytes+=vertices.size_bytes();liveUploadBytes+=vertices.size_bytes();
            });
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

    void drawFinishedPart(CachedGpuStroke& item, int part, float r, float g, float b,const tablet::spans::Range* cut=nullptr) {
        const auto& vertices = *item.source[part];
        if (vertices.empty()) return;
        if(cut&&(cut->first%6||cut->count%6||cut->first>vertices.size()||cut->count>vertices.size()-cut->first))cut=nullptr;
        if(cut&&!cut->count){++spanStats.renderedRows;spanStats.verticesSaved+=vertices.size();return;}
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
        drawCached(item.vbo[part],cut?GLsizei(cut->count):item.count[part],r,g,b,cut?GLint(cut->first):0);
        if(cut){++spanStats.renderedRows;spanStats.verticesSaved+=vertices.size()-cut->count;}
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
            std::lock_guard publish(publicationMutex);
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
            auto& row=appendNodes.at(id);row={0,doc.revision()+1,appended,appendedBytes,doc.find(id)->value.rgba(),prev};
            appendPatch.at(id)=row;
            if(prev) {
                auto& predecessor=finished.at(prev);predecessor.next=id;appendPatch.at(prev)=predecessor;
                pendingScene.erase(prev);
            }
            finished.merge(appendNodes);pendingScene.merge(appendPatch);
            sceneHead=doc.first();sceneBytes+=appendedBytes;sceneRevision=doc.revision()+1;
            return document::Status::Ok;
        }
        // A live ticket must use its pre-reserved append nodes. Do not enter
        // the edit path (which takes publicationMutex itself) on a broken ticket.
        if(appended) return document::Status::Conflict;
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
                auto shape=std::make_shared<FinishedTrace>();
                if(!reconstructV1(r->value.ink,*shape,r->value.dx,r->value.dy,sceneLimit-projected,canonicalLimit)) {valid=false;return;}
                dst.shape=std::move(shape);
                dst.bytes=meshBytes(*dst.shape);
                if(dst.bytes>sceneLimit-projected) {valid=false;return;}
                projected+=dst.bytes;
            } else if(!dst.shape) {dst=finished.at(id);dst.next=r->next;}
            dst.rgba=r->value.rgba();dst.prev=r->prev;
        };
        // Subjects first: adjacent new subjects may not yet have a projection.
        for(auto e:touched) if(e.geometry) include(e.id,true);
        if(!valid) return document::Status::Memory;
        for(auto e:touched) {if(!e.geometry) include(e.id,false);include(e.prev,false);include(e.next,false);}
        Scene notifications=stage;
        std::unique_lock lock(stateMutex,std::defer_lock);
        if(worker) lock.lock();
        if(persistent) {
            if(pendingCount()+stage.size()>publicationLimit)
                return document::Status::QueueLimit;
            auto s=saves.prepare(doc,touched);
            if(s!=document::Status::Ok) return s;
        }
        // Expensive rebuild and save planning are finished. The port only
        // publishes the prepared patch; GL never waits if a large merge runs.
        std::lock_guard publish(publicationMutex);
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
        {std::lock_guard publish(publicationMutex);sceneDocument=model->identity();}
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

    desiredView.publish(panX,panY,std::clamp((double)zoom,MIN_ZOOM,MAX_ZOOM));planner.request();
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
           (persistent && (pendingCount()+2>publicationLimit ||
                           model->canonicalBytes()+1024>canonicalLimit || sceneLimit-sceneBytes<54*sizeof(Vertex) ||
                           !admits(54*sizeof(Vertex),1)))) return JNI_FALSE;
        auto trace=std::make_unique<Trace>(std::move(inputProcessor.value()),std::move(fitter.value()));
        trace->mirror=std::make_unique<LiveMirror>();
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
    publishLive(trace);
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
    // Append notification and live removal are one atomic publication. No
    // renderer may observe a partly filled FinishedTrace or double-draw it.
    LiveView retiredPublication;
    std::unique_lock<std::mutex> publish(publicationMutex,std::defer_lock);
    try {
        auto shape=std::make_shared<FinishedTrace>();
        if(!trace.sealed) trace.sealed=std::make_shared<tablet::AdoptAccepted>(trace.input,trace.brush);
        sceneSink.appended=shape;
        sceneSink.appendedBytes=(trace.rawMesh.capacity()+trace.cleanMesh.capacity()+trace.stableMesh.capacity())*sizeof(Vertex);
        peak(activeHigh,workingBytes(trace));
        publish.lock();
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
    std::swap(retiredPublication,publishedLive);++livePublicationSequence;
    publish.unlock();
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

// Reconstruct from finalized bounded spans; never allocate raw/clean sample
// histories or a full curve coefficient vector on this replay path. Outputs
// remain whole-stroke resident v1 meshes until the residency milestone.
static bool reconstructV1(const std::shared_ptr<const document::Ink>& record,FinishedTrace& out,double dx,double dy,
                          size_t allowance,size_t scratchLimit) {
    struct Observation {
        int64_t start=profileNs();bool ok=false;
        size_t samples=0,spans=0,chords=0,bytes=0;
        Observation(){++replayCounters.attempts;}
        ~Observation(){
            const auto ns=uint64_t(profileNs()-start);
            replayCounters.ns.fetch_add(ns);peak(replayCounters.maxNs,ns);
            peak(replayCounters.peakMesh,bytes);
            if(ok){++replayCounters.success;replayCounters.samples+=samples;replayCounters.spans+=spans;replayCounters.chords+=chords;}
            else ++replayCounters.failed;
        }
    } observation;
    constexpr size_t fixedScratch=16u<<10; // rolling state + fitter stack + call stack allowance
    if(!record || !record->valid() || scratchLimit<fixedScratch || !admits(0,0,fixedScratch)) return false;
    auto stream=document::CanonicalStream::create(record->brush);
    if(!stream) return false;
    try {
        auto geometry=std::make_shared<CompletedGeometry>();
        struct Sink {
            std::array<Mesh,3>& meshes;
            size_t allowance;
            Observation& obs;
            bool append(size_t i,float ax,float ay,float bx,float by,float a,float b) {
                if(!std::isfinite(ax)||!std::isfinite(ay)||!std::isfinite(bx)||!std::isfinite(by)||
                   !std::isfinite(a)||!std::isfinite(b)||a<0||b<0||!std::isfinite(std::hypot(bx-ax,by-ay))) return false;
                auto& m=meshes[i];
                if(m.capacity()-m.size()<6) {
                    size_t other=0;
                    for(size_t j=0;j<3;++j) if(i!=j) other+=meshes[j].capacity();
                    const auto limit=allowance/sizeof(Vertex);
                    if(other>limit || m.size()>limit-other || limit-other-m.size()<6) return false;
                    const size_t required=m.size()+6;
                    const size_t next=m.size()+std::min(std::max(m.size(),size_t(6)),limit-other-m.size());
                    if(next<required || !admits((other+next)*sizeof(Vertex),0,fixedScratch)) return false;
                    m.reserve(next);
                    if(m.capacity()>limit-other) return false;
                    obs.bytes=(other+m.capacity())*sizeof(Vertex);
                }
                quad(m,ax,ay,bx,by,a,b);++obs.chords;return true;
            }
            static bool consume(void* p,const document::ReplaySpan& s) {
                auto& self=*static_cast<Sink*>(p);
                if(s.kind==document::ReplaySpan::Terminal) {
                    if(!self.append(0,s.bx,s.by,s.bx,s.by,s.arrival,s.arrival)) return false;
                    for(size_t i=1;i<3;++i) if(!s.chords([&](auto... v){return self.append(i,v...);})) return false;
                    return true;
                }
                if(s.kind==document::ReplaySpan::Curve) ++self.obs.spans;
                const size_t i=size_t(s.kind);
                return s.chords([&](auto... v){return self.append(i,v...);});
            }
        } sink{geometry->derived,allowance,observation};
        auto& replay=stream.value();
        auto remaining=std::span(record->samples);
        while(!remaining.empty()) {
            const auto n=std::min(remaining.size(),document::wire::chunkSamples);
            if(!replay.append(remaining.first(n),&sink,Sink::consume)) return false;
            remaining=remaining.subspan(n);
        }
        if(!replay.finish(&sink,Sink::consume)) return false;
        Bounds b;
        // Preserve v1 float conversion and post-reconstruction translation.
        for(auto& mesh:geometry->derived) {
            if(dx!=0 || dy!=0) for(auto& v:mesh) {
                const double x=double(v.x)+dx,y=double(v.y)+dy;
                if(!std::isfinite(x)||!std::isfinite(y)||std::max(std::abs(x),std::abs(y))>std::numeric_limits<float>::max()/1024.) return false;
                v.x=float(x);v.y=float(y);
            }
            b.include(mesh,0);
        }
        observation.samples=replay.samples();
        // Record capacity before publication, and retain charge through final owner.
        for(const auto& m:geometry->derived) geometry->bytes+=m.capacity()*sizeof(Vertex);
        peak(switchHigh,geometry->bytes+fixedScratch+document::Ink::liveBytes.load()+retainedMeshes.load()+gpuRequested.load());
        retainedMeshes.fetch_add(geometry->bytes,std::memory_order_relaxed);
        out={record,{geometry,&geometry->derived[0]},{geometry,&geometry->derived[1]},
                    {geometry,&geometry->derived[2]},b.minX,b.minY,b.maxX,b.maxY,0,dx,dy};
        observation.ok=true;return true;
    } catch(const std::bad_alloc&) {return false;}
      catch(const std::length_error&) {return false;}
}
JNIEXPORT jlongArray JNICALL Java_com_infinipaint_demo_NativeBridge_canonicalReplayCounters(JNIEnv* env,jclass) {
    const std::array<jlong,11> a={jlong(replayCounters.attempts.load()),jlong(replayCounters.success.load()),
        jlong(replayCounters.failed.load()),jlong(replayCounters.samples.load()),jlong(replayCounters.spans.load()),
        jlong(replayCounters.chords.load()),jlong(replayCounters.ns.load()),jlong(replayCounters.maxNs.load()),
        jlong(replayCounters.peakMesh.load()),jlong(sizeof(document::CanonicalStream)),jlong(16u<<10)};
    auto array=env->NewLongArray(a.size());if(array)env->SetLongArrayRegion(array,0,a.size(),a.data());return array;
}

JNIEXPORT jlong JNICALL Java_com_infinipaint_demo_NativeBridge_revision(JNIEnv*,jclass) {
    return visibleRevision.load();
}
JNIEXPORT jlongArray JNICALL Java_com_infinipaint_demo_NativeBridge_memory(JNIEnv* env,jclass) {
    std::array<jlong,16> a{};
    {
        std::lock_guard guard(stateMutex);
        uint64_t active=0;
        for(const auto& [id,t]:live) active+=workingBytes(*t);
        a={jlong(document::Ink::liveBytes.load()),jlong(canonicalCharge.load()),jlong(retainedMeshes.load()),
           jlong(active),jlong(activeHigh.load()),jlong(gpuRequested.load()),jlong(gpuHigh.load()),
           jlong(historyCharge.load()),jlong(saves.bytes()),jlong(queueHigh.load()),jlong(switchHigh.load()),
           jlong(0),jlong(pendingCount()),jlong(livePublicationBytes.load()),
           jlong(livePublicationHigh.load()),jlong(publicationRetries.load())};
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
// Exercise the real edit observer/publication path with a middle splice.
// Host-only fixture, not a new application operation or JNI interface.
bool ipTestMoveBatch(int64_t first,int64_t last,double dy) {
    using namespace document;
    if(!model||!suspended||!live.empty()||first<=0||last<first||last-first>=128)return false;
    std::vector<Edit> edits;for(auto id=first;id<=last;++id){const auto* row=model->find(id);if(!row)return false;auto value=row->value;value.dy+=dy;edits.push_back({Edit::Replace,id,std::move(value)});}
    sceneSink.worker=true;struct Reset{~Reset(){sceneSink.worker=false;}} reset;
    if(model->edit(model->revision(),edits)!=Status::Ok)return false;
    visibleRevision.store(model->revision());historyCharge.store(model->historyBytes());canonicalCharge.store(model->canonicalBytes());peak(queueHigh,saves.bytes());return true;
}
bool ipTestMiddleSplice(int64_t after,int64_t removedId,int variant) {
    using namespace document;
    if(!model||!suspended||!live.empty())return false;
    Brush brush;brush.radiusWorld=3.5f;
    brush.rgba=variant?std::array<float,4>{.1f,.8f,.2f,.5f}:std::array<float,4>{.95f,.8f,.1f,.65f};
    input::RawInputStroke samples={{{-500.,26.5},1,.8f,0,0},{{40.5,25.5},2,.9f,0,0},{{600.,26.5},3,.7f,0,0}};
    std::array<Edit,2> edits{{{Edit::Remove,removedId,{}},{Edit::InsertAfter,after,Value{std::make_shared<const Ink>(std::move(samples),brush)}}}};
    sceneSink.worker=true;struct Reset{~Reset(){sceneSink.worker=false;}} reset;
    const auto group=removedId?std::span<const Edit>(edits):std::span<const Edit>(edits).subspan(1);
    const auto result=model->edit(model->revision(),group);
    if(result!=Status::Ok)return false;
    visibleRevision.store(model->revision());historyCharge.store(model->historyBytes());canonicalCharge.store(model->canonicalBytes());peak(queueHigh,saves.bytes());
    return true;
}
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
    {std::lock_guard publish(publicationMutex);pendingScene=finished;
    ++finishedEpoch;}
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

    if(live.erase(id)) {retireLivePublication();model->cancelStroke();if(persistent) saves.cancelStroke();}
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
    retireLivePublication();
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
    desiredView.viewport(width,height);planner.request();

    glViewport(0, 0, width, height);
}

JNIEXPORT void JNICALL
Java_com_infinipaint_demo_NativeBridge_contextCreated(JNIEnv*, jclass) {
#ifdef __ANDROID__
    planner.start();
#endif
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
    Scene newCompleted;
    LiveView retiredView;
    bool submitDelta=false;
    const bool planning=planner.enabled.load(std::memory_order_acquire);
    const auto snapshotStartNs=profileNs();
    if(program && buffer) {
        std::unique_lock publish(publicationMutex,std::try_to_lock);
        publicationRetry=!publish.owns_lock();
        if(publish.owns_lock()) {
            const bool sceneChanged=renderLatch.epoch!=finishedEpoch || renderLatch.revision!=sceneRevision ||
                renderLatch.head!=sceneHead || !pendingScene.empty();
            const bool liveChanged=renderLatch.liveSequence!=livePublicationSequence;
            const bool full=planning && ((sceneChanged && !planner.deltas.canPush()) ||
                (liveChanged && renderLatch.active.id && !planner.retiredLive.canPush()));
            if(full) {
                // Keep the entire previous coherent scene/live latch. Camera
                // may still reproject that scene if it is the same document.
                publicationRetry=true;++planner.backpressure;
                if(renderLatch.epoch==finishedEpoch) renderLatch.view=desiredView.latch();
            } else {
                renderLatch.view=desiredView.latch();
                renderLatch.epoch=finishedEpoch;
                renderLatch.revision=sceneRevision;
                newCompleted.swap(pendingScene);
                renderLatch.head=sceneHead;renderLatch.doc=sceneDocument;
                if(liveChanged) {
                    retiredView=std::move(renderLatch.active);
                    renderLatch.active=publishedLive;
                    renderLatch.liveSequence=livePublicationSequence;
                }
                submitDelta=planning && sceneChanged;
            }
        } else publicationRetries.fetch_add(1,std::memory_order_relaxed);
    } else {
        publicationRetry=false;return;
    }
    if(planning && retiredView.id) planner.retire(std::move(retiredView));
    else retiredView={}; // fallback destruction outside the publication port
    const auto& frame=renderLatch;
    const auto frameHead=frame.head;
    const auto frameEpoch=frame.epoch;
    const double framePanX=frame.view.x, framePanY=frame.view.y, frameZoom=frame.view.zoom;
    ViewTrace active;
    const auto& liveView=frame.active;
    if(liveView.id) {
        active.bounds=liveView.bounds;active.lastX=liveView.lastX;active.lastY=liveView.lastY;
        active.filteredX=liveView.filteredX;active.filteredY=liveView.filteredY;
        active.hasLast=liveView.hasLast;active.hasFiltered=liveView.hasFiltered;
        active.radiusWorld=liveView.radiusWorld;active.filteredRadius=liveView.filteredRadius;
        if(mode==0 || mode==3) active.raw=snapshotUpload(growth,liveView.parts[0],liveView.id,0);
        if(mode==1 || mode==3) active.clean=snapshotUpload(growth,liveView.parts[1],liveView.id,1);
        if(mode==2 || mode==3) {
            active.stable=snapshotUpload(growth,liveView.parts[2],liveView.id,2);
            active.tail=snapshotUpload(growth,liveView.parts[3],liveView.id,3,liveView.tailRevision);
        }
    }
    auditSnapshotNs.fetch_add(profileNs()-snapshotStartNs,std::memory_order_relaxed);
    auditSnapshotFrames.fetch_add(1,std::memory_order_relaxed);
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
    }
    for (const auto& [id,patch] : newCompleted) cacheFinished(id,patch);
    for (const auto& [id,patch] : newCompleted) if(patch.shape) {
        auto& node=gpuFinished.at(id);
        node.following=node.next?&gpuFinished.at(node.next):nullptr;
    }
    if(submitDelta) planner.submit(newCompleted,frame);
    // Cull in the same quantized viewport used by the existing float shader.
    const double viewMinX = (float)framePanX, viewMinY = (float)framePanY;
    const double viewMaxX = viewMinX + width/(double)(float)frameZoom;
    const double viewMaxY = viewMinY + height/(double)(float)frameZoom;
    uint64_t visibleCompleted = 0;
    auto drawCompleted=[&](CachedGpuStroke& cached,const tablet::spans::Range* cut=nullptr) {
        ++visibleCompleted;
        auto drawPart = [&](int part, float r, float g, float b) {
            if(part==2) {r=cached.rgba[0];g=cached.rgba[1];b=cached.rgba[2];}
            coverageLayer({cached.minX,cached.minY,cached.maxX,cached.maxY},framePanX,framePanY,frameZoom,r,g,b,
                [&]{drawFinishedPart(cached,part,r,g,b,part==2?cut:nullptr);},cached.rgba[3]);
        };
        if (mode == 0) drawPart(0, 0.20f, 0.95f, 0.78f);
        else if (mode == 1) drawPart(1, 1.0f, 0.76f, 0.20f);
        else if (mode == 2) drawPart(2, 0.28f, 0.58f, 1.0f);
        else {
            drawPart(2, 0.28f, 0.58f, 1.0f);
            drawPart(1, 1.0f, 0.76f, 0.20f);
            drawPart(0, 0.20f, 0.95f, 0.78f);
        }
    };
    const auto* plan=planner.latch(frame,width,height);
    // Validate all IDs before drawing anything. A stale/malformed plan never
    // produces a partly drawn scene; exact resident v1 traversal is the oracle.
    if(plan && std::all_of(plan->ids.begin(),plan->ids.begin()+plan->count,
                          [](Key id){return gpuFinished.contains(id);})) {
        ++planner.hits;
        if(plan->indexed)++planner.indexedFrames;else ++planner.linearFrames;
        if(plan->stamp.view.sequence!=frame.view.sequence)++planner.envelopeFrames;
        planner.planIdsTested+=plan->count;
        size_t cutIndex=0;
        for(size_t i=0;i<plan->count;++i) {
            auto& cached=gpuFinished.at(plan->ids[i]);
            const tablet::spans::Range* cut=nullptr;
            if(cutIndex<plan->cutCount&&plan->cuts[cutIndex].index==i){const auto& c=plan->cuts[cutIndex++];if(c.geometry==cached.geometry)cut=&c.vertices;}
            if(cached.maxX<viewMinX || cached.minX>viewMaxX || cached.maxY<viewMinY || cached.minY>viewMaxY)continue;
            drawCompleted(cached,cut);
        }
    } else {
        if(plan) ++planner.fallbacks;
        uint64_t tested=0;
        for(auto* node=frameHead?&gpuFinished.at(frameHead):nullptr;node;node=node->following) {
            ++tested;auto& cached=*node;
            if(cached.maxX<viewMinX || cached.minX>viewMaxX || cached.maxY<viewMinY || cached.minY>viewMaxY) continue;
            drawCompleted(cached);
        }
        planner.residentFallbackRows+=tested;
    }

    // Retire live buffers after a stroke ends/cancels. GL thread owns deletion.
    for (auto it = gpuLive.begin(); it != gpuLive.end();) {
        if (it->first!=liveView.id) {
            for (auto& part : it->second) if (part.vbo) {
                gpuRequested.fetch_sub(part.capacityVertices*sizeof(Vertex));glDeleteBuffers(1,&part.vbo);
                liveVboCharge.fetch_sub(part.capacityVertices*sizeof(Vertex));
            }
            it = gpuLive.erase(it);
        } else ++it;
    }

    // Active strokes use the same mapping as completed strokes.
    if(liveView.id) {
        const uint64_t strokeId=liveView.id;
        auto& trace=active;
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
                            gpuFinished.size(), gpuFinished.size(), size_t(liveView.id!=0), (int)mode);
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

JNIEXPORT void JNICALL Java_com_infinipaint_demo_NativeBridge_configureSpatialCache(JNIEnv* env,jclass,jstring path) {
    if(!path)return;
    const char* chars=env->GetStringUTFChars(path,nullptr);
    if(chars){planner.configureCache(chars);env->ReleaseStringUTFChars(path,chars);}
}
JNIEXPORT jlongArray JNICALL Java_com_infinipaint_demo_NativeBridge_spatialCounters(JNIEnv* env,jclass) {
    const auto stats=spatialStats.snapshot();std::array<jlong,stats.size()+9> a{};
    std::copy(stats.begin(),stats.end(),a.begin());
    a[stats.size()]=planner.envelopeReuse.load();a[stats.size()+1]=planner.indexedPlans.load();
    a[stats.size()+2]=planner.linearPlans.load();a[stats.size()+3]=planner.linearTests.load();
    a[stats.size()+4]=planner.indexedFrames.load();a[stats.size()+5]=planner.linearFrames.load();
    a[stats.size()+6]=planner.envelopeFrames.load();a[stats.size()+7]=planner.planIdsTested.load();a[stats.size()+8]=planner.residentFallbackRows.load();
    auto arr=env->NewLongArray(a.size());if(arr)env->SetLongArrayRegion(arr,0,a.size(),a.data());return arr;
}
JNIEXPORT jlongArray JNICALL Java_com_infinipaint_demo_NativeBridge_spanStreamCounters(JNIEnv* env,jclass) {
    const auto stats=spanStats.snapshot();std::array<jlong,stats.size()> a{};std::copy(stats.begin(),stats.end(),a.begin());
    auto arr=env->NewLongArray(a.size());if(arr)env->SetLongArrayRegion(arr,0,a.size(),a.data());return arr;
}
JNIEXPORT jlongArray JNICALL Java_com_infinipaint_demo_NativeBridge_spatialCompactionCounters(JNIEnv* env,jclass) {
    const auto stats=spatialStats.compact.snapshot();std::array<jlong,stats.size()> a{};std::copy(stats.begin(),stats.end(),a.begin());
    auto arr=env->NewLongArray(a.size());if(arr)env->SetLongArrayRegion(arr,0,a.size(),a.data());return arr;
}
JNIEXPORT jlongArray JNICALL Java_com_infinipaint_demo_NativeBridge_paintOrderCounters(JNIEnv* env,jclass) {
    const auto stats=spatialStats.order.snapshot();std::array<jlong,stats.size()> a{};
    std::copy(stats.begin(),stats.end(),a.begin());
    auto arr=env->NewLongArray(a.size());if(arr)env->SetLongArrayRegion(arr,0,a.size(),a.data());return arr;
}
JNIEXPORT jlongArray JNICALL Java_com_infinipaint_demo_NativeBridge_spatialLatency(JNIEnv* env,jclass) {
    // Edges + histogram counts + exact max; cumulative successes only.
    constexpr auto edges=tablet::spatial::Stats::latencyEdges;
    std::array<jlong,edges.size()*2+2> a{};
    for(size_t i=0;i<edges.size();++i)a[i]=jlong(edges[i]);
    for(size_t i=0;i<spatialStats.latency.size();++i)a[edges.size()+i]=jlong(spatialStats.latency[i].load());
    a.back()=jlong(spatialStats.successfulMaxNs.load());
    auto arr=env->NewLongArray(a.size());if(arr)env->SetLongArrayRegion(arr,0,a.size(),a.data());return arr;
}
JNIEXPORT jlongArray JNICALL Java_com_infinipaint_demo_NativeBridge_plannerCounters(JNIEnv* env,jclass) {
    std::array<jlong,9> a{jlong(planner.enabled.load()),jlong(planner.hits.load()),jlong(planner.fallbacks.load()),
        jlong(planner.built.load()),jlong(planner.cancelled.load()),jlong(planner.invalid.load()),
        jlong(planner.backpressure.load()),jlong(planner.retired.load()),jlong(plannerFixedCharge.load())};
    auto arr=env->NewLongArray(a.size());if(arr) env->SetLongArrayRegion(arr,0,a.size(),a.data());return arr;
}

JNIEXPORT jboolean JNICALL Java_com_infinipaint_demo_NativeBridge_needsPublicationRetry(JNIEnv*,jclass) {
    return publicationRetry?JNI_TRUE:JNI_FALSE;
}

} // extern "C"

namespace engine::tablet {
#ifndef __ANDROID__
// Host-only probe: failures may not mutate the caller's output or leak the
// completed geometry charge. No scene, GL or planner is involved.
std::array<bool,3> hostCanonicalReplayProbe(const std::shared_ptr<const document::Ink>& ink,size_t allowance,size_t scratch,double dx) {
    const auto before=retainedMeshes.load(),owners=sceneOwners.load();
    bool ok=false,untouched=false;
    {
        FinishedTrace out;out.minX=1234.f;
        ok=reconstructV1(ink,out,dx,0,allowance,scratch);
        untouched=ok?(out.record==ink&&out.raw&&out.clean&&out.spline&&retainedMeshes.load()>before):
            (!out.record&&!out.raw&&!out.clean&&!out.spline&&out.minX==1234.f&&retainedMeshes.load()==before);
    }
    return {ok,untouched,retainedMeshes.load()==before&&sceneOwners.load()==owners};
}
std::array<uint64_t,2> hostOwnerCharges(){return {sceneOwners.load(),liveVboCharge.load()};}
const void* hostFirstRetiringMesh(){return gpuFinished.begin()->second.source[2]->data();}
void hostLockMutation(){stateMutex.lock();}
void hostUnlockMutation(){stateMutex.unlock();}
void hostLockPublication(){publicationMutex.lock();}
void hostUnlockPublication(){publicationMutex.unlock();}
uint64_t hostLivePublicationBytes(){return livePublicationBytes.load();}
void hostStartPlanner(){planner.start();}
void hostForcePlannerFallback(bool force){planner.testForceFallback=force;}
std::array<uint64_t,8> hostPlannerStats(){return {planner.enabled.load(),planner.hits.load(),planner.fallbacks.load(),planner.built.load(),planner.cancelled.load(),planner.invalid.load(),planner.backpressure.load(),planner.retired.load()};}
std::array<uint64_t,39> hostSpatialStats(){return spatialStats.snapshot();}
std::array<uint64_t,32> hostPaintOrderStats(){return spatialStats.order.snapshot();}
std::array<uint64_t,34> hostSpanStats(){return spanStats.snapshot();}
std::array<uint64_t,15> hostCompactionStats(){return spatialStats.compact.snapshot();}
std::array<uint64_t,5> hostSpatialFrameStats(){return {planner.indexedFrames.load(),planner.linearFrames.load(),planner.envelopeFrames.load(),planner.planIdsTested.load(),planner.residentFallbackRows.load()};}
void hostSpatialFault(int fault){planner.testSpatialFault.store(fault);planner.request();}
void hostParkPlanner(){planner.testPark.store(true);planner.request();}
bool hostPlannerParked(){return planner.testParked.load();}
void hostResumePlanner(){planner.testPark.store(false);planner.testRelease.release();}

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
        replacement.emplace(id,Published{r.next,r.geometry,std::move(shape),bytes,r.value.rgba(),r.prev});
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
        std::lock_guard publish(publicationMutex);
        model.swap(stagedModel);
        finished.swap(stagedScene);
        retired.swap(pendingScene);
        pendingScene.swap(stagedPublication);
        sceneHead=model->first();
        sceneRevision=model->revision();
        sceneBytes=stagedBytes;sceneLimit=stagedLimit;canonicalLimit=stagedCanonicalLimit;
        desiredView.publish(x,y,zoom);planner.request();
        ++finishedEpoch; persistent=true;sceneDocument=model->identity();setDurable(finishedEpoch,model->revision());
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
    std::lock_guard guard(stateMutex);const bool ok=saves.acknowledge(token);
    if(ok&&model&&token->document==model->identity())setDurable(finishedEpoch,token->base+1);
    return ok;
}
}
