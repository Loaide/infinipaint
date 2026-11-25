# Prioritized integrated backlog — proposal, not execution authority

Historical baseline investigation: [M00 evidence](M00_AUDIT.md). M00.5, M01, W01 and M02 are accepted. Each subsequent milestone requires separate owner approval; preserve earlier regression suites.

|Order|Milestone and owner decision|Exit evidence|
|---|---|---|
|P0|M00 gate: identify/authorize authoritative Git baseline and narrow compile repair or supply known-good source. Keep history/ZIPs distinct.|Branch + full SHA mapped to reviewed bytes, owner decision recorded, reproducible debug APK before calling baseline operational.|
|P0|M01 shell and camera: approve Java diagnostic vs Kotlin product boundary; establish CI, context/lifecycle policy and camera/input tests.|Clean wrapper build, unit tests, tablet launch/pan/pinch, rotate/resume/context loss, idle redraw count and baseline capture.|
|Complete|M02 live ink: owner accepted tested `44f559b0d38c6ec0b194132df01569c7ee9bafec` with documented limits.|[Automated geometry/input/scaling and physical acceptance, including presentation percentiles](../reports/LATEST.md). Physical dots inconclusive; ADR-006 comparison deferred for lack of matched backend workloads.|
|Active: decision gate|M03 start authorized; [ADR-013 proposal, plan and acceptance matrix](M03_PROPOSAL.md) awaiting approval before implementation.|Ordered editing, integrated save/reopen and pixels, recovery/fault suites, matched scaling measurements, APK and physical owner gate.|
|P1|M04 scale: spatial queries, order-preserving batches, bounded cache, streaming and adaptive geometry; reevaluate renderer with measurements.|Seeded sparse/clustered/dense-overlap 1k/10k/100k/1M scenes, visibility and compositing oracle, memory/transfer/draw and frame distributions on named hardware.|
|P1|M05 practical extreme zoom: quantify cancellation and local-coordinate precision before changing coordinate scheme.|Document supported range and pixel-error budget, no missing strokes or LOD popping against goldens.|
|P2|M06 workspace: validate notebook/search/select/edit/export journeys and stylus-friendly tablet layout, text/shapes/images.|Workflow and accessibility tests, screenshot regression and export fidelity.|
|P2|M07 hardening: licensing, reproducible signed release, recovery and long-session testing.|Device matrix, thermal/pressure soak and release checklist with unresolved failures disclosed.|
