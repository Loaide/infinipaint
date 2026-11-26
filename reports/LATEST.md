# M03 Android verification candidate — not accepted

Branch `feature/m03-document-engine`; see [handoff](../docs/SESSION_HANDOFF.md) for tested SHA and [approved matrix](../docs/M03_PROPOSAL.md) for unchanged criteria. No installation or physical run. Source inspection covered `document/{document,change_queue,working_set}`, `native_bridge.cpp`, `tablet/{session,store_jni}`, Java `storage/{EditorStore,StorePort,DocumentDatabase}`, host document/render tests and isolated Android fixtures. The [source map](../docs/CODE_MAP.md) and [contract](../docs/M03_CONTRACT.md) describe the integration.

Tested source `d8a804a15e2e123cca26eec6ef54854e4fada3b0`. `aapt dump badging` verified target `com.infinipaint.demo.m03verify` (arm64-v8a, armeabi-v7a, x86, x86_64) and runner package `com.infinipaint.demo.m03verify.test`. SHA-256:

- `app/build/outputs/apk/m03Verify/app-m03Verify.apk`: `6270b6a1cb355cd82da99902a21147c7c32742c49ad88bb00553a20ab7ed0322`
- `app/build/outputs/apk/androidTest/m03Verify/app-m03Verify-androidTest.apk`: `6a7ac00ff312662579cbe5abf55634adb8f9bef5ccae9963f2134c2e31a7a87b`

`captures/m03-gate-regression.txt`: Mesa 26.0.3 llvmpipe GLES2, same-branch parity zero in both coverage branches. Cross-branch diagonal taper maximum delta 8/255, not a same-branch mismatch. Mock path reported zero unrelated undo upload.

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
