#!/bin/sh
# Build and run port/recomp/tests/guest_heap_test.cpp (Linux, no game files needed).
set -e
cd "$(dirname "$0")/.."
${CXX:-c++} -std=c++17 -m64 -DFNV_GUEST_BASE_RUNTIME -Wall -o "${TMPDIR:-/tmp}/guest_heap_test" \
    port/recomp/tests/guest_heap_test.cpp port/recomp/guest_heap.cpp
"${TMPDIR:-/tmp}/guest_heap_test"
