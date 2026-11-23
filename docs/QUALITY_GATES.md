# Quality gates
- M00: verified tree/branch/SHA, inventory, dependency/license review, reference comparison, risks and approved baseline.
- M01: clean debug build, launch on tablet, camera pan/pinch/lifecycle tests, CI unit suite.
- M02: replay raw/filter/spline, same-timestamp cases, pressure, sharp turns, cap/join goldens, measured input and frame timings.
- M03: stable IDs, ordering, undo/redo, storage round-trip, interruption/crash recovery, corruption handling.
- M04: 1k/10k/100k/1M distributions plus dense-overlap adversarial cases; viewport culling, cache budgets, memory and frame percentiles.
- M05: precision error budget at documented zoom/coordinate limits; no LOD popping or missing geometry.
- M06: UI workflow tests, stylus ergonomics, accessibility, import/export.
- M07: thermal, lifecycle, long sessions, data integrity, performance regression, signed release checks.

Report per run: device, OS, refresh rate, build SHA, scene seed, sample count, p50/p95/p99 frame times, jank, CPU/GPU memory, GPU uploads, visible stroke count, screenshot evidence and failures. Proposed targets require calibration on hardware.
