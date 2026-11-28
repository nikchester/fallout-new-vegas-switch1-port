#!/bin/sh
# Build and run port/recomp/tests/native_math_test.cpp against port/recomp/native_math.cpp (Linux, no game files needed).
set -e
cd "$(dirname "$0")/.."
${CXX:-c++} -std=c++17 -m64 -DFNV_GUEST_BASE_RUNTIME -DFNV_NATIVE_MATH -DFNV_NATIVE_NO_ORACLE -Wall -o "${TMPDIR:-/tmp}/native_math_test" \
    port/recomp/tests/native_math_test.cpp port/recomp/native_math.cpp
"${TMPDIR:-/tmp}/native_math_test"
