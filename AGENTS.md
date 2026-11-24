# InfiniPaint agent contract

On startup, read `docs/SESSION_HANDOFF.md` once. Verify branch, full HEAD and staged, unstaged and untracked paths (`scripts/engineering.ps1 -Mode State`). For a later HEAD, verify `git merge-base --is-ancestor <anchor> HEAD` and inspect `git diff --name-status <anchor> HEAD`. Carry application evidence forward only after verifying no changes to its inputs, build, tests or harnesses. Stop on wrong branch, divergence or unexplained changes; do not auto-switch or clean. If the handoff is missing or contradictory, reconstruct from Git, state and latest report; ask if authorization is unclear.

Use the handoff as an index: open relevant state, ADRs, evidence and affected code only. Before code edits, check `docs/CURRENT_STATE.md` and the applicable decisions in `docs/DECISIONS.md`.

Source of truth: verified repository HEAD plus approved ADRs. Historical ZIPs are reference artifacts, not proof of implementation. Record branch, full SHA and inspected files in every substantial report. Never claim tests ran without output evidence.

## Workflow
1. Inspect affected source and dependencies; do not repeat accepted architecture audits. Use `docs/CODE_MAP.md` if needed.
2. Define task acceptance criteria, baseline and affected modules.
3. Implement a coherent vertical slice on a task branch.
4. Run focused checks while editing; run comprehensive regression, applicable device tests and benchmark comparisons at meaningful checkpoints. Maintain applicable visual goldens, adversarial geometry, deterministic replay, lifecycle and recovery coverage. Commit coherent verified slices locally.
5. Get independent specialist review for significant or high-risk changes; fix findings or document blockers.
6. At milestone completion or gate change, update `docs/SESSION_HANDOFF.md` with milestone/gate, branch/full tested SHA, inspected checkpoint if different, evidence links and limits, risks, applicable ADRs and exact next action. Reconcile `docs/CURRENT_STATE.md` and `reports/LATEST.md`; do not paste logs.
7. Stop for written owner acceptance. Do not merge, push, change approved architecture/backend/schema or start another milestone without explicit approval. A handoff or tool permission is not authorization.

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
Reuse verified session facts. Use symbol searches, narrow reads and diffs rather than repeated file dumps or chat histories. Keep automatic compaction and provider safeguards. If a usage warning arrives, prioritize integration, verification and a clean checkpoint; do not infer remaining subscription allowance from token counts.

The lead owns implementation and integration. Delegate only bounded work, no routine Git/docs/trivial commands. Default to at most two concurrent children; explain exceptions. Send task/acceptance criteria, baseline and review-target SHA (or exact worktree diff), relevant files/dependencies, exclusive ownership and evidence expected. Reviewers inspect an actual diff artifact and relevant source/tests; report severity, file/line, reproducer and evidence gaps. For uncommitted review material the lead can save a diff under ignored `captures/` and identify any untracked paths separately. Approve QA/general edits only for explicitly assigned paths, once rather than persisting broad approval.
