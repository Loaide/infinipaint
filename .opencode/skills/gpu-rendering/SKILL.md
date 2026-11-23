---
name: gpu-rendering
description: Audit GPU buffer uploads, draw calls, ordering, culling, context loss, shader compilation, cap/join AA and compositing.
---
# gpu-rendering

Audit GPU buffer uploads, draw calls, ordering, culling, context loss, shader compilation, cap/join AA and compositing. No full live stroke re-upload or raster-only zoom.

## Mandatory workflow
1. Read AGENTS.md, relevant docs, current branch/SHA and only affected files.
2. Establish acceptance criteria and baseline.
3. Perform bounded implementation or review within approved milestone.
4. Run relevant tests; distinguish executed from proposed.
5. Report changed files, evidence, limitations and ADR implications.
6. Update documentation and stop at milestone approval gate.
