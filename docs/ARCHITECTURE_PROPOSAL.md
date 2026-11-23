# Proposed architecture — owner review only

Scope: the local M00 snapshot (branch/SHA unavailable; [inventory and evidence](M00_AUDIT.md)). Preserve the Java/GLES2 app as a comparison fixture. Do not promote its JNI ABI, graphics resources or CSV layout to product interfaces.

|Boundary|Ownership and proposed contract|
|---|---|
|Native Android shell|Kotlin shell per ADR-002; keep platform lifecycle, pointer arbitration and display scheduling here. Views vs Compose is open. UI owns neither fitted geometry nor GL objects.|
|Input and active ink|Normalize timestamped samples with pointer/tool identity and cancel semantics. Explicitly define coordinate space and units for filtering and width. Keep stable segments separate from replaceable tail. Avoid UI waits on GL or whole-stroke mesh generation; define bounded handoff, overload and cancellation behavior before threading changes.|
|Camera|CPU double transforms with explicit zoom limits. Subtract a nearby origin **before** converting to float. Derive viewport bounds conservatively including stroke width. Compare screen-space errors over large offsets; hierarchical coordinates require evidence and approval.|
|Document|Canonical editable vectors and monotonic drawing order; allocate stable identities independent of buffer names, indexes or file offsets. Commit or discard an active stroke once; derive meshes from canonical data. Begin in memory.|
|Visibility and geometry|Query ordered candidates via bounds references; preserve temporal order even across overlapping chunks. Bound tessellation by measured screen error; bound cache/staging memory and work per frame. Dense overlap still requires its own benchmark.|
|Presentation|One owner for context-bound GL resources; version snapshots, invalidate/rebuild after loss; render only for work. Keep the provisional GLES3 experiment separate from diagnostic GLES2. Compare low-latency/backend alternatives on identical scenes before any selection.|
|Durability and diagnostics|Storage behind a document abstraction; require versioning, round-trip and interrupted-write tests before choosing a format. Opt-in bounded telemetry, sample/frame correlation and offline replay independent of persistence.|

**First product journey (proposed):** open a local canvas → draw one pen stroke → lift → pan/pinch without changing ink → background and resume. Idle + stylus-down enters drawing; stylus-up commits; cancellation discards. Fingers navigate in the absence of active ink; during ink, ignore palm contacts rather than starting navigation. Test pointer transitions, rotation and interrupted input. Durable reopen belongs to M03, not a shell acceptance claim. Later: notebook → search/select → edit → export. Specify tablet layout, original design tokens, touch targets and accessibility in M06; do not infer product UI from the diagnostic toolbar.

Dependencies: source identity and compilable baseline before a shell migration; input/coordinate contracts before moving geometry off the hot path; document identity/order before indexing, persistence and cache policies. No migration, API change, backend switch or schema is authorized here.
