// Allocator inside the guest window (fnv_guest_base + [lo, hi)): VirtualAlloc/VirtualFree regions and the HeapAlloc family.
// Addresses are uint32_t and may be above 2 GB (LAA). The page backend is injectable; the default one commits with
// mprotect/madvise on the window the host reserved (PROT_NONE) before the guest runs.
#pragma once
#include <stdint.h>

struct GuestHeapBackend
{
    bool (*commit)(uint32_t addr, uint32_t size);     // make [addr, addr+size) readable/writable and zeroed
    void (*decommit)(uint32_t addr, uint32_t size);   // return the pages, contents are discarded
};

enum { GH_MEM_COMMIT = 0x1000, GH_MEM_RESERVE = 0x2000, GH_MEM_DECOMMIT = 0x4000, GH_MEM_RELEASE = 0x8000 };

// Guest limit: addresses in [lo, hi) are handed out (hi is a uint64_t so 0x1'0000'0000 means the full 4 GB). backend null = default.
void guest_heap_init(uint32_t lo, uint64_t hi, const GuestHeapBackend* backend = nullptr);

// VirtualAlloc / VirtualFree semantics (64 KB reserve granularity, 4 KB pages). 0 / false on failure.
uint32_t guest_vm_alloc(uint32_t addr, uint32_t size, uint32_t type);
bool guest_vm_free(uint32_t addr, uint32_t size, uint32_t type);

// HeapAlloc family; blocks are 16-byte aligned. alloc returns 0 when the limit is exhausted.
uint32_t guest_heap_alloc(uint32_t size, bool zero);
void guest_heap_free(uint32_t p);                     // ignores 0 and pointers that are not ours
bool guest_heap_owns(uint32_t p);
uint32_t guest_heap_size(uint32_t p);                 // requested size, 0xffffffff if not ours
uint32_t guest_heap_realloc(uint32_t p, uint32_t size, bool zero);   // 0 on failure (the old block stays valid)
