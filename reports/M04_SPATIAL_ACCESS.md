# M04 — Production spatial access, first increment (functionally verified)

Source: owner Git archive of `feature/m04-scalable-renderer` at `107b6fb6c1de2535afe8eebda3d8f5d64a3b73b6`. GitHub main inspected at `d79105f52109970e807f3cabd35fda24a269a63c`; feature commit lookup returned HTTP 422. M04.3-C remains the functionally verified baseline.

## Scope and invariants

The existing production planner now owns an ephemeral packed STR R-tree, bounded mutable BVH, and disk-backed ID-to-paint-rank page tree. The validated resident canonical scene remains the authority. This is an incremental implementation of the approved spatial/order architecture, **not full canonical or geometry streaming**. No shader, canonical schema, vector recipe, paint order, or backend migration is included.

- 16 KiB checked pages; spatial fanout 64, rank fanout 256. Both roots derived from the same reciprocal paint chain; never use incomplete generations.
- Whole-stroke conservative bounds from the existing v1 mesh. Query inclusively, deduplicate, order by exact ordinal; never clip a stroke to a spatial partition or resolve opacity separately per page.
- Mutable overlay: at most 256 keys including tombstones. Balanced BVH over present entries. Topology-preserving replacements, deletes and append suffixes update incrementally; arbitrary middle insert/reorder, overlay exhaustion or invalid state rebuild both roots. No full-document traversal on a successful indexed query, but rebuilds remain O(N) resident work plus sorting.
- Private `mkstemp` scratch file in Android cache directory, immediately unlinked, close-on-exec. Process-local native page encoding, checksummed reads, exact I/O loops, no reuse across epochs or process death. No durable sidecar or change to SQLite integrity.
- Explicit limits: 64 MiB scratch-file ceiling, 32 MiB dynamic allocation ceiling (also charged through the existing admission ledger); 32 shared page slots (512 KiB payload), 4096 rank-cache slots, 65,536 visible IDs per plan. Fixed storage is included in planner charge. 32 MiB free-space reserve checked before creation; this is not a guarantee against concurrent disk exhaustion. OS cache, allocator overhead and physical peaks excluded. Allocation charges reserve before admission and release on deallocation.
- Same epoch, revision, head, mapping generation and viewport dimensions required to use a plan. Conservative 20% margin each side permits envelope reuse (1.96x viewport area, <1 viewport extra). Exact viewport cull on the renderer; no approximation of stroke geometry. If expanded query overflows, retry exact viewport. If unavailable, corrupt, too large or incomplete, use exact resident fallback, never a partial plan. Cancel obsolete work; no unbounded work queue.

## Verification

See `M04_SPATIAL_HOST_RESULTS.txt` for the observed host output. Strict GCC C++20 compilation; 12 additional seeded edit workloads plus the default workload; independent ordered visibility oracle; failure injection, real partial-write failure, cache corruption, read truncation, cancellation, dense-capacity overflow, overlay compaction. Standalone Clang ASan/UBSan with leak detection. The existing mock bridge, ownership, publication, scheduler, ink pipeline and M03 editing/allocation regressions passed. Real Mesa llvmpipe GLES2 pbuffer comparisons compare the indexed path to the exact resident reference within each coverage mode, not MAX to fallback. Host real GL compiled using minimal portable ABI declaration shims and host JNI; not an Android NDK build. Long opaque centerline coverage witness checked 128 pixels at each of 17 positions per mode; 521-stroke multi-page fixture includes long crossing and self-overlapping translucent strokes, dispersed dots, scrambled IDs, subpixel camera motion, supported zoom extrema, context recovery, corruption fallback, document epoch change, and overlay undo/redo. Existing v1 shader cross-mode differences unchanged.

## Telemetry and physical acceptance

`ip-frames` includes `spatial` and `spatial_latency` alongside existing counters. All numeric counters are cumulative for the process; collect before and after a single unchanged-process capture, not across recreation. `ready`, `baseRows`, `overlayRows` (includes tombstones), `overlayActive`, `diskBytes`, `dynamicBytes` are current gauges; peaks are high-water charges. `buildAttempts/builds`, `incremental`, `compactions` distinguish rebuilds and updates. `queryAttempts/queries`, `cancelled/fallbacks`, `ioFaults/invalid/capacity` distinguish outcome. Reads/cache hits describe process-level page-cache I/O, not physical disk reads; rank-cache hits distinct. Work counters include failed/cancelled work. Time sums/maxima are nanoseconds; successful-only totals and bounded 17-bin successful-latency histogram (16 microsecond upper edges; final bin overflow) permit approximate quantiles, not exact percentile timings. Counters are individually atomic, not a transactional snapshot. `indexedPlans/linearPlans` and `indexedFrames/linearFrames` distinguish index and resident paths; `envelopeReuse/envelopeFrames` identify reuse; `linearTests`, `residentFallbackRows`, `planIdsTested` expose work. GL fallback can occur even if a valid index exists because no compatible publication is ready. The existing `planner` counters remain separate.

Android build, tablet functional acceptance, camera-active counter deltas, idle behavior and physical memory/disk/driver performance remain to verify. Check long translucent crossing strokes during pan/zoom, erase/undo/redo, document switches, clear/reopen, surface recreation; verify no omitted/duplicated ink, no new seams, and nonzero indexed frames in a populated unchanged-process camera capture. Do not infer 90 Hz presentation from callback duration. No extra backend campaign required.

## Explicit remaining work

The scratch rank tree is keyed ID->ordinal, not the final counted B+ sequence plus ID locator. Full middle-splice incremental ranking and bounded affected-range compaction remain. Spatial entries are whole-stroke v1 bounds, not final bounded curve spans. Full rebuild enumerates the resident scene; the 64 MiB scratch and 32 MiB dynamic ceilings can cause exact linear fallback. No million-stroke production guarantee. Fully retained canonical records/meshes and exact resident fallback remain; later canonical span streaming, persistent/rebuildable sidecar provenance, certified vector LOD, bounded residency and production native Vulkan integration are needed. No physical peak-memory or extreme-zoom seam certification. Do not treat this slice as full M04 spatial streaming acceptance.

## Owner tablet acceptance

- Android build and unit-test tasks: PASS.
- Functional checks: 12/12 PASS.
- No visual seams, missing geometry or compositing
  discrepancies reported during navigation.
- Camera capture: 54.005 s, 2280 camera requests,
  2280 rendering callbacks.
- Indexed frames: 2238/2280 (98.16%).
- Exact resident fallback frames: 42.
- Resident fallback row visits: 26460.
- Envelope reuse frames: 2181.
- Successful spatial queries: 99.
- Mean successful query duration: 0.235 ms.
- Native render P50/P95/P99:
  0.874/1.803/2.695 ms.
- Zero index I/O faults, invalid results or capacity failures.
- Zero index rebuilds during the navigation capture.
- Scratch storage: 245760 bytes.
- Dynamic index peak: 40640 bytes.
- Idle: 11.473 s, zero rendering callbacks,
  spatial queries or rebuilds.
- Durable and visible document revisions: 720/720.

Existing limitation encountered:
- Resident mesh charge reached 134217216 bytes.
- Native input rejected with Document geometry budget
  (code 7).
- Ink pipeline entered its explicit blocked recovery state.
- Subsequent acceptance completed successfully.
- No index failure was reported.
- Resolve the resident geometry ceiling through the
  approved canonical streaming, LOD and residency work,
  not arbitrary budget increases.

The first production spatial-access increment is
functionally verified. Full counted sequence ordering,
canonical span streaming, bounded geometry residency,
physical memory peaks and actual presentation pacing
remain outstanding.
