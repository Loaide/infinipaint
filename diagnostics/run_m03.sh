#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
exe=$(mktemp)
trap 'rm -f "$exe"' EXIT
c=app/src/main/cpp
c++ -std=c++20 -Wall -Wextra -Wpedantic -Werror -O2 -I"$c" diagnostics/document_test.cpp \
    "$c"/document/*.cpp -o "$exe"
"$exe"
