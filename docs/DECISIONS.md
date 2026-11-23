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

Pending owner decisions: repository and baseline provenance; authorization for a narrow build repair; Kotlin shell transition and JNI boundaries. Later decisions require evidence before selecting a renderer, coordinate hierarchy, persistence format, UI framework, cache budgets or LOD tolerances. [Proposal](ARCHITECTURE_PROPOSAL.md); [evidence](M00_AUDIT.md).
