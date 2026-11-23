# Reference tablet acceptance checklist

Record device model, Android version, GPU, refresh rate, build commit, and test timestamp.

- [ ] App launches and displays its diagnostic canvas
- [ ] Stylus draws; one-finger and two-finger gesture semantics match approved behavior
- [ ] Palm contact does not interrupt stylus unexpectedly
- [ ] Live ink does not visibly trail excessively
- [ ] Zoom/pan preserves stroke alignment and existing stroke geometry
- [ ] Loops, sharp corners, dots, round caps and pressure transitions are visually inspected
- [ ] App resumes after backgrounding and rotation without crashes
- [ ] No continuous redraw while idle (verify with instrumentation)
- [ ] Record p50/p95/p99 frame timing, upload bytes and input-to-present telemetry where available
- [ ] Run replay/smoke tests; distinguish host mock success from actual GPU validation

Never mark a device test PASS unless it was actually run.
