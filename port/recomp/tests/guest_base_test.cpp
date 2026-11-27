// Linux unit test for -DFNV_GUEST_BASE_RUNTIME: a 4 GB guest window at an arbitrary base; M32, MF, gstr, gbuf and the
// address <-> pointer conversion must land inside it, including guest addresses above 2 GB (LAA: no sign extension).
// Build and run: tools/test_guest_base.sh
#include <stdio.h>
#include <sys/mman.h>
#include "../pal.h"

// The two globals the runtime (fnv_recomp_rt.cpp) normally defines; the test does not link the runtime.
extern "C" uintptr_t fnv_guest_base = 0;
extern "C" int fnv_weak_memory = 0;

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

int main()
{
    const size_t kWindow = 1ull << 32;
    // Reserve the whole window, then commit one page at each address under test.
    void* w = mmap(nullptr, kWindow + (1u << 20), PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    CHECK(w != MAP_FAILED);
    fnv_guest_base = ((uintptr_t)w + 0xffffu) & ~(uintptr_t)0xffffu;       // arbitrary, 64 KB aligned
    CHECK(fnv_guest_base != 0 && (fnv_guest_base >> 32) != 0);

    const uint32_t addrs[] = {0x00001000u, 0x7ffff000u, 0x80000000u, 0x90000000u, 0xfffff000u};
    for (uint32_t a : addrs)
        CHECK(mprotect((void*)(fnv_guest_base + a), 4096, PROT_READ | PROT_WRITE) == 0);

    for (uint32_t a : addrs)
    {
        uint8_t* host = (uint8_t*)(fnv_guest_base + a);
        CHECK(FNV_G2H(a) == (uintptr_t)host);
        CHECK(FNV_H2G(host) == a);
        CHECK(FNV_G2H((int32_t)a) == (uintptr_t)host);                      // a signed copy of the address must not extend

        M32(a) = 0xdeadbeefu;
        CHECK(*(uint32_t*)host == 0xdeadbeefu);
        M8(a + 4) = 0x5a; M16(a + 6) = 0x1234; M64(a + 8) = 0x0102030405060708ull;
        CHECK(host[4] == 0x5a && *(uint16_t*)(host + 6) == 0x1234 && *(uint64_t*)(host + 8) == 0x0102030405060708ull);
        MF(a + 16) = 1.5f; MD(a + 24) = 2.25;
        CHECK(*(float*)(host + 16) == 1.5f && *(double*)(host + 24) == 2.25);

        strcpy(gbuf(a + 64), "guest");
        CHECK(!strcmp((const char*)host + 64, "guest") && !strcmp(gstr(a + 64), "guest"));
        CHECK((const void*)gstr(a) == (const void*)host && (void*)gbuf(a) == (void*)host);

        fnv_weak_memory = 1; M32(a + 128) = 7; MREL(a + 128, 0); CHECK(*(uint32_t*)(host + 128) == 0);
        fnv_weak_memory = 1; MREL(a + 130, 0); CHECK(*(uint32_t*)(host + 128) == 0);   // unaligned release store
    }
    printf(g_fail ? "guest_base_test: %d failures\n" : "guest_base_test: ok\n", g_fail);
    return g_fail != 0;
}
