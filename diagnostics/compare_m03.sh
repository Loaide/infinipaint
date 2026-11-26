#!/bin/sh
# Matched sequential host bridge probes. Read the accepted sources directly from
# Git; never alter either worktree or the accepted APK. No device or disk-writer claims.
set -eu
cd "$(dirname "$0")/.."
: "${NDK_HEADERS:?NDK sysroot include required}"
t=$(mktemp -d); trap 'rm -rf "$t"' EXIT
base=44f559b0d38c6ec0b194132df01569c7ee9bafec
mkdir "$t/base"
git archive "$base" app/src/main/cpp diagnostics | tar -xC "$t/base"
cp "$NDK_HEADERS/jni.h" "$t/"
build() {
    root=$1;name=$2;shift 2
    c="$root/app/src/main/cpp"
    c++ -O2 -std=c++20 -I"$t" -I"$c" -I"$root/diagnostics/test_support" \
        "$root/diagnostics/renderer_smoke.cpp" "$c/native_bridge.cpp" "$c"/input/*.cpp "$c/stroke/spline.cpp" "$@" -o "$t/$name"
}
build "$t/base" base.bin
build . current app/src/main/cpp/document/*.cpp
lscpu | grep 'Model name'
for run in 1 2 3; do
    case "$run" in 1) modes="base off queue";; 2) modes="queue base off";; 3) modes="off queue base";; esac
    for m in $modes; do
        echo "run=$run mode=$m samples_per_size=101"
        case "$m" in base) "$t/base.bin" bench;; off) "$t/current" bench;; queue) "$t/current" bench-queue;; esac
    done
done
