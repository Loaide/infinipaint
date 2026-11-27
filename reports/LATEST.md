# M03 accepted — latest milestone status

M04 [bounded data-access checkpoint](M04_2_DATA_ACCESS.md) complete on host: validated reopen, corruption controls, matched file-backed spatial queries and sparse order. Stop at backend selection before production GPU resource/scheduling design. [HOST_02 findings](M04_HOST_02.md) accepted, v1 retained. [Binding conditions](../docs/M04_PROPOSAL.md) stand. M03 is formally accepted with documented limitations.

See [formal acceptance](M03_ACCEPTED.md) and
[engineering closure](M03_ACCEPTANCE.md).

The historical pending statuses below are superseded by the
owner acceptance of 2026-11-26. Measurement limitations remain.

---

## Preserved historical record

### M03 Android verification candidate — not accepted

## P1 correction — `d543ed4a0ab4dd0e760ae4ebdb5a616043f88a8f`

Branch `feature/m03-document-engine`, baseline `8e4948712305a1936883ea8c4124dba2362fd84f`. Inspected the four changed files: `app/src/main/cpp/{native_bridge.cpp,document/working_set.hpp}`, `diagnostics/{renderer_smoke.cpp,run_m02.sh}`. The preserved review diff passed `git apply --reverse --check` before resuming; it was not reapplied.

Scene-map allocations are charged across clone, merge, swap and GL-local ownership. Snapshot growth is reserved before releasing the input lock. Upload transfers the reservation to live GL storage; failure, retirement, adoption or context loss ends that charge. Completed geometry reserves a cache-node credit held across erasure. A specialist found an early decrement during vector destruction: a base-class charge now releases **after** the derived vectors free their allocations.

Independent follow-up at the tested SHA confirmed the destructor-order and cache-node finding resolved. Cache-node ordering was checked by source inspection; the allocator hook independently exercises the mesh-free boundary. No physical residency claim.

`captures/m03-p1-release-owners.txt`: mock bridge and deterministic parked-GL test passed. At a 64,161,023-byte modeled slice, the second document was rejected with 64 patch nodes held; the identical switch fit after retirement. The test detected no admission headroom increase at the allocator hook immediately before the outgoing mesh free. Cancel-before-upload retained 95,544 bytes through upload and returned exactly that headroom at deletion. Failed upload, retry, cancel-after-upload, context loss without deleting stale names, and completed-cache handoff were also asserted. These are logical allocation/request charges, not RSS or driver residency. `captures/m03-p1-document.txt`: core oracle, rollback and seven allocation-failure cases passed (no core changes after that run).

`captures/m03-p1-release-pixels.txt`: Mesa 26.0.3 llvmpipe GLES2, MAX and fallback both passed the reconstruction vertex/4-mode RGBA parity suite, same-branch maximum delta 0/255. Mock bridge asserted zero unrelated undo uploads. `captures/m03-p1-release-build.log`: offline isolated Gradle assemble succeeded; the app was repackaged and the unchanged instrumentation package was up to date. No fresh JVM/lint, matched latency or broad benchmark run.

Both archived files were hash-checked against build outputs. `aapt` verified `com.infinipaint.demo.m03verify` (arm64-v8a, armeabi-v7a, x86, x86_64) and `com.infinipaint.demo.m03verify.test`:

|Archive path|SHA-256|
|---|---|
|`captures/m03-d543ed4-gate/app-m03Verify.apk`|`94b45501c757b62fb91fdf42caf4130a18dfea4493fabfaf6951a214993c4f7e`|
|`captures/m03-d543ed4-gate/app-m03Verify-androidTest.apk`|`6a7ac00ff312662579cbe5abf55634adb8f9bef5ccae9963f2134c2e31a7a87b`|

No installation or device measurements. The prior latency finding, Android SQLite/recovery/scale runs, physical memory and written owner gate are unchanged. See [isolated Android procedure](../docs/M03_TABLET.md).

## Previous candidate — retained evidence and open acceptance criteria

Branch `feature/m03-document-engine`; see [handoff](../docs/SESSION_HANDOFF.md) for tested SHA and [approved matrix](../docs/M03_PROPOSAL.md) for unchanged criteria. No installation or physical run. Source inspection covered `document/{document,change_queue,working_set}`, `native_bridge.cpp`, `tablet/{session,store_jni}`, Java `storage/{EditorStore,StorePort,DocumentDatabase}`, host document/render tests and isolated Android fixtures. The [source map](../docs/CODE_MAP.md) and [contract](../docs/M03_CONTRACT.md) describe the integration.

Tested source `d8a804a15e2e123cca26eec6ef54854e4fada3b0`. `aapt dump badging` verified target `com.infinipaint.demo.m03verify` (arm64-v8a, armeabi-v7a, x86, x86_64) and runner package `com.infinipaint.demo.m03verify.test`. SHA-256:

- `app/build/outputs/apk/m03Verify/app-m03Verify.apk`: `6270b6a1cb355cd82da99902a21147c7c32742c49ad88bb00553a20ab7ed0322`
- `app/build/outputs/apk/androidTest/m03Verify/app-m03Verify-androidTest.apk`: `6a7ac00ff312662579cbe5abf55634adb8f9bef5ccae9963f2134c2e31a7a87b`

`captures/m03-gate-regression.txt`: Mesa 26.0.3 llvmpipe GLES2, same-branch parity zero in both coverage branches. Cross-branch diagonal taper maximum delta 8/255, not a same-branch mismatch. Mock path reported zero unrelated undo upload.

**Post-build specialist review — P1 memory admission blocker.** Read-only review at documentation HEAD `901c9b572677f95ff761d97714b793822b48162e`, relative to `8195bdef36c62efb79a794ed801c4405f607d4f7`, found that `native_bridge.cpp` counts the staged replacement once before cloning its publication map, stops counting a patch when GL takes `newCompleted` rather than when GL releases it, and does not charge canceled live VBOs until retirement. At a modeled 256 MiB slice a switch between two 36k-dot documents with GL parked can appear to fit (~515 MiB vs ~521 MiB available) despite missing owners. Static counterexample, not a device OOM or data-loss observation. No new Clear rollback/publication defect found. Do not use this APK as evidence that the aggregate bound holds. Before physical stress or acceptance, track charges through ownership transfers and retirement, and test parked GL, near-limit switch/rejection and canceled VBOs; rerun affected checks and identify replacement artifacts.

|Gate|Evidence and limits|
|---|---|
|Editing, codec and rollback|`captures/m03-gate-core.txt`: seeded 10,000-step ordered oracle, budget/ticket checks, queue saturation/retry, seven injected allocation failures. Host coverage, not persistence evidence.|
|Ink and renderer|`diagnostics/run_m02.sh`: raw sample replay, mock upload accounting, spline scaling, and real surfaceless Mesa GLES2 MAX/fallback pixel fixtures. Matched replay checks geometry bytes and RGBA; switching, Clear/undo/redo, EGL loss and offscreen return are covered. Mesa does not establish tablet GPU behavior or SQLite round trips.|
|Android build|`captures/m03-gate-sdk-build.log` and `captures/workflow/20261126-134547-804-13908/gradle.log`: isolated target + runner built; W01 comprehensive, 11 JVM tests passed, lint 0 errors/13 warnings. The preceding missing-SDK configuration attempt did not run tests.|
|Latency **above threshold**|`captures/m03-gate-matched.txt`: i5-6300U, three sequential rotated runs, 101 lifts per size/mode. At 4,096 samples M02 end p50/p95 ranges 5.0–6.0/14.7–23.6 µs; M03 off 9.5–13.7/30.0–31.4; on 10.0–12.9/27.8–36.9. Queue-off alone exceeds 20%; enabling the queue is not the sole cause. Median hold rises from 2.2–2.6 to 6.8–9.1 µs off. Ticket preallocation did not restore M02 time. This includes canonical publication and the document commit under lock; there is no disk writer or concurrent GL in this probe. No claim that one suboperation explains the entire delta.|
|Document scaling|Reused `captures/m03-core-scale.txt`: original 1M-dot core probe, peak RSS 293304 KiB, no SQL/GL. `captures/m03-phase-latency.txt` localized a subsequent 17.436 ms append to splice (17.432 ms), with thread CPU 17.441 ms, no observed faults or switches. Not proof of the cause of the historical 143.566 ms maximum. Warmed lookup cost grows with document size; not evidence of a linear SQL rewrite.|
|Memory and backpressure **partial**|Counted canonical and derived capacity, bounded history, save slots, coalesced views, staged-switch and delayed-publication estimates are not process RSS or driver memory. Verify actual peaks during edits, backlog and reopen on physical hardware. Dense bulk rebuild/GL application can still stall.|
|SQLite and recovery **unexecuted**|Compiled isolated JNI/SQLite/GLES round trip (41,633 samples), failed select, corrupt/version cases, private child-process kill boundaries, and opt-in 1k/10k/100k SQL scaling. Need actual Android results for WAL/FULL, write/checkpoint bytes, warmed median over 100× growth (investigate >4×), and acknowledged-state recovery.|
|Physical and owner **pending**|[Authorized-only commands and SM-X626B checklist](../docs/M03_TABLET.md). Record presentation, idle, input, recovery, memory and camera evidence; retain any failure. [M02](M02_ACCEPTED.md) remains unchanged. No merge, push or M04.|
