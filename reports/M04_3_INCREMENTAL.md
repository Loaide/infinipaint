# M04.3 incremental execution checkpoint — candidate, not accepted

Source supplied by owner: `feature/m04-scalable-renderer`, HEAD `0dcffac7cb783ab91e36082ac954287c2e6d5613` plus interrupted working-tree changes in `m04-3-interrupted.zip`. Remote main `d79105f52109970e807f3cabd35fda24a269a63c`; local HEAD not available via remote. Preserve all existing gate evidence. Production Vulkan migration has not begun.

## Scope

The interrupted implementation adds the native latest-view mailbox, frozen DOWN mapping and smoothing preset, const completed-geometry handles, frame head/epoch/revision and patch capture, and a bounded single-consumer Java ink lane. This incremental correction preserves those changes. It moves retained-prefix retry/discard onto that same lane, collects historical UP samples before the final sample, makes accepted historical prefixes request redraw even if a later sample cannot be admitted, rechecks document-menu actions at selection, allows camera navigation during edits on the current document (not during document switches), and exposes queue occupancy/high-water/rejection counts in `ip-frames` output. Existing GLES2/v1, SQLite and canonical ink semantics unchanged. No change to approved scalable vector architecture.

## Verified here

- OpenJDK 17-compatible standalone test (`javac --release 17`): parked consumer, FIFO exactly once, terminal reserve at saturation, bounded rejection counters, 100 ring reuse cycles and idle interrupt. PASS.
- GCC C++20 standalone test: 100,000 concurrent latest-view publications, coherent snapshots, monotonic sequence, frozen capture value, viewport generation. PASS.
- Python syntax compilation of interrupted `scripts/verify_m04_backend.py` and `diagnostics/run_m04_host.py`. PASS (not execution of those programs).

## Residual risks and next step

Android SDK/NDK compilation and on-device integration not run in this environment. Do not accept the checkpoint until local build and focused device checks. Existing renderer still takes `stateMutex` while latching scene and copying live geometry; camera publication has its own small mutex, but presentation can still wait. A single outstanding stroke is admitted until terminal UI acknowledgement, so rapid successive pen-down may be explicitly rejected; this is not acceptable as a final handwriting throughput guarantee. Queue cap is 256 pending runnable entries (including reserved terminal slot), not a complete byte-credit ledger; Java capture allocations and diagnostic export remain to be measured/bounded. Native suspend and lifecycle operations still have synchronous waits. No independent render-planning lane, complete scene-root versioning, actual presented-view tracking or GPU retirement redesign yet. No million-stroke scalability or 90 Hz/latency claims for this implementation. Next: validate this increment, then remove renderer dependence on expensive mutable state, implement multi-stroke ordered admission with explicit credits, stale-work rejection and resource lifetime controls. Do not repeat backend campaign.

## Owner tablet verification

- Android build and unit-test tasks: PASS.
- Functional tablet checks: 10/10 PASS.
- Idle: 16.178 s, zero frame requests and callbacks.
- Active handwriting: 26.547 s, 5239 stylus requests.
- Ink queue: high-water 7/256, zero rejections,
  zero pending operations at capture end.
- Native render P50/P95/P99: 9.154/11.466/12.163 ms.
- 90 Hz presentation performance remains unverified.
- Shared renderer lock and stalled-worker multi-stroke
  admission remain outstanding.

Incremental functional checkpoint verified.
Full M04.3 acceptance remains pending.
