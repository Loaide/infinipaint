# Current handoff — M04 canonical span streaming, first slice

Inspected `4b0710b3e9f099d86aa1ce6973f4964f7a3a3dc7` on owner branch `feature/m04-scalable-renderer`. [Scope, evidence and acceptance](../reports/M04_CANONICAL_STREAM.md). Rolling v1 production replay and durable single-record ordinal-range reader implemented; Host tests, Android instrumentation and tablet acceptance passed. Then integrate persistent span bounds/checkpoints and actual viewport demand with revision-compatible roots. No new GPU backend work.

# Current handoff — M04 counted B+ paint ordering

Inspected `bdcc417f5b0d66716986d89833446ea4b9d82a4f`, branch `feature/m04-scalable-renderer`. [Scope, tests and acceptance](../reports/M04_COUNTED_ORDER.md). Counted sequence and ID locator integrated in existing planner, with bounded validated splices and exact rendering. Final host tests, Android build and tablet acceptance passed. Next: canonical curve-span streaming. Preserve known resident mesh limit and later canonical streaming/residency obligations. No Vulkan cutover or backend rerun.

# Current handoff — M04 production spatial access

Inspected owner archive `107b6fb6c1de2535afe8eebda3d8f5d64a3b73b6`, branch `feature/m04-scalable-renderer`. [Scope, telemetry and acceptance](../reports/M04_SPATIAL_ACCESS.md). New derived index and rank pages are integrated in the production resident planner; strict host tests passed. Android build and tablet acceptance passed. Next: counted paint ordering and canonical streaming. No backend rerun. Do not treat whole-stroke page lookup as completed canonical/geometry streaming or change approved architecture. Previous M04.3 handoff follows.

# M04.3 — backend selected; production execution authorized

`feature/m04-scalable-renderer`, inspected `2af1a8fc4a5df3e777027d4a6c946f00515a159e`. Owner selected native Vulkan with compact instancing for the reference tablet. Preserve GLES2/v1 and ADR-013. See [selection and fidelity requirement](../reports/M04_BACKEND_GATE.md). Next: isolated publication and editing, not another diagnostic campaign. No device authorization.

## M04.3-C — Bounded execution scheduling (functionally verified)

Source `115390a56a89907e0053b4d8a060129c73c2d428`. Ordered multi-gesture ink admission, paused prefix recovery, resident visibility planner, bounded delta and retirement transfer. See [scope, tests and acceptance](../reports/M04_3_EXECUTION_SCHEDULING.md). Android build and 11/11 tablet checks passed; exact resident fallback remains until spatial streaming. No Vulkan migration.

## M04.3-B — Render publication isolation (functionally verified)

Owner archive `6018eac32e31f094a6ce6b74c9d7a4b328c07b60`, branch `feature/m04-scalable-renderer`. Separate publication port and immutable live pages implemented; GL no longer acquires mutation mutex. Prior latch retained/retried on publication contention. See [report and exact limits](../reports/M04_3_RENDER_PUBLICATION.md). Host mock, ownership, concurrency and Mesa pixel tests passed; Android build and 9/9 tablet checks passed. No production Vulkan migration, no full M04.3 acceptance. Next: M04.3-C bounded admission, scheduling and retirement.

## M04.3 incremental publication checkpoint (functionally verified)

See [M04.3 incremental report](../reports/M04_3_INCREMENTAL.md). Camera mailbox and ordered ink lane are in the production GLES2 application; frame planning still uses the existing shared lock. Host tests, Android build and 10/10 tablet functional checks passed. See report for active timing and remaining limitations. No production Vulkan migration.

## Previous selection request (historical)

`feature/m04-scalable-renderer`, inspected `f25961daf457aafae5d0732b58dbe751878e8fe3`. Owner-reported benchmark source `8f9b53044d240f413f10f69a61aafe35fa02c350`, local APK/archive hashes verified; exact installed-image/source linkage not independently proven. Preserve benchmark checkpoint, host timeout (`ab0541a`) and captures. [Gate decision request and targeted validation](../reports/M04_BACKEND_GATE.md); [compact evidence](../reports/M04_BACKEND_EVIDENCE_COMPACT.md). M03 tested application unchanged. Next: owner disposition of native Vulkan + compact instances and outstanding capacity/lifecycle/thermal/peak checks. Do not repeat the campaign, start migration or enter M04.3. ADR-006 remains provisional; ADR-013 unchanged.

## Prior M04.2 handoff

Branch `feature/m04-scalable-renderer`; inspected and measured WIP `610202e00b912ea0a889a01a9fa2b2972bdc9f8c` descends from `3590abe26167693cbce06f1720e580c2a89b8442`, diagnostics only. [M04.2 findings, limits and decision](../reports/M04_2_DATA_ACCESS.md) · [host output](../reports/M04_2_HOST_RESULTS.txt). Accepted M03 app `d543ed4a0ab4dd0e760ae4ebdb5a616043f88a8f` unchanged. Owner accepted HOST_02: retain v1/GLES2; subpixel and temporal obligations open, no more AA trials now. M04.2 was authorized for host feasibility only. Full validation remains required; 65,536 samples/record admission, dot-heavy 100k maximum, unbounded index builds and missing physical peaks constrain the result. ADR-003/004/006–010/012/013 unchanged.

**Next action:** owner reviews M04.2 and authorizes a matched-device comparison plan for the **highest reliably supported GLES version vs native Vulkan**, with GLES2 as compatibility reference. Stop **before** production GPU resource and scheduling architecture. No device actions or backend migration without explicit authorization; no production index approved. Preserve the two known untracked M02 captures.

## Prior checkpoint — HOST_02

On `feature/m04-scalable-renderer`, proposal `7cb54e5f3199ae47f1e9d85e3f8ad452a3b7aba5` approved in principle with [four binding conditions](M04_PROPOSAL.md). Inspected checkpoint `96757ba2497274da9a176ef2d524c9964427699b`; host probe and conditions at `c870534a38ba3f5c2f9b604c89931c30b07f10b9`. [Measured host results and limits](../reports/M04_HOST_01.md). M03 tested app `d543ed4a0ab4dd0e760ae4ebdb5a616043f88a8f` unchanged. ADR-003/004/006–010/012/013 standing unchanged.

Owner authorized only the focused host coverage comparison. Inspected `0eec300a10e72cc55131d4fac6cb6ea364ef44e9`; measured sources `9576bbfd9c786984fbcd1df68515f973dd54b9dc`. [Second checkpoint](../reports/M04_HOST_02.md): shared samples preserve ordering/correlation but fixed 8×8 fails accuracy, continuity and v1 compatibility. No production change.

That host-only data probe was subsequently authorized and completed above. Dense/cold feasibility, telemetry overhead bound, finite v1 precision, physical peak and latency tails remain open.

## M03 accepted — prior handoff

M03 engineering closure and written owner acceptance are complete.

Tested application:
d543ed4a0ab4dd0e760ae4ebdb5a616043f88a8f

See [formal acceptance](../reports/M03_ACCEPTED.md) and
[engineering closure](../reports/M03_ACCEPTANCE.md).

Do not repeat M03 testing without a specific defect or evidence
that invalidates an existing result.

Next milestone: M04 scalability architecture and implementation
under its own approval and verification gates.

---

## Preserved historical record

### M03 — physical campaign complete; written acceptance pending

Closure checkpoint: owner tested the isolated app on SM-X626B. On `feature/m03-document-engine`, inspected HEAD `cb814c49e583c32a96c8481f95a616c9564ad06f` descends from the tested `d543ed4a0ab4dd0e760ae4ebdb5a616043f88a8f`; intervening changes were documentation only. Local archive `captures/m03-owner-gate-d543ed4.zip` and [SHA-256 inventory](../reports/M03_EVIDENCE_SHA256.txt) preserve the raw windows, device preflight, Android tests, APKs, host timing and Comprehensive output. Nothing installed or changed on the tablet during reconciliation. No repeat of the physical campaign. Next: owner disposition of the limitations in the acceptance report, not merge or M04.

**Current tested application:** `d543ed4a0ab4dd0e760ae4ebdb5a616043f88a8f` on `feature/m03-document-engine`. P1 logical admission correction only: staged clone, GL-held patch, live upload reservation and canceled-buffer ownership are charged until released. Follow-up review found premature completed-mesh release; the charge now outlives vector destruction and cache-node deallocation. A deterministic host fixture probes admission immediately before the first outgoing mesh free. See [latest evidence and artifact identities](../reports/LATEST.md). This is not physical-memory or milestone acceptance.

Prior candidate `d8a804a15e2e123cca26eec6ef54854e4fada3b0`; blocker recorded at `8e4948712305a1936883ea8c4124dba2362fd84f`. ADR-013 and its six clarifications remain approved; no new architecture decision. For a later HEAD verify ancestry and inspect the diff before reusing evidence.

[Latest report](../reports/LATEST.md): host core, fault suite, mock/Mesa, Android tests, physical campaign and residuals. ADR-013 unchanged. Keep `captures/m02-accepted-8b5c24d/`, both root M02 presentation captures and interrupted patch intact.
