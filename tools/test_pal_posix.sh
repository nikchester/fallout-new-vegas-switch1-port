#!/bin/sh
# Build and run port/recomp/tests/pal_host_test.cpp: the POSIX host (host_posix.cpp) with the PAL kernel layer, the std
# file backend and the guest allocator, with an empty function table (Linux, no game files needed).
set -e
cd "$(dirname "$0")/.."
OUT="${TMPDIR:-/tmp}/pal_posix"
mkdir -p "$OUT"
FLAGS="-std=c++17 -g -O1 -pthread -Wall -Wno-unused-function -fno-strict-aliasing -fwrapv -Iport/recomp -DFNV_GUEST_BASE_RUNTIME"
PAL="port/recomp/host_posix.cpp port/recomp/fnv_recomp_rt.cpp port/recomp/pal_kernel.cpp port/recomp/pal_fs_std.cpp port/recomp/guest_heap.cpp"
${CXX:-c++} $FLAGS -DFNV_HOST_NO_MAIN -o "$OUT/pal_host_test" port/recomp/tests/pal_host_test.cpp $PAL
"$OUT/pal_host_test"
