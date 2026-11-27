# M04.0 — proposal ready for written owner decision

On `feature/m04-scalable-renderer`, review [M04 proposal](M04_PROPOSAL.md) at `7cb54e5f3199ae47f1e9d85e3f8ad452a3b7aba5`. Inspected accepted main `d79105f52109970e807f3cabd35fda24a269a63c`; M03 tested app `d543ed4a0ab4dd0e760ae4ebdb5a616043f88a8f` remains the application evidence anchor. No app/build/harness changes or new test run. Both independent architecture reviews informed the proposal. ADR-003/004/006–010/012/013 standing unchanged.

**Next action:** owner reviews the final decision list, workload envelope and ordered checkpoints. Wait for written scope/authorization before implementation or device work. Dense/cold feasibility, finite v1 precision, physical peak and latency tails are open, not passes. Keep the two known untracked M02 presentation captures untouched.

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
