# M04.3-C — Bounded execution scheduling (functionally verified)

Source: owner Git archive of `feature/m04-scalable-renderer` at `115390a56a89907e0053b4d8a060129c73c2d428`. Remote main inspected at `d79105f52109970e807f3cabd35fda24a269a63c`; feature commit unavailable remotely. No production Vulkan cutover or storage schema change. M04.3-A/B remain accepted functional checkpoints.

## Implementation and working limits

- Process-scoped ordered ink pipeline: up to 32 admitted, unacknowledged gestures, one open capture, 256 queued packets with reserved terminal capacity, one separate resolution command. Native admission/seal failure pauses FIFO; explicitly retry or discard the blocked stroke before later native DOWN. Acknowledgements bound callback backlog. Deferred edits wait for acknowledged gestures. No intentional dropping of accepted packets; if admission or sample processing fails, expose rejection or explicit prefix resolution. Lifecycle changes cancel open capture, preserve already closed queued gestures.
- Resident v1 planner runs on its own thread, mirrors ordered immutable scene deltas and publishes exact visible IDs. One pending ordered delta plus one being consumed, three fixed plan slots (65,536 visible IDs each), coalesced wake, cancellation checks every 128 records and before publication. Match epoch, revision, head, camera sequence, mapping generation and dimensions; invalid/stale/unavailable plan uses exact resident v1 traversal. No raster placeholder or fidelity reduction.
- Eight-slot live-view retirement queue; CPU mirror references and removed nodes are released on planner thread. If transfer capacity is exhausted, retain coherent prior scene/live latch and retry. Same-document camera remains independently usable. Fixed planner storage is charged to admission; map nodes and retained meshes use existing ownership accounting.
- GL still applies published patches, uploads GPU buffers, handles GPU deletion, and may traverse the resident scene for an exact fallback. This is the transition to spatial streaming, not the final frame planner or a general guarantee of nonblocking publication. Linear resident traversal and fully retained meshes remain. The new cap values are implementation defaults pending physical telemetry, not architecture changes.

## Actual host verification

Strict GCC C++20 warnings-as-errors build and Java 17 target (`javac --release 17 -Xlint:all -Werror` with host JDK 21). `diagnostics/run_renderer_smoke.sh`: PASS all suites including original mock bridge, parked GL and ownership, parked mutation and publication isolation, actual planner stale-camera/delta/epoch/retirement backpressure and context recovery, concurrent live prefix (8,193 values), scheduler (200,000 ordered moves and 200,000 latest-value publications), existing InkLane, and actual InkPipeline with controllable fake native engine. InkPipeline tests: 5,329 seeded gestures per run, paused admission and seal, prefix failure and explicit resolution, cancellation racing retry, terminal reserve, bounded acknowledgement backlog. Twenty consecutive pipeline stress runs and thirty planner runs passed before final ticket/phase snapshot correction; final strict smoke suite passed again afterward. AddressSanitizer + UndefinedBehaviorSanitizer passed the standalone scheduler suite (not full application).

Existing M03 core oracle (10,000 seeded edits) and seven injected allocation failures passed by direct compilation; the original run_m03.sh had CRLF shell parsing trouble in this Linux extraction. No document code change.

Real Mesa llvmpipe GLES2 using production shaders: 85 planner-versus-exact-resident RGBA comparisons per MAX/fallback coverage mode (170 total), zero differences. Existing pixel regression including multipage 300-sample stroke, editing, opacity/order, context rebuild passed in both modes. This compares identical resident scene state, not cross-backend Vulkan parity. Reproduction: `IP_PLAN_TEST=1 sh diagnostics/run_m02.sh pixels` on a Linux host with portable NDK headers and Mesa as described in diagnostics/README.md. `sh diagnostics/run_renderer_smoke.sh` includes new scheduler and ink tests (JDK 17+).

## Acceptance boundary

Android SDK/NDK compilation and physical device tests are not performed in host environment. Test integration on isolated m03Verify: rapid pen lifts, long strokes, editing after queued ink, pan/zoom, clear, document switch, rotation, background, reopen, no idle callbacks; collect `ip-frames` and `ip-document` without process recreation between mark and capture. Planner hits/fallbacks/cancelled/backpressure and ink admission counts must be interpreted, not assumed zero. Host failure injection establishes ordered recovery behavior for the fake native engine, not actual Android allocation exhaustion. No actual 90 Hz presentation, total physical memory peak, full renderer isolation from all work or million-stroke acceptance claimed. Production Vulkan lifecycle/fidelity and resource residency remain future integration obligations. Next: device acceptance of this increment, then approved production spatial/order access and bounded vector streaming; no new backend campaigns.

## Owner tablet verification

- Android build and unit-test tasks: PASS.
- Tablet functional checks: 11/11 PASS.
- Active capture: 26.070 s, 6090 stylus requests,
  1411 rendering callbacks.
- Ink packet high-water: 17/256; zero rejections.
- Gesture admission high-water: 1/32;
  zero rejected strokes or blocked gestures at end.
- Planner deltas: 1336 hits, 75 exact fallbacks,
  75 built, zero invalid, cancelled or backpressure events.
- Planner hit rate: 94.685%.
- Deferred live retirements: 1267.
- Native render P50/P95/P99: 9.388/11.010/11.920 ms.
- Request wait P99: 8.721 ms.
- Saved strokes: 707/707; zero pending IDs.
- Retained meshes: 125779608 bytes.
- GPU VBO current/peak: 6723432/25306848 bytes.
- Fixed planner charge: 1575296 bytes.
- Live publication current/peak: 0/18082016 bytes.
- Publication retries: 9 cumulative.
- Idle: 11.342 s, zero frame requests or callbacks.

Limitations:
- Active capture contained no camera requests.
- Physical gesture backlog was not stressed.
- Earlier handwriting comparisons are not matched.
- Allocation counters are not physical memory peaks.
- Actual 90 Hz presentation remains unverified.
- Exact resident fallback remains until spatial streaming.

M04.3-C functionally verified.
Proceed to production spatial and paint-order streaming.
Full scalable engine acceptance remains outstanding.
