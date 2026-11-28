# M04 — Counted B+ paint ordering (functionally verified)

Inspected owner Git archive `bdcc417f5b0d66716986d89833446ea4b9d82a4f`, branch `feature/m04-scalable-renderer`. GitHub `main` inspected at `d79105f52109970e807f3cabd35fda24a269a63c`; feature checkpoint lookup returned HTTP 422. Prior spatial increment passed owner tablet acceptance. No change to canonical SQLite, v1 ink, shaders, backend selection, gesture admission, or geometry budgets.

## Delivered scope

- Production planner's previous ID-to-absolute-ordinal tree is replaced by a counted B+ sequence in link order (128-entry pages) plus a separate B+ ID-to-sequence-leaf locator (256-entry pages). Both bulk-load from the validated canonical link traversal. Sequence parent links and subtree counts permit exact prefix ranks; the locator uses ID separators, not paint order. Balanced split, borrow, merge and root collapse; moved child parent links and moved leaf ID locators repaired, free page slots recycled. Empty-to-nonempty and nonempty-to-empty are supported.
- Bounded splice classifier checks final reciprocal links and reduced survivor order, complete inserted runs, deleted gaps, head/tail, size, and bounds before any write. Supports disjoint insert/remove/replace batches including restored IDs. Geometry-only edits do not write order pages. Only changed membership IDs mutate the counted tree; no full rank relabel. Unsupported survivor reorders, oversized deltas or incompatible base go to exact rebuild. Spatial overlay capacity is preflighted before tree writes.
- Sparse queries resolve missing ID locators in sorted ID order, group by sequence leaf, compute each leaf's prefix once and order candidates by unique exact rank. Fixed 4096-slot rank cache shifts positions after each successful insertion/removal, based on the actual tree rank; no whole-document rank rewrite. Rank cache is cleared on generation invalidation/rebuild. Query failures/overflow/cancellation publish no partial plan.
- All mutations and queries occur on the sole planner owner. Current private scratch pages can be updated in place because no other lane reads the trees; only completed immutable compatible frame plans leave the worker. Write/read/validation/allocation failure discards both spatial and order roots and rebuilds from the authoritative scene or uses exact resident fallback. No new durable-sidecar consistency protocol. CRC32C covers page bytes including unused padding except checksum field; structural header/count/locator checks and reciprocal rank-path checks supplement it. Scratch files remain unlinked and process-local; detection on page read, not continuous verification of cached pages. CRC is not cryptographic authentication.
- Whole-stroke selection and coverage union remain unchanged: no clipping at page boundaries, no separate opacity resolve per spatial page, no shader/LOD changes. Exact paint order retained on all successful plans.

## Resource bounds and telemetry

16 KiB pages, total scratch-file ceiling 64 MiB (including reusable free pages), dynamic allocation ceiling 32 MiB with shared admission ledger, 32 page-cache slots, 256 spatial overlay keys including tombstones, 4096 rank-cache slots, 65,536 candidate IDs per plan. Fixed planner object accounted by existing charge. Compact order mutation nodes and bounded tree depth (4 limit, valid trees under cap at most 3 levels). Stack test on a 1 MiB POSIX worker passed, including cascading branches, sanitizer build; actual Android thread stack and physical peaks unmeasured. File may retain freed slots until reset; they are reused. Initial free-space reserve is not a guarantee against concurrent storage depletion. No memory limit increase. CRC table is fixed read-only data.

`ip-frames` adds `paint_order` with 32 fields, retaining old `spatial` and `spatial_latency` field positions. First three are current sequence/locator/free page gauges; `writes/writeBytes` count completed whole order/free-page writes (not failed partial writes or physical disk I/O). `reused/splits/merges/borrows/rootCollapses/remaps` record actual successful page operations/logical remaps even if a later operation invalidates the generation. `spliceAttempts/Batches/Failures` count mutation phase, successful complete batches and failed mutation phase; `inserted/removed` count IDs in completed batches only. `spliceNs/MaxNs` includes mutation and overlay finalization, including failure; excludes classification. `rankSteps/locatorSteps` count query path visits (cached ranks need none); `maintenanceReads/Hits` count worker maintenance page-cache events. `validationChecks/Rejected/Ns/MaxNs` describes bounded classification; `largeBatches/noCompatibleBase/overlayPreflight` distinguish rebuild reasons. `rankCacheShifted/Dropped/Seeded` count shifted entries, removed-key cache invalidations and insert seeding, not eviction due to collision. All non-gauge counters cumulative, not transactional snapshots. Compare deltas in the same process. Query success histogram remains in `spatial_latency`. Neither callback time nor scratch I/O indicates actual presentation or physical storage latency.

## Host verification

See [observed output](M04_COUNTED_ORDER_HOST_RESULTS.txt). Final strict GCC C++20 suites: 26 seeded direct runs (78,000 randomized operations; 404,194 mixed rank/full-structure checks), 26 paired spatial runs (39,000 edits; 118,092 ordered visibility comparisons), 16 multi-gap runs (9,600 batches and exact comparisons; 9,445 incremental, 155 spatial compactions, zero classification rejects). Direct audit traverses all reachable pages and free list independently; checks minimum occupancy, uniform depth, parent links, subtree counts, locator separators, exact ordered sequence, exact ID-to-leaf map and page ownership. It also drains to empty, regrows, and checks random ranks. Cascade at N=16,384 and 65,536, 160 forced split/merge cycles plateau at 114,688 bytes, borrow paths. 93 partial-write injection sites across clustered and scattered split/merge cases, actual kernel partial-write failure, 10 charged allocation sites, exact disk-capacity denial, semantic checksum-valid count/separator faults, checksum/truncation/cancellation/overflow. ASan/UBSan with leak detection on standalone direct/paired suites. No independent external specialist review or full-app sanitizer/TSan certification.

Production native/JDK bridge, existing document oracle and 7 document allocation failures, existing input/scheduler/ownership tests and 100 additional planner repeats passed. Mesa llvmpipe production GLES2 in both MAX and fallback coverage branches: 256 exact matched-plan vs exact resident RGBA frames each (512 total), including long-stroke continuity, viewport crossing, translucent middle replacement/insertion, 8 undo/redo cycles, context recreation, document switching and fault fallback. Middle-splice fixture verifies no index rebuild and a visible change, followed by exact recovery. Host fixture acknowledges simulated save queue between rounds; not a SQLite persistence test. Android ABI declaration shims and host JNI were used for graphics; not an Android NDK compile. A combined host command hit timeout after 100 planner repetitions; the graphics commands were rerun separately on final source and both passed. Early development iterations exposed cold-rank cost and excess locator write amplification; grouped locator writes, compact nodes, batched ranks and bounded rank-cache repair were retested. Earlier fixture save-queue exhaustion was corrected in the host harness, not by changing production storage.

## Measured scale and limitations

Fixed 256 visible base IDs, 100 insert/remove cycles at each N=1k/10k/100k, one reusable overlay key, 600 total membership splices and exact post-splice queries. No rebuilds in this workload. Final host run at 100k: splice p50 118.769 us, p95 1508.194 us, max 2421.001 us, page writes p50/p95/max 8/72/137; post-insertion query p50 58.608 us, p95 92.229 us, max 2678.869 us. The distinct warm query test at 100k measured median 5.909 us, first process-cache miss 2724.067 us, six spatial pages/query. Cache repair substantially reduced repeated post-edit queries in this fixture but cold misses and branch reparenting still have nontrivial tails; no claim of the final sparse-rank timing budget for all workloads or tablet storage performance. Worst 137-page write batch is ~2.14 MiB of scratch writes; affected pages only, not N ranks. 100k bulk index occupied 45,432,832 bytes; edit workload peak 46,776,320 bytes. Existing 64 MiB cap is deliberately unchanged and does not admit arbitrary million-record whole-stroke indexes.

Current paired spatial overlay still triggers full rebuild of both roots after its bounded 256-key capacity is exceeded; this is explicitly NOT final affected-range spatial compaction. Large batches, unsupported reorder or corruption can also rebuild. Worker-local scratch pages are not yet immutable durable sidecar generations. Resident canonical/derived geometry and the exact linear fallback remain. The known 128 MiB resident mesh ceiling is unchanged. Canonical curve-span streaming, affected-range compaction, certified vector LOD, bounded residency and production Vulkan remain next. Full 90 Hz, physical storage and memory peaks, and extreme-zoom guarantees remain unverified. No new geometry seam detected in the tested fixtures; not a guarantee for future span splitting/LOD.

## Android acceptance procedure

Build with JDK17 (`:app:assembleDebug :app:testDebugUnitTest`), then isolated `m03Verify`. Use a disposable document with memory headroom (avoid the known resident mesh ceiling). Test overlapping translucent ink, erasing strokes in the middle, repeated undo/redo, rapid drawing and navigation, clear/undo, switch documents, rotate/background/reopen and persistence. Compare `paint_order` and `spatial` deltas around a deliberate erase/undo/redo sequence: successful inserted/removed and splice batches must increase, no unexplained mutation failures or rebuilds for small valid changes below overlay cap; indexed frames increase, no visual ordering/gap/opacity regressions. Verify idle counters unchanged after settling. No additional backend campaign. Checkpoint only after owner acceptance.

## Owner tablet verification

- Android build and unit-test tasks: PASS.
- Functional acceptance: 10/10 PASS.
- Fresh test document used.
- Active editing observation: 108.847 s.
- Splice attempts/successes: 44/44.
- Inserted/removed IDs: 22/22.
- Zero splice failures or validation rejections.
- Zero index rebuilds during marked capture.
- Logical order-page writes: 88 (1441792 bytes).
- Mean successful splice duration: 0.679 ms.
- Indexed frames: 394/438 (89.95%).
- Exact fallback frames: 44.
- Successful spatial queries: 44.
- Mean successful query duration: 49.56 us.
- Zero index I/O faults, invalid results or capacity failures.
- Native render P50/P95/P99:
  8.981/10.561/14.936 ms.
- Current scratch storage: 49152 bytes.
- Current dynamic index charge: 696 bytes.
- Cumulative peak dynamic charge: 40640 bytes.
- Durable/visible document revisions: 148/148.
- Idle: 11.266 s, zero rendering callbacks,
  queries, rebuilds or tree mutations.

Limitations:
- Tablet sequence and locator trees occupied one leaf each.
  Cascading structural operations were verified on host.
- Marked capture contained no camera requests.
- Allocation counters are not physical memory peaks.
- Actual 90 Hz presentation remains unverified.
- Canonical streaming, bounded spatial compaction,
  geometry residency and Vulkan integration remain open.

Counted paint ordering is functionally verified.
Proceed to canonical curve-span streaming.
