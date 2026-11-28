# M04 — Integrated span access and local compaction candidate

Baseline `9db632b4662da8113273d95eb0abca6d901d9156`. [Scope, limitations and host evidence](../reports/M04_SPAN_STREAMING.md): persistent per-record derived span pages, viewport-driven exact resident vertex-range pruning and bounded coarse-tree compaction. Host candidate, Android pending. Package 1 is not complete: global span roots, persistent paired manifest and nonresident viewport retrieval remain open. No new architecture approval or backend change.

# M04 — Canonical span streaming (first slice, functionally verified)

Counted ordering checkpoint `4b0710b` is owner-accepted. [Canonical replay and targeted range reader](../reports/M04_CANONICAL_STREAM.md) functionally verified; Android and tablet acceptance passed. Production reconstruction is rolling and bounded; full viewport streaming, paired span roots, compaction, LOD and residency remain open. No budget or backend change.

## Current M04 increment — counted paint ordering (functionally verified)

Baseline `bdcc417f5b0d66716986d89833446ea4b9d82a4f`. [Production counted B+ sequence and locator](../reports/M04_COUNTED_ORDER.md) replace absolute rank pages. Valid bounded middle splices no longer force a rebuild; paired spatial overlay compaction, canonical streaming, residency and full engine gates remain open. Final host suites passed; Android build and 10/10 tablet checks passed.

## Current M04 increment — spatial access (functionally verified)

Baseline `107b6fb6c1de2535afe8eebda3d8f5d64a3b73b6` includes functionally verified M04.3-C. [First production spatial access slice](../reports/M04_SPATIAL_ACCESS.md) implements ephemeral packed pages, bounded BVH overlay and sparse paint-rank lookup in the existing planner. Host tests, Android build and 12/12 tablet checks passed. Canonical/mesh streaming, counted sequence tree, certified LOD, residency and Vulkan cutover remain open.

# Current project state — M03 accepted

- M00, M00.5, M01 and M02: accepted.
- M03: formally accepted with documented limitations.
- M04: [direction approved in principle with conditions](M04_PROPOSAL.md); HOST_02 findings accepted; [M04.2 bounded host data feasibility](../reports/M04_2_DATA_ACCESS.md) complete. Production M04.3 work in progress; [renderer isolation candidate](../reports/M04_3_RENDER_PUBLICATION.md) host and tablet functionally verified.
- M05, M06 and M07: upcoming.

M04.3-C [functionally verified scheduling implementation](../reports/M04_3_EXECUTION_SCHEDULING.md): multi-gesture admission, bounded resident planning and CPU retirement. Android build and 11/11 tablet checks passed. Exact resident fallback remains; full M04.3 and scalability gates open.

See [M03 formal acceptance](../reports/M03_ACCEPTED.md).

Backend selection closed by owner: native Vulkan + compact instances for SM-X626B. M04.3 authorized; retain GLES2/v1. Diagnostic capacity, sustained pacing and three same-backend recovery stages passed, not production certification. Landscape fidelity and physical peaks remain open.

---

## Preserved historical record

### Current state — M03 implementation, not accepted

The reviewed parked-GL/scene-switch and canceled-buffer admission defect is corrected at `d543ed4a0ab4dd0e760ae4ebdb5a616043f88a8f`. Host tests cover rollback, retirement, failed upload and context loss; physical memory peaks remain unknown. See the [report](../reports/LATEST.md).

M00.5, M01, W01 and M02 are accepted. Integrated local `main`: `8b5c24dbafbfbb78123c22ab819cd581897a2245`; tested M02 app `44f559b0d38c6ec0b194132df01569c7ee9bafec`, verified ancestor with documentation-only delta. The [accepted report](../reports/M02_ACCEPTED.md) retains physical evidence and limitations (including inconclusive dots and pressure irregularity).

The owner approved [ADR-013 and its binding clarifications](M03_PROPOSAL.md). `feature/m03-document-engine` is at the isolated Android verification gate. The prior pen-up threshold finding remains open; it was not remeasured for the P1 correction. Android storage, process recovery, actual memory and physical acceptance remain unverified. See [handoff](SESSION_HANDOFF.md) and [engineering report](../reports/LATEST.md). No M03 owner acceptance.
