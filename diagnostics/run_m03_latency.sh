#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
awk '/MemAvailable:/{exit !($2>=1048576)}' /proc/meminfo || { echo 'Insufficient physical memory'; exit 1; }
ulimit -v 786432
t=$(mktemp)
trap 'rm -f "$t"' EXIT
c=app/src/main/cpp
c++ -DIP_DOCUMENT_PHASE_PROBE -std=c++20 -O2 -Wall -Wextra -Werror -I"$c" diagnostics/document_latency.cpp "$c"/document/*.cpp -o "$t"
"$t"
