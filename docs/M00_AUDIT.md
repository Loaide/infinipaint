# M00 — architecture and repository audit, 2026-11-23

**Recommendation: retain these bytes as a diagnostic reference, not an operational product baseline.** The host fixtures, native engine and Gradle build are real. The unmodified Android app fails Java compilation. The workspace has no Git identity. Neither a working APK nor a renderer decision follows from the archive name “CORRECTED”.

## Identity and scope

- Audited `D:\DOWNLAODS\hello\InfiniPaint` on Windows. `git status -sb` and `git rev-parse HEAD`: exit 1, not a Git repository. **Branch: none; full local commit SHA: none.** No task branch possible. No initialization, production edits, push or merge.
- `git ls-remote --symref https://github.com/Loaide/infinipaint.git` returned no advertised refs (exit 0). No current remote commit/tree available from that query to compare. The previously reported `main` / `1b1f5ac337940ea3d2068c0cdc2a9a87836dc188` is not this snapshot's SHA; no desktop code was inferred from commit messages.
- Inspected `AGENTS.md`, `prompts/M00_FIRST_PROMPT.md`, `docs/{ENGINEERING_CONTRACT,CURRENT_STATE,DECISIONS,QUALITY_GATES,PRODUCT_SPEC,ROADMAP,DEVICE_TEST_CHECKLIST,PERFORMANCE_BUDGET}.md`, `reports/LATEST.md`, starter and historical READMEs, both ZIPs, extracted legacy decisions, build files and code below. Scope was inspection, isolated builds, tests and documentation only.
- Independent ZIP member comparison by decompressed bytes (both CRC checks passed): `references/InfiniPaint_90Hz_Tablet_Test_CORRECTED.zip`, SHA-256 `a253b364347d3e08d8aa0435e53062aada57d8688643fea51c160a47898ae61e`: 37/46 files identical including app, wrapper, diagnostics and fixtures; eight IDE files absent, `.gitignore` changed. Workflow scaffolding is additional. `references/InfinitePaper_Documentation_v0.1.0.zip`, SHA-256 `9845fab49e2161c16eb33499a2fee34f097d5bd719cd761e86dc21f7e30c0938`: 9/9 identical under `references/legacy-documentation`, no extras. Documentation archive contains an app placeholder, not Kotlin product code. The earlier RAR is unavailable. ZIPs are ignored by Git; preserve their hashes/access separately.

## Implementation inventory and architecture findings

Paths below `app/src/main/` unless noted. All code is in one diagnostic `:app`; these are responsibilities, not independently deployable modules.

|Files inspected|Actual role and boundary|
|---|---|
|`AndroidManifest.xml`, `java/com/infinipaint/demo/MainActivity.java`|One exported launcher Activity, programmatic controls, GLES2 `GLSurfaceView`, render-on-demand, refresh request and pause/resume handling. Input, gestures, viewport, profiles and UI are interleaved. Requesting 90 Hz is not proving refresh or pacing. No notebook/workspace shell.|
|`java/com/infinipaint/input/MotionEventAdapter.java`, `demo/NativeBridge.java` (under `java/com/infinipaint`)|Separate adapter exists, but live dispatch uses `MainActivity.handleTouch` → JNI. MOVE processes history by pointer identity; UP only current tip. Pointer transitions/canceled UP need explicit regression tests. API 34+ event nanoseconds vs millisecond fallback; no invented interval for equal times.|
|`java/com/infinipaint/demo/InputAuditRecorder.java`|Optional CSV export of sample data and outcomes; pause/long-press export is not document persistence. Per-export row cap does not bound queued batches; CSV construction competes with input on UI thread.|
|`cpp/core/{result.hpp,math/vec2.hpp}`, `cpp/input/{types.hpp,one_euro_filter.hpp,.cpp,input_processor.hpp,.cpp}`|Result/error and vector helpers; raw/filtered histories, validation, One-Euro position/velocity, pressure and tilt. Equal time holds filtered position and updates raw predecessor; native bridge omits equal-time spline knot, decreasing time cancels stroke. Actual filtering uses world coordinates; zoom-1 replay is not evidence of screen-invariant smoothing.|
|`cpp/stroke/{types.hpp,spline.hpp,spline.cpp}`|Incremental centripetal Catmull-Rom fit in Hermite form; local origin, stable prefix and provisional end. At most four knots refitted, but `reserve(size()+1)` in `IncrementalSplineBuilder::append` can repeatedly copy growing arrays. Knot interpolation tests alone cannot establish between-knot fidelity.|
|`cpp/native_bridge.cpp`|JNI ingestion, camera, stroke lifetime, CPU tessellation/bounds, embedded shaders, VBO caches, cull/draw and counters in one translation unit. UI and GL snapshots share `stateMutex`. Finishing copies history, assembles spline mesh and computes bounds under the lock. Retains original input plus multiple display meshes; not a canonical editable document.|
|`cpp/CMakeLists.txt`, root/app Gradle, wrapper and settings|Four native translation units; one Java app, not Kotlin. See [versions, provenance and license review](DEPENDENCY_MATRIX.md). No Android unit/instrumentation test source sets or CI workflow found.|
|`diagnostics/{replay.cpp,run_replay.sh,renderer_smoke.cpp,run_renderer_smoke.sh,README.md,synthetic_input.csv,synthetic_same_timestamp.csv,test_support/GLES2/gl2.h,test_support/android/log.h}`; root `replay_points.csv`|Standalone real filter/spline replay and mock-GL JNI smoke. Root output CSV has header only. Replay sorts input, groups by pointer ID (not stroke lifetime), omits rejected rows, uses its own preset and can report failures with success exit. Sample-time spline evaluation measures stored knots rather than full curve. Mock checks uploads/calls, not pixels.|

**Rendering and scale.** The active path appends stable geometry and replaces the tail; growth or context recreation can require full uploads. Completed meshes upload lazily per mode. Viewport rejection reduces draws, not traversal: `render` walks every `gpuFinished` entry on each rendered frame. Completion-order traversal is not an order-preserving spatial index. CPU camera doubles do not repair `Vertex` world floats or subtraction in the shader; zoom clamped to 0.1–16. No demonstrated extreme zoom, production AA, memory budget, cache eviction, streaming, LOD, save/recovery, stable document IDs, undo/redo or million-stroke acceptance.

**Observability.** `IP_PIPELINE`, `IP90_PROFILE`, `IP_GPU_INCREMENTAL`, `IP_VIEW_CULL` and Java `IP_FRAME_TIMING` address different stages. Callback time is not presentation. Latest-request/sample association, rejection of large intervals and averages hide stalls. No FrameTimeline, physical latency, thermal, idle-power or memory evidence. See [risk register](RISK_REGISTER.md) for verification priorities.

## Android verification — executed

To avoid generating project files in the audit tree, copied `app`, `gradle`, `gradlew`, `gradlew.bat`, `settings.gradle`, `build.gradle` to `%LOCALAPPDATA%\Temp\opencode\m00-gradle-c70e4784-261e-4a83-9be6-b82562cfb55b`. In that directory, PowerShell:

```powershell
$env:JAVA_HOME='C:\Program Files\Java\jdk-17'
$env:ANDROID_HOME="$env:LOCALAPPDATA\Android\Sdk"
.\gradlew.bat --offline --console=plain :app:assembleDebug :app:testDebugUnitTest :app:lintDebug :app:assembleDebugAndroidTest
```

Gradle 8.7 / JDK 17.0.8 / SDK 35 / pinned NDK installed. Exit 1: `:app:compileDebugJavaWithJavac FAILED`, 12 `cannot find symbol` diagnostics at `MainActivity.java:614–624` for `renderWindowQueueNs`, `renderWindowQueueSamples`, `renderWindowNativeNs`, `renderWindowCount`. 22 tasks executed; manifest/resource processing succeeded. CMake configuration warned about SDK XML version 4 vs supported 3. **No APK; unit test, androidTest packaging and lint not verified.** No repair attempted. Captured output: `m00-build.log` in that scratch directory.

Separate `.\gradlew.bat --offline --console=plain :app:externalNativeBuildDebug` in the same isolated copy: **BUILD SUCCESSFUL**, nine tasks, 1m 12s, CMake build for arm64-v8a, armeabi-v7a, x86 and x86_64 (`m00-native.log`). This verifies Android native compilation/linking, not JNI invocation by an installed app.

`adb devices -l` returned an empty device list. No install, instrumented test, real GL shader compile, tablet launch or physical performance measurement. Reference device is the Galaxy Tab S10 FE+ per historical ADR-007, not the test host.

## Host verification — executed independently of Gradle

Ubuntu 26.04 LTS under WSL2, x86_64, GCC 15.2.0. Replay runner normally writes its executable into the source tree; used equivalent commands with scratch output. The stock `wsl -- sh diagnostics/run_renderer_smoke.sh` initially failed on missing `jni.h`. QA reran the same smoke sources and strict warning flags with the installed NDK's standalone JNI declarations, not a JVM:

```bash
T=/tmp/ip-m00-host.dGAxFB  # created with mktemp -d /tmp/ip-m00-host.XXXXXX
C=app/src/main/cpp
c++ -std=c++20 -O2 -I"$C" diagnostics/replay.cpp "$C/input/input_processor.cpp" \
  "$C/input/one_euro_filter.cpp" "$C/stroke/spline.cpp" -o "$T/replay"
for f in synthetic_input synthetic_same_timestamp; do
  "$T/replay" "diagnostics/$f.csv" "$T/$f.csv"
done
cp /mnt/c/Users/lenovo/AppData/Local/Android/Sdk/ndk/30.0.16248370/toolchains/llvm/prebuilt/windows-x86_64/sysroot/usr/include/jni.h "$T/jni.h"
c++ -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror -Idiagnostics/test_support \
  -I "$T" -I"$C" diagnostics/renderer_smoke.cpp "$C/native_bridge.cpp" \
  "$C/input/one_euro_filter.cpp" "$C/input/input_processor.cpp" "$C/stroke/spline.cpp" -o "$T/smoke"
"$T/smoke"
```

Normal replay: **120 accepted, 0 failures**; all 120 timestamps preserved in order. Equal-time replay: **70 accepted, 0 failures**, all 35 adjacent ties preserved. QA checked numeric CSV output was finite. Renderer smoke: **PASS**, including 2,112 mock transfer bytes for ten additional raw samples with capacity available. Checks include mode/preset differences, lazy uploads, culling, zoom reuse, simulated surface rebuild and clear. None validate driver compilation, actual context loss, rasterization or runtime Android scheduling.

## Gate

See [current state](CURRENT_STATE.md), [decision reconciliation](DECISIONS.md), [architecture proposal](ARCHITECTURE_PROPOSAL.md) and [ordered backlog](MILESTONE_BACKLOG.md). Accept the audit separately from approving a code baseline. The source-identity and operational-baseline gates remain open.
