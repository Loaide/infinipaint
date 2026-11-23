# InfiniPaint agent contract

On session start, read the engineering contract and `docs/SESSION_HANDOFF.md`. Compare the expected branch and full SHA with `git branch --show-current` and `git rev-parse HEAD`; inspect staged, unstaged and untracked paths with `git status -sb -uall`. For a later HEAD, verify `git merge-base --is-ancestor <anchor> HEAD` and inspect `git diff --name-status <anchor> HEAD`. Carry application test evidence forward unchanged only for documentation-only descendants. Stop on wrong branch, divergence or unexplained changes; do not auto-switch or clean. If the handoff is missing or contradictory, reconstruct from Git, state and latest report before acting; ask if authorization is unclear.

Use the handoff as an index: open relevant state, ADRs, evidence and affected code only. Before code edits, check `docs/CURRENT_STATE.md` and the applicable decisions in `docs/DECISIONS.md`.

Source of truth: verified repository HEAD plus approved ADRs. Historical ZIPs are reference artifacts, not proof of implementation. Record branch, full SHA and inspected files in every substantial report. Never claim tests ran without output evidence.

## Workflow
1. Audit the current source and dependencies; reconcile prototype and documentation.
2. Define task acceptance criteria, baseline and affected modules.
3. Implement a coherent vertical slice on a task branch.
4. Run targeted tests, regression suite and device tests when applicable.
5. Invoke independent reviewer; fix findings or document blockers.
6. At milestone completion or a material change of gate, refresh `docs/SESSION_HANDOFF.md` and reconcile state/report before stopping. Follow the handoff checklist in `docs/ENGINEERING_CONTRACT.md`.
7. Stop at milestone gate; request owner approval. Do not merge/push without approval.

## Nonnegotiables
- Vector document is authoritative; GPU meshes and caches are derived.
- No full active-stroke re-upload per sample, no all-document per-frame scans.
- Stylus feedback path is latency-sensitive and separate from background work.
- Preserve temporal stroke order across spatial culling and batching.
- CPU double camera, camera-relative GPU floats; quantify precision limits.
- Bound caches, queues, allocations, telemetry and memory.
- No invented timestamps for same-time stylus samples.
- Never replace real GPU tests with mock-only claims.
- Do not change renderer API, storage schema or architectural ADR silently.
- After two unsuccessful symptom patches, stop and produce root-cause analysis.
- No continuous idle redraw unless required by animation or pending work.
- No copying proprietary app assets or source.

## Context efficiency
Use symbol search and narrow reads. Don't dump whole files without need. Keep `docs/CURRENT_STATE.md` and `reports/LATEST.md` concise and accurate. Review diffs rather than whole chat histories. Limit concurrent agents editing the same subsystem.
