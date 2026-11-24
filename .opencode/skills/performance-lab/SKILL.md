---
name: performance-lab
description: Establish baseline on named hardware and commit; deterministic 1k/10k/100k/1M scenes and dense-overlap stress; collect p50/p95/p99, Perfetto traces, memory and upload bytes.
---
# performance-lab

Choose scene sizes appropriate to the approved milestone. Record baseline and candidate SHA, named hardware, scene seed, refresh mode, thermal state and warmup. Use `docs/M01_FRAME_CAPTURE.md` for app counters; use Perfetto/FrameTimeline where applicable for presentation. Keep callback time, presentation, GPU uploads and physical pen latency separate. Record sample counts and p50/p95/p99 where captured; mark missing metrics unmeasured.
