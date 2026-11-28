# M04.3-B — Render publication isolation (host-verified candidate)

Branch `feature/m04-scalable-renderer`; inspected owner Git archive with embedded HEAD `6018eac32e31f094a6ce6b74c9d7a4b328c07b60`. Remote `main` inspected at `d79105f52109970e807f3cabd35fda24a269a63c`; local commit unavailable on remote (422). Baseline M04.3-A functionally verified by owner. Scope: `native_bridge.cpp`, `tablet/live_prefix.hpp`, `MainActivity.java`, `NativeBridge.java`, host renderer and live-prefix tests/runners. ADR-013 and M04 approved architecture unchanged. No Vulkan migration or backend campaign.

## Production changes

- `stateMutex` remains the mutation-side lock. GL never acquires it. A separate publication port transfers the completed delta map, head, epoch, revision and immutable live handles together. GL tries the port once per frame; contention retains the prior complete scene/camera latch and requests a retry, not a blocked render. Camera/scene switches publish under the same port. No work request remains when a successful latch has consumed the publication. Writer-side map merging can still take time; there is no independent planner yet.
- Live raw, filtered and stable spline prefixes mirror only newly emitted vertices into 512-vertex pages. Pages expose a frozen count; writes occur only beyond published counts. A two-level immutable directory copies a small root and at most one 64-pointer group on page growth. The provisional spline tail is an immutable replacement with a three-slot reuse pool (charged transient candidate if all are held). All necessary allocations precede sample commit; accepted geometry publication cannot allocate. The existing v1 builder and canonical completed geometry remain authoritative. No full active mesh copy per frame; the GPU uploads directly from page spans, retaining suffix updates and the existing safe completed-prefix adoption. Buffer growth/context loss still needs full upload; multiple subdata calls can occur at page boundaries.
- Sealing an ink ticket and removing its live publication is a single port transaction; a frame cannot observe the same stroke as both live and committed. Cancel, clear, document switching and codec replay keep the previous epoch semantics. The reserved append path fails closed if its preallocated nodes do not match. GL owns its cache and resource deletion as before.
- Live mirror storage plus conservative late-upload capacity credit (2x page/tail payload) remains charged until the final owner releases it, in addition to existing VBO reservation charges. New `ip-document` fields report logical publication charge, high water and cumulative latch retries. Old upload-scratch field is zero because frame CPU mesh copies were removed. This is intentionally conservative, not physical allocation measurement or a complete cross-lane credit proof. Accepted maximum live stroke may decrease near memory limits. Actual device peaks still require measurement.

## Executed host verification

- GCC C++20 `-O2 -Wall -Wextra -Wpedantic -Werror`: mock GLES bridge full suite PASS, including incremental upload (2,376 bytes in existing fixture), allocation-failure retry, context recreation, geometry/ordering, culling, undo/redo. Existing owner-credit suite PASS; parked 64-node switch threshold 64,161,023 modeled bytes; cancel retains 95,544 requested live VBO bytes plus the new mirror credit until retirement. Exact logical headroom restored.
- New bridge isolation suite PASS: actual render returns while mutation lock held by another thread; publication lock held causes prior coherent latch and retry; next frame picks up latest camera; live producer can append and seal after GL latches old pages; next frame replaces live with committed once; full context rebuild bytes identical.
- New live prefix suite PASS: concurrent frozen view through 8,193 values and directory-group rollover, injected page/group reserve failures, exact charge rollback, final-owner release.
- Real software GLES2 via Mesa 25.0.7 llvmpipe, using temporary minimal host ABI declarations and existing test harness: MAX and fallback suites PASS, including 300-sample multi-page live and sealed four-mode context rebuild (byte-identical RGBA); v1 reconstruction/codec parity, pressure, coverage, editing, document switching. Existing cross-coverage diagonal taper difference 8/255, not a new same-path mismatch. No tablet GPU claims.
- Existing mailbox 100,000 publications PASS and standalone Java InkLane FIFO/saturation/reuse/idle PASS; NativeBridge.java compiled with `javac --release 17`. Android MainActivity and NDK build not run here. `run_m04_host.py` instrumentation anchor updated; Python syntax checked, Windows-specific host attribution campaign not repeated.
- Existing document core and allocation-failure suites compiled/run directly (archived `run_m03.sh` has CRLF and did not run under sh). PASS: seeded 10,000-step ordered oracle and seven grouped-edit allocation failures; codec, reversible splices, history, ID ranges and save queue reserve/saturation/retry.

## Remaining gate requirements

This is not full M04.3 acceptance. Confirm local Android assemble/unit tests and isolated tablet behavior (drawing, rapid lifts, modes, navigation, editing, rotate/resume/reopen, idle). Capture active/idle `ip-frames` and `ip-document`; compare with M04.3-A only for similar workload. Frame timing includes callbacks, not actual presentation. Publication contention may show the prior camera/scene briefly with retry; no actual presentation sequence tracking yet. Bulk delta apply, map traversal, GPU upload, deletion and allocator reclamation still run on the GL lane. Mirror allocation/copy overhead and physical peaks need device verification. Independent render planning, stale-result handling, full revision/session-root leases, explicit total byte credits and ordered multi-stroke admission during worker stalls remain pending; UI storage lifecycle waits remain. No scalability or 90 Hz promise. Do not increase scene limits or switch production to Vulkan at this checkpoint. Next M04.3-C: ordered admission and bounded scheduling/retirement behind existing v1 reference.

## Owner tablet verification

- Android build and unit-test tasks: PASS.
- Tablet functional checks: 9/9 PASS.
- Active capture: 30.939 s, 7568 stylus requests.
- Ink queue high-water: 11/256; zero rejected operations.
- Native render P50/P95/P99: 9.249/13.587/16.240 ms.
- Comparison with M04.3-A is not workload-matched.
- Document reopened with 529/529 strokes saved.
- Retained meshes: 99205128 bytes.
- GPU VBO: 2418552 bytes.
- Active and live publication charge after reopening: zero.
- Idle: 11.664 s, zero requests and callbacks.
- Process recreation reset publication peak/retry counters;
  active publication peaks remain unverified.
- Actual 90 Hz presentation and physical memory peaks
  remain outstanding.

M04.3-B functionally verified.
Proceed to M04.3-C without repeating acceptance tests.
Full M04.3 acceptance remains pending.
