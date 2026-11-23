---
name: repo-audit
description: Inventory git branch/SHA, source tree, build scripts, languages, dependencies, tests, licenses and history.
---
# repo-audit

Inventory git branch/SHA, source tree, build scripts, languages, dependencies, tests, licenses and history. Compare historical ZIPs without merging blindly. Detect desktop-vs-Android mismatch. Produce evidence-based gap matrix and stop for approval.

## Mandatory workflow
1. Read AGENTS.md, relevant docs, current branch/SHA and only affected files.
2. Establish acceptance criteria and baseline.
3. Perform bounded implementation or review within approved milestone.
4. Run relevant tests; distinguish executed from proposed.
5. Report changed files, evidence, limitations and ADR implications.
6. Update documentation and stop at milestone approval gate.
