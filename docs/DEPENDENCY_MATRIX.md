# M00 dependency and licensing matrix

Local snapshot, no branch or SHA. Sources: `settings.gradle`, root and app `build.gradle`, wrapper scripts/JAR/properties, manifest, `app/src/main/cpp/CMakeLists.txt`, host scripts and archive comparison in [M00 audit](M00_AUDIT.md).

|Component|Declared or inspected|Audit implication|
|---|---|---|
|Build resolution|One `:app`; Google, Maven Central, Gradle Plugin Portal (plugin resolution); AGP 8.6.1|No external app-library `dependencies` block, Kotlin plugin, version catalog or lockfile. Build-tool transitives still exist. Offline run used existing machine caches, not a clean download test.|
|Wrapper|Gradle 8.7 `-bin.zip`, HTTPS, no `distributionSha256Sum`; bundled JAR SHA-256 `497c8c2a7e5031f6aa847f88104aa80a93532ec32ee17bdb8d1d2f67a194a9c7`|JAR inherited from corrected ZIP; not the [8.7 wrapper JAR digest](https://services.gradle.org/distributions/gradle-8.7-wrapper.jar.sha256). Wrapper can launch 8.7; inventory/pin wrapper and distribution separately before CI.|
|Java/Android|Java 17 source/target; compile/target 35, min 28. `com.infinipaint.demo` v3 `0.3-renderfix`|Tested with Oracle JDK 17.0.8 instead of PATH Java 25.0.2. No runtime library beyond platform/native components declared. API 34 timestamp path and fallback both need coverage.|
|Native build|NDK `30.0.16248370`; CMake minimum 3.22.1; C++20; `ip_tablet` shared library links `GLESv2` and `log`|Exact NDK installed; SDK CMake 3.22.1 available. JNI, NDK libc++ and platform GL/driver required. No external geometry or storage package declared.|
|Host diagnostics|C++20 compiler/standard library; smoke needs JDK JNI headers, local GLES2/log substitutes; shell scripts assume POSIX tools|Not Gradle tests. A passing mock does not exercise Android, shaders or a driver.|
|Distribution rights|No project-wide LICENSE or third-party notice inventory; legacy README explicitly grants no project license; Gradle wrapper includes Apache notices|Do not infer redistribution rights for source, archives or artwork. Before release: establish ownership/license, inventory shipped native/runtime components and notices, verify dependency hashes and build SBOM.|
