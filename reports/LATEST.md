# M02 — accepted and complete, with documented limitations

The owner completed physical acceptance on Samsung SM-X626B (Android 16, 2880 × 1800) and accepted M02. No closure blocker remains. This is milestone acceptance, not release approval or authorization to merge. On `feature/m02-live-ink`, closure verified the tested application `44f559b0d38c6ec0b194132df01569c7ee9bafec` is an ancestor of documentation checkpoint `767f8753f3854b67e8c8bc72a9f2b6f5d5877810`; only documentation changed between them. Local `app/build/outputs/apk/debug/app-debug.apk` matched owner-tested SHA-256 `1c8f6f3a835836e6d9dff8753db8d8563b25e2279223f224bd567092bb7aed43`. No new build, test run or device interaction at closure.

## Physical tablet acceptance

**Owner-reported:** active refresh 90 Hz during presentation captures; thermal status 0 at environment capture; window alpha bits=8, `max_blend=1`. Handwriting, perceived latency, pressure including stationary changes, corners, curves, pen-up consistency, AA, smoothing modes, accumulated strokes, zoom and pan passed. Physical dots were **inconclusive**, not failed; the separate automated Mesa dot checks passed. A minor pressure-transition irregularity was visible at magnification; cause unknown, deferral accepted.

Long-stroke operation, stylus with palm, finger navigation and pointer transitions, rotation, split screen, background/foreground, lock/unlock, interrupted stroke, mode switching and repeated navigation passed. EGL recreation passed: process 17521 initialized rendering at 22:12:56, 22:14:49 and 22:15:11 on different rendering threads with unchanged coverage capabilities. The owner confirmed existing ink survived and interaction continued.

**Idle:** 41,575 ms initially and 50,811 ms finally (92,386 ms total); both intervals had zero camera, stylus or other requests, zero callbacks and zero unmatched callbacks.

**Active CPU telemetry:** each recent distribution has 512 observations; values below are p50 / p95 / p99 / max in ms. They measure native render calls and request-to-callback waits, not presentation or physical pen latency.

|Interval|Duration ms|Requests / callbacks|Native render ms|Request-to-callback ms|
|---|---:|---:|---|---|
|Camera|36,480|1556 / 1556|0.139102 / 0.293203 / 1.692305 / 2.341758|0.656484 / 1.287578 / 1.962422 / 2.412226|
|Writing|43,921|2829 / 2821|0.734531 / 1.549258 / 2.025586 / 3.037305|0.271875 / 1.433594 / 1.910703 / 7.307890|
|Stress|130,770|10729 / 10729|0.342578 / 0.939883 / 1.404649 / 2.157461|0.284179 / 1.387226 / 1.726406 / 1.961172|

Largest recorded individual stroke: 41,633 raw and filtered samples, 415,590 spline vertices, pen-up mutex hold 21.289 µs and acquisition wait 0.820 µs. Stress included multiple strokes; an uninterrupted two-minute individual stroke is **unverified**. Repeated GPU windows had zero reallocations and 120 incremental appends; largest live upload window was 9,734,040 B, associated with growth. Cumulative maximum native render was 23.676406 ms, distinct from the recent-window maxima.

**Actual presentation:** owner identified the canvas SurfaceView BLAST layer. Recalculation from the two local SurfaceFlinger captures used their 11,111,111 ns period, adjacent differences of the second timestamp column after excluding the `INT64_MAX` pending sentinel, and nearest-rank percentiles. Both contain 126 valid presented frames / 125 intervals. Rate = intervals / elapsed seconds.

|Activity|Elapsed s|FPS|Min / p50 / p95 / p99 / max interval (ms)|Apparent missed refresh opportunities|
|---|---:|---:|---|---:|
|Continuous writing|1.387378789|90.09796|10.809453 / 11.098477 / 11.182070 / 11.339296 / 11.345312|0|
|Camera navigation|1.409543438|88.68120|10.993086 / 11.102109 / 11.196211 / 22.123359 / 22.154414|2; cause unestablished|

Local capture identities (untracked, left unchanged): `m02-writing-presentation.txt` SHA-256 `571ade827949af43f3970879e943878fc19968566a021de14d07c646cd71b861`; `m02-camera-presentation.txt` SHA-256 `a02b5905064139796ee528602065c3175a2fbce5c83f617209a40c31f23516d4`. These are short active windows, not sustained arbitrary-workload benchmarks or physical pen-to-photon measurements.

**Accepted limits and deferred probes:** the maximum raw-to-filtered deviation of 137.1237 px requires matched zoom and smoothing context; it does not diagnose the pressure observation. Capacity-growth spikes, occasional CPU stalls, per-stroke coverage cost, extreme-zoom tessellation and precision limits remain. No million-stroke claim. Device fallback cost is untested (MAX was active); first completed-frame payload and complete memory scaling were not directly measured on the tablet. No independent pen-to-photon test or sustained thermal benchmark. The owner did not supply a matched audit/screenshot series or individual results for every subcase of the [tablet checklist](../docs/M02_TABLET.md); subpixel comparisons, exact offscreen/cache/Clear sequences and any other unreported probes remain **unverified on device**, not inferred passes. The automated evidence below remains separate. ADR-006 stays deferred: single-backend tablet data is not a comparable cross-backend driver workload. No approved decision changed.

## Prior implementation and automated evidence (pre-acceptance)

Branch `feature/m02-live-ink`; clean recovery at `6da9d96dfbcb76fd3bc26ce4832a9de5fa78b456`, following tested integration `57e12c4524a40e388c99c9a916a2817d2e8000b1`. The accepted `main` baseline is `410a49446abddbb46dd26144ff82ff7a4aae8675`. Input history, M01 scheduling and the diagnostic backend are preserved. [Earlier measurements, failure tests and evidence boundaries](M02_INITIAL.md) remain available. None of these host measurements establish Android presentation or pen-to-photon latency.

## Why the AA correction works

Coverage commit `1ec398e4599c49709e499e094565e4753689e380`. `MainActivity` selects RGBA8; `native_bridge.cpp::coverageLayer` uses window alpha as temporary coverage for exactly one visible stroke representation. It clears only alpha inside the clipped conservative stroke rectangle, gathers the stable prefix, provisional tail and tip, then resolves the stroke color onto prior RGB once and restores alpha to one. Separate strokes and diagnostic layers still composite in order. No offscreen texture, FBO bind, framebuffer copy or additional viewport-sized allocation in the renderer. The EGL window itself now requires eight alpha bits. A long stroke can cover the viewport, but small strokes do not pay a full-screen resolve. This is not a new residency system.

On GLES2 with `EXT_blend_minmax` (or ES3 core support), take the maximum of the existing analytic capsule coverages. Repeated identical edges are idempotent. Without MAX, draw the gathered parts at 16 ascending opaque coverage thresholds. The highest surviving band encodes the rounded maximum (17 levels); geometry is uploaded once regardless of pass count. This core fallback trades precision and submissions for correctness without imposing its cost on capable drivers. Non-overlapping strips alone would not handle arbitrary self-retrace; a separate mask texture would require extra storage, binds and sampling.

The predeclared equivalence criterion was **≤2/255 max RGB error**, allowing RGBA8 rounding but not accumulated fringe opacity. In real Mesa GLES2 pbuffer readback, the prior dense/sparse constant-pressure case measured **66/255**. The corrected full-frame comparisons cover sparse vs dense spline, constant-width retrace, pressure-ramp retrace and a repeated tight corner. Live vs finished, pressure dwell, dot footprints, completed order and opaque alpha are also checked. The portable path's diagonal ramp is compared separately against the MAX image: tolerance 10/255 (half a coverage step plus rounding), not the density tolerance.

## GL publication

`cacheFinished` moves names **only on the GL thread**, indexed by unique live generation. Parts 0/1/2 contain immutable raw/clean/stable prefixes; never adopt replaceable tail part 3. Reuse only a successful upload whose buffer fits the final mesh without retaining more than 50% spare storage. Cap additional growth headroom; upload just the missing suffix on first visible use, with no fence, mutex or device readback added. A `glGetError` after a live transfer or first completed transfer prevents publication of a rejected upload. Failed completion transfer invalidates the buffer so a later draw can retry from the immutable CPU geometry.

The 256/4,096-point JNI fixture renders a live frame immediately before end. Prior first-finished payloads were 55,512 / 884,952 B with one new storage call each. Transfer savings are **conditional**: skipped live frames can increase the suffix or exceed capacity. An unseen mode, context loss, small high-slack VBO or capacity miss uses a full lazy upload. Mode switching, offscreen completion/pan back, late append without render, simulated upload failure, generation reuse after cancel/Clear, and context rebuild are compared byte-for-byte against clean-context geometry. Payload bytes are not allocated bytes or GPU time.

## Allocation and curve limits

`SplineFitter::coefficients` now accepts caller-owned spans. Batch fitting and the four-knot live path use the same math. Bounded local arrays replace six transient allocations per typical moving append; candidate validation and all retained reservations still precede commit. The host probe counts scalar allocation requests, not memory residency. On the same deterministic moving stroke:

|Points|Candidate → corrected allocation calls|Cumulative requested bytes|
|---:|---:|---:|
|16,384|98,358 → 60|17,431,012 → 4,849,516|
|131,072|786,498 → 72|139,459,044 → 38,797,164|

The focused paired WSL/GCC 15.2 `-O2` runs (Intel i5-6300U; no thermal controls or discarded warmup) used `diagnostics/spline_scaling.cpp`, instrumenting the largest allocation *within each append*. At 16,384 moving points p50/p95/p99/max µs were **0.5/1.3/5.9/3156.7 → 0.3/0.5/2.8/578.4**. At 131,072: **0.4/1.2/4.6/6849.3 → 0.3/0.6/2.9/5870.6**. In both 131k runs the slowest sample was ordinal 65,538, requesting an 11,534,336 B segment array on geometric growth. At 16k the previous slowest requested only 264 B; a large retained-vector copy cannot explain that observation. The earlier 10.961/14.075 ms maxima were not individually instrumented, so their causes cannot be assigned retrospectively. Allocator and host scheduling contributions were not isolated. Stationary 16k remains 17 allocations. No specialized allocator or up-front maximum-size reserve.

The new coefficient parity tests compare incremental and batch geometry/length for uniform, chordal and centripetal curves, including ties, stationary samples and reversals; stable prefixes must not change. Live centripetal input that collapses a distinct interval under origin subtraction is rejected transactionally. Extreme-coordinate representation remains deferred.

For the same 0.20 capture-pixel target, a 2,000 px synthetic corner/reversal sequence hit the old 64-step cap on 2/4 segments (sampled interpolation error 0.307 px). The cap is now **96**, not an unbounded adaptive loop: the same fixture hits 0/4 with sampled maximum 0.197 px. Tight loop: 0/5, 0.179 px. A deliberately outsize 40,000 px sequence still hits 4/4: 2.740 px sampled, analytic bound 2.767 px. These are parameter interpolation errors before GPU rounding, sampled at 31 interior positions per chord, not measured human visibility. The larger safety cap increases worst-case per-tail geometry and preflight space by about half; ordinary inputs still use their computed step count. No M04 LOD or M05 precision work.

## Independent review and gate

**Final tested application and harness: `44f559b0d38c6ec0b194132df01569c7ee9bafec`.** Full `diagnostics/run_m02.sh` passed (`captures/m02-hardened.txt`, local). Both CSVs accepted in order (120/120 and 70/70); cap hits 0/119 and 0/69 segments; sampled maxima 0.136 and 0.057 capture px. Adversarial geometry/input, transactional failure/retry, scaling thresholds, mock GL ownership/resize/context and real GLES2 suites passed. On llvmpipe Mesa 26.0.3, **all four equivalent-path maximum deltas were 0/255 on each branch**, and the shallow diagonal taper differed by **8/255** between branches. Taper cross-sections and dot/pen-up tests passed on both. The test intercepts the production capability log to assert branch selection (1 then 0); no assumption based on a Mesa override alone.

|First finished spline draw, 1200×800 viewport|256 points|4,096 points|
|---|---:|---:|
|Previous → hardened upload payload (B)|55,512 → 648|884,952 → 648|
|New storage operations|1 → 0|1 → 0|
|Adopted storage retained (B)|61,704|905,688|
|Mask draw + clipped resolve, alpha clears (MAX)|1 + 1, one clear|1 + 1, one clear|
|Scissor area / 960,000 pixels|60,116|84,588|

One GL error query in each above completed transfer, none on subsequent unchanged cached draws. These are GL calls/rectangle areas, **not measured GPU bandwidth**. On MAX, added one clipped clear and one clipped resolve per visible layer; core fallback submits 16 mask passes plus one resolve, with no geometry reupload between passes. Actual Android driver costs remain unmeasured.

Full-run host bridge µs at 4,096 samples, p50/p95/p99/max: sample **1.2/4.0/14.8/9443.9**; end **5.6/17.5/98.1/208.4**; end mutex hold **2.5/9.3/19.6/46.9**. The initial candidate end median/p99 were 4.2/18.2 µs versus accepted baseline 15675.5/38994.7 µs. This run had a higher pen-up tail than the initial candidate; a focused correction run had end p99 21.6 µs. These are single-host, uncontrolled elapsed measurements (final checks overlapped an Android build), not proof of stable tail-latency improvement. The avoided transfer and allocation counts are deterministic on these inputs. The final 131k append maximum was 5.40 ms at the same 11.5 MB capacity jump.

W01 `Comprehensive` passed: `captures/workflow/20261124-201312-878-16028/gradle.log`, JVM task executed **11/11**, 0 failures/skips; debug APK and native builds, lint **0 errors / 8 warnings**, tracked and staged diff checks passed. Verified archive contains `libip_tablet.so` for arm64-v8a, armeabi-v7a, x86 and x86_64. APK `app/build/outputs/apk/debug/app-debug.apk`, SHA-256 **`1c8f6f3a835836e6d9dff8753db8d8563b25e2279223f224bd567092bb7aed43`**. Camera/gesture tests retained; on-demand scheduling unchanged. No fresh Android device, idle, actual presentation or physical stylus measurement.

W01 project usage snapshots at AA checkpoint and integration prep: 433 → 454 steps; input 1,679,326 → 1,814,132; output 127,965 → 137,404; reasoning 19,848 → 23,824; cache reads 18,299,264 → 21,033,984; writes 0, reported cost 0. Resumed the scoped graphics and performance reviewers; no synthetic dispatch or quota inference.

Graphics review of `captures/m02-aa.diff` found no blocker, requested proof of actual fallback branch and cross-path banding checks, and flagged Android window alpha and fallback cost. Both paths now assert the selected branch; actual window/driver validation remains a gate. Performance review of `captures/m02-perf-hardening.diff` identified excess tiny-stroke retained capacity, unsafe adoption after GL errors, conditional upload savings and missing parity/lifecycle evidence. Added the slack limit, error handling, ownership and coefficient parity fixtures. Reviewed changed renderer, spline, shader, EGL config, scaling/replay and mock/Mesa tests; no repeat architecture audit.

At that pre-acceptance checkpoint, tablet testing was pending. The physical acceptance above supersedes that gate, not the measurement limits: linear document traversal, limited world-float precision, synchronous capacity growth, conditional GPU residency and in-memory authoritative vectors remain. No merge, push or release is authorized.
