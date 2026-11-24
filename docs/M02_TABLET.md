# M02 tablet gate (not executed)

No install, clear, downgrade or modification of the owner's installed application is authorized. Obtain written approval for the exact candidate APK before replacing it; protect existing drawings and exports. Use an empty disposable canvas **only with permission**. Record Git SHA, APK SHA-256, SM-X626B build fingerprint, pen, display resolution, actual refresh mode, battery/power and thermal state. Keep smoothing, zoom, viewport, camera and refresh matched for comparisons.

### Install only after separate owner approval

Use PowerShell from the repo root; select the authorized serial shown by `adb devices -l`. Compare the local hash with the **hardened** application entry in [LATEST](../reports/LATEST.md). Stop on mismatch, incompatible signature, an unexpected device, or unprotected work; do not uninstall or clear data to work around an error. A package update restarts the process and in-memory ink may be lost even with `-r`.

```powershell
$apk = (Resolve-Path app/build/outputs/apk/debug/app-debug.apk).Path
Get-FileHash -Algorithm SHA256 $apk
adb devices -l
$serial = Read-Host 'Approved tablet serial'
adb -s $serial shell getprop ro.product.model
adb -s $serial shell getprop ro.build.fingerprint
# Only after confirming approval, identity, hash and protected work:
adb -s $serial install -r $apk
adb -s $serial shell am start -W -n com.infinipaint.demo/.MainActivity
adb -s $serial logcat -d -s IP_COVERAGE
```

Record actual refresh setting and active display mode (`adb -s $serial shell dumpsys display`) before and during the runs; a requested refresh rate is not proof of an actual presentation interval. Verify `alpha bits=8` (or higher), record `max_blend` and repeat this window-surface check after EGL recreation. A black/blank window is a failure, not an idle-rendering pass.

1. Repeat **all 17** accepted [M01 checks](../reports/M01.md), including actual EGL context recreation, transformed camera, pointer arbitration/cancellation and the [idle/active capture procedure](M01_FRAME_CAPTURE.md). Recheck that the device actually runs the candidate after rotation/background/resume. No stale VBOs, jumps or stuck tips.
2. Capture the same physical input in all display modes by drawing **once**, switching raw/One-Euro/spline/overlay on the retained drawing, and exporting the input audit. Record controlled slow handwriting (small counters, crosses, intentional cusps), fast signatures, isolated dots, pressure ramps both ways, stationary presses/releases, reversals, tight loops, sharp joins, and overlapping strokes. Distinguish raw jitter from meaningful features; check legibility, endpoint lag, lost corners and pressure blots. Do not score by raw RMS alone.
3. In each mode compare the last stationary frame before lift to the first finished frame (screen recording if available). Compare a light long line with a firm stationary press at its tip: earlier ink should not widen. Try pressure drop while the filter catches up. Check dot fringes, subpixel short strokes, joins at sparse vs dense spacing, intersections and layering. Pan and inspect from minimum through maximum zoom. Record visible faceting or darker seams; the error target is at **capture** zoom.
4. Draw one continuous moving stroke for at least two minutes; also press and dwell. Keep the latest physical tip visible. Record sample counts, `IP_PEN_UP` wait/hold, VBO payload/reallocations, first finished frame and memory. Compare early/middle/late portions and repeated lifts. Note stalls, thermal throttling and dropped/rejected samples. Test stylus interruption by a lifecycle transition.
5. Fresh `ip-frames-mark` for idle, camera, writing and repeated lifts. Dump at the end of each interval: `recent_ns` holds nearest-rank p50/p95/p99/max for at most **512** matched waits, native calls and callback bodies per series since mark. Report sample counts and elapsed durations. Idle must have no requests or callbacks over a settled 30 s. Record any unmatched platform callbacks; do not generate frames just to fill a histogram.
6. When permitted and supported, capture the same active intervals with Android System Tracing/Perfetto **FrameTimeline** and SurfaceFlinger; select the application surface and report actual present-interval p50/p95/p99/max, missed deadlines and refresh changes. Record unavailable tracks as missing, not zero. JNI wall time, callback time, GL payload and presentation are distinct. Physical pen-to-photon requires an independently synchronized high-speed observation.

Attach evidence and failures per check. No shipping-quality or latency acceptance until the owner evaluates the actual tablet; no automatic merge or backend change.

**Hardening probes:** At a subpixel camera offset compare a sparse and densely traced straight line at constant pressure, a smooth pressure ramp, tight V, crossing and an exact retrace. Look for a bright internal seam or a pulse as the provisional tip becomes stable. Distinct strokes must still composite in temporal order. If `max_blend=0`, inspect a shallow diagonal for coverage banding and record composite-mode cost (up to 16 mask submissions per part). Pan a just-finished stroke offscreen and back, switch modes, cancel and restart a stroke, then recreate the surface. Compare to the original drawing at the same zoom: no missing prefix, stale tip or resurrection after Clear. The first completed draw may need a full upload after a skipped live frame or context loss.
