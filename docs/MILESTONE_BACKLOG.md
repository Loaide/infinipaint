# Prioritized integrated backlog — proposal, not execution authority

Baseline: no local branch/SHA; see [M00 evidence](M00_AUDIT.md). Each row is gated on the previous row and separate owner approval; preserve earlier regression suites.

|Order|Milestone and owner decision|Exit evidence|
|---|---|---|
|P0|M00 gate: identify/authorize authoritative Git baseline and narrow compile repair or supply known-good source. Keep history/ZIPs distinct.|Branch + full SHA mapped to reviewed bytes, owner decision recorded, reproducible debug APK before calling baseline operational.|
|P0|M01 shell and camera: approve Java diagnostic vs Kotlin product boundary; establish CI, context/lifecycle policy and camera/input tests.|Clean wrapper build, unit tests, tablet launch/pan/pinch, rotate/resume/context loss, idle redraw count and baseline capture.|
|P0|M02 ink: normalize MotionEvents, handle equal/regressing times and cancellation, bound append/commit work, test geometry between knots.|Deterministic adversarial replay, dots/caps/joins/pressure goldens, per-sample and pen-up scaling, on-device presentation percentiles. Revisit ADR-006 at minimal stroke renderer + target-driver evidence, before backend coupling grows; record comparable workloads or reason to defer.|
|P0|M03 document: stable IDs/order, editing and undo/redo, approve persistence ADR before format implementation.|Save/load equivalence, atomic recovery, corruption handling and interruption suite.|
|P1|M04 scale: spatial queries, order-preserving batches, bounded cache, streaming and adaptive geometry; reevaluate renderer with measurements.|Seeded sparse/clustered/dense-overlap 1k/10k/100k/1M scenes, visibility and compositing oracle, memory/transfer/draw and frame distributions on named hardware.|
|P1|M05 practical extreme zoom: quantify cancellation and local-coordinate precision before changing coordinate scheme.|Document supported range and pixel-error budget, no missing strokes or LOD popping against goldens.|
|P2|M06 workspace: validate notebook/search/select/edit/export journeys and stylus-friendly tablet layout, text/shapes/images.|Workflow and accessibility tests, screenshot regression and export fidelity.|
|P2|M07 hardening: licensing, reproducible signed release, recovery and long-session testing.|Device matrix, thermal/pressure soak and release checklist with unresolved failures disclosed.|
