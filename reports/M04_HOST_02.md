# M04 host checkpoint 2 — keep sample correlation, reject fixed 8×8 as a replacement

**Decision:** one shared 8×8 sample grid gives much better area estimates than v1 on the tested subpixel marks. It does **not** give a sufficiently accurate, continuous or v1-compatible production result. Keep v1 rendering, editing and recipe unchanged. Bounded paging feasibility can proceed independently **if authorized**: page the canonical data and conservative vector bounds, not sample masks, approximated opacity or a new appearance. This result is not a dense-scene throughput pass.

**Identity and reproduction.** `feature/m04-scalable-renderer`; inspected baseline `0eec300a10e72cc55131d4fac6cb6ea364ef44e9`; measured sources committed as `9576bbfd9c786984fbcd1df68515f973dd54b9dc`. Inspected `native_bridge.cpp` (`quad`, pressure mapping, `finishV1`, `reconstructV1`, coverage shader), `diagnostics/{egl_ink_test.cpp,run_m04_host.py,m04_host_probe.hpp,m04_coverage.hpp}`, current state, decision register, handoff and [prior checkpoint](M04_HOST_01.md). Ran `python diagnostics/run_m04_host.py --coverage` in VS x64 developer prompt, JDK 17, i5-6300U, `/O2`, RGBA8 128×64 EGL pbuffer, GL ES 2.0 SwiftShader 4.0.0.1 MAX. Exit 0, including analytic and convergence checks. No APK, device, backend or application edits.

### What was compared

The actual v1 renderer reconstructs each fixture from canonical samples. A hook in a disposable bridge copy reads the finalized spline quad descriptors; no shader, geometry, order or blend changes. All three methods use those same tapered segments/discs, so a difference tests **coverage and composition**, not a different curve fit. Error is relative to box-filtered ordered ink in the renderer's channel space, not a claim about display gamma or human perception.

1. **Candidate:** test the same 64 pixel-local sites for every primitive. A point belongs to a tapered capsule if the minimum squared distance minus squared radius along its center segment is nonpositive. OR membership within one stroke (a join/dwell does not deposit twice). At each site apply source-over for each containing **distinct stroke**, in link order. Average premultiplied RGB and opacity only after that composition.
2. **Reference:** intersect endpoint circles and their two external tangent segments with a horizontal slice. Sort all interval endpoints and pixel edges, classify each resulting interval per stroke, composite in order and integrate its exact length. Integrate vertically at 1,024 midpoint slices/pixel; halving to 512 changed no reported channel, union or opacity by more than **0.011/255** (empirical convergence, not a universal bound). Independent of candidate point tests. Analytic checks for centered, coincident, reversed and disjoint discs passed to 0.0001 absolute area/channel.
3. **Compatibility:** read actual v1 pixels on its opaque background. For a second rendering with identical geometry and opacity but white ink, infer optical opacity from `(outputRed−backgroundRed)/(255−backgroundRed)`; quantized estimate, not the framebuffer alpha (which remained 255). No v1 mask is assumed to be geometric area.

**Why per-stroke averages fail:** both radius-0.15 px dots fit inside one pixel. Let `A = π·0.15² ≈ 0.07069`. With two half-opaque coincident dots the true opacity is `0.75A`; if disjoint it is `A`. Blending two separately averaged opacities instead gives `A − A²/4` for **both**. Even exact single-disc area cannot recover their intersection. When coincident, the lower stroke contributes `0.25A` of its color, the upper `0.5A`; reversing them swaps those contributions. The candidate keeps this distinction, but still quantizes area to multiples of 1/64.

### Measured worst-pixel errors

Units are levels on a **0–255** scale; union = fraction of pixel covered by any geometric support, independent of paint opacity. Candidate RGB is transparent premultiplied; v1 RGB and candidate-v1 compare over the actual opaque background. Each entry is a maximum over the 12×10 ROI (all ink contained), all relevant channels and views, not a mean.

|Fixture (final primitives)|Candidate vs area: RGB / opacity / union|v1 vs area: RGB / inferred opacity|Candidate vs v1 RGB|
|---|---:|---:|---:|
|Centered dot (1)|1.04 / 1.04 / 2.09|72.20 / 73.94|73.22|
|Off-center dot (1)|2.94 / 2.94 / 5.88|44.20 / 45.27|41.33|
|Thin oblique capsule path (3)|1.17 / 1.17 / 2.33|53.01 / 54.29|52.44|
|Same path, rising pressure (3)|1.84 / 1.84 / 3.69|41.53 / 42.53|41.09|
|Coincident red then blue (2)|1.04 / 1.57 / 2.09|63.21 / 125.76|64.11|
|Reversed (2)|1.04 / 1.57 / 2.09|71.31 / 125.76|72.31|
|Separated, same pixel (2)|1.04 / 2.09 / 4.18|58.41 / 91.55|59.41|
|Bent, returning pressure stroke incl. join and terminal (12)|2.03 / 2.03 / 4.06|20.49 / 20.98|22.09|
|Join plus distinct dot, zoom sweep (13)|7.63 / 7.63 / 15.27|67.80 / 77.01|72.75|

**Continuity:** 33 evenly spaced views, zoom 0.1→0.2 around a fixed screen anchor, no level switch. Across adjacent views the maximum true RGB change was 3.66 levels. The maximum error in the **signed frame-to-frame change** was **13.82 levels** for the candidate versus **6.02** for v1, even though v1 had the larger absolute color bias. Sample sites crossing a long edge together can cause a visible step. This defeats treating this candidate as a certified imperceptible approximation. No physical motion or perceptual threshold was measured.

### Cost, memory and scope

CPU candidate evaluation of one 120-pixel ROI, final host run: centered dot 175 µs, taper 236 µs, join 808 µs; 33-view mean join+dot **908 µs/view**. Actual v1 SwiftShader warmed callback plus amortized completion (4 frames, one drain per batch), respectively 150, 160, 162 and **340 µs/view** over the 128×64 pbuffer with scissored coverage. These are small diagnostic wall timings, not matched GPU workloads or latency percentiles. They exclude fixture rebuild, correctness readback, reference integration and camera setup. No evidence of 90 Hz on hardware.

Candidate evaluates **7,680 pixel-sites** per ROI, up to 99,840 primitive predicates for 13 primitives before short-circuiting. Measured `sizeof` payloads: **4,800 B** candidate output tile (five doubles/pixel incl. diagnostic union), **40 B** per-site accumulator reused, **48 B** transformed descriptor per primitive (624 B for 13); no 64-image allocation. Not process peak: container overhead, oracle, comparison tiles, v1 and driver storage excluded. **GPU projection only:** a naive RGBA8 per-site color buffer is 256 B/pixel versus 4 B resolved; even a transient 16×16 block would need 64 KiB before other scratch. Serial site evaluation trades that storage for repeated geometry tests and ordered blends; no cost-free single scalar mask across strokes. Neither GPU strategy was built or timed.

**Owner gate:** accept this as a negative feasibility result, not a request to tune more sample patterns. Unresolved: a bounded, temporally stable dense coverage method, the appearance decision if area correctness conflicts with v1, and device cost. A separate bounded **validated-reopen/paging** experiment can use untouched v1 replay and hit/order oracles, measure cold read volume, validation time and working-set peak, and inject offscreen corruption before selection. It must preserve all M03 validation/transactions and take no dependency on this candidate; no approval here for major paging implementation or a revised integrity protocol.
