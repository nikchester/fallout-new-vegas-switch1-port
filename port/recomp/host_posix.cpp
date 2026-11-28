// POSIX (Linux) host for the whole-game recompiler (docs/RECOMP.md): the platform half of fnv_recomp_host.h and pal.h
// for a 64-bit native process, so the PAL can be built and tested on Linux without the game, a console or the exe.
// The function table stays empty until a build links lifted shards (tools/recomp_build_posix.py); tests register
// their own guest functions with fnv_register_functions.
//
// Memory: the guest's 4 GB address space is a window at fnv_guest_base (fnv_recomp.h), reserved PROT_NONE here and
// committed by guest_heap. The exe is copied to its original addresses inside it. Host functions get addresses from
// kPosixHostBase up: dispatch keys, never dereferenced.
#include "host_posix.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <string>

#include "guest_heap.h"
#include "pal.h"
#include "pal_fs.h"

namespace {
const uint32_t kGuestStack = 4u << 20, kMainStack = 16u << 20;   // guest stacks (committed lazily by the kernel)
const size_t kNativeStack = 64u << 20;                           // native stack of a spawned thread (recompiled code is deep)

std::string g_exe_path;
uint32_t g_image_lo, g_image_hi, g_entry;
uint32_t g_tls_start, g_tls_end, g_tls_zero, g_tls_index, g_tls_callbacks;
bool g_has_tls;

struct HostThread
{
    FnvCpu cpu;
    uint32_t teb, stack_lo, stack_hi, tls_block;
};
// Releases a thread's guest memory when its OS thread ends (also after ExitThread's longjmp, which skips the caller)
struct ThreadHolder
{
    HostThread* ht = nullptr;
    ~ThreadHolder();
};
thread_local ThreadHolder t_holder;
HostThread* g_main_ht;

ThreadHolder::~ThreadHolder()
{
    if (!ht) return;
    if (ht == g_main_ht) g_main_ht = nullptr;
    guest_vm_free(ht->stack_lo, 0, GH_MEM_RELEASE);
    guest_heap_free(ht->teb);
    guest_heap_free(ht->tls_block);
    delete ht;
    ht = nullptr;
}
inline HostThread* cur() { return t_holder.ht; }

void logf(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fflush(stderr);
}

HostThread* new_thread(uint32_t stack_size)
{
    HostThread* ht = new HostThread();
    memset((void*)ht, 0, sizeof *ht);
    ht->stack_lo = guest_vm_alloc(0, stack_size, GH_MEM_RESERVE | GH_MEM_COMMIT);
    ht->teb = guest_heap_alloc(0x1000, true);
    if (!ht->stack_lo || !ht->teb) { logf("[posix] out of guest memory for a thread\n"); abort(); }
    ht->stack_hi = ht->stack_lo + stack_size;
    uint32_t teb = ht->teb;
    M32(teb + 0x00) = 0xffffffffu;       // SEH chain end
    M32(teb + 0x04) = ht->stack_hi;      // stack base
    M32(teb + 0x08) = ht->stack_lo;      // stack limit
    M32(teb + 0x18) = teb;               // self
    M32(teb + 0x2c) = teb + 0x800;       // TLS slot array
    if (g_has_tls)
    {
        uint32_t len = g_tls_end - g_tls_start;
        ht->tls_block = guest_heap_alloc(len + g_tls_zero + 16, true);
        memcpy((void*)FNV_G2H(ht->tls_block), (void*)FNV_G2H(g_tls_start), len);
        M32(teb + 0x800) = ht->tls_block;
    }
    ht->cpu.fs_base = teb;
    ht->cpu.esp = ht->stack_hi - 64;
    ht->cpu.host = ht;
    t_holder.ht = ht;
    return ht;
}

void run_tls_callbacks(HostThread* ht, uint32_t reason)
{
    if (!g_has_tls || !g_tls_callbacks) return;
    for (uint32_t p = g_tls_callbacks; M32(p); p += 4)
    {
        uint32_t args[3] = {g_image_lo, reason, 0};
        fnv_guest_call(ht->cpu, M32(p), args, 3);
    }
}

// OS thread ids of spawned threads, for fnv_host_set_priority
std::mutex g_tid_mx;
std::map<uintptr_t, pid_t> g_tids;
pid_t this_tid() { return (pid_t)syscall(SYS_gettid); }
} // namespace

// ---- setup -------------------------------------------------------------------------------------------------------------
bool fnv_posix_init()
{
    const size_t kWindow = 1ull << 32;
#if defined(FNV_GUEST_BASE_RUNTIME)
    void* w = mmap(nullptr, kWindow + 0x10000, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (w == MAP_FAILED) { logf("[posix] cannot reserve the 4 GB guest window\n"); return false; }
    fnv_guest_base = ((uintptr_t)w + 0xffffu) & ~(uintptr_t)0xffffu;
#elif defined(FNV_GUEST_BASE)
    void* w = mmap((void*)FNV_GUEST_BASE_ADDR, kWindow, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (w != (void*)FNV_GUEST_BASE_ADDR) { logf("[posix] cannot map the guest window at %p\n", (void*)FNV_GUEST_BASE_ADDR); return false; }
#else
    logf("[posix] build with FNV_GUEST_BASE or FNV_GUEST_BASE_RUNTIME\n");
    return false;
#endif
    guest_heap_init(kPosixHeapLo, kPosixHostBase);
    return true;
}

bool fnv_posix_load_exe(const char* path)
{
    PalFile* f = pal_fs_open(path, "rb");
    if (!f) { logf("[posix] cannot open %s\n", path); return false; }
    std::string file;
    char chunk[1 << 16];
    for (size_t n; (n = pal_fs_read(chunk, sizeof chunk, f)) > 0;) file.append(chunk, n);
    pal_fs_close(f);
    const uint8_t* d = (const uint8_t*)file.data();
    auto u32 = [&](size_t o) { uint32_t v = 0; if (o + 4 <= file.size()) memcpy(&v, d + o, 4); return v; };
    auto u16 = [&](size_t o) { uint16_t v = 0; if (o + 2 <= file.size()) memcpy(&v, d + o, 2); return v; };
    uint32_t pe = u32(0x3c), opt = pe + 24;
    if (file.size() < 0x200 || d[0] != 'M' || d[1] != 'Z' || u32(pe) != 0x4550 || u16(opt) != 0x10b)
    {
        logf("[posix] %s is not a 32-bit PE image\n", path);
        return false;
    }
    uint16_t nsec = u16(pe + 6), optsize = u16(pe + 20);
    uint32_t base = u32(opt + 28), img = u32(opt + 56), hdrs = u32(opt + 60);
    if (!img || hdrs > file.size() || (uint64_t)base + img > kPosixHostBase) { logf("[posix] bad image range %08x+%x\n", base, img); return false; }
    if (guest_vm_alloc(base, img, GH_MEM_RESERVE | GH_MEM_COMMIT) != base)
    {
        logf("[posix] cannot place the image at %08x (+%x)\n", base, img);
        return false;
    }
    memcpy((void*)FNV_G2H(base), d, hdrs);
    for (int i = 0; i < nsec; ++i)
    {
        size_t sh = opt + optsize + 40 * (size_t)i;
        uint32_t vsize = u32(sh + 8), rva = u32(sh + 12), rsize = u32(sh + 16), raw = u32(sh + 20);
        uint32_t n = std::min(rsize, vsize ? vsize : rsize);
        if ((uint64_t)rva + n > img || (uint64_t)raw + n > file.size()) { logf("[posix] section %d is outside the image\n", i); return false; }
        memcpy((void*)FNV_G2H(base + rva), d + raw, n);
    }
    g_image_lo = base;
    g_image_hi = base + img;
    g_entry = base + u32(opt + 16);
    g_exe_path = path;
    g_has_tls = false;
    uint32_t tls_rva = u32(opt + 96 + 9 * 8);
    if (tls_rva)
    {
        uint32_t t = base + tls_rva;
        g_tls_start = M32(t); g_tls_end = M32(t + 4); g_tls_index = M32(t + 8); g_tls_callbacks = M32(t + 12);
        g_tls_zero = M32(t + 16);
        g_has_tls = true;
        M32(g_tls_index) = 0;
    }
    logf("[posix] image %08x-%08x entry %08x, %d sections\n", g_image_lo, g_image_hi, g_entry, (int)nsec);
    return true;
}

uint32_t fnv_posix_entry() { return g_entry; }

uint32_t fnv_posix_run_main(uint32_t fn, uint32_t arg)
{
    HostThread* ht = new_thread(kMainStack);
    g_main_ht = ht;
    run_tls_callbacks(ht, 1);
    uint32_t args[1] = {arg};
    return fnv_guest_call(ht->cpu, fn, args, 1);
}

// ---- runtime and PAL hooks ---------------------------------------------------------------------------------------------
uint32_t fnv_host_base() { return kPosixHostBase; }
void fnv_host_log(const char* text) { logf("%s", text); }
bool fnv_host_is_main_thread() { return cur() && cur() == g_main_ht; }
void fnv_host_unknown(FnvCpu& s, uint32_t addr) { fnv_rtrap(s, addr, "dispatch to an address with no function"); }

[[noreturn]] void fnv_host_fatal(FnvCpu& s, uint32_t addr, const char* what)
{
    logf("[posix] stopping: %s at %08x (guest esp %08x)\n", what, addr, s.esp);
    HostThread* ht = cur();
    // the guest stack: return addresses (a `call` ends 5 bytes before them) into the image, as far as the stack is ours
    if (ht && s.esp >= ht->stack_lo && s.esp + 16u <= ht->stack_hi)
    {
        for (int i = 0; i < 16 && s.esp + 4u * i + 16u <= ht->stack_hi; i += 4)
            logf("[posix]   [esp+%02x] %08x %08x %08x %08x\n", 4 * i, M32(s.esp + 4 * i), M32(s.esp + 4 * i + 4),
                 M32(s.esp + 4 * i + 8), M32(s.esp + 4 * i + 12));
        for (uint32_t p = s.esp, shown = 0; p + 4 <= ht->stack_hi && shown < 24; p += 4)
        {
            uint32_t v = M32(p);
            if (g_image_lo && v >= g_image_lo + 5 && v < g_image_hi && M8(v - 5) == 0xe8)
            {
                logf("[posix]   guest stack +%04x: return to %08x\n", p - s.esp, v);
                ++shown;
            }
        }
    }
    exit(3);
}

uint32_t fnv_host_thread_main(uint32_t fn, uint32_t arg)
{
    HostThread* ht = new_thread(kGuestStack);
    run_tls_callbacks(ht, 2);
    uint32_t args[1] = {arg};
    return fnv_guest_call(ht->cpu, fn, args, 1);
}

uintptr_t fnv_host_spawn(void (*fn)(void*), void* arg)
{
    struct P { void (*fn)(void*); void* arg; std::atomic<pid_t> tid; };
    P* p = new P{fn, arg, {0}};
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, kNativeStack);
    pthread_t th;
    int rc = pthread_create(&th, &at, [](void* q) -> void* {
        P* p = (P*)q;
        auto f = p->fn;
        void* a = p->arg;
        uintptr_t self = (uintptr_t)pthread_self();
        p->tid.store(this_tid());               // the spawner waits for this, then frees p
        f(a);
        std::lock_guard<std::mutex> l(g_tid_mx);
        g_tids.erase(self);
        return nullptr;
    }, p);
    pthread_attr_destroy(&at);
    if (rc) { logf("[posix] pthread_create failed (%d)\n", rc); delete p; return 0; }
    pid_t tid;
    while (!(tid = p->tid.load())) sched_yield();
    {
        std::lock_guard<std::mutex> l(g_tid_mx);
        g_tids[(uintptr_t)th] = tid;
    }
    delete p;
    pthread_detach(th);
    return (uintptr_t)th;
}

// Win32 -15..15 maps to nice 19..-19. Raising a priority needs privileges, then the call fails and is ignored.
void fnv_host_set_priority(uintptr_t token, int priority)
{
    pid_t tid;
    if (!token) tid = this_tid();
    else
    {
        std::lock_guard<std::mutex> l(g_tid_mx);
        auto it = g_tids.find(token);
        if (it == g_tids.end()) return;
        tid = it->second;
    }
    int nice = std::max(-19, std::min(19, -priority * 19 / 15));
    setpriority(PRIO_PROCESS, (id_t)tid, nice);
}

int64_t fnv_host_wait_foreign(uint32_t, uint32_t) { return -1; }
bool fnv_host_close_foreign(uint32_t) { return false; }
uint32_t fnv_host_image_base() { return g_image_lo; }
const char* fnv_host_exe_path() { return g_exe_path.c_str(); }

uint64_t fnv_host_rdtsc(void)
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec) * 3u;    // a notional 3 GHz
}
void fnv_host_cpuid(int out[4], int leaf, int)
{
    memset(out, 0, 16);
    if (leaf == 0) { out[0] = 1; memcpy(&out[1], "Genu", 4); memcpy(&out[3], "ineI", 4); memcpy(&out[2], "ntel", 4); }
    else if (leaf == 1) { out[0] = 0x000006f2; out[3] = (1 << 0) | (1 << 4) | (1 << 15) | (1 << 23) | (1 << 25) | (1 << 26); }
}

#ifndef FNV_HOST_NO_MAIN
// usage: fnv_posix --exe <FalloutNV.exe>
// Without lifted shards linked in there is nothing to run: this loads the image and reports it.
int main(int argc, char** argv)
{
    const char* exe = nullptr;
    for (int i = 1; i + 1 < argc; ++i)
        if (!strcmp(argv[i], "--exe")) exe = argv[i + 1];
    if (!fnv_posix_init()) return 1;
    if (!exe) { logf("usage: %s --exe <FalloutNV.exe>\n", argv[0]); return 2; }
    if (!fnv_posix_load_exe(exe)) return 1;
    logf("[posix] %u lifted functions\n", fnv_function_count());
    if (!fnv_function_count()) return 0;
    return (int)fnv_posix_run_main(fnv_posix_entry(), 0);
}
#endif
