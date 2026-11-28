#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
t=$(mktemp -d)
trap 'rm -rf "$t"' EXIT
c++ -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror -Iapp/src/main/cpp diagnostics/m04_spatial_pages_test.cpp -o "$t/spatial"
"$t/spatial"
if [ "${1:-}" = stress ]; then
    for seed in 13792 $(seq 1 40) 4294967295; do "$t/spatial" "$seed" 1500; done
fi
