# InfiniPaint agent contract

Read `docs/ENGINEERING_CONTRACT.md`, `docs/CURRENT_STATE.md`, `docs/DECISIONS.md` and relevant `docs/` before code edits. If a file is missing, create a proposed version during M00 and mark it unapproved.

Source of truth: verified repository HEAD plus approved ADRs. Historical ZIPs are reference artifacts, not proof of implementation. Record branch, full SHA and inspected files in every substantial report. Never claim tests ran without output evidence.

## Workflow
1. Audit the current source and dependencies; reconcile prototype and documentation.
2. Define task acceptance criteria, baseline and affected modules.
3. Implement a coherent vertical slice on a task branch.
4. Run targeted tests, regression suite and device tests when applicable.
5. Invoke independent reviewer; fix findings or document blockers.
6. Update docs, write concise handoff with branch/SHA, test commands/results and limitations.
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
