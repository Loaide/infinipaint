# Verified engineering lessons

- `:app:testDebugUnitTest` can report `NO-SOURCE` while the build succeeds. Check executed JUnit cases; label a cached task result as reused rather than freshly executed.
- `GLSurfaceView` callback timing excludes swap and presentation. In M01, the stable idle check used both request and callback deltas; zero requests alone does not rule out platform draws. See [capture commands](M01_FRAME_CAPTURE.md).
- Resizing a viewport does not necessarily create a context. GLES names belong to a context; discard stale names after recreation instead of deleting them in the new context. M01 mocks cover this contract; the owner separately confirmed actual recovery ([evidence](../reports/LATEST.md)).
- On the installed OpenCode 2.4.1, a leading blank line before agent YAML caused the body to load but silently omitted model and permission frontmatter. Confirm effective agent fields with `debug agents`; never remove an active agent while editing it.
