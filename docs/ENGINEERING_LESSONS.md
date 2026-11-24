# Verified engineering lessons

- A four-knot fit is not enough to guarantee linear cumulative work: measure allocator traffic of the **retained** arrays too. Distinguish total requested bytes from peak live storage and inspect capacity-jump maxima as well as p99.
- Test geometry at a tied timestamp by ordinal knot, not just by time. A time-only query cannot identify several distinct positions. Retain all samples, including stationary pressure changes; a dwell can deposit ink locally without widening the incoming segment.
- Shader math needs pixel evidence. Use the Mesa pbuffer for deterministic readback, mock GLES for transfer/resource accounting and the tablet for driver and presentation claims. In particular, source-over AA at capsule intersections need not equal a single union coverage.

- `:app:testDebugUnitTest` can report `NO-SOURCE` while the build succeeds. Check executed JUnit cases; label a cached task result as reused rather than freshly executed.
- `GLSurfaceView` callback timing excludes swap and presentation. In M01, the stable idle check used both request and callback deltas; zero requests alone does not rule out platform draws. See [capture commands](M01_FRAME_CAPTURE.md).
- Resizing a viewport does not necessarily create a context. GLES names belong to a context; discard stale names after recreation instead of deleting them in the new context. M01 mocks cover this contract; the owner separately confirmed actual recovery ([evidence](../reports/LATEST.md)).
- On the installed OpenCode 2.4.1, a leading blank line before agent YAML caused the body to load but silently omitted model and permission frontmatter. Confirm effective agent fields with `debug agents`; never remove an active agent while editing it.
