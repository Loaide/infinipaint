# Diagnostics and validation

## Device input audit

Long-press **Clear** to export a CSV **without clearing**, or background the app. `IP_INPUT_AUDIT` logs its path, normally under `Android/data/com.infinipaint.demo/files/diagnostics/`. A tap on Clear clears the drawing. The audit receives historical and current MotionEvent samples, logging original coordinates/pressure/tilt, `sample_ns`, `callback_ns`, outcome, and whether a point came from history. Android API 34+ event timestamps are read at nanosecond resolution when provided; earlier APIs provide milliseconds. Same-time samples are accepted in raw/filtered form rather than artificially timed, out-of-order samples are rejected. The recorder is downstream of Android event dispatch, not hardware-level capture. Its in-memory budget is 250,000 entries per export.

## Real C++ input / spline replay (host)

```bash
bash diagnostics/run_replay.sh diagnostics/synthetic_input.csv /tmp/ip_replay.csv
bash diagnostics/run_replay.sh diagnostics/synthetic_same_timestamp.csv /tmp/ip_replay_same.csv
```

Uses production One-Euro, InputProcessor and IncrementalSplineBuilder without GLES. Replay groups by pointer in arrival order, preserves ties and compares ordered knot positions against the same raw records. These errors are not ground truth or between-knot errors. Replay parameters differ from selectable UI presets.

### M02 integrated host suite

The full run asserts the GLES MAX and extension-free paths independently. The second Mesa process hides the minmax extension and checks its reported path. `pixels` runs just these two pbuffer tests; `perf` runs the mock ownership/failure tests and scaling probes. The 2/255 tolerance is for equivalent geometry within the **same** path; MAX vs the portable quantized resolve uses its own half-step-plus-rounding criterion.

On Linux/WSL run `NDK_HEADERS=/path/to/ndk/toolchains/llvm/prebuilt/HOST/sysroot/usr/include sh diagnostics/run_m02.sh` with GCC C++20, Mesa EGL/GL dispatch and swrast. Use LF shell scripts. Outputs use disposable scratch storage. Includes deterministic geometry/input tests, CSVs, strict mock bridge, allocation-failure checks, end/append benchmarks, and a separate real GLES2 Mesa pbuffer readback. `spline_scaling` without `--check` is the short baseline probe; the checked run also exercises 131072 samples. The pbuffer is software pixels, **not** a tablet driver or timing test.

## Native bridge / renderer logic regression (host)

```bash
bash diagnostics/run_renderer_smoke.sh
```

Requires `c++` with C++20 and JDK `jni.h` (set `JAVA_HOME` when necessary). Uses a small **mock GLES 2** implementation from `diagnostics/test_support`, not real graphics. Tests: begin/sample/end; same-time distinct coordinates; incremental live buffer transfer; GPU buffer persistence per part; actual raw vs One-Euro vs spline tessellation differences; Strong vs Medium preset changes; lazy completed-geometry upload on mode switch; viewport culling; zoom without retransfers; context recreation; clear.

This test checks native CPU/GPU API call contracts, but a mock cannot detect actual shader compile errors, driver issues, frame pacing, or missing pixels.

## Logcat profiles

- `IP90_PROFILE`: native frame CPU time, per active-frame interval (skipping idle gaps), input age, last-sample-to-frame, GPU uploads, and draw calls, averaged across 120 render callbacks.
- `IP_PIPELINE`: native state-mutex contention, input filter time, spline append/tessellation time, and render snapshot time.
- `IP_GEOMETRY`: final raw/filtered/spline vertex counts, filtered deviation.
- `IP_GPU_INCREMENTAL`: live VBO allocations, append calls and upload bytes.
- `IP_VIEW_CULL`: total retained versus visible completed strokes on the reported frame.
- `IP_FRAME_TIMING`: recent request-to-`onDrawFrame` entry and native render duration; because rendering is event-driven, 120 callbacks may span multiple disconnected interactions.
- `IP90_INPUT`: errors in native ingestion and spline building.

`GLSurfaceView` swaps *after* `onDrawFrame`; **none of these values measure actual stylus-to-photon latency or frame presentation**. Verify the 90 Hz requirement with Perfetto FrameTimeline / SurfaceFlinger (and a high-speed camera if necessary), not only with `onDrawFrame` CPU timing.

## Project scope

The Android diagnostic test is not the production InfinitePaper/InfiniPaint engine. Its existing GLES 2 and float world-coordinate shader differ from the approved production GLES 3 provisional / camera-relative coordinate architecture. No architecture decision was changed and no GitHub code was modified by this corrective ZIP.
