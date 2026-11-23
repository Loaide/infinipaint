# Corrective build: actual changes and test evidence (2026-11-16)

**Source:** user-uploaded `InfiniPaint_90Hz_Tablet_Test(1).rar` (no embedded `.git` metadata). **Target:** isolated Android diagnostic renderer; official repository `Loaide/infinipaint`, branch `main`, inspected HEAD SHA `1b1f5ac337940ea3d2068c0cdc2a9a87836dc188`, *not modified*.

## Identified bottlenecks

1. Per-frame full CPU copy of active `rawMesh` even in filtered/spline mode; plus full `cleanMesh` and stable spline copies when those modes were selected. This grew with stroke length even though GPU buffers used incremental uploads.
2. Repeated `IncrementalSplineBuilder::snapshot()` while stabilizing **every new cubic segment** copied all previous samples/coefficients: quadratic total work as strokes lengthened.
3. Every 90 Hz Choreographer callback re-requested GL rendering even with no input/camera changes; and every historical sample also requested a frame from within the same MotionEvent.
4. Finished-stroke metadata arrays were copied on **every** render; all three completed representations were uploaded to GPU regardless of selected mode, and all strokes were drawn even if outside the camera.
5. Stroke triangles were padded at the minimum zoom factor of 0.1, producing very large GPU fragment coverage at high zoom: visual stroke width plus up to ~240px of unnecessary padding per side at 16x.
6. One-Euro defaults `min_cutoff=25, beta=0.12, dcutoff=8` provided almost raw geometry for ordinary strokes. Spline mode drew only stable sections plus a raw dot, visually hiding the new curve near the moving tip.
7. `String.format` ran on the high-rate stylus input callback; and frame-interval diagnostics included long idle gaps.

## Corrections

- `native_bridge.cpp`: delta-only GL uploads for live raw/clean/stable meshes; revisable bounded tail buffer for spline; exact GPU accumulation; append-only completed CPU ingestion; mode-lazy completed VBOs; conservative world bounds/culling; camera-dependent quad expansion in the ES2 vertex shader; combined raw-tip endpoint and optional connector for filtered/spline live strokes; robust clear/context restore handling; updated input and profile logs.
- `spline.hpp` / `spline.cpp`: O(1) `geometry_at(segment, u)` evaluation from cached coefficients, used for incremental stable and provisional geometry. `snapshot()` still exists for final durable full curve and replay.
- `MainActivity.java`: API34+ nanosecond MotionEvent timestamps, original millisecond timestamps on older Android; at most one GL request per MotionEvent; no unconditional Choreographer redraw while idle; smoothing preset control and actual onDrawFrame frequency calculation; Camera and gesture behavior retained.
- `InputAuditRecorder.java`: per-sample CSV assembly with a lightweight builder, no synchronous disk I/O.
- `NativeBridge.java`: additional `setSmoothingPreset()` JNI function.
- `diagnostics`: C++ bridge mock GLES smoke test plus existing real C++ algorithm replays.

### Smoothing presets

| Preset | min_cutoff | beta | dcutoff |
|---|---:|---:|---:|
| Light | 8.0 | 0.025 | 1.5 |
| Medium (default) | 3.5 | 0.009 | 1.0 |
| Strong | 1.6 | 0.002 | 1.0 |

These are **experimental test presets**, not scientifically optimized thresholds. A causal low-pass filter necessarily introduces some **geometric phase lag**; the immediate physical raw-tip overlay prevents intentional waiting for smoother vertices, but cannot guarantee zero hardware/display latency or zero geometric offset. Splines have a provisional, up-to-date last segment; completed strokes retain all three visualizations.

### Observed host verification

`diagnostics/run_renderer_smoke.sh` **PASS** on this system, including distinct One-Euro / spline / strong-preset geometry and 2,112 B newly transferred for 10 raw sample additions after capacity was available (vs full-history transfer). Context-loss recovery, viewport culling, mode-lazy cache, camera zoom, same-time samples, and clear also passed in a simulated GLES backend. Both `diagnostics/synthetic_input.csv` and `diagnostics/synthetic_same_timestamp.csv` were processed without replay failures by `diagnostics/run_replay.sh`.

A successful **host simulation cannot establish actual GPU timing, shader compile success on Xclipse 540, or the absence of rendering stutter**. Android SDK and real device were not accessible. The benchmark in this ZIP is explicitly CPU/logic-only and should not be mistaken for 90 Hz tablet acceptance.
