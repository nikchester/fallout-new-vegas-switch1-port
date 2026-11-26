// Linux aarch64 (or any host) build of the CPU micro-benchmark, to compare with other ARM CPUs.
#include <cstdio>

#include "../source/cpubench.h"

int main() {
    CpuBench r;
    if (!cpubench_run(&r)) {
        puts("cpubench: out of memory");
        return 1;
    }
    printf("mem 32KiB: %.2f ns/iter\nmem 64MiB: %.2f ns/iter\nindirect call: %.2f ns/iter\n(sink %u)\n", r.mem_small_ns,
           r.mem_big_ns, r.call_ns, r.sink);
    return 0;
}
