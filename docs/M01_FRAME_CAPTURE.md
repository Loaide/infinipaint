# Tablet frame capture

With the debug APK installed, connect the unlocked tablet via USB. In PowerShell:

```powershell
adb shell getprop ro.product.model
adb shell getprop ro.build.fingerprint
adb shell am start -W -n com.infinipaint.demo/.MainActivity
Start-Sleep -Seconds 5
adb shell dumpsys activity com.infinipaint.demo/.MainActivity ip-frames-mark
Start-Sleep -Seconds 30
adb shell dumpsys activity com.infinipaint.demo/.MainActivity ip-frames
```

Leave the canvas visible and untouched, pen away, for both waits. Save the output,
tested Git SHA, APK hash and tablet refresh setting. Verify `mark_set`, then
`since_mark elapsed_ms` of at least 30000. Expect zero camera, stylus and other
requests **and zero callbacks** in steady idle. Record any nonzero callback
count even if there were no requests; repeat after settling to distinguish
a transient surface redraw from continuous rendering.

For active captures, set a fresh mark, perform one action, then dump `ip-frames`.
One-finger pan/two-finger pinch should increment `camera`; pen drawing should
increment `stylus`; tapping the mode button once should increment `other` once.
Check that callbacks advance after each. Set a new mark after rotation or Activity
recreation; `no_mark` means the idle interval cannot be evaluated.

Requests can coalesce. `withoutPending` counts draw callbacks with no observed
outstanding app request, not a definitive attribution to Android. The timing
line is cumulative per Activity; `sum_ns / samples` gives a mean when samples
is nonzero. Each first pending request is timed to the next observed callback,
without excluding long stalls. `native_render` times the Java-to-native render
call only. Neither measure includes the subsequent buffer swap or measures
display presentation or physical pen latency.
