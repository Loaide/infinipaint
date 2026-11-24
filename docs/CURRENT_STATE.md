# Current state — M01 owner-test candidate

M00.5 was accepted on tablet and merged into `main` at `db36eaba697ef46beb14f77e396405d7af20cf65`. The owner authorized M01 on the Java/C++/GLES2 baseline; this is not a new production backend decision.

M01 is locally implemented and host-verified: one production navigation camera, pointer-ID gesture handling, saved view settings, context-specific resource initialization, on-demand frame counters, JVM tests and Android CI configuration. Only one diagnostic canvas may own the process-wide native state. See the [checkpoint and next action](SESSION_HANDOFF.md) and [test evidence](../reports/LATEST.md).

No M01 tablet result or remote CI run yet. Documents remain in-memory, GPU coordinates remain world floats, and large-scene performance is unmeasured. No merge, push or later milestone is authorized.
