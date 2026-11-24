# M01 — application foundation

Branch `feature/m01-app-foundation`, based on the verified local and remote `main` at `db36eaba697ef46beb14f77e396405d7af20cf65`. [Session handoff](../docs/SESSION_HANDOFF.md) identifies the application checkpoint and owner gate.

## What prompted the changes

- The M00.5 Gradle test task had no test sources. The touch listener owned camera and pointer state, which made JVM regression testing difficult.
- The activity called `GLSurfaceView.onPause/onResume` and cancelled active input on pause, but recreated activities started with the default camera, display mode and smoothing preset. The audit exporter also had a `close()` method which activity destruction never called.
- A surface-size change called the same native initialization path as context creation. Rebuilding GPU names is necessary for a new context, not for a resized viewport in the same context.
- Independent graphics review found that a second instance of the activity could create a second GL context while sharing native cache bookkeeping with the first. The manifest now enforces a single instance of this diagnostic canvas, including across tasks. Supporting independently resumed canvases would require per-renderer native ownership; it is not an M01 feature.
- `RENDERMODE_WHEN_DIRTY` and one request per stylus MOVE batch were already present. There was no reason to replace the scheduler. The old 120-callback timing window read a last-request timestamp without consuming it; it could reuse the timestamp for a different callback and could not report a quiet interval.

## Verification of application commit `ac4310a6c4cf1c6eb0e864bbd84dc48c5654cb78`

|Check|Executed result|
|---|---|
|Android debug, native, tests, lint|Windows, JDK 17, Gradle wrapper 8.7, AGP 8.6.1, SDK 35/build tools 34.0.0, NDK 30.0.16248370, CMake 3.22.1. `.\gradlew.bat --offline --console=plain clean :app:assembleDebug :app:testDebugUnitTest :app:externalNativeBuildDebug :app:lintDebug` succeeded (61 tasks executed); final manifest-only fix verified by repeating the same tasks without `clean`. APK contains `libip_tablet.so` for arm64-v8a, armeabi-v7a, x86 and x86_64. `aapt dump xmltree` confirms launch mode `0x3` (singleInstance).|
|JVM tests|JUnit XML in `app/build/test-results/testDebugUnitTest`: CanvasCamera 3, FingerNavigation 5, FrameTelemetry 2; **10 executed, 0 failed, 0 errors, 0 skipped**. Tests drive the production camera/pointer state using synthetic event coordinates and the production counters using a controlled clock. They do not dispatch Android MotionEvents or drive a real surface.|
|Lint|`app/build/reports/lint-results-debug.xml`: 0 errors, 8 warnings (3 SetTextI18n, 2 ClickableViewAccessibility, 1 each DiscouragedApi, ObsoleteSdkInt and MissingApplicationIcon). M00.5 reported seven; the additional finding concerns the diagnostic app's absent launcher icon. No suppression added.|
|Host input|WSL C++20 [host build recipe](../docs/M00_AUDIT.md), unchanged input/filter/spline sources: 120/120 and 70/70 accepted, 0 failures. All 190 output timestamps matched fixture order exactly, 35 adjacent ties retained, all numeric outputs finite. A first combined shell invocation passed smoke but hit a line-ending error before running replay; corrected invocation ran both fixtures.|
|Mock GLES|Strict host compile (`-Wall -Wextra -Wpedantic -Werror`, pinned NDK `jni.h`); smoke passed, including same-context resize without GL name churn or upload, fresh-context recovery of active and completed geometry, Clear, culling, and nonidentity camera mapping through real raw vertices and projection uniforms. Later live append transferred 2,112 bytes on this fixture. No shader pixels or driver timings measured.|
|CI and diff|Workflow YAML parsed locally; executed its XML count guard against the local results (10). Local wrapper tasks passed. No GitHub Actions run or cold Ubuntu provisioning was performed. `git diff --check` passed.|

**APK:** `D:\DOWNLAODS\hello\InfiniPaint\app\build\outputs\apk\debug\app-debug.apk`, SHA-256 `319971d5c233ae1e597c3834c6d18379d15cda7c53a3e7a7f92630a0550acad8`. The owner reported the same SHA-256 for the tested APK.

**Review and files:** the focused graphics reviewer approved the single-instance fix on source review. The application commit contains the complete file manifest (`git show --stat ac4310a`): workflow, Gradle, manifest, MainActivity, NativeBridge, CanvasCamera, FingerNavigation, FrameTelemetry, their three JVM suites, native bridge and renderer smoke. Inspection also covered the frame-capture procedure, audit exporter, CMake, diagnostic scripts, engineering contract, state/handoff, roadmap, backlog and decision register. The [accepted M00.5 report](https://github.com/Loaide/infinipaint/blob/8bca6fc0be92f8f3fd0f8ad169d80c48e7442914/reports/LATEST.md) is historical, not evidence for this APK.

**Idle evidence:** no continuous scheduler found. An unchanged finger MOVE now requests no frame. In the synthetic telemetry test, a 30-second clock advance produced a zero callback delta; one injected unrequested callback then incremented both the callback and `withoutPending` deltas by one. That verifies accounting, not device idle behavior. No M01 FPS, presentation latency or power measurement is claimed.

## Owner acceptance — all 17 checks passed; M01 complete

Device: SM-X626B, fingerprint `samsung/gts10fepxx/gts10fep:16/BP2A.250605.031.A3/X626BXXS7BZB7_OJM7BZB7:user/release-keys`. Refresh-rate setting was not recorded.

The first five checks (debug build, four native ABIs, 10 JVM tests, host replay and mock GLES) have independent local evidence above. No code was rebuilt at closure. The following interval deltas were supplied by the owner:

|Interval|Duration (ms)|Camera requests|Stylus requests|Other requests|Draw callbacks|Without pending|
|---|---:|---:|---:|---:|---:|---:|
|Initial idle|38715|0|0|0|0|0|
|Camera|35017|2046|0|0|2046|0|
|Stylus|43786|0|2590|1|2591|0|
|Final idle|40332|0|0|0|0|0|

The two idle windows total **79.047 seconds with zero requests and zero draw callbacks**. Mean request-to-callback time was approximately 0.649 ms in the camera interval and 0.444 ms in the stylus interval (which included one other request). Mean native render-call durations were approximately 0.182 ms and 0.382 ms respectively; maximum observed native call was 11.12004 ms. These are callback and native-call measurements, not display FPS, GPU completion time or physical stylus latency.

Separately, the owner reports correct drawing and smoothing, smooth pan/pinch without jumps, and correct palm rejection and pointer transitions. Rotation retained ink, camera and settings; split-screen resize, background/foreground and lock/unlock passed. Activity recreation retained completed ink, and the owner verified actual graphics-context recreation. Those lifecycle results are owner observations, not conclusions inferred from the counters or independent device captures by engineering.

## Tablet test procedure (reference)

1. Record APK hash, app SHA, tablet model, OS build and refresh mode. Updating the app can discard in-memory ink. When ready, install with `adb install -r app/build/outputs/apk/debug/app-debug.apk`.
2. Draw a landmark, choose nondefault display/smoothing modes, pan and pinch. Check the landmark stays under the moving focal point, the surviving finger does not jump, and fingers work again after cancellation. Verify stylus drawing and historical samples, preview, pen-up, filter changes and Clear.
3. Rotate twice and resize in split screen. Check the same world coordinate at the viewport origin, scale, controls, ink order and ability to draw. The policy preserves the camera origin, not the viewport center.
4. Background/foreground, lock/unlock, interrupt an active contact, then draw and navigate again. While the app is visible, try `adb shell am start -f 0x18000000 -n com.infinipaint.demo/.MainActivity`: the existing canvas should be brought forward, not duplicated. With **Don't keep activities** temporarily enabled, repeat the background/return test without killing the process. Restore the setting. Check no stuck pointer, blank canvas, missing completed strokes or GL errors. Look for a fresh `IP90: Renderer initialized with camera support` in `adb logcat -s IP90:I` in the same process before attributing recovery to context recreation.
5. Follow the [frame capture commands](../docs/M01_FRAME_CAPTURE.md) across a settled 30-second hands-off interval, then across separate camera and stylus intervals. Record requested and actual frames; requests can coalesce, and lifecycle/resize can draw without an app request. Report device results separately from JVM arithmetic and mock GLES. Neither callback timing nor a requested display mode measures presentation or physical stylus latency.
6. Process death is a separate case: transient view settings can be restored when Android restores instance state, but the native drawing is **not a saved document**. Do not test with irreplaceable work. No M01 claim of persistence, extreme-zoom rendering or million-stroke capacity.
