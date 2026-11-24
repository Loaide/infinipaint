---
name: milestone-closure
description: Reconcile verified evidence and owner acceptance at a milestone gate.
---

Follow `AGENTS.md`; check branch and checkpoint with `scripts/engineering.ps1 -Mode State`. Review the diff from the tested SHA before carrying forward evidence. Update the handoff, state and latest report with evidence links and unresolved limits. Stop at the owner gate; this skill does not authorize publication, architecture changes or a new milestone.
