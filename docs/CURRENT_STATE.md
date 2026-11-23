# Current state — Android baseline stabilization

M00 is approved as an audit. Its no-checkout finding describes the earlier snapshot. The M00.5 repair on `fix/android-baseline` is based on `568163c0f984f024731f8a32cb129865aed690d9`, verified against GitHub `refs/heads/main` at stabilization time.

The local repair restores the four renderer-owned frame-timing accumulators and resets them together after every 120 callbacks. No stylus, filtering, spline, renderer API, native code, storage or Gradle configuration changed. See the [stabilization handoff](../reports/LATEST.md) for verification and remaining limits; the [M00 audit](M00_AUDIT.md) remains historical evidence.

M00.5 accepted after owner tablet testing; tested commit `8bca6fc0be92f8f3fd0f8ad169d80c48e7442914` was pushed without force to `origin/fix/android-baseline`. See [session handoff](SESSION_HANDOFF.md) for automated results, owner observations, limitations and next actions. No merge into `main` or M01 start authorized.
