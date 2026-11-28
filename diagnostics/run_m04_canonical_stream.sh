#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
t=$(mktemp -d)
trap 'rm -rf "$t"' EXIT
c=app/src/main/cpp
JAVA_HOME=${JAVA_HOME:-/usr/lib/jvm/java-21-openjdk-amd64}
${CXX:-c++} -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror -pthread -I$c diagnostics/m04_canonical_stream_test.cpp "$c/document/codec.cpp" "$c"/input/*.cpp "$c/stroke/spline.cpp" -o "$t/stream"
"$t/stream" 13792 30
"$t/stream" faults
"$t/stream" scale
if [ "${1:-}" = stress ]; then
    for seed in $(seq 1 40) 4294967295; do "$t/stream" "$seed" 30; done
fi
${CXX:-c++} -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror -pthread -fPIC -shared -Wl,--no-undefined \
    -I$c -I"$JAVA_HOME/include" -I"$JAVA_HOME/include/linux" diagnostics/m04_canonical_jni_stub.cpp \
    "$c/tablet/store_jni.cpp" "$c"/document/*.cpp "$c"/input/*.cpp "$c/stroke/spline.cpp" -o "$t/libip_tablet.so"
"$JAVA_HOME/bin/javac" --release 17 -Xlint:all -Werror -d "$t" app/src/main/java/com/infinipaint/storage/StorePort.java diagnostics/m04_canonical_jni_test.java
"$JAVA_HOME/bin/java" -Xcheck:jni -Djava.library.path="$t" -cp "$t" com.infinipaint.storage.CanonicalJniHostTest
