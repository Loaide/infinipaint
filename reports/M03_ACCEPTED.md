# M03 — Accepted with documented limitations

## Owner acceptance — 2026-11-26

"I formally accept M03 application commit
d543ed4a0ab4dd0e760ae4ebdb5a616043f88a8f
with all documented limitations and deferred engineering
obligations. Proceed with final documentation and integration."

## Identity

- Branch: feature/m03-document-engine
- Tested application: d543ed4a0ab4dd0e760ae4ebdb5a616043f88a8f
- Engineering closure checkpoint: 2c8b8ec
- Device: Samsung SM-X626B, Android 16
- Target refresh rate: 90 Hz

## Accepted evidence

- Android instrumentation: 11/11 passed.
- Manual drawing: 11/11 passed.
- Manual editing: 14/14 passed.
- Manual persistence: 13/13 passed.
- Four controlled process-kill recovery boundaries passed.
- Warmed SQL edit scaling from 1,000 to 100,000 records passed.
- Idle rendering: 90.423 seconds with zero requests or callbacks.
- Continuous stroke: 57,956 samples, six owner checks passed.
- Controlled writing presentation: 90.095 FPS.
- Controlled navigation presentation: 90.103 FPS.
- Both controlled presentation windows had zero estimated
  skipped refresh intervals.
- Settled process PSS returned from 448828 KiB to 277048 KiB
  after reopening the smaller document.

## Accepted limitations and deferred obligations

- Measured transactional pen-up overhead exceeds the original
  host regression investigation threshold.
- Historical append/splice latency outliers remain unresolved.
- The observed 40.798 ms native-rendering maximum is retained.
- Original presentation gaps remain part of the evidence.
- Physical restoration and backlog memory peaks are not fully
  measured. Logical accounting is not physical GPU residency.
- Rotation temporarily changes viewport centering.
- Physical unfinished-stroke cancellation remains unverified.
- Remaining storage-failure and concurrency coverage is retained.
- Physical pen-to-photon latency has not been measured.
- All other limitations in M03_ACCEPTANCE.md remain binding.

Acceptance does not convert unverified criteria into passes.

## Forward obligations

M04 must address spatial indexing, sparse drawing-order queries,
bounded residency, streaming, adaptive geometry and large-scale
rendering performance.

Retain latency tails and physical memory peaks as explicit
scalability investigation items.

Address rotation centering during camera/workspace development
and remaining fault coverage during hardening.

ADR-013 and previously approved architecture remain unchanged.

## Evidence

- M03_ACCEPTANCE.md
- M03_EVIDENCE_SHA256.txt
- captures/m03-owner-gate-d543ed4.zip

The ZIP is a local ignored artifact, not a Git-backed remote backup.

M02 acceptance remains unchanged.
No application or instrumentation source changes are introduced
by this acceptance record.
