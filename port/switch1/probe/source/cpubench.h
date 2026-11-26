// CPU micro-benchmark in the style of recompiled guest code: loads and stores at base + addr, and indirect calls
// through a table. Plain C++ so the same code builds for Switch 1 (libnx) and for Linux aarch64 (linux/Makefile).
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>

struct CpuBench {
    double mem_small_ns;  // dependent load+store, 32 KiB working set (L1)
    double mem_big_ns;    // dependent load+store, 64 MiB working set (cache misses)
    double call_ns;       // indirect call through a table of 8 functions
    uint32_t sink;        // keeps the compiler from dropping the loops
};

static inline uint64_t cpubench_now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

template <int N> __attribute__((noinline)) static uint32_t cpubench_fn(uint32_t x) { return x * (2654435761u + N) + N; }
typedef uint32_t (*CpuBenchFn)(uint32_t);

static inline double cpubench_mem(uint8_t* base, uint32_t mask, uint32_t iters, uint32_t* sink) {
    uint32_t addr = 12345, v = 1;
    uint64_t t0 = cpubench_now_ns();
    for (uint32_t i = 0; i < iters; i++) {
        addr = (addr * 1664525u + 1013904223u + v) & mask & ~3u;
        v = *(volatile uint32_t*)(base + addr);
        *(volatile uint32_t*)(base + (addr ^ 64)) = v + i;
    }
    *sink += v;
    return double(cpubench_now_ns() - t0) / iters;
}

// Returns false if the 64 MiB buffer cannot be allocated.
static inline bool cpubench_run(CpuBench* r) {
    const uint32_t big = 64u << 20;
    uint8_t* buf = (uint8_t*)calloc(big, 1);
    if (!buf) return false;
    r->sink = 0;
    r->mem_small_ns = cpubench_mem(buf, 32 * 1024 - 1, 20000000, &r->sink);
    r->mem_big_ns = cpubench_mem(buf, big - 1, 5000000, &r->sink);
    free(buf);
    static const CpuBenchFn table[8] = {cpubench_fn<0>, cpubench_fn<1>, cpubench_fn<2>, cpubench_fn<3>,
                                        cpubench_fn<4>, cpubench_fn<5>, cpubench_fn<6>, cpubench_fn<7>};
    const CpuBenchFn* volatile t = table;  // volatile: no devirtualisation
    uint32_t x = 1;
    const uint32_t iters = 20000000;
    uint64_t t0 = cpubench_now_ns();
    for (uint32_t i = 0; i < iters; i++) x = t[x & 7](x);
    r->call_ns = double(cpubench_now_ns() - t0) / iters;
    r->sink += x;
    return true;
}
