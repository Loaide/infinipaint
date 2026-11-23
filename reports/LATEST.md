# Latest handoff — Android debug baseline stabilization

M00 approved as an audit. **fix/android-baseline** was initially clean at **568163c0f984f024731f8a32cb129865aed690d9**, verified against remote main. That SHA identifies the parent of the M00.5 repair, not the repair itself. To identify the repair commit from this handoff: `git log -1 --format=%H --fixed-strings --grep="Restore Android frame timing window counters" fix/android-baseline`.

**Changed files:** `app/src/main/java/com/infinipaint/demo/MainActivity.java` (anonymous `GLSurfaceView.Renderer` fields and report-window reset), `docs/CURRENT_STATE.md`, `reports/LATEST.md`. No native code, stylus dispatch, filters, spline, shader, renderer API, build settings or document changes.

**Timing verification:** the renderer instance starts with zeroed `long` nanosecond sums and `int` counts. Only GL draw callbacks access them; the existing UI → GL request timestamp remains `volatile`. Exactly 120 callbacks contribute to the native mean; the queue mean uses only valid nonnegative delays under 100 ms and reports `-1` with no samples. Logging precedes reset of all four fields. Idle gaps need not discard a partial window. Independent read-only performance review found no blocking issue. A temporary JDK 17 harness extracted the actual field declarations and aggregation block, replacing Android Log with capture; two synthetic 120-callback windows passed (60 qualifying 3 ms delays, then none; native means 2 ms then 1 ms; no premature report, counters zero after each). This tested arithmetic, not Android scheduling.

**Android commands:** Windows PowerShell, `JAVA_HOME=C:\Program Files\Java\jdk-17`, `ANDROID_HOME=%LOCALAPPDATA%\Android\Sdk`, wrapper 8.7, SDK 35, NDK `30.0.16248370`, CMake 3.22.1:

```powershell
.\gradlew.bat --console=plain clean :app:assembleDebug :app:externalNativeBuildDebug :app:testDebugUnitTest :app:lintDebug
# Second build: separate git archive of HEAD with only the modified MainActivity.java overlaid
.\gradlew.bat --offline --no-build-cache --console=plain :app:assembleDebug
```

**Working-tree build:** `BUILD SUCCESSFUL` (3m 43s, 58 tasks executed, one up-to-date). Javac, DEX, APK packaging and native compilation/linking succeeded; no additional compile repairs were needed. `:app:testDebugUnitTest NO-SOURCE` (zero Android JVM tests). `:app:lintDebug` completed with **0 errors, 7 warnings**: DiscouragedApi (1), ObsoleteSdkInt (1), ClickableViewAccessibility (2), SetTextI18n (3). Javac also noted deprecated API use. Report: `app/build/reports/lint-results-debug.html`. APK: `app/build/outputs/apk/debug/app-debug.apk`, SHA-256 `7dc73ae18637f2ac99b63840bd79623adcee8a87cd369631ad3f6a8b939e3e5e`. Both builds had identical APK entry names; only the four native library entries differed in decompressed bytes. Repeatable compilation verified, **not** binary determinism.

**Isolated rebuild:** the second command succeeded (42 tasks executed). SHA-256 of that debug APK: `81d14a6ff904d26946221dd195ce8ee9d4642b9bd64856bf6162d0ae6c555951`. `apksigner verify --verbose` verified its v2 signature. The APK contained `classes.dex`, the manifest and `libip_tablet.so` for arm64-v8a, armeabi-v7a, x86 and x86_64. Offline here means using installed SDK components and cached dependencies, not a cold network bootstrap or a claim of byte-identical builds.

**Independent host run:** WSL2 x86_64/GCC 15.2, [M00 replay/smoke commands](../docs/M00_AUDIT.md) with `T=/tmp/ip-stabilization.sIYenE`. Normal fixture: 120 accepted, zero failures; equal-time fixture: 70 accepted, zero failures. QA checked all timestamps in order, all 35 ties and finite numeric outputs. Renderer mock passed (`incremental_bytes=2112`), using the pinned NDK's JNI header. An initial shell quoting failure occurred before executing tests; retry via Bash stdin succeeded.

**Final review:** all seven lint warnings are outside the telemetry diff: `AndroidManifest.xml:1` flags the explicit orientation attribute (`unspecified`, not a portrait lock); `MainActivity.java:506` tests API ≥23 despite minSdk 28; line 638 flags both lack of a `performClick` override on the canvas and lack of a click dispatch in its listener; lines 673, 684 and 706 hardcode `InfiniPaint`, `Mode 0` and `Clear`. No immediate correction required for the debug compilation repair. Review accessible canvas actions, localization and tablet orientation behavior before release. The cause of the native payload differences between builds was not established.

**Installation artifact:** `D:\DOWNLAODS\hello\InfiniPaint\app\build\outputs\apk\debug\app-debug.apk` (working-tree build hash above).

**Limits and gate:** `adb devices -l` listed no devices. No installed-app, real shader, visual, lifecycle, frame presentation, or stylus-to-photon result. The asynchronous request timestamp remains approximate. [M00 findings](../docs/M00_AUDIT.md) outside this narrow repair remain open. Local three-file repair only; no merge, push or M01 work.
