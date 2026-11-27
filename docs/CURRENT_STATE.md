# Current project state — M03 accepted

- M00, M00.5, M01 and M02: accepted.
- M03: formally accepted with documented limitations.
- M04: [direction approved in principle with conditions](M04_PROPOSAL.md); HOST_02 findings accepted; [M04.2 bounded host data feasibility](../reports/M04_2_DATA_ACCESS.md) complete. No production implementation.
- M05, M06 and M07: upcoming.

See [M03 formal acceptance](../reports/M03_ACCEPTED.md).

M04.2 accepted for feasibility only. The [matched backend gate](../reports/M04_BACKEND_GATE.md) recommends native Vulkan with compact instancing; owner decision pending. Capacity, lifecycle recovery, sustained thermal behavior and peak memory are unproven. No production migration or M04.3.

---

## Preserved historical record

### Current state — M03 implementation, not accepted

The reviewed parked-GL/scene-switch and canceled-buffer admission defect is corrected at `d543ed4a0ab4dd0e760ae4ebdb5a616043f88a8f`. Host tests cover rollback, retirement, failed upload and context loss; physical memory peaks remain unknown. See the [report](../reports/LATEST.md).

M00.5, M01, W01 and M02 are accepted. Integrated local `main`: `8b5c24dbafbfbb78123c22ab819cd581897a2245`; tested M02 app `44f559b0d38c6ec0b194132df01569c7ee9bafec`, verified ancestor with documentation-only delta. The [accepted report](../reports/M02_ACCEPTED.md) retains physical evidence and limitations (including inconclusive dots and pressure irregularity).

The owner approved [ADR-013 and its binding clarifications](M03_PROPOSAL.md). `feature/m03-document-engine` is at the isolated Android verification gate. The prior pen-up threshold finding remains open; it was not remeasured for the P1 correction. Android storage, process recovery, actual memory and physical acceptance remain unverified. See [handoff](SESSION_HANDOFF.md) and [engineering report](../reports/LATEST.md). No M03 owner acceptance.
