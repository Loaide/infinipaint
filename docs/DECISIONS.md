# Decision register — M00 reconciliation, no new approvals

The [full historical ADRs](../references/legacy-documentation/docs/DECISIONS.md) are preserved verbatim in the extracted v0.1.0 documentation, verified byte-for-byte against its ZIP. That file controls the wording, rationale and revisit conditions. This index does not supersede it.

|Historical standing|Decisions|What M00 found|
|---|---|---|
|Approved|001 independent Android; 002 Kotlin/native Android; 005 stylus first|Independent implementation required; Java diagnostic is not approval to replace Kotlin. Live stylus/finger arbitration needs device tests.|
|Approved|003 canonical vector document; 012 defer specialized allocators/storage|Retained traces and display meshes exist, but no editable durable document. No schema selected.|
|Approved principles|004 CPU doubles / camera-relative floats; 008 spatial references separate from temporal order|Float-world GLES2 and a linear traversal are prototype limitations, not exceptions for production.|
|Provisional|006 GLES3 first renderer experiment|GLES2 mock coverage does not validate GLES3 or select a shipping backend. Revisit with driver data and identical workloads.|
|Approved|007 Tab S10 FE+ reference; 009 no fixed 10⁸× gate; 010 physical RAM only|Device configuration and usable precision/memory range still require measurement.|
|Approved|011 Git and documentation as source of truth|Local snapshot is identifiable by reference bytes but has no Git identity.|

Pre-M01 update: repository identity is verified and M00.5 compilation repair accepted (see [handoff](SESSION_HANDOFF.md)). Acceptance of the diagnostic baseline does not select a production architecture. Kotlin shell transition and JNI boundaries remain pending; later decisions require evidence before selecting a renderer, coordinate hierarchy, persistence format, UI framework, cache budgets or LOD tolerances. [Proposal](ARCHITECTURE_PROPOSAL.md); [evidence](M00_AUDIT.md).
## Approved M03 decision

[ADR-013: document edits and durable revisions](M03_PROPOSAL.md). Owner approved proposal `1b71523179ce6c66b619f07dae1ea80e5755955e` with binding additions recorded there. Android platform SQLite; implementation and device acceptance are separate gates.

## Open decision — M04 graphics backend (not an approved ADR)

M04.2 accepted for feasibility only. [Selection gate](../reports/M04_BACKEND_GATE.md) open: measure highest reliably supported GLES against native Vulkan and retain GLES2/v1 compatibility. ADR-006 remains provisional. Host build evidence is not tablet capability or a shipping API choice. Owner approval required before production GPU resource/scheduling design, backend migration or M04.3.
