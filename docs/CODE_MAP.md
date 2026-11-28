## M04 direct span cache and local compaction candidate

`document/source_digest.hpp`: fixed-state source/mesh identity. `tablet/span_store.hpp`: explicit checked span packets and ordinal/spatial page reads; `tablet/span_cache.hpp`: bounded dirty/durable disposable cache, promotion after acknowledgement. `native_bridge.cpp::FramePlanner::refineSpans`: worker demand and compatible exact vertex ranges. `tablet/spatial_pages.hpp`: local overlay compaction and bounded write-back. `MainActivity.dump`: span and compaction telemetry. `diagnostics/run_m04_span_store.sh`, `run_m04_spatial.sh stress`, `egl_ink_test.cpp`: disk/fault/visibility/refined RGBA oracles. [Report and limitations](../reports/M04_SPAN_STREAMING.md).

## M04 canonical span streaming — first slice

`stroke/spline.*`: rolling exact v1 spline. `document/canonical_stream.hpp`: fixed-state replay; `document/canonical_range.hpp`: bounded CPS1 interval packets and conservative v1 bounds. `native_bridge.cpp::reconstructV1`: production mesh reconstruction. `storage/DocumentDatabase.java`: shared record validation and targeted durable read; `storage/StorePort.java` / `tablet/store_jni.cpp`: bounded native lease. `MainActivity.dump`: canonical telemetry. `diagnostics/run_m04_canonical_stream.sh stress` and `storage/CanonicalRangeTest.java`: host and Android verification. [Report](../reports/M04_CANONICAL_STREAM.md). Range reader not yet connected to viewport planner.

## M04 production spatial access (first slice)

`tablet/spatial_pages.hpp`: checked ephemeral packed pages, bounded page/rank caches, edit BVH, sparse ordered query and charges. `tablet/counted_order.hpp`: counted sequence, ID locator, split/borrow/merge/reclaim, prefix rank. `tablet/spatial_delta.hpp`: validates bounded arbitrary membership splices before mutations; rebuild on unsupported survivor reorder, overlay capacity or incompatible base. `native_bridge.cpp` FramePlanner: paired roots per resident scene, compatible candidate envelope and exact fallback, telemetry. `MainActivity.dump`: spatial counters/histogram. `diagnostics/run_m04_spatial.sh stress` and `egl_ink_test.cpp`: ordered oracle, faults and seam/pixel tests. [Scope and constraints](../reports/M04_SPATIAL_ACCESS.md).

# Source map — accepted Java/JNI/C++ GLES2 diagnostic

|Subsystem|Start here|Verification|
|---|---|---|
|Android shell, lifecycle, input routing|`app/src/main/AndroidManifest.xml`; `app/src/main/java/com/infinipaint/demo/MainActivity.java`; `NativeBridge.java` in the same package|M01 tablet checklist in [report](../reports/LATEST.md)|
|Camera and pointer-ID navigation|`demo/CanvasCamera.java`, `demo/FingerNavigation.java` under `app/src/main/java/com/infinipaint/`|Matching JVM suites under `app/src/test/java/com/infinipaint/demo/`|
|Input timestamps, filter, spline|`MainActivity.sendStylusSample`, `demo/InputAuditRecorder.java`; `app/src/main/cpp/input/{input_processor,one_euro_filter}.cpp`, `stroke/spline.cpp`|`diagnostics/replay.cpp`, two synthetic CSVs and [diagnostic recipes](../diagnostics/README.md)|
|Viewport, GL ownership, live/completed meshes and culling|`app/src/main/cpp/native_bridge.cpp`: `contextCreated`, `resizeViewport`, `render`; toolchain declarations in `app/build.gradle`, `app/src/main/cpp/CMakeLists.txt`|`diagnostics/renderer_smoke.cpp` (mock calls, not pixels); physical tablet for driver/context acceptance|
|Frame requests and callback accounting|`demo/FrameTelemetry.java`, `MainActivity.dump`|`FrameTelemetryTest.java`, [frame-capture commands](M01_FRAME_CAPTURE.md)|
|Build/CI and engineering workflow|Gradle wrapper, `.github/workflows/android.yml`, `scripts/engineering.ps1`, `opencode.json`, `.opencode/agents/`|[Workflow commands and activation](W01_WORKFLOW.md)|

The M02 baseline had no durable document store. M03 adds the store below; a production spatial index remains M04 work.

M02: `native_bridge.cpp` owns sample preflight, capture-zoom tessellation, tapered capsule shaders and completed geometry publication. `stroke/spline.{cpp,hpp}` holds the ordered attribute history, cached Hermite coefficients and interpolation-error bound. `FrameTelemetry` exposes recent timing distributions only on dump. Start validation at `diagnostics/run_m02.sh` (input/faults/scaling, mock bridge and real Mesa pbuffer); see [tablet gate](M02_TABLET.md).

Hardening: `coverageLayer` gathers one representation, accumulates into scissored window alpha and resolves in temporal order; `cacheFinished` conditionally adopts successful live VBO prefixes. `SplineFitter::coefficients` is shared by batch fitting and the fixed four-knot append workspace. `diagnostics/tessellation_probe.hpp` reports cap hits and interpolation error.
M03 (in progress): `app/src/main/cpp/document/` contains linked order/ID ownership,
reversible edit groups, save delta queue and v1 header/sample codec. `native_bridge.cpp`
contains `appendV1`, `finishV1` and `reconstructV1` (no GL calls in reconstruction).
`storage/DocumentDatabase.java` under the Java package is the worker-confined SQLite
adapter. Host core oracle: `diagnostics/run_m03.sh`; matched reconstruction pixels:
`diagnostics/run_m02.sh pixels`. Android storage fixtures are under `app/src/androidTest/`.

`tablet/{session.hpp,store_jni.cpp}` and `storage/{EditorStore,StorePort}.java` implement the staged switch and bounded worker/codec port. `document/{eraser,working_set}.hpp` implement swept ink coverage and aggregate admission. See the [v1 contract](M03_CONTRACT.md), [isolated tablet procedure](M03_TABLET.md), `diagnostics/run_m03_latency.sh` and `diagnostics/compare_m03.sh`. Only `src/m03Verify` has the test-process recovery service.

M04.3-C functionally verified: `demo/InkPipeline.java` and `InkLane.java` own ordered multi-gesture admission and explicit blocked-head resolution; `tablet/frame_scheduler.hpp` owns bounded transfer and latest plan primitives; `native_bridge.cpp` owns resident planner, stamped visibility and CPU retirement. See [report](../reports/M04_3_EXECUTION_SCHEDULING.md).

M04.3-B candidate: `tablet/live_prefix.hpp` owns append-only published page handles and immutable directories; `native_bridge.cpp` owns the publication port, live mirror/tail and GL latch. `diagnostics/run_renderer_smoke.sh` runs mock bridge, owner-credit, parked-lock and live-prefix tests; real EGL regression includes paged context rebuild. See [report](../reports/M04_3_RENDER_PUBLICATION.md).
