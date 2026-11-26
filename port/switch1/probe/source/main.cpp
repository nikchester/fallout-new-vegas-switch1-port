// fnv1 probe: measures what the console offers (memory, address space, CPU, GPU, SD). Writes to the screen and to
// sdmc:/switch/fnv1/probe.log; changes nothing else on the SD card. Every svc failure is logged with its result code.
#include <switch.h>

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <initializer_list>
#include <sys/stat.h>

#include <deko3d.h>

#include "cpubench.h"

static const u64 MiB = 1ull << 20, GiB = 1ull << 30;
static const u64 kWindow = 4 * GiB;
static FILE* g_log;

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

    // Variant B: svcSetHeapSize + svcMapMemory (destination: stack region, then the virtmem reservation).
    u64 heap = 0, stack = info(InfoType_StackRegionAddress), ssize = info(InfoType_StackRegionSize);
    void* hp = NULL;
    while (heap < kWindow && R_SUCCEEDED(svcSetHeapSize(&hp, heap + 64 * MiB))) heap += 64 * MiB;
    say("B) svcSetHeapSize: up to %llu MiB at %p; stack region %llx+%llx", heap / MiB, hp, stack, ssize);
    // svcMapMemory makes the source inaccessible, so each source chunk is used once.
    auto mapB = [&](u64 dst, u64 o, u64 n) { return svcMapMemory((void*)(dst + o), (u8*)hp + o, n); };
    auto unmapB = [&](u64 dst, u64 o, u64 n) { svcUnmapMemory((void*)(dst + o), (u8*)hp + o, n); };
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
    svcSetHeapSize(&hp, 0);  // the virtmem reservation is kept until exit
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
    DkDevice dev = dkDeviceCreate(&dm);
    if (!dev) return say("  dkDeviceCreate failed");
    DkMemBlock blocks[16];
    int n = 0;
    while (n < 16) {
        DkMemBlockMaker mm;
        dkMemBlockMakerDefaults(&mm, dev, 256 * MiB);
        mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
        if (!(blocks[n] = dkMemBlockCreate(&mm))) break;
        n++;
    }
    say("  256 MiB memblocks: %d (%d MiB)%s", n, n * 256, n == 16 ? " (cap reached)" : " then refused");
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

    AppletType at = appletGetAppletType();
    say("== Launch mode ==");
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
    consoleExit(NULL);
    return 0;
}
