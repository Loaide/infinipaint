# Product and architecture brief

Product: original offline-first Android vector infinite canvas, stylus-first drawing, fingers for navigation, editable documents, tablet productivity workspace. The user seeks practical extreme zoom, crisp strokes and efficient navigation of million-stroke documents. These are acceptance goals, not established performance.

The accepted working baseline is Java/JNI/C++ with GLES2. For current implementation and evidence, use [session handoff](SESSION_HANDOFF.md). [DECISIONS.md](DECISIONS.md) indexes approved and provisional historical ADRs; this brief does not replace them.

## Proposed architecture (not approved)
Android UI/interaction -> input normalization -> incremental spline -> canonical vector document + low-latency preview -> spatial query -> adaptive geometry -> graphics backend -> presentation. Persistence and diagnostics crosscut. Evaluate GLES low-latency and Vulkan before selecting production renderer.

Permanent engineering rules and approval gates live in [AGENTS.md](../AGENTS.md). See [ROADMAP.md](ROADMAP.md) for milestone order.
