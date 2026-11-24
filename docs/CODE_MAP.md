# Source map — accepted Java/JNI/C++ GLES2 diagnostic

|Subsystem|Start here|Verification|
|---|---|---|
|Android shell, lifecycle, input routing|`app/src/main/AndroidManifest.xml`; `app/src/main/java/com/infinipaint/demo/MainActivity.java`; `NativeBridge.java` in the same package|M01 tablet checklist in [report](../reports/LATEST.md)|
|Camera and pointer-ID navigation|`demo/CanvasCamera.java`, `demo/FingerNavigation.java` under `app/src/main/java/com/infinipaint/`|Matching JVM suites under `app/src/test/java/com/infinipaint/demo/`|
|Input timestamps, filter, spline|`MainActivity.sendStylusSample`, `demo/InputAuditRecorder.java`; `app/src/main/cpp/input/{input_processor,one_euro_filter}.cpp`, `stroke/spline.cpp`|`diagnostics/replay.cpp`, two synthetic CSVs and [diagnostic recipes](../diagnostics/README.md)|
|Viewport, GL ownership, live/completed meshes and culling|`app/src/main/cpp/native_bridge.cpp`: `contextCreated`, `resizeViewport`, `render`; toolchain declarations in `app/build.gradle`, `app/src/main/cpp/CMakeLists.txt`|`diagnostics/renderer_smoke.cpp` (mock calls, not pixels); physical tablet for driver/context acceptance|
|Frame requests and callback accounting|`demo/FrameTelemetry.java`, `MainActivity.dump`|`FrameTelemetryTest.java`, [frame-capture commands](M01_FRAME_CAPTURE.md)|
|Build/CI and engineering workflow|Gradle wrapper, `.github/workflows/android.yml`, `scripts/engineering.ps1`, `opencode.json`, `.opencode/agents/`|[Workflow commands and activation](W01_WORKFLOW.md)|

There is no durable document store or production spatial index in this baseline. See [decisions](DECISIONS.md) before changing architectural boundaries.

M02: `native_bridge.cpp` owns sample preflight, capture-zoom tessellation, tapered capsule shaders and completed geometry publication. `stroke/spline.{cpp,hpp}` holds the ordered attribute history, cached Hermite coefficients and interpolation-error bound. `FrameTelemetry` exposes recent timing distributions only on dump. Start validation at `diagnostics/run_m02.sh` (input/faults/scaling, mock bridge and real Mesa pbuffer); see [tablet gate](M02_TABLET.md).
