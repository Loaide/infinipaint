#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
c++ -std=c++20 -O2 -Iapp/src/main/cpp diagnostics/replay.cpp \
  app/src/main/cpp/input/input_processor.cpp app/src/main/cpp/input/one_euro_filter.cpp \
  app/src/main/cpp/stroke/spline.cpp -o diagnostics/replay
./diagnostics/replay "$1" "${2:-replay_points.csv}"
