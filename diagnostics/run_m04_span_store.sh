#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
t=$(mktemp -d)
trap 'rm -rf "$t"' EXIT
c=app/src/main/cpp
${CXX:-c++} -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror -pthread -I$c diagnostics/m04_span_store_test.cpp "$c/document/document.cpp" "$c/document/codec.cpp" "$c"/input/*.cpp "$c/stroke/spline.cpp" -o "$t/span"
"$t/span" "${1:-24}"
"$t/span" faults
"$t/span" scale
