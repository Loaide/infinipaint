# Current state — Android baseline stabilization

M00 is approved as an audit. Its no-checkout finding describes the earlier snapshot. The M00.5 repair on `fix/android-baseline` is based on `568163c0f984f024731f8a32cb129865aed690d9`, verified against GitHub `refs/heads/main` at stabilization time.

The local repair restores the four renderer-owned frame-timing accumulators and resets them together after every 120 callbacks. No stylus, filtering, spline, renderer API, native code, storage or Gradle configuration changed. See the [stabilization handoff](../reports/LATEST.md) for verification and remaining limits; the [M00 audit](M00_AUDIT.md) remains historical evidence.

Debug APK built from both the working tree and an isolated clean source copy; all four native ABIs compiled. Lint: 0 errors, 7 warnings, none blocking this repair. Gradle unit tests: no source. Both host replays and mock-GLES smoke passed. Tablet installation and launch remain unverified. Owner authorized a local commit only; M01 has not started.
