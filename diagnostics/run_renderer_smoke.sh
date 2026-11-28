#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
JAVA_HOME=${JAVA_HOME:-/usr/lib/jvm/java-21-openjdk-amd64}
c++ -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror \
  -Idiagnostics/test_support -I"$JAVA_HOME/include" -I"$JAVA_HOME/include/linux" \
  -pthread -Iapp/src/main/cpp diagnostics/renderer_smoke.cpp \
  app/src/main/cpp/native_bridge.cpp app/src/main/cpp/document/*.cpp \
  app/src/main/cpp/input/one_euro_filter.cpp \
  app/src/main/cpp/input/input_processor.cpp \
  app/src/main/cpp/stroke/spline.cpp -o /tmp/infinipaint_renderer_smoke
/tmp/infinipaint_renderer_smoke
/tmp/infinipaint_renderer_smoke owners
/tmp/infinipaint_renderer_smoke isolation
c++ -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror -pthread -Iapp/src/main/cpp diagnostics/m04_live_prefix_test.cpp -o /tmp/infinipaint_live_prefix
/tmp/infinipaint_live_prefix
