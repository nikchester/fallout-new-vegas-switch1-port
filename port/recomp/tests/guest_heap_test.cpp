// Linux unit test for guest_heap: VirtualAlloc/HeapAlloc over a 4 GB window; the hard non-LAA limit keeps everything below 2 GB.
// Build and run: tools/test_guest_heap.sh
#include <stdio.h>
#include <sys/mman.h>
#include "../pal.h"
#include "../guest_heap.h"

extern "C" uintptr_t fnv_guest_base = 0;
extern "C" int fnv_weak_memory = 0;

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

int main()
{
    void* w = mmap(nullptr, (1ull << 32) + (1u << 20), PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    CHECK(w != MAP_FAILED);
    fnv_guest_base = ((uintptr_t)w + 0xffffu) & ~(uintptr_t)0xffffu;

    // Window [0x70000000, 0x90000000) is clamped to [0x70000000, 0x80000000); exhaustion at 256 MB.
    guest_heap_init(0x70000000u, 0x90000000ull);

    // VirtualAlloc: reserve, commit, write, decommit, release.
    uint32_t r = guest_vm_alloc(0, 0x100000, GH_MEM_RESERVE);
    CHECK(r >= 0x70000000u && !(r & 0xffff));
    CHECK(guest_vm_alloc(r, 0x2000, GH_MEM_COMMIT) == r);
    M32(r) = 0x11223344u; M32(r + 0x1ffc) = 5;
    CHECK(guest_vm_alloc(r, 0x2000, GH_MEM_COMMIT) == r && M32(r) == 0x11223344u);   // recommit keeps data
    CHECK(guest_vm_alloc(r + 0x200000, 0x1000, GH_MEM_COMMIT) == 0);                  // outside any region
    CHECK(guest_vm_free(r, 0x1000, GH_MEM_DECOMMIT) && guest_vm_alloc(r, 0x1000, GH_MEM_COMMIT) == r && M32(r) == 0);
    CHECK(M32(r + 0x1ffc) == 5);
    CHECK(!guest_vm_free(r + 0x10000, 0, GH_MEM_RELEASE) && !guest_vm_free(r, 0x1000, GH_MEM_RELEASE));
    CHECK(guest_vm_free(r, 0, GH_MEM_RELEASE));
    CHECK(guest_vm_alloc(0, 0x1000, GH_MEM_RESERVE | GH_MEM_COMMIT) == r);          // address reused

    // Addresses >= 0x80000000 are refused (non-LAA), also fixed ones and ones straddling the limit.
    CHECK(guest_vm_alloc(0x80000000u, 0x20000, GH_MEM_RESERVE | GH_MEM_COMMIT) == 0);
    CHECK(guest_vm_alloc(0xf0000000u, 0x1000, GH_MEM_RESERVE) == 0);
    CHECK(guest_vm_alloc(0x7fff0000u, 0x20000, GH_MEM_RESERVE) == 0);
    uint32_t edge = guest_vm_alloc(0x7fff0000u, 0x10000, GH_MEM_RESERVE | GH_MEM_COMMIT);
    CHECK(edge == 0x7fff0000u);
    CHECK(guest_vm_free(edge, 0, GH_MEM_RELEASE));

    // Heap: alloc, free, reuse, realloc, zeroing.
    uint32_t a = guest_heap_alloc(100, true), b = guest_heap_alloc(100, false);
    CHECK(a && b && a != b && !(a & 15) && guest_heap_size(a) == 100 && guest_heap_owns(a));
    for (int i = 0; i < 100; ++i) CHECK(M8(a + i) == 0);
    memset((void*)FNV_G2H(a), 0xab, 100);
    guest_heap_free(a);
    uint32_t c = guest_heap_alloc(90, true);
    CHECK(c == a);                                                                    // same class: block reused
    for (int i = 0; i < 90; ++i) CHECK(M8(c + i) == 0);
    memset((void*)FNV_G2H(c), 0x5a, 90);
    uint32_t d = guest_heap_realloc(c, 5000, false);
    CHECK(d && d != c && guest_heap_size(d) == 5000 && M8(d) == 0x5a && M8(d + 89) == 0x5a);
    guest_heap_free(b); guest_heap_free(d);
    guest_heap_free(0); guest_heap_free(0x12345670u); guest_heap_free(d);             // ignored, no crash
    CHECK(guest_heap_size(0x12345670u) == 0xffffffffu);

    // Large block gets its own region and is released on free.
    uint32_t big = guest_heap_alloc(8u << 20, true);
    CHECK(big && guest_heap_size(big) == (8u << 20) && M8(big + (8u << 20) - 1) == 0);
    guest_heap_free(big);
    CHECK(guest_heap_alloc(8u << 20, false) == big);

    // Fill the window: nothing is handed out at or above 0x80000000.
    uint32_t top = 0;
    for (int i = 0; i < 10; ++i)
    {
        uint32_t p = guest_heap_alloc(16u << 20, false);
        CHECK(p);
        if (p > top) top = p;
        M32(p) = i;
    }
    CHECK(top < 0x80000000u && FNV_H2G(FNV_G2H(top)) == top);

    // Exhaustion: limit 0x10000000 total, so further huge allocations fail and nothing crashes; freeing recovers.
    uint32_t held[64], n = 0;
    while (n < 64 && (held[n] = guest_heap_alloc(64u << 20, false))) ++n;
    CHECK(n > 0 && n < 64);
    CHECK(guest_heap_alloc(64u << 20, false) == 0 && guest_vm_alloc(0, 64u << 20, GH_MEM_RESERVE) == 0);
    guest_heap_free(held[0]);
    CHECK(guest_heap_alloc(64u << 20, false) == held[0]);
    CHECK(guest_heap_alloc(0xfffffff0u, false) == 0);

    CHECK(guest_heap_high_water() > 0x70000000u && guest_heap_high_water() <= GH_NO_LAA_LIMIT);
    printf(g_fail ? "guest_heap_test: %d failures\n" : "guest_heap_test: ok\n", g_fail);
    return g_fail != 0;
}
