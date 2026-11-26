# M03 engineering closure — decision requested, not accepted

**Decision:** ready for written owner disposition of the tested APK; **not an unconditional pass of every matrix item**. Drawing, durable recovery, ordinary editing, isolated Android tests and warmed incremental SQL scaling passed. The measured lift regression, unsampled transition peak and coverage limits below remain on the record. No application change or tablet repeat is proposed on this evidence.

## Identity and chain of evidence

`feature/m03-document-engine`; tested application `d543ed4a0ab4dd0e760ae4ebdb5a616043f88a8f`. Verified ancestor of inspection HEAD `cb814c49e583c32a96c8481f95a616c9564ad06f`; only three documentation files differed. This closure added host-only diagnostic/evidence and documentation, not app, JNI, schema, Gradle or instrumentation inputs. **No completed device check was invalidated.**

|Archived artifact|SHA-256|
|---|---|
|`captures/m03-d543ed4-gate/app-m03Verify.apk`|`94b45501c757b62fb91fdf42caf4130a18dfea4493fabfaf6951a214993c4f7e`|
|`captures/m03-d543ed4-gate/app-m03Verify-androidTest.apk`|`6a7ac00ff312662579cbe5abf55634adb8f9bef5ccae9963f2134c2e31a7a87b`|
|`captures/m03-owner-gate-d543ed4.zip`|`11e1f0b5fb43f79cbbb568110e25097ab85105bc957912bce0ff4c128dd598cb`|

Archive verified by reading back every entry against the [per-file SHA-256 inventory](M03_EVIDENCE_SHA256.txt): 96 inputs plus the manifest, including all 72 physical captures, **both original and controlled** presentation windows, device preflight, thermal captures, Android results, host phase probe and both APKs. ZIP is local under ignored `captures/`; the tracked inventory identifies its bytes, not a remote backup. APK hashes refer to the archived artifacts, not a newly pulled installed image. Owner manual observations come from the supplied campaign record; they are distinct from dump-derived measurements.

Preflight: Samsung SM-X626B, Android 16, build `X626BXXS7BZB7_OJM7BZB7`, physical 1800×2880, MemTotal 7,598,596 KiB; internal display reported 90 Hz. Existing owner package `com.infinipaint.demo`; runner targets **`com.infinipaint.demo.m03verify`**. Coverage dump: alpha 8, MAX available. Thermal status 0 at recorded endpoints, not a continuous thermal trace. No ADB action during closure.

## Physical results

|Area|Outcome and scope|
|---|---|
|Drawing/editing/persistence|Owner **11/11 A, 14/14 B, 13/13 C**. Undo worked after rotation, background/resume, screen lock, split-screen and interruption; subsequent editing worked. Full process restart restored the selected document, saved ink and camera. Not a promise of persistent undo. Rotation temporarily changes viewport centering; rotating back restores the original position. Record for later camera/workspace work. **Physical stroke cancellation unverified:** owner could not trigger it reliably.|
|Idle/ordinary writing|90,423 ms since mark: zero camera, stylus or other requests and zero callbacks. Writing 30,243 ms / 2,124 callbacks; recent 512 native-render p50/p95/p99/max **0.948/1.777/2.452/4.406 ms**, not presentation. Saved 267/267, zero save charge at capture.|
|Continuous stroke|Owner reports ≥2 uninterrupted minutes, all six checks passed, no perceptible deterioration during writing or lift. 57,956 samples, pen-up lock **130,586 ns**, wait **625 ns**; saved **268/268**. Mark window 128,435 ms, 11,427 callbacks; recent native p95 0.902 ms, max 1.703 ms. **Cumulative** native-render max rose to **40,797,812 ns**; no per-frame trace connects it to lift, switch, GC or a skipped presentation.|
|Memory|Small → large → restored small settled PSS **288,092 → 448,828 → 277,048 KiB** (RSS 401,048 → 562,232 → 390,552 KiB). Retained CPU mesh bytes **12,647,448 → 78,535,008 → 12,647,448**; small revision 39/39, large 270/270, both saved. Baseline small and reopened small differ in history: reopening ends the prior session. Restore-labelled dumps are not synchronized with allocation: all ten `restore-large` document dumps still show small 39/39; all ten `restore-small` show large 270/270. Sampled PSS endpoints cannot bound the instantaneous overlap.|

**SurfaceFlinger recomputation.** Used the middle (actual-present) timestamp of each valid triple, excluded zero/pending `INT64_MAX`, preserved order. All four windows have 126 valid timestamps / 125 intervals; refresh period 11,111,111 ns. FPS = `125e9/(last-first)`; estimated skipped refreshes = sum of `max(0,round(interval/period)-1)`. These are short windows of presented buffers, not a sustained 90 FPS guarantee, a count of app faults, or pen-to-photon latency.

|Window|First / last actual present (ns)|FPS|Est. skipped|Max gap (ms)|
|---|---|---:|---:|---:|
|Original writing|32599741727810 / 32601439945154|73.607|28|288.506|
|Original navigation|32629088398044 / 32631397017302|54.145|83|366.299|
|Controlled writing|32855336939255 / 32856724366912|90.095|0|11.396|
|Controlled navigation|32872674197693 / 32874061499294|90.103|0|11.345|

## Acceptance matrix reconciliation

Read against the unchanged [ADR-013 matrix and binding additions](../docs/M03_PROPOSAL.md) and [v1 contract](../docs/M03_CONTRACT.md). “Passed” below refers to the specified experiment, not to all possible fault schedules.

|Criterion|Evidence|Boundary / disposition|
|---|---|---|
|Canonical core, identity, order, grouped edits, budgets|Host `m03-p1-document.txt`, `document_test.cpp` and `document_failure.cpp`: seeded 10,000 transitions compared after each transition, missing IDs, edit rollback, range exhaustion, duplicate ticket/end, prefix admission/cancel, group-count/byte caps and oversized Clear. Seven allocation failures preserved IDs, revision, queue and budgets and allowed retry. Split/translate, style vs geometry revisions, undo/redo and branching have deterministic fixtures. Replay covers dot, ties, stationary and long strokes.|Random oracle varies insert/remove/Clear/undo/redo; split/replace/transform are separate fixtures, **not** randomized value comparisons across all command kinds. Balanced tree lookup is logarithmic, not constant. Conservative geometry bounds/culling exercised on fixtures, not a proof for all valid inputs. No whole-model clone in ordinary append.|
|Sparse order service / history lifetime|M03 persists reciprocal links and revisions; storage validates traversal. Sparse service contract in `document.hpp`; stable order compared in host/device rendering. History survives same-session lifecycle; successful switch/reopen ends history; failed select leaves old scene/revision/selection intact. Oversized Clear is not made irreversible.|The sublinear sparse-rank implementation, sparse-query oracle and work counter are **M04 dependencies, not M03 measured passes**. Failed-select fixture does not exhaustively test prior undo history or every switch interleaving. Bulk history beyond 64 MiB is not supported.|
|Admission, save queue, memory|Ticket and saturation/failure fixtures, queue incl. in-flight limited to 32 / 64 MiB, separate 128-group / 64 MiB history. P1 parked-GL switch rejected at modeled 64,161,023-byte slice, identical retry after retirement succeeded; canceled VBO charge exactly 95,544 bytes through deletion. Tests cover failure, retry, adoption, context loss, and admission immediately before mesh free. Owner round trip above; long fixture logged 78,083,968 native heap bytes, not whole-process peak.|Physical cap uses `R=min(physical/16,384 MiB)`, so `R=384 MiB` here; canonical ≈48 MiB, CPU mesh ≈128 MiB and aggregate logical allowance 3R. Logical restore-observed high 115,408,248 B, live/VBO counters and bounded rejection are useful evidence, **not actual switch peak, driver residency, or physical exhaustion**. No such peak was captured.|
|Storage format, reopen, durability, failures|`m03-android-instrumentation.txt`: **OK (11 tests), 94.442 s**. JNI/SQLite/GLES 41,633-sample record, tied timestamps and 4-mode pixel equality after reopen; erase/undo/redo/Clear groups flushed. Range reservation across UUID/restart, camera seq guards, failed staged select, missing catalog, orphan sidecars, corruption/checksum/link/count/missing chunks and unknown version; faults inside a group leave old revision. Host explicit wire bytes/endian, bitwise re-encode, NaN, truncated sample and unsupported recipe. Controlled private-process deaths **before write, mid-group, after commit before ack, after ack** produced complete old/old/new/new revisions.|Not a field power-loss test. Fault injection is not actual media-full or persistent hardware I/O failure. Not a reopen after *each* transform/split intermediate state; no distinct evidence of a real ENOSPC retry, exhaustive chunk-boundary mutations or adversarial concurrent flush/switch/shutdown. Manual save/retry and successful process restart are not substitutes for those faults.|
|Camera and session isolation|Independent edit/view revisions; Android stale/duplicate view writes rejected without altering edit revision. Two-document switch, restart and distinct camera/ink passed. Coalesced pending view design inspected.|No per-gesture SQL write-count trace; do not infer camera centering invariant across rotated viewports.|
|Reconstruction, publication and GL|Both Mesa GLES2 coverage branches: identical corresponding vertex buffers and zero same-branch pixel delta in four modes; cross-branch taper max 8/255. Overlap/style/order, translated switch, Clear/undo/redo, offscreen return, EGL recreation and terminal ink covered. Mock ordinary undo had **zero unrelated uploads**; live prefix probe 648 B suffix, zero new storage calls. Android reopen pixels and manual ink reinforce this.|Mock counts are not physical driver counters. Tests cover synchronous revision/epoch publication and coalesced edits; not an exhaustive stale-completion scheduler or all grouped replacements on all GPUs. No new rendering backend.|
|Scaling and tails|SQL and host results below. Full core distributions for append/lookup/delete/undo/redo 1k/10k/100k/1M in `m03-core-scale.txt`; original 1M single-sample probe peak RSS 293,304 KiB, history 61,440 B. No rerun of million-record fixture.|Not a million-visible-stroke benchmark. Separate warmed SQL load, commit and checkpoint times; no clean cold-cache series, independent flush-barrier percentiles, captured `EXPLAIN QUERY PLAN`, or joined physical allocation/driver peak.|
|Regression and integration|Prior full M02 host input/spline/mock/Mesa, final P1 affected mock/Mesa/fault tests, Android/JVM and manual campaign. Final W01 Comprehensive **once** on unchanged app inputs: `workflow/20261126-200535-023-4700/gradle.log`, Gradle success, native/assembleDebug and lint; JVM task UP-TO-DATE, XML 11/11, lint 0 errors / 13 warnings, tracked diff check 0. Not 11 newly executed unit tests. Host lift investigation below.|Investigation threshold crossed, not waived. Archive identifies the isolated APK pair independently of the debug build. No source changes requiring device repetition.|
|Device / owner|Results above; zero idle callbacks, ordinary/long writing, navigation, save statuses, physical lifecycle and controlled private kill tests. Archived raw windows include earlier gaps.|Physical cancellation unverified, transient peak missing, rotation centering and render maximum retained. Written acceptance is separate.|

## Scaling: what grew, and what did not

Android `StorageScalingTest`: same device/configuration, one-dot SQL edit (not an end-to-end stylus lift), 31 warmed observations per catalog, page size 4,096, cache setting −2,000. Write bytes are process I/O deltas including checkpoint, not flash-controller writes. Raw p50/p95/p99/max, WAL frames and checkpoint tails in `m03-android-storage-memory.txt`.

|Rows|Commit p50 / p95 ns|Checkpoint p50 / max ns|Write p50 / max B|DB / WAL B|Validated SQL load p50, n=5|
|---:|---:|---:|---:|---:|---:|
|1,000|918,008 / 2,114,101|772,149 / 1,150,274|36,864 / 86,016|327,680 / 366,712|0.108 s|
|10,000|877,695 / 1,174,961|809,414 / 1,527,344|36,864 / 110,592|2,940,928 / 524,288|1.002 s|
|100,000|890,469 / 1,042,890|1,002,304 / 1,275,899|36,864 / 90,112|29,118,464 / 524,288|9.918 s|

**100× catalog growth: commit median 0.970×, write median 1.000×; both pass the 4× investigation threshold.** Median WAL frames three at all sizes. SQL changes key by `(doc,id)` and `(doc,id,ordinal)`; unchanged payloads aren't re-encoded by the append delta. Full validated load grows with rows, as expected; excludes native reconstruction and presentation.

In the core probe 1k→100k p50 append/delete 400→800 / 500→1,000 ns; lookup 100→900 ns. Two warmed batched experiments confirmed the lookup signal (uniform ~8.6–10.4×, hot eight ~1.6×). Tree depth and access locality matter; data do not isolate a hardware cache cause or imply a full scan. Historical 1M append **143.566 ms max** vs p50/p95/p99 1.0/1.6/5.0 µs. Another run observed 9.416 ms; a phased run saw 17.436 ms wall at index 334419, 17.432 ms inside splice, 17.441 ms thread CPU, zero recorded faults/switches at that event. Splice includes existing tree-node insertion and link repair, not a whole-map copy/rehash. No phase/CPU trace exists for the original 143.566 ms event. These were generic unobserved core edits, not Android pen-up or the 40.798 ms native-render event. Preserve as tail risk; neither “scheduler stall” nor “fixed” follows.

## Lift regression: required work versus opportunities

Historical matched capture `m03-gate-matched.txt`: i5-6300U, accepted M02 `44f559b0d38c6ec0b194132df01569c7ee9bafec` vs pre-P1 M03 `d8a804a15e2e123cca26eec6ef54854e4fada3b0`, 3 rotated runs, 101 lifts per size/mode. At 4,096 samples, p50/p95 ranges in µs: M02 **5.0–6.0/14.7–23.6**, M03 queue off **9.5–13.7/30.0–31.4**, on **10.0–12.9/27.8–36.9**. Off exceeded +20% at p50 and p95 in all three paired runs at **both 256 and 4,096**. No repeated equivalent sample-processing breach; isolated run-level flags remain. Median hold grew from 2.2–2.6 to 6.8–9.1 µs off; median wait 0–0.1 µs. Outer callback includes post-unlock logging and destruction. No SQLite writer, concurrent GL, Android dispatch or presentation in these host timings.

**Targeted attribution on the physically tested SHA.** `diagnostics/m03_lift_phases.py` compiled temporary instrumented copies of the two exact Git trees, same mock, `-O2`, fixed 4,096-sample path, 101 observations/mode. Clocks only on the successful lift path; output after lock release and trace retirement. This is one diagnostic series, not a replacement matched threshold test; no thermal controls or discarded warmup, per-phase quantiles not additive. Selected p50/p95 µs:

|Phase|M02|M03 off|M03 on|
|---|---:|---:|---:|
|Allocate shape and seal canonical shared owner|—|2.2 / 7.1|2.1 / 5.4|
|Ticket commit including history and scene observer|—|3.9 / 5.5|4.3 / 11.5|
|Finish terminal geometry (M02 includes publication)|1.4 / 4.9|1.6 / 5.6|1.4 / 5.6|
|Post-unlock retired trace destruction|1.9 / 5.1|2.4 / 3.9|2.6 / 5.9|

Even with saving off, M03 must take immutable ownership of accepted samples, preserve rollback, splice the reserved ordered node, publish the history group/revision and update scene links before unlocking. Seal + logical commit, not refitting or database writes, are the clearest measured additional costs. The sample vector moves; no full-stroke copy/scan or unrelated VBO upload was found. Queue-on also enables persistent admission/budgets: on-minus-off is **not** a pure SQLite/queue cost.

Two shared-owner allocations at lift, duplicate small-map lookups, counters and reclaiming abandoned redo/evicted history under lock are candidates for improvement; none is measured separately as a material fault. Seal is necessary; that exact allocation placement is not a transactional requirement. Ordinary accounting median 0.3 µs; branch-heavy reclamation is not isolated by this fixture. Do not remove the transaction, defer unbounded frees, or invalidate the tested APK merely to improve a host percentage. Owner reports no perceptible long-lift deterioration; **one successful physical lift and a short 90 Hz window do not negate the regression**.

## Requested written disposition

1. Accept the measured M03 transaction/ownership cost and unresolved host/renderer tails for this milestone, or specify which targeted tail evidence is required. No claim of a universal frame or pen latency bound.
2. Accept the settled physical return plus logical admission/retirement tests **without asserting a transition peak**, or require a specifically authorized peak capture. The peak requirement is not marked passed.
3. Confirm disposition of physical cancellation, reversible rotation-centering shift, original presentation gaps and the matrix coverage limits above (notably persistent media failure/race coverage and sparse-rank performance reserved for M04). No new test authorization is implied.

If those limitations are unacceptable, keep M03 open on the named item(s); don't repeat the completed campaign. If acceptable, **written owner acceptance of this SHA** closes the milestone. No merge, push, install, tablet change or M04 work is authorized by this report. M02 acceptance is unchanged.
