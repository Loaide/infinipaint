# Engineering contract (proposed; approval required)

Product: original offline-first Android vector infinite canvas, stylus-first drawing, fingers for navigation, editable documents, tablet productivity workspace. The user seeks practical extreme zoom, crisp strokes and efficient navigation of million-stroke documents. These are acceptance goals, not established performance.

## Verified historical baseline
- Documentation archive v0.1.0: native Android/Kotlin (ADR-002), vector-first document (ADR-003), CPU double camera + camera-relative GPU float (ADR-004), offline-first, bounded resource budgets.
- Corrected reference Android test: Java application, JNI/C++, One-Euro filter, incremental spline builder (centripetal Catmull-Rom with Hermite representation), diagnostic OpenGL ES rendering, host replays and mock GLES smoke tests.
- GitHub source tree not audited. M00 must determine whether it is the intended product repo before modifying app code.

## Proposed architecture (not approved)
Android UI/interaction -> input normalization -> incremental spline -> canonical vector document + low-latency preview -> spatial query -> adaptive geometry -> graphics backend -> presentation. Persistence and diagnostics crosscut. Evaluate GLES low-latency and Vulkan before selecting production renderer.

## Quality gates
No 'done' without tests, device evidence when needed, benchmark comparison, review and updated docs. Maintain visual goldens, adversarial geometry tests, deterministic replay, lifecycle and recovery tests. Separate simulation measurements from physical stylus-to-photon claims.

## Milestone approval
At completion and after owner acceptance, the lead replaces the short snapshot in `docs/SESSION_HANDOFF.md` (aim for one screen). Include milestone and approval gate; branch and full tested/inspected SHA; test results, provenance and limits; open risks; applicable approved/provisional ADRs; next authorized actions. Distinguish the tested application SHA from later documentation commits. Link detailed evidence, never paste transcripts. Reconcile with `docs/CURRENT_STATE.md` and `reports/LATEST.md`. On resume verify Git identity/status, inspect only relevant differences and follow evidence links as needed. A handoff records approvals; it does not grant them.

M00 Audit; M01 integrated app shell; M02 ink; M03 document and persistence; M04 large-scale renderer; M05 extreme zoom; M06 productivity UI; M07 hardening. Each gate requires written acceptance and owner approval.
