#!/bin/sh
# NDK_HEADERS: NDK toolchain sysroot/usr/include (only portable headers used).
set -eu
cd "$(dirname "$0")/.."
: "${NDK_HEADERS:?set to NDK sysroot/usr/include}"
t=$(mktemp -d)
trap 'rm -rf "$t"' EXIT
c=app/src/main/cpp
cp "$NDK_HEADERS/jni.h" "$t/"
build() { c++ -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror -I"$c" "$@"; }
build diagnostics/live_input_test.cpp "$c"/input/*.cpp "$c"/stroke/spline.cpp -o "$t/input"
"$t/input"
build diagnostics/replay.cpp "$c"/input/*.cpp "$c"/stroke/spline.cpp -o "$t/replay"
for f in diagnostics/synthetic*.csv; do "$t/replay" "$f" "$t/$(basename "$f")"; done
build -I"$t" -Idiagnostics/test_support diagnostics/renderer_smoke.cpp "$c/native_bridge.cpp" \
  "$c"/input/*.cpp "$c/stroke/spline.cpp" -o "$t/smoke"
"$t/smoke"
"$t/smoke" bench
c++ -std=c++20 -O2 -I"$c" diagnostics/spline_scaling.cpp "$c/stroke/spline.cpp" -o "$t/scaling"
"$t/scaling" --check
mkdir "$t/android"
cp diagnostics/test_support/android/log.h "$t/android/"
cp -R "$NDK_HEADERS/EGL" "$NDK_HEADERS/GLES2" "$NDK_HEADERS/KHR" "$t/"
# GLVND libGL dispatches these common GL calls to the current EGL GLES context.
build -DEGL_NO_X11 -I"$t" diagnostics/egl_ink_test.cpp "$c/native_bridge.cpp" \
  "$c"/input/*.cpp "$c/stroke/spline.cpp" -l:libEGL.so.1 -l:libGL.so.1 -o "$t/pixels"
EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1 MESA_GLES_VERSION_OVERRIDE=2.0 "$t/pixels"
