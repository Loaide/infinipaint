# InfiniPaint 90 Hz Tablet Test — renderer/input corrective build

**Scope:** Standalone Android Studio *test application*, built from the user's uploaded `InfiniPaint_90Hz_Tablet_Test(1).rar`. This is not the production renderer, persistence engine, spatial index, or the complete InfiniPaint app. There were **no changes to the GitHub repository**.

## Open and build

1. Extract the ZIP and **open the `InfiniPaint_90Hz_Tablet_Test` folder** in Android Studio (it contains `settings.gradle` and the Gradle wrapper).
2. Sync/build/install on your tablet. Java 17+, Android SDK 35, Gradle 8.7, Android Gradle Plugin 8.6.1, CMake 3.22.1, and NDK 30.0.16248370 as configured in the project are required. Android Studio may offer to install missing SDK components.
3. The personal `local.properties`, previous APK, `.gradle`, `.cxx`, and Gradle build caches have deliberately **not** been packaged: generated Windows paths and stale binaries would conflict with a clean build. Android Studio will recreate them.
4. Stylus draws; fingers pan/pinch. Use **Mode** (Raw / One-Euro / Spline / Compare), **Smooth** (Light / Medium / Strong; next stroke), and **Clear**. Long press Clear to export raw input CSV without clearing.

## What this fixes

- Native GL no longer copies entire active-stroke meshes for every render call. It snapshots only the not-yet-uploaded vertex range, with geometrically growing VBO capacity.
- Spline stable tessellation no longer calls the O(stroke length) `snapshot()` on every sample. Cached coefficients are evaluated in O(1) per newly stable segment. A small provisional tail is replaced while the pen is moving.
- The fragment workload no longer expands each stroke to the **maximum possible min-zoom padding** on the CPU. Vertex shader expands a compact strip at the *current* zoom, so magnifying strokes does not magnify padding needlessly.
- Completed GPU meshes are acquired only on first appearance, are uploaded **lazily by visual mode**, and are culled against the camera. CPU frame snapshots no longer copy all completed stroke metadata on every frame.
- Rendering is **event-driven** rather than constantly re-requested at 90 Hz while idle; one request per Android movement event rather than one for each historical sample.
- One-Euro parameters are now three distinct, user-selectable presets. The active mode never includes a hidden raw-stroke underlay. Spline mode draws the provisional curve rather than only previously stabilized segments. The latest physical pen tip is drawn immediately while a filtered curve is still catching up.
- All same-timestamp stylus samples remain in raw/filtered data; spline knots do not receive fabricated times. The final raw endpoint is retained at stroke completion even if repeated timestamp samples arrived.
- Input audit avoids per-sample `String.format`, and render timing statistics omit idle gaps when computing frame intervals.

## Testing status

**Passed here:** Host C++ full native-bridge smoke test using a GLES mock (incremental uploads, actual differences in generated raw/filtered/spline and preset geometry, visual-mode VBO caching, camera culling/zoom, same timestamp, clear, GL surface recreation); standalone real C++ input/spline replays for both synthetic traces; C++ bridge compilation with strict warnings. Run `bash diagnostics/run_renderer_smoke.sh` and `bash diagnostics/run_replay.sh diagnostics/synthetic_input.csv /tmp/ip_replay.csv` in a Linux environment with a C++20 compiler and JDK headers.

**Not validated here:** Android Gradle build, device GPU shader compilation, actual 90 Hz frame presentation, and measured stylus-to-photon latency (no Android SDK/NDK, GLES hardware, or tablet connected in this environment). Host GLES mocks do **not** prove device compatibility or smoothness. Test the supplied source on-device before calling the issues resolved.

### Quick acceptance check on the tablet

1. Draw three or more wavy strokes in **Raw**. Move/zoom the canvas: it should not start stuttering just because a few strokes exist.
2. Tap **One-Euro**, choose **Strong**, draw a visibly shaky/zigzag stroke. It should visibly differ from Raw, though the live pen tip still tracks your hand.
3. Tap **Spline** and draw a curved stroke slowly and quickly. The blue line should remain connected to the pen tip and be distinguishable from the raw polyline, particularly at bends.
4. Draw a long stroke; toggle all four display modes after pen-up. The **same retained stroke** must switch representations without disappearing.
5. Zoom in and out, move far from strokes, return, rotate the device, background/foreground, and clear. No crashes or missing strokes.
6. Filter Logcat by `IP90_PROFILE`, `IP_PIPELINE`, `IP_FRAME_TIMING`, `IP_GEOMETRY`, `IP_GPU_INCREMENTAL`, `IP_VIEW_CULL`, and `IP90_INPUT`. For display/physical latency use Perfetto FrameTimeline or external measurement, not just the native render time.

## Architecture and provenance

Official source of truth for the *production project*: `https://github.com/Loaide/infinipaint.git`, branch **`main`**, inspected commit **`1b1f5ac337940ea3d2068c0cdc2a9a87836dc188`** (repository metadata / tree). The uploaded stand-alone RAR did **not** contain `.git`, so its own source commit cannot be attributed. This corrective test is based specifically on that RAR, and has **not** been merged or validated against the main application.

The uploaded architecture baseline approves Kotlin for the production shell, vector document data and double-precision camera-relative GPU coordinates; those decisions are **unchanged**. This existing diagnostic app continues to use Java + GLES 2 and world floats, and is **not proof of production-grade large-coordinate precision**. Rendering millions of strokes still requires production spatial indexing, batching/LOD, camera-relative transforms, persistence, and on-device benchmarks. The changes here fix the immediate diagnostic hot paths without silently redefining production architecture.

See `README_LATENCY_FIX.md` and `diagnostics/README.md` for detailed source changes and diagnostic limitations.
