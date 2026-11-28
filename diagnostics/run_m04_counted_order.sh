#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
t=$(mktemp -d)
trap 'rm -rf "$t"' EXIT
${CXX:-c++} -std=c++20 -O2 -pthread -Wall -Wextra -Wpedantic -Werror -Iapp/src/main/cpp diagnostics/m04_counted_order_test.cpp -o "$t/order"
"$t/order" 13792 3000
"$t/order" multi 13792
"$t/order" cascades
"$t/order" faults
"$t/order" scale
if [ "${1:-}" = stress ]; then
    for seed in $(seq 1 24) 4294967295; do "$t/order" "$seed" 3000; done
    for seed in $(seq 1 15); do "$t/order" multi "$seed"; done
fi
