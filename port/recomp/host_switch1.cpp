// Nintendo Switch (Horizon, libnx) host for the whole-game recompiler: the platform half of fnv_recomp_host.h and pal.h,
// same interface and structure as host_posix.cpp. Differences:
//  - Guest memory is a dense block at the start of the process heap (method C of NIK-327): guest address = fnv_guest_base
//    + offset, 2048 MiB, addresses strictly below 0x80000000. It is committed up front by svcSetHeapSize, so guest_heap's
//    commit only zeroes. The rest of the heap (about 1.1 GB) stays with malloc for the host and the renderer.
//  - Threads are libnx threads on cores 0-2, Win32 priorities map to Horizon priorities.
//  - The log is sdmc:/switch/fnv1/log.txt, the exe is sdmc:/switch/fnv1/FalloutNV.exe.
// The function table stays empty until a build links lifted shards.
#include "host_switch1.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

#include <switch.h>

#include "guest_heap.h"
#include "pal.h"
#include "pal_fs.h"

// ---- heap layout -------------------------------------------------------------------------------------------------------
// Replaces libnx's __libnx_initheap: take all memory the process may use, give the first kGuestBytes to the guest window
// and the rest to newlib's malloc.
extern "C" {
extern char* fake_heap_start;
extern char* fake_heap_end;
}
namespace {
uintptr_t g_heap_addr;
size_t g_heap_size, g_guest_bytes;
}
extern "C" void __libnx_initheap(void)
{
    void* addr = nullptr;
    size_t size = 0;
    if (envHasHeapOverride())
    {
        addr = envGetHeapOverrideAddr();
        size = envGetHeapOverrideSize();
    }
    else
    {
        uint64_t total = 0, used = 0;
        svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
        svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
        const uint64_t kReserve = 0x2000000;   // thread stacks, libnx's own mappings
        if (total > used + kReserve) size = (size_t)((total - used - kReserve) & ~0x1fffffull);
        if (size && R_FAILED(svcSetHeapSize(&addr, size))) size = 0;
    }
    g_heap_addr = (uintptr_t)addr;
    g_heap_size = size;
    g_guest_bytes = 0;
    uintptr_t base = (g_heap_addr + 0xffffu) & ~(uintptr_t)0xffffu;
    size_t usable = size - (base - g_heap_addr);
    if (addr && usable >= kSwitch1GuestBytes + kSwitch1HostMin)
    {
        g_guest_bytes = kSwitch1GuestBytes;
        fnv_guest_base = base;
        fake_heap_start = (char*)(base + kSwitch1GuestBytes);
        fake_heap_end = (char*)addr + size;
    }
    else
    {
        fake_heap_start = (char*)addr;
        fake_heap_end = (char*)addr + size;
    }
}

namespace {
const uint32_t kGuestStack = 4u << 20, kMainStack = 16u << 20;   // guest stacks, inside the guest window
const size_t kNativeStack = 4u << 20;                            // native stack of a spawned thread (recompiled code is deep)
const char* const kLogPath = "sdmc:/switch/fnv1/log.txt";
const char* const kExePath = "sdmc:/switch/fnv1/FalloutNV.exe";

std::string g_exe_path;
uint32_t g_image_lo, g_image_hi, g_entry;
uint32_t g_tls_start, g_tls_end, g_tls_zero, g_tls_index, g_tls_callbacks;
bool g_has_tls;

struct HostThread
{
    FnvCpu cpu;
    uint32_t teb, stack_lo, stack_hi, tls_block;
};
thread_local HostThread* t_ht;
HostThread* g_main_ht;

// Releases a thread's guest memory; the thread entry calls it when the guest function ended (also after ExitThread's
// longjmp, which skips the caller). No thread_local destructor: libnx has no __cxa_thread_atexit.
void release_thread()
{
    HostThread* ht = t_ht;
    if (!ht) return;
    if (ht == g_main_ht) g_main_ht = nullptr;
    guest_vm_free(ht->stack_lo, 0, GH_MEM_RELEASE);
    guest_heap_free(ht->teb);
    guest_heap_free(ht->tls_block);
    delete ht;
    t_ht = nullptr;
}
inline HostThread* cur() { return t_ht; }

FILE* g_log;
std::mutex g_log_mx;
void logf(const char* fmt, ...)
{
    std::lock_guard<std::mutex> l(g_log_mx);
    va_list ap;
    va_start(ap, fmt);
    if (g_log) { vfprintf(g_log, fmt, ap); fflush(g_log); }
    va_end(ap);
}

HostThread* new_thread(uint32_t stack_size)
{
    HostThread* ht = new HostThread();
    memset((void*)ht, 0, sizeof *ht);
    ht->stack_lo = guest_vm_alloc(0, stack_size, GH_MEM_RESERVE | GH_MEM_COMMIT);
    ht->teb = guest_heap_alloc(0x1000, true);
    if (!ht->stack_lo || !ht->teb) { logf("[switch1] out of guest memory for a thread\n"); abort(); }
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
    t_ht = ht;
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

} // namespace

// ---- setup -------------------------------------------------------------------------------------------------------------
bool fnv_switch1_init()
{
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/fnv1", 0777);
    g_log = fopen(kLogPath, "w");
    logf("[switch1] fnv host, heap %zu MiB at %p\n", g_heap_size >> 20, (void*)g_heap_addr);
    if (!g_guest_bytes)
    {
        logf("[switch1] not enough memory for the 2048 MiB guest window plus %u MiB for the host: the heap is %zu MiB. "
             "Start the NRO as a title (hold R while launching a game) so it gets the full memory, not from the album applet.\n",
             (unsigned)(kSwitch1HostMin >> 20), g_heap_size >> 20);
        return false;
    }
    fnv_guest_base = (g_heap_addr + 0xffffu) & ~(uintptr_t)0xffffu;
    memset((void*)fnv_guest_base, 0, 0x10000);
    guest_heap_init(kSwitch1HeapLo, kSwitch1HeapHi);
    logf("[switch1] guest window %p, %zu MiB, host heap %zu MiB\n", (void*)fnv_guest_base, g_guest_bytes >> 20,
         (size_t)(fake_heap_end - fake_heap_start) >> 20);
    return true;
}

bool fnv_switch1_load_exe(const char* path)
{
    PalFile* f = pal_fs_open(path, "rb");
    if (!f) { logf("[switch1] cannot open %s\n", path); return false; }
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
        logf("[switch1] %s is not a 32-bit PE image\n", path);
        return false;
    }
    uint16_t nsec = u16(pe + 6), optsize = u16(pe + 20);
    uint32_t base = u32(opt + 28), img = u32(opt + 56), hdrs = u32(opt + 60);
    if (!img || hdrs > file.size() || (uint64_t)base + img > kSwitch1HeapHi) { logf("[switch1] bad image range %08x+%x\n", base, img); return false; }
    if (guest_vm_alloc(base, img, GH_MEM_RESERVE | GH_MEM_COMMIT) != base)
    {
        logf("[switch1] cannot place the image at %08x (+%x)\n", base, img);
        return false;
    }
    memcpy((void*)FNV_G2H(base), d, hdrs);
    for (int i = 0; i < nsec; ++i)
    {
        size_t sh = opt + optsize + 40 * (size_t)i;
        uint32_t vsize = u32(sh + 8), rva = u32(sh + 12), rsize = u32(sh + 16), raw = u32(sh + 20);
        uint32_t n = std::min(rsize, vsize ? vsize : rsize);
        if ((uint64_t)rva + n > img || (uint64_t)raw + n > file.size()) { logf("[switch1] section %d is outside the image\n", i); return false; }
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
    logf("[switch1] image %08x-%08x entry %08x, %d sections\n", g_image_lo, g_image_hi, g_entry, (int)nsec);
    return true;
}

uint32_t fnv_switch1_entry() { return g_entry; }

uint32_t fnv_switch1_run_main(uint32_t fn, uint32_t arg)
{
    HostThread* ht = new_thread(kMainStack);
    g_main_ht = ht;
    run_tls_callbacks(ht, 1);
    uint32_t args[1] = {arg};
    uint32_t r = fnv_guest_call(ht->cpu, fn, args, 1);
    release_thread();
    return r;
}

// ---- runtime and PAL hooks ---------------------------------------------------------------------------------------------
uint32_t fnv_host_base() { return kSwitch1HostBase; }
void fnv_host_log(const char* text) { logf("%s", text); }
bool fnv_host_is_main_thread() { return cur() && cur() == g_main_ht; }
void fnv_host_unknown(FnvCpu& s, uint32_t addr) { fnv_rtrap(s, addr, "dispatch to an address with no function"); }

[[noreturn]] void fnv_host_fatal(FnvCpu& s, uint32_t addr, const char* what)
{
    logf("[switch1] stopping: %s at %08x (guest esp %08x)\n", what, addr, s.esp);
    HostThread* ht = cur();
    // the guest stack: return addresses (a `call` ends 5 bytes before them) into the image, as far as the stack is ours
    if (ht && s.esp >= ht->stack_lo && s.esp + 16u <= ht->stack_hi)
    {
        for (int i = 0; i < 16 && s.esp + 4u * i + 16u <= ht->stack_hi; i += 4)
            logf("[switch1]   [esp+%02x] %08x %08x %08x %08x\n", 4 * i, M32(s.esp + 4 * i), M32(s.esp + 4 * i + 4),
                 M32(s.esp + 4 * i + 8), M32(s.esp + 4 * i + 12));
        for (uint32_t p = s.esp, shown = 0; p + 4 <= ht->stack_hi && shown < 24; p += 4)
        {
            uint32_t v = M32(p);
            if (g_image_lo && v >= g_image_lo + 5 && v < g_image_hi && M8(v - 5) == 0xe8)
            {
                logf("[switch1]   guest stack +%04x: return to %08x\n", p - s.esp, v);
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

// A libnx thread; finished ones are joined and closed by the next spawn (their stacks are mapped memory)
struct HostOsThread
{
    Thread th;
    void (*fn)(void*);
    void* arg;
    volatile bool done;
};
std::mutex g_os_mx;
std::vector<HostOsThread*> g_os_threads;
unsigned g_next_core;

void os_thread_entry(void* p)
{
    HostOsThread* t = (HostOsThread*)p;
    t->fn(t->arg);
    release_thread();      // no-op if the guest never started on this thread
    t->done = true;
}

// Horizon: 0x1c (highest) .. 0x3b for applications, a normal thread is 0x2c. Win32 priority 0 -> 0x2c, +15 -> 0x1d, -15 -> 0x3b.
int horizon_priority(int win32)
{
    return std::max(0x1c, std::min(0x3b, 0x2c - win32));
}

uintptr_t fnv_host_spawn(void (*fn)(void*), void* arg)
{
    HostOsThread* t = new HostOsThread();
    t->fn = fn;
    t->arg = arg;
    t->done = false;
    std::lock_guard<std::mutex> l(g_os_mx);
    for (size_t i = 0; i < g_os_threads.size();)
    {
        HostOsThread* o = g_os_threads[i];
        if (o->done)
        {
            threadWaitForExit(&o->th);
            threadClose(&o->th);
            delete o;
            g_os_threads.erase(g_os_threads.begin() + i);
        }
        else ++i;
    }
    int core = (int)(g_next_core++ % 3);
    Result rc = threadCreate(&t->th, os_thread_entry, t, nullptr, kNativeStack, 0x2c, core);
    if (R_SUCCEEDED(rc)) rc = threadStart(&t->th);
    if (R_FAILED(rc))
    {
        logf("[switch1] thread creation failed (%08x)\n", (unsigned)rc);
        delete t;
        return 0;
    }
    g_os_threads.push_back(t);
    return (uintptr_t)t;
}

// Win32 -15..15 -> Horizon priority (a lower number runs first). The token is the HostOsThread, 0 = the calling thread.
void fnv_host_set_priority(uintptr_t token, int priority)
{
    Handle h;
    if (!token) h = threadGetSelf()->handle;
    else
    {
        std::lock_guard<std::mutex> l(g_os_mx);
        if (std::find(g_os_threads.begin(), g_os_threads.end(), (HostOsThread*)token) == g_os_threads.end()) return;
        h = ((HostOsThread*)token)->th.handle;
    }
    svcSetThreadPriority(h, horizon_priority(priority));
}

int64_t fnv_host_wait_foreign(uint32_t, uint32_t) { return -1; }
bool fnv_host_close_foreign(uint32_t) { return false; }
uint32_t fnv_host_image_base() { return g_image_lo; }
const char* fnv_host_exe_path() { return g_exe_path.c_str(); }

uint64_t fnv_host_rdtsc(void)
{
    return armGetSystemTick() * 625u / 4u;    // the 19.2 MHz system counter as a notional 3 GHz
}
void fnv_host_cpuid(int out[4], int leaf, int)
{
    memset(out, 0, 16);
    if (leaf == 0) { out[0] = 1; memcpy(&out[1], "Genu", 4); memcpy(&out[3], "ineI", 4); memcpy(&out[2], "ntel", 4); }
    else if (leaf == 1) { out[0] = 0x000006f2; out[3] = (1 << 0) | (1 << 4) | (1 << 15) | (1 << 23) | (1 << 25) | (1 << 26); }
}

#ifndef FNV_HOST_NO_MAIN
namespace {
// Shows the message on the console and waits for a button, so the owner sees why the NRO left (the log has it too)
void report_and_wait(const char* msg)
{
    consoleInit(nullptr);
    printf("%s\n\nLog: %s\nPress + to exit.\n", msg, kLogPath);
    consoleUpdate(nullptr);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    while (appletMainLoop())
    {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        consoleUpdate(nullptr);
    }
    consoleExit(nullptr);
}
} // namespace

// Without lifted shards linked in there is nothing to run: this loads the image and reports it.
int main(int, char**)
{
    int rc = 0;
    const char* err = nullptr;
    if (!fnv_switch1_init()) err = "Not enough memory for the game (see the log).";
    else if (!fnv_switch1_load_exe(kExePath)) err = "Cannot load sdmc:/switch/fnv1/FalloutNV.exe: copy the game's exe there (see the log).";
    else
    {
        logf("[switch1] %u lifted functions\n", fnv_function_count());
        if (fnv_function_count()) rc = (int)fnv_switch1_run_main(fnv_switch1_entry(), 0);
        else err = "The exe is loaded, but this build has no game code (empty function table).";
    }
    if (err)
    {
        logf("[switch1] %s\n", err);
        report_and_wait(err);
        rc = 1;
    }
    if (g_log) fclose(g_log);
    return rc;
}
#endif
