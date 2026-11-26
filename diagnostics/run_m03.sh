#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
exe=$(mktemp)
trap 'rm -f "$exe"' EXIT
c=app/src/main/cpp
c++ -std=c++20 -Wall -Wextra -Wpedantic -Werror -O2 -I"$c" diagnostics/document_test.cpp \
    "$c"/document/*.cpp -o "$exe"
"$exe"
c++ -std=c++20 -Wall -Wextra -Wpedantic -Werror -O2 -I"$c" diagnostics/document_failure.cpp "$c"/document/*.cpp -o "$exe"
"$exe"
if [ "${1:-}" = scale ]; then
    c++ -std=c++20 -Wall -Wextra -Wpedantic -Werror -O2 -I"$c" diagnostics/document_scaling.cpp \
        "$c"/document/*.cpp -o "$exe"
    grep -E 'Mem(Total|Available)' /proc/meminfo
    lscpu | grep 'Model name'
    # Never start the million-record probe with less than 1 GiB currently available.
    if awk '/MemAvailable:/{exit !($2>=1048576)}' /proc/meminfo; then set -- million; else set --; fi
    ulimit -v 786432
    /usr/bin/time -v "$exe" "$@"
fi
