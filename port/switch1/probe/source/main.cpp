// fnv1 probe: measures what the console offers (memory, address space, CPU, GPU, SD). Writes to the screen and to
// sdmc:/switch/fnv1/probe.log; changes nothing else on the SD card. Every svc failure is logged with its result code.
// When started with `nxlink -s`, every line is also sent to the PC.
#include <arpa/inet.h>  // struct in_addr for __nxlink_host
#include <switch.h>

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <sys/stat.h>
#include <unistd.h>

#include <deko3d.h>

#include "cpubench.h"

static const u64 MiB = 1ull << 20, GiB = 1ull << 30;
static const u64 kWindow = 4 * GiB;
static FILE* g_log;
static int g_net = -1;  // nxlink socket, -1 when not started by nxlink

// The process heap is split: malloc never touches the guest part [g_heap, g_heap + g_guest); the host part above it
// is newlib's (deko3d, buffers). This is the dense guest heap layout: guest address = offset in the heap. The probe
// must never shrink the heap with svcSetHeapSize, that would free memory newlib already uses.
static const u64 kHostPart = 768 * MiB;
static u8* g_heap;
static u64 g_heap_size, g_guest;

extern "C" {
extern char* fake_heap_start;
extern char* fake_heap_end;

void __libnx_initheap(void) {
    void* addr = NULL;
    u64 size = 0;
    if (envHasHeapOverride()) {  // hbloader has already set the heap to all free memory
        addr = envGetHeapOverrideAddr();
        size = envGetHeapOverrideSize();
    } else {
        u64 total = 0, used = 0;
        svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
        svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
        size = (total - used - 2 * MiB) & ~(2 * MiB - 1);
        if (R_FAILED(svcSetHeapSize(&addr, size)))
            diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
    }
    g_heap = (u8*)addr;
    g_heap_size = size;
    g_guest = size > kHostPart ? (size - kHostPart) & ~(64 * MiB - 1) : 0;
    if (g_guest > kWindow) g_guest = kWindow;
    fake_heap_start = (char*)addr + g_guest;
    fake_heap_end = (char*)addr + size;
}
}

static void say(const char* fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    puts(line);
    if (g_log) {
        fprintf(g_log, "%s\n", line);
        fflush(g_log);
    }
    if (g_net >= 0) {
        write(g_net, line, strlen(line));
        write(g_net, "\n", 1);
    }
    consoleUpdate(NULL);
}

static u64 info(InfoType t, u64 sub = 0) {
    u64 v = 0;
    Result rc = svcGetInfo(&v, t, CUR_PROCESS_HANDLE, sub);
    if (R_FAILED(rc)) say("  svcGetInfo(%d) failed: 0x%x", (int)t, rc);
    return v;
}

// Maps `chunk`-sized pieces back to back from `base` up to kWindow with `map`, stops at the first failure and
// unmaps everything with `unmap`. Logs how far it got and whether anything above 2 GiB was mapped (LAA guests).
template <class Map, class Unmap> static u64 fill(const char* name, u64 chunk, Map map, Unmap unmap) {
    u64 off = 0;
    Result rc = 0;
    while (off < kWindow && R_SUCCEEDED(rc = map(off, chunk))) off += chunk;
    say("  %s, %llu MiB chunks: %llu MiB mapped, stop 0x%x (%s)", name, chunk / MiB, off / MiB, off < kWindow ? rc : 0,
        off < kWindow ? "refused" : "window full");
    say("  %s above 2 GiB (0x80000000-0xFFFFFFFF): %s", name, off > 0x80000000ull ? "YES" : "NO");
    for (u64 o = 0; o < off; o += chunk) unmap(o, chunk);
    return off;
}

static void window_tests() {
    say("== Guest window (4 GiB) ==");
    virtmemLock();
    void* res = virtmemFindAslr(kWindow, 0x10000);
    say("  virtmem 4 GiB reservation: %p (ASLR region %llx+%llx)", res, info(InfoType_AslrRegionAddress),
        info(InfoType_AslrRegionSize));
    if (res) virtmemAddReservation(res, kWindow);
    virtmemUnlock();

    // Variant C: dense guest heap, the guest part of the process heap used as is.
    say("C) dense heap at %p: %llu MiB; guest part %llu MiB, host part (malloc) %llu MiB", g_heap, g_heap_size / MiB,
        g_guest / MiB, (g_heap_size - g_guest) / MiB);
    u64 ok = 0;
    for (u64 o = 0; o < g_guest; o += 2 * MiB) {  // one word per 2 MiB: write, read back
        volatile u32* p = (volatile u32*)(g_heap + o);
        *p = u32(o >> 21) ^ 0x5a5a5a5au;
        if (*p != (u32(o >> 21) ^ 0x5a5a5a5au)) break;
        ok = o + 2 * MiB;
    }
    say("  C written and read back: %llu MiB; above 2 GiB: %s", ok / MiB, ok > 0x80000000ull ? "YES" : "NO");

    // Variant A: svcMapPhysicalMemory in the alias region.
    u64 alias = info(InfoType_AliasRegionAddress), asize = info(InfoType_AliasRegionSize);
    say("A) svcMapPhysicalMemory, alias region %llx+%llx (%llu MiB)", alias, asize, asize / MiB);
    static const u64 sizes[] = {0x1000, 0x10000, 2 * MiB, 64 * MiB};
    for (u64 s : sizes) {  // granularity: does a single map/unmap of this size work?
        Result rc = svcMapPhysicalMemory((void*)alias, s);
        say("  single %llu KiB map: 0x%x", s / 1024, rc);
        if (R_SUCCEEDED(rc)) svcUnmapPhysicalMemory((void*)alias, s);
    }
    fill("A", 64 * MiB, [&](u64 o, u64 n) { return svcMapPhysicalMemory((void*)(alias + o), n); },
         [&](u64 o, u64 n) { svcUnmapPhysicalMemory((void*)(alias + o), n); });
    for (u64 off : {0x80000000ull, 0xFFE00000ull}) {  // sparse: only the high part, nothing below it
        Result rc = off + 2 * MiB <= asize ? svcMapPhysicalMemory((void*)(alias + off), 2 * MiB) : MAKERESULT(1, 1);
        say("  A sparse 2 MiB at +0x%llx: 0x%x%s", off, rc, off + 2 * MiB > asize ? " (outside alias region)" : "");
        if (R_SUCCEEDED(rc)) svcUnmapPhysicalMemory((void*)(alias + off), 2 * MiB);
    }

    // Variant B: svcMapMemory of guest-part heap pages (destination: stack region, then the virtmem reservation).
    u64 heap = g_guest, stack = info(InfoType_StackRegionAddress), ssize = info(InfoType_StackRegionSize);
    say("B) svcMapMemory from the guest part; stack region %llx+%llx", stack, ssize);
    // svcMapMemory makes the source inaccessible, so each source chunk is used once.
    auto mapB = [&](u64 dst, u64 o, u64 n) { return svcMapMemory((void*)(dst + o), g_heap + o, n); };
    auto unmapB = [&](u64 dst, u64 o, u64 n) { svcUnmapMemory((void*)(dst + o), g_heap + o, n); };
    for (u64 dst : {stack, (u64)res}) {
        if (!dst) continue;
        u64 limit = dst == stack ? ssize : kWindow;
        u64 off = 0;
        Result rc = 0;
        while (off < heap && off < limit && R_SUCCEEDED(rc = mapB(dst, off, 64 * MiB))) off += 64 * MiB;
        say("  B into %s: %llu MiB mapped, stop 0x%x", dst == stack ? "stack region" : "virtmem reservation", off / MiB,
            rc);
        for (u64 o = 0; o < off; o += 64 * MiB) unmapB(dst, o, 64 * MiB);
    }
    // the virtmem reservation is kept until exit
}

static void cpu_tests() {
    say("== CPU ==");
    say("  core mask 0x%llx, total memory %llu MiB", info(InfoType_CoreMask), info(InfoType_TotalMemorySize) / MiB);
    Result rc = clkrstInitialize();
    if (R_SUCCEEDED(rc)) {
        ClkrstSession s;
        u32 hz = 0;
        rc = clkrstOpenSession(&s, PcvModuleId_CpuBus, 3);
        if (R_SUCCEEDED(rc)) rc = clkrstGetClockRate(&s, &hz);
        if (R_SUCCEEDED(rc)) say("  CPU clock: %u MHz", hz / 1000000);
        clkrstCloseSession(&s);
        clkrstExit();
    }
    if (R_FAILED(rc)) say("  clkrst failed: 0x%x", rc);
    CpuBench r;
    if (!cpubench_run(&r)) return say("  cpubench: out of memory");
    say("  mem 32 KiB: %.2f ns/iter, mem 64 MiB: %.2f ns/iter, indirect call: %.2f ns/iter", r.mem_small_ns,
        r.mem_big_ns, r.call_ns);
}

static void gpu_tests() {
    say("== GPU (deko3d) ==");
    DkDeviceMaker dm;
    dkDeviceMakerDefaults(&dm);
    dm.cbDebug = [](void*, const char* ctx, DkResult r, const char* msg) {
        say("  deko3d: %s: result %d %s", ctx, (int)r, msg ? msg : "");
    };
    say("  dkDeviceCreate...");  // a line before each step, so a crash shows where it happened
    DkDevice dev = dkDeviceCreate(&dm);
    if (!dev) return say("  dkDeviceCreate failed");
    DkMemBlock blocks[48];
    int n = 0;
    while (n < 48) {
        say("  memblock %d (64 MiB)...", n);
        DkMemBlockMaker mm;
        dkMemBlockMakerDefaults(&mm, dev, 64 * MiB);
        mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
        if (!(blocks[n] = dkMemBlockCreate(&mm))) break;
        n++;
    }
    say("  64 MiB memblocks from the host part: %d (%d MiB)%s", n, n * 64, n == 48 ? " (cap reached)" : " then refused");
    while (n) dkMemBlockDestroy(blocks[--n]);
    dkDeviceDestroy(dev);
}

static void sd_test() {
    say("== SD read ==");
    const char* path = "sdmc:/switch/fnv/Data/Fallout - Meshes.bsa";
    FILE* f = fopen(path, "rb");
    if (!f) return say("  skipped: %s not found (errno %d)", path, errno);
    static u8 buf[MiB];
    u64 total = 0, t0 = cpubench_now_ns();
    for (size_t n; total < 256 * MiB && (n = fread(buf, 1, sizeof buf, f)) > 0;) total += n;
    double s = (cpubench_now_ns() - t0) / 1e9;
    fclose(f);
    say("  read %llu MiB in %.2f s: %.1f MiB/s", total / MiB, s, total / (double)MiB / s);
}

int main() {
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    mkdir("sdmc:/switch/fnv1", 0777);  // EEXIST is fine
    g_log = fopen("sdmc:/switch/fnv1/probe.log", "w");
    if (!g_log) printf("cannot open the log (errno %d); screen only\n", errno);
    // Sockets only when nxlink started us, so a normal run measures memory without them.
    bool sockets = __nxlink_host.s_addr && R_SUCCEEDED(socketInitializeDefault());
    if (sockets) g_net = nxlinkConnectToHost(false, false);

    AppletType at = appletGetAppletType();
    say("== Launch mode ==");
    say("  nxlink: %s", g_net >= 0 ? "connected" : "off");
    say("  applet type %d (%s)", (int)at, at == AppletType_Application ? "Application: OK" : "NOT Application: hold R while starting the game");
    say("  memory: total %llu MiB, used %llu MiB", info(InfoType_TotalMemorySize) / MiB, info(InfoType_UsedMemorySize) / MiB);
    window_tests();
    cpu_tests();
    gpu_tests();
    sd_test();
    say("Done. Press + to exit.");
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        consoleUpdate(NULL);
    }
    if (g_log) fclose(g_log);
    if (g_net >= 0) close(g_net);
    if (sockets) socketExit();
    consoleExit(NULL);
    return 0;
}
