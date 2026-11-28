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
/tmp/infinipaint_renderer_smoke canonical
/tmp/infinipaint_renderer_smoke owners
/tmp/infinipaint_renderer_smoke isolation
/tmp/infinipaint_renderer_smoke planner
c++ -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror -pthread -Iapp/src/main/cpp diagnostics/m04_live_prefix_test.cpp -o /tmp/infinipaint_live_prefix
/tmp/infinipaint_live_prefix
c++ -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror -pthread -Iapp/src/main/cpp diagnostics/m04_frame_scheduler_test.cpp -o /tmp/infinipaint_frame_scheduler
/tmp/infinipaint_frame_scheduler
t=$(mktemp -d)
trap 'rm -rf "$t"' EXIT
"$JAVA_HOME/bin/javac" --release 17 -Xlint:all -Werror -d "$t" app/src/main/java/com/infinipaint/demo/InkLane.java app/src/main/java/com/infinipaint/demo/InkPipeline.java diagnostics/m04_ink_lane_test.java diagnostics/m04_ink_pipeline_test.java
"$JAVA_HOME/bin/java" -cp "$t" com.infinipaint.demo.InkLaneHostTest
"$JAVA_HOME/bin/java" -cp "$t" com.infinipaint.demo.InkPipelineHostTest
