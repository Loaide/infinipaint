# M04 — Canonical span streaming, first production slice (functionally verified)

Inspected owner Git archive `4b0710b3e9f099d86aa1ce6973f4964f7a3a3dc7`, branch `feature/m04-scalable-renderer` (owner-reported branch, archive commit comment verified). GitHub main inspected at `d79105f52109970e807f3cabd35fda24a269a63c`; feature commit lookup returned HTTP 422. No source or architecture change on remote.

## Delivered scope

- `StreamingSpline` reuses the v1 fitter with a rolling four-knot window and fixed state. Equal timestamps, stationary pressure dwell, endpoint conditions, origin-relative coefficients and terminal connector follow the original recipe. No sample-history allocation in the decoder. `CanonicalStream` takes arbitrary ordered raw-sample chunks and emits raw/filtered chords, spline intervals and a terminal packet. Sink failure poisons the stream; staged output must be discarded. No artificial cap or separate coverage resolve at an internal interval boundary.
- Production `reconstructV1` (document restore, edits and undo/redo) now uses the streaming replay instead of the full-history input/fitter scratch path. It still creates all three compatibility meshes, using existing quad construction, float conversions and post-reconstruction translation. Mesh growth is checked against existing admission budgets; final ownership retains the charge. Fixed 16 KiB logical scratch reservation for decoder/fitter/call stack. Live pen path and GPU rendering remain unchanged.
- The production full-document SQLite load uses the common `streamRecord` validator, still validating the whole linked document and per-record SHA-256 before activation. New worker-only targeted `readStroke` and `readCurveRange` check document UUID, durable revision, record ID, content/geometry revisions and immediate reciprocal order links in a coherent transaction. The caller must already have a validated root and revalidate desired scene before publishing. No change to durable schema/format. No pending visible edits included. Cancellation checked at chunks and 64-sample JNI boundaries. No result escapes on digest, stale revision, cancellation or order-neighbor failure.
- `CanonicalRange` returns at most 128 independently addressable spline/terminal ordinals, with conservative v1 bounds. The CPS1 derived wire format is explicit little endian: 108-byte header (7 u32 plus 80-byte canonical header), 164-byte packets (kind/ordinal, five xy binary64 coefficient vectors, 11 binary32 pressure/position/radius fields and 4 binary64 bounds). Fixed maximum result 21,100 bytes. Outward interval Horner enclosure and actual float compatibility chord bounds include radius and 10 world-unit AA allowance for supported v1 zoom >=0.1. Not a certified M05 numerical or LOD guarantee. Terminal packet is part of the same stroke. These bounds are not yet used in the production spatial index.
- Single native range lease, matching tokens for append/finish/discard, strict sample extent and chunk checks, no render lock. Each JNI slice copies at most 2,304 raw bytes to stack, avoids new Java sub-chunk arrays. The native decoder validates samples and exact count; Java verifies persisted digest and revision. Full requested stroke is replayed, even for its first interval, so a corrupt late chunk cannot produce a successful early range. This is sequential replay within one record, **not** direct seeking to a stored curve page. It is O(samples in requested stroke); no need to replay other strokes. The durable reader currently shares the storage worker and transaction with the writer. There is no camera-driven request queue yet.

## Management and telemetry

Host sizes: rolling decoder 512 bytes (compile-time <=2 KiB), native range state 94,496 bytes (fixed <=256 KiB admission envelope including one full raw chunk); returned bytes <=21,100 per request, one active native lease, raw SQLite chunks <=65,520 bytes. The return buffer is read-only through the Java interface. This is not a total Java/SQLite/driver/physical-memory bound or aggregate budget for consumers retaining multiple returned ranges. Existing resident geometry and canonical budgets remain unchanged; no attempt to solve the 128 MiB mesh ceiling by increasing memory. Current reconstruction's 16 KiB scratch reservation is logical, not measured stack peak.

`ip-frames` adds:
- `canonical_replay [attempts,success,failed,samples,curveSpans,chords,totalNs,maxNs,peakMeshCapacity,stateBytes,scratchReservation]`: attempts/failures/timing include failed reconstruction; sample/span/chord totals count successful mesh reconstruction only; chords count all emitted compatibility meshes (including terminal). Peak mesh capacity is logical vector capacity, not physical memory.
- `canonical_reads [recordAttempts,verifiedRecords,chunks,rawBytes,targetAttempts,targetSuccess,targetFailure,checksumOrSizeFailure,stale,cancelled,targetNs,targetMaxNs]`: verified record is not a fully activated document. Chunk/byte work includes later-rejected records. Digest/remaining-size failure is distinct from other validation failures. Target cancellation has its own count. Invalid argument checks before admission do not count as attempts.
- `canonical_range [admissions,releases,slices,inputBytes,returnedPackets,outputBytes,sliceNs,sliceMaxNs,active,stateBytes,scratchCeiling,sliceSamples]`: native lease and decode accounting. Native staged results can precede final Java transaction/neighbor verification; targetSuccess is the verified request counter. No claim of a measured physical storage latency from JNI timing.

Compare cumulative deltas within one process. The target range counters normally stay unchanged during ordinary UI navigation at this stage; only the production full-load and reconstruction counters should increase. No continuous background work was added.

## Host verification and iterations

Final strict GCC C++20 seeded campaign: 42 runs, 1,260 strokes, 726,312 raw samples, 4,308,452 exact chord comparisons, 9,152,647 bit-exact curve evaluations, 12,947,860 bounds checks, 214,822 range packet checks. Uses independent original full-history pipeline as oracle; varies chunk partitions, equal timestamps, duplicate coordinates, pressure dwell, corners, large-coordinate offsets and terminal cases. Separate Clang C++20 30-stroke seed passed. Decoder fault suite: malformed header/extent/time/count, truncation, excess samples, sink refusal and poison, cancellation, terminal failure, allocation failure, range limits and empty end sentinel. Allocation-disabled replay: 1,000,000 stationary samples (1 terminal emission), 100,000 moving samples (99,999 curve intervals); fixed state, no history growth. Standalone ASan/UBSan/leak detection passed seed/fault/scale runs; not full Android sanitization.

Actual host JNI shared library with JDK `-Xcheck:jni`: 108 admissions and releases, 668 slices, 100 discard cycles, stale/busy token, overlapping ranges, partition parity, terminal, extent and incomplete finish tests; zero active leases at end. Max observed slice 222,836 ns on this host, not Android latency. Native production mesh sink: 31 injected allocation failures, four mesh budget failures, scratch and translated-coordinate rejection, exact retry, charge lifetime. Existing renderer ownership/publication/scheduler/input suites passed (200k ordered transfers and latest publications; 5,329 seeded gestures), existing document 10,000-step oracle and seven allocation failure checks passed.

Actual Mesa llvmpipe GLES2 shader paths (MAX coverage and explicit fallback verified): 4,097-sample multi-chunk fixture, byte-identical reconstructed vertex data, 48 four-mode RGBA comparisons per mode after context recreation (96 new comparisons total). Existing spatial/order plan parity: 351 matched-plan/fallback RGBA comparisons per mode (702 total; separate from new replay comparisons). No detected pixel difference, seam or pressure discontinuity in the tested reconstruction fixture. Portable EGL/GLES declarations and host JNI, not NDK compilation. Earlier graphics harness hit its deliberately small scratch limit; moved the multi-chunk fixture to its own disposable host document, unchanged production budget. Earlier targeted JNI and decoder runs repeated after tightening bounds/lease/cancellation tests. Final production smoke and seeded/JNI campaign rerun after admission-failure tests added. No external specialist review or full-app TSan.

Full output: [M04_CANONICAL_STREAM_HOST_RESULTS.txt](M04_CANONICAL_STREAM_HOST_RESULTS.txt).

## Remaining work and acceptance

This is the first canonical streaming slice, **not full viewport-driven streaming**. Production restore still retains raw Ink and meshes for every stroke. The current planner uses whole-stroke spatial records; no persistent curve checkpoint/addressable coefficient pages, span indexing, compatible immutable paired roots, pending edit overlay reader, independent reader lane, viewport demand scheduler or bounded geometry residency yet. Next connect span storage/bounds to paired roots and actual viewport request execution, then implement affected-range spatial compaction, certified vector LOD and residency in approved order. The 128 MiB resident mesh ceiling remains. No change to live shaders, exact order, v1 format, native Vulkan selection or overall 90 Hz/physical memory gates. Long single-record replay cost and writer serialization must be addressed before scalable interactive streaming acceptance.

Android build/NDK and real SQLite instrumentation were not available on the host. Build `:app:assembleDebug :app:testDebugUnitTest :app:assembleM03VerifyAndroidTest` with JDK17. Run `:app:connectedM03VerifyAndroidTest -Pandroid.testInstrumentationRunnerArguments.class=com.infinipaint.storage.CanonicalRangeTest` on reference tablet. New instrumentation tests cover revision and namespace, overlapping ranges, rollback/reopen, cancellation, wrong thread, size cap and late-chunk digest corruption rejecting even a first-span request. These tests are supplied but unexecuted here. Existing full-load instrumentation suite should also pass. Then targeted disposable document functional acceptance: long curves spanning chunks, overlap, erase/undo/redo, switch, rotate/background/reopen, persisted parity, replay/read deltas and settled idle. Target read telemetry comes from instrumentation, not normal camera navigation. Do not commit until owner acceptance. Actual 90 Hz presentation and physical peak RAM remain unverified.

## Document-open acceptance blocker / targeted follow-up

Owner reported New document and Open next document leave the active UUID and revision unchanged. Before/after status: Saved, 5268d7b4, 17/17; no reported mesh or input admission failure. The supplied SQLite errors are from other process IDs, not the current activity PID 17944. Root cause remains unverified. Android canonical range tests and storage regression task previously passed (one scaling test skipped). Seven of nine manual checks and idle passed.

The follow-up adds command dispatch, worker-phase and exception logging under IP_DOC_OPEN, a persistent last-open failure in the status, a document_open dump line and an explicit single-document outcome. It does not alter canonical format, SQLite transactions, native admission or renderer algorithms. It addresses the confirmed observability issue: save completion could replace the open failure status. Actual create/switch failure diagnosis and tablet acceptance remain pending.

Host verification of follow-up: compiled actual EditorStore with deterministic mocked Android handlers, SQLite and native port; 200 injected stage failures with queued autosave retained the error, 200 successful retries, 200 duplicate busy requests and 200 switches passed; 601/601 staged-transfer cleanup calls observed. This is a simulated state-machine test, not a tablet UI or SQLite test.

## Owner tablet acceptance

- Android compilation and unit-test task: PASS.
- CanonicalRangeTest: 3/3 PASS.
- Storage regression instrumentation: successful,
  one scaling test skipped.
- Manual functional acceptance: 9/9 PASS.
- Document-open follow-up installed and verified.
- New document creation and switching: PASS.
- Persisted record attempts/verified: 84/84.
- Reconstruction attempts/successes: 84/84.
- Samples: 96987; curve spans: 96675.
- Generated compatibility chords: 349168.
- Storage chunks: 108; raw bytes: 3491532.
- Total replay duration: 754288475 ns.
- Maximum replay duration: 43916524 ns.
- Decoder state: 512 bytes.
- Logical scratch reservation: 16384 bytes.
- Zero reconstruction failures, checksum failures,
  stale revisions or spatial I/O faults.
- Native rendering P50/P95/P99:
  0.591/10.987/13.244 ms.
- Maximum native rendering duration: 59.725 ms.
- Idle observation: 11.240 s, zero callbacks
  and unchanged reconstruction/index counters.
- Durable/visible revisions: 17/17.

Document-open follow-up:
- Improved failure reporting and operation diagnostics.
- Earlier creation failure did not recur after update
  and restart; original cause remains unconfirmed.

Remaining limitations:
- Targeted range reader not yet driven by viewport.
- Direct checkpoint seeking and span indexing pending.
- Existing resident geometry ceiling unchanged.
- Physical memory and actual 90 Hz presentation
  remain unverified.

First canonical streaming slice functionally verified.
