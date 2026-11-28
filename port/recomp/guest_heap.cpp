// Guest-window allocator, see guest_heap.h.
#include "guest_heap.h"
#include "fnv_recomp.h"
#include <algorithm>
#include <map>
#include <mutex>
#include <stdio.h>
#include <string.h>
#ifndef __SWITCH__
#include <sys/mman.h>
#endif

namespace {

const uint64_t kPage = 0x1000, kGran = 0x10000;
const uint64_t kNoLaaLimit = GH_NO_LAA_LIMIT;   // the port runs as a non-LAA process: guest memory stays below 2 GB
const uint32_t kHeader = 16, kMagic = 0x48564e46;   // "FNVH"
const uint32_t kChunk = 0x100000, kMaxSmall = 0x20000;   // blocks up to 128 KB come from 1 MB chunks, larger ones get a region
const int kClasses = 12;                             // 32 B .. 128 KB, powers of two (header included)

#ifdef __SWITCH__
// Horizon: the guest window is a dense block of the process heap (host_switch1.cpp), already mapped and writable, so
// commit only has to hand out zeroed pages and decommit has nothing to return.
bool posix_commit(uint32_t a, uint32_t n) { memset((void*)FNV_G2H(a), 0, n); return true; }
void posix_decommit(uint32_t, uint32_t) {}
#else
bool posix_commit(uint32_t a, uint32_t n) { return mprotect((void*)FNV_G2H(a), n, PROT_READ | PROT_WRITE) == 0; }
void posix_decommit(uint32_t a, uint32_t n)
{
    madvise((void*)FNV_G2H(a), n, MADV_DONTNEED);
    mprotect((void*)FNV_G2H(a), n, PROT_NONE);
}
#endif
const GuestHeapBackend kPosix = {posix_commit, posix_decommit};

struct Region { uint64_t size; };

std::mutex g_mu;
GuestHeapBackend g_be = kPosix;
uint64_t g_lo = 0, g_hi = 0;
uint64_t g_high = 0;                                 // highest guest address handed out so far (exclusive end)
std::map<uint64_t, Region> g_regions;                // reserved ranges, keyed by start
std::map<uint64_t, uint64_t> g_committed;            // committed page runs (start -> end), merged
uint32_t g_free[kClasses];                           // free-list heads (user pointers), next link in the first 4 bytes
uint64_t g_chunk_cur = 0, g_chunk_end = 0, g_chunk_committed = 0;

uint64_t up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

// First fit at a 64 KB boundary.
uint64_t find_free(uint64_t size)
{
    uint64_t at = up(g_lo, kGran);
    for (auto& r : g_regions)
    {
        if (r.first >= at + size) break;
        at = std::max(at, up(r.first + r.second.size, kGran));
    }
    return at + size <= g_hi ? at : 0;   // exhaustion: g_hi never exceeds the 2 GB limit
}
bool range_free(uint64_t a, uint64_t size)
{
    if (a < g_lo || a + size > g_hi)
    {
        if (a + size > kNoLaaLimit) fprintf(stderr, "guest_heap: refused %#llx+%#llx, above the 2 GB non-LAA limit\n", (unsigned long long)a, (unsigned long long)size);
        return false;
    }
    auto it = g_regions.upper_bound(a);
    if (it != g_regions.end() && it->first < a + size) return false;
    if (it != g_regions.begin() && std::prev(it)->first + std::prev(it)->second.size > a) return false;
    return true;
}
const Region* region_of(uint64_t a, uint64_t* start = nullptr)
{
    auto it = g_regions.upper_bound(a);
    if (it == g_regions.begin()) return nullptr;
    --it;
    if (a >= it->first + it->second.size) return nullptr;
    if (start) *start = it->first;
    return &it->second;
}

bool commit_pages(uint64_t a, uint64_t n)
{
    // Commit only the not-yet-committed parts, so a recommit keeps the data (Windows semantics).
    uint64_t pos = a, end = a + n;
    while (pos < end)
    {
        auto it = g_committed.upper_bound(pos);
        uint64_t stop = it == g_committed.end() ? end : std::min(end, it->first);
        if (it != g_committed.begin() && std::prev(it)->second > pos) { pos = std::min(end, std::prev(it)->second); continue; }
        if (!g_be.commit((uint32_t)pos, (uint32_t)(stop - pos))) return false;
        g_committed[pos] = stop;
        pos = stop;
    }
    // merge runs
    for (auto it = g_committed.begin(); it != g_committed.end();)
    {
        auto nx = std::next(it);
        if (nx != g_committed.end() && nx->first <= it->second) { it->second = std::max(it->second, nx->second); g_committed.erase(nx); }
        else it = nx;
    }
    return true;
}
void decommit_pages(uint64_t a, uint64_t n)
{
    uint64_t end = a + n;
    auto it = g_committed.upper_bound(a);
    if (it != g_committed.begin()) --it;
    while (it != g_committed.end() && it->first < end)
    {
        uint64_t s = it->first, e = it->second;
        it = g_committed.erase(it);
        if (e <= a) continue;
        uint64_t ds = std::max(s, a), de = std::min(e, end);
        g_be.decommit((uint32_t)ds, (uint32_t)(de - ds));
        if (s < ds) g_committed[s] = ds;
        if (de < e) g_committed[de] = e;
        it = g_committed.upper_bound(de);
    }
}

uint32_t vm_alloc(uint32_t addr, uint32_t size, uint32_t type)
{
    if (!size || !(type & (GH_MEM_COMMIT | GH_MEM_RESERVE))) return 0;
    uint64_t sz = up(size, kPage), at = addr;
    if (type & GH_MEM_RESERVE)
    {
        sz = up(size, kGran);
        if (addr)
        {
            at = addr & ~(kGran - 1);
            sz = up((uint64_t)addr + size - at, kGran);
            if (!range_free(at, sz)) return 0;
        }
        else if (!(at = find_free(sz))) return 0;
        g_regions[at] = {sz};
        g_high = std::max(g_high, at + sz);
        if (type & GH_MEM_COMMIT)
        {
            uint64_t cn = up(size, kPage);
            if (!commit_pages(at + (addr ? addr - at : 0), cn)) { g_regions.erase(at); return 0; }
        }
        return (uint32_t)(addr ? addr : at);
    }
    // commit only: inside a reserved region (explicit address required, as on Windows apart from address 0 = fresh region)
    if (!addr) return vm_alloc(0, size, GH_MEM_RESERVE | GH_MEM_COMMIT);
    uint64_t start, a = addr & ~(kPage - 1), n = up((uint64_t)addr + size, kPage) - a;
    const Region* r = region_of(a, &start);
    if (!r || a + n > start + r->size) return 0;
    return commit_pages(a, n) ? addr : 0;
}

bool vm_free(uint32_t addr, uint32_t size, uint32_t type)
{
    uint64_t start;
    const Region* r = region_of(addr, &start);
    if (!r) return false;
    if (type == GH_MEM_RELEASE)
    {
        if (size || start != addr) return false;
        decommit_pages(start, r->size);
        g_regions.erase(start);
        return true;
    }
    if (type != GH_MEM_DECOMMIT) return false;
    uint64_t a = addr & ~(kPage - 1), n = size ? up((uint64_t)addr + size, kPage) - a : r->size;
    if (a + n > start + r->size) return false;
    decommit_pages(a, n);
    return true;
}

int size_class(uint32_t total)
{
    int c = 0;
    uint32_t s = 32;
    while (s < total) s <<= 1, ++c;
    return c;
}

uint32_t carve(uint32_t total)
{
    if (g_chunk_cur + total > g_chunk_end)
    {
        uint32_t c = vm_alloc(0, kChunk, GH_MEM_RESERVE);
        if (!c) return 0;
        g_chunk_cur = c, g_chunk_end = (uint64_t)c + kChunk, g_chunk_committed = c;
    }
    uint64_t need = up(g_chunk_cur + total, kGran);
    if (need > g_chunk_committed)
    {
        if (!commit_pages(g_chunk_committed, need - g_chunk_committed)) return 0;
        g_chunk_committed = need;
    }
    uint32_t p = (uint32_t)g_chunk_cur;
    g_chunk_cur += total;
    return p;
}

void set_header(uint32_t base, uint32_t size, uint32_t cls)
{
    uint32_t* h = (uint32_t*)FNV_G2H(base);
    h[0] = size, h[1] = kMagic, h[2] = cls, h[3] = 0;
}

uint32_t heap_alloc(uint32_t size, bool zero)
{
    uint64_t total = (uint64_t)size + kHeader;
    uint32_t base, cls;
    if (total <= kMaxSmall)
    {
        cls = size_class((uint32_t)total);
        if (g_free[cls])
        {
            uint32_t p = g_free[cls];
            g_free[cls] = M32(p);
            base = p - kHeader;
        }
        else if (!(base = carve(32u << cls))) return 0;
    }
    else
    {
        if (total > 0xffffffffull) return 0;
        cls = 0xff;
        base = vm_alloc(0, (uint32_t)total, GH_MEM_RESERVE | GH_MEM_COMMIT);
        if (!base) return 0;
    }
    set_header(base, size, cls);
    if (zero) memset((void*)FNV_G2H(base + kHeader), 0, size);
    return base + kHeader;
}

bool is_committed(uint64_t a, uint64_t n)
{
    auto it = g_committed.upper_bound(a);
    return it != g_committed.begin() && std::prev(it)->second >= a + n;
}

bool heap_owns(uint32_t p)
{
    if (!p || (p & 15) || p < kHeader || !is_committed(p - kHeader, kHeader)) return false;
    return M32(p - kHeader + 4) == kMagic;
}

void heap_free(uint32_t p)
{
    if (!heap_owns(p)) return;
    uint32_t cls = M32(p - kHeader + 8);
    M32(p - kHeader + 4) = 0;
    if (cls == 0xff) { vm_free(p - kHeader, 0, GH_MEM_RELEASE); return; }
    M32(p) = g_free[cls];
    g_free[cls] = p;
}

} // namespace

void guest_heap_init(uint32_t lo, uint64_t hi, const GuestHeapBackend* backend)
{
    std::lock_guard<std::mutex> l(g_mu);
    g_be = backend ? *backend : kPosix;
    g_lo = lo, g_hi = std::min<uint64_t>(hi, kNoLaaLimit);
    if (hi > kNoLaaLimit) fprintf(stderr, "guest_heap: limit %#llx clamped to %#llx (non-LAA)\n", (unsigned long long)hi, (unsigned long long)kNoLaaLimit);
    g_high = 0;
    g_regions.clear(), g_committed.clear();
    memset(g_free, 0, sizeof g_free);
    g_chunk_cur = g_chunk_end = g_chunk_committed = 0;
}
uint32_t guest_vm_alloc(uint32_t a, uint32_t n, uint32_t t) { std::lock_guard<std::mutex> l(g_mu); return vm_alloc(a, n, t); }
bool guest_vm_free(uint32_t a, uint32_t n, uint32_t t) { std::lock_guard<std::mutex> l(g_mu); return vm_free(a, n, t); }
uint32_t guest_heap_alloc(uint32_t n, bool z) { std::lock_guard<std::mutex> l(g_mu); return heap_alloc(n, z); }
void guest_heap_free(uint32_t p) { std::lock_guard<std::mutex> l(g_mu); heap_free(p); }
bool guest_heap_owns(uint32_t p) { std::lock_guard<std::mutex> l(g_mu); return heap_owns(p); }
uint32_t guest_heap_size(uint32_t p) { std::lock_guard<std::mutex> l(g_mu); return heap_owns(p) ? M32(p - kHeader) : 0xffffffffu; }
uint32_t guest_heap_realloc(uint32_t p, uint32_t n, bool z)
{
    std::lock_guard<std::mutex> l(g_mu);
    if (!p) return heap_alloc(n, z);
    if (!heap_owns(p)) return 0;
    uint32_t old = M32(p - kHeader);
    uint32_t q = heap_alloc(n, z);
    if (!q) return 0;
    memcpy((void*)FNV_G2H(q), (void*)FNV_G2H(p), std::min(old, n));
    heap_free(p);
    return q;
}

uint64_t guest_heap_high_water() { std::lock_guard<std::mutex> l(g_mu); return g_high; }
