# M00 — Baseline audit (do not implement features yet)

This directory is a new clean GitHub baseline containing the corrected Android test app plus OpenCode scaffolding. The original reference archives are under `references/`, with historical documentation extracted under `references/legacy-documentation/`.

1. Record the current Git branch and full SHA. If there is no commit yet, explicitly say so.
2. Inventory the actual Java, JNI, C++, GLES, input, spline, diagnostic and Gradle files. Identify what is only a test/prototype.
3. Compare the integrated prototype with the historical documents and ZIP. Do not silently import outdated claims.
4. Run build/tests where toolchain permits; record exact commands, outcomes, environment and blockers.
5. Create `docs/M00_AUDIT.md`, `docs/ARCHITECTURE_PROPOSAL.md`, `docs/DEPENDENCY_MATRIX.md`, `docs/RISK_REGISTER.md`, and a prioritized integrated milestone backlog.
6. Keep the existing working Android prototype operational. Propose, but do not implement, renderer/storage/API changes.
7. Stop for owner approval. Do not push, merge, delete files, or advance to M01.
