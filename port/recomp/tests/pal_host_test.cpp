// Linux unit test for the POSIX host (host_posix.cpp) and the PAL over it: the guest window with a base, the exe
// image copied to its original addresses, guest threads (TEB, stack, priority), files through pal_fs_std.cpp and the
// guest allocator. The "exe" is a PE image the test builds itself; no game files. Build and run: tools/test_pal_posix.sh
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <initializer_list>
#include <string>
#include <vector>

#include "../guest_heap.h"
#include "../host_posix.h"
#include "../pal.h"
#include "../pal_fs.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

// ---- calling PAL imports from the test ------------------------------------------------------------------------------
static HostFn find_import(const char* name)
{
    for (const PalEntry* e = pal_kernel_entries(); e->name; ++e)
        if (!strcmp(e->name, name)) return e->fn;
    printf("FAIL: PAL has no %s\n", name);
    ++g_fail;
    return nullptr;
}
// Call a PAL host function the way recompiled code does: stdcall arguments and a return address on the guest stack.
static uint32_t pal_call(FnvCpu& cpu, const char* name, std::initializer_list<uint32_t> args)
{
    HostFn fn = find_import(name);
    if (!fn) return 0;
    uint32_t esp0 = cpu.esp;
    std::vector<uint32_t> a(args);
    for (size_t i = a.size(); i-- > 0;) { cpu.esp -= 4; M32(cpu.esp) = a[i]; }
    cpu.esp -= 4; M32(cpu.esp) = 0x00480ffcu;
    fn(cpu);
    cpu.esp = esp0;
    return cpu.eax;
}
static uint32_t gstring(const char* s)
{
    uint32_t p = guest_heap_alloc((uint32_t)strlen(s) + 1, true);
    strcpy((char*)FNV_G2H(p), s);
    return p;
}

// ---- guest code the test registers (the function table is otherwise empty) -------------------------------------------
enum { kWorkerFn = 0x00480000, kMainFn = 0x00480010 };
static void guest_worker(FnvCpu& s)         // DWORD WINAPI worker(void* out): records what the thread looks like from inside
{
    uint32_t out = harg(s, 0), teb = s.fs_base;
    M32(out + 0) = teb;
    M32(out + 4) = M32(teb + 0x18) == teb && s.esp >= M32(teb + 0x08) && s.esp < M32(teb + 0x04);
    M32(out + 8) = M32(teb + 0x2c) == teb + 0x800;
    M32(out + 12) = fnv_host_is_main_thread();
    M32(s.esp - 64) = 0x5a5a5a5au;                          // the stack is committed memory
    hret(s, 77, 1);
}
static void guest_main(FnvCpu& s)
{
    M32(harg(s, 0)) = fnv_host_is_main_thread() ? 1 : 2;
    hret(s, 5, 1);
}

// ---- a minimal PE32 image ---------------------------------------------------------------------------------------------
static std::string make_pe(uint32_t base, uint32_t entry_rva)
{
    std::string f(0x1000 + 0x400, 0);
    auto w32 = [&](size_t o, uint32_t v) { memcpy(&f[o], &v, 4); };
    auto w16 = [&](size_t o, uint16_t v) { memcpy(&f[o], &v, 2); };
    f[0] = 'M'; f[1] = 'Z';
    w32(0x3c, 0x80);
    size_t pe = 0x80, opt = pe + 24;
    w32(pe, 0x4550);
    w16(pe + 4, 0x14c);
    w16(pe + 6, 2);                         // sections
    w16(pe + 20, 0xe0);                     // optional header size
    w16(opt, 0x10b);
    w32(opt + 16, entry_rva);
    w32(opt + 28, base);
    w32(opt + 56, 0x4000);                  // size of image
    w32(opt + 60, 0x200);                   // size of headers
    size_t sh = opt + 0xe0;
    memcpy(&f[sh], ".text", 5);
    w32(sh + 8, 0x100); w32(sh + 12, 0x1000); w32(sh + 16, 0x200); w32(sh + 20, 0x200);
    memcpy(&f[sh + 40], ".data", 5);
    w32(sh + 48, 0x2000); w32(sh + 52, 0x2000); w32(sh + 56, 0x200); w32(sh + 60, 0x400);   // virtual size > raw size
    for (int i = 0; i < 0x200; ++i) f[0x200 + i] = (char)(0xc0 + (i & 0x1f));
    memcpy(&f[0x400], "DATA-SECTION", 12);
    f.resize(0x600);
    return f;
}

static std::string tmp_dir()
{
    const char* t = getenv("TMPDIR");
    return std::string(t ? t : "/tmp") + "/pal_host_test." + std::to_string((long)getpid());
}

int main()
{
    CHECK(fnv_posix_init());
    CHECK(fnv_guest_base != 0 && !(fnv_guest_base & 0xffff));
    CHECK(fnv_host_base() == kPosixHostBase);
    const uint32_t regs[] = {kWorkerFn, kMainFn};
    const FnvFn fns[] = {guest_worker, guest_main};
    fnv_register_functions(regs, fns, 2);

    std::string dir = tmp_dir();
    CHECK(pal_fs_mkdir(dir.c_str()));

    // image: sections at their original addresses, the rest of the image zeroed, entry and path remembered
    std::string exe = dir + "/test.exe";
    {
        std::string pe = make_pe(0x00400000u, 0x1010);
        PalFile* f = pal_fs_open(exe.c_str(), "w+b");
        CHECK(f && pal_fs_write(pe.data(), pe.size(), f) == pe.size());
        pal_fs_close(f);
    }
    CHECK(fnv_posix_load_exe(exe.c_str()));
    CHECK(fnv_host_image_base() == 0x00400000u && fnv_posix_entry() == 0x00401010u);
    CHECK(!strcmp(fnv_host_exe_path(), exe.c_str()));
    CHECK(M8(0x00400000) == 'M' && M8(0x00401000) == 0xc0 && M8(0x00401001) == 0xc1 && M8(0x004010ff) == 0xc0 + (0xff & 0x1f));   // virtual size 0x100 < raw size
    CHECK(M8(0x00401100) == 0 && !memcmp((void*)FNV_G2H(0x00402000), "DATA-SECTION", 12));
    CHECK(M32(0x00403ffc) == 0);
    CHECK(guest_vm_alloc(0x00400000u, 0x1000, GH_MEM_RESERVE) == 0);   // the allocator knows the image is taken
    CHECK(!fnv_posix_load_exe((dir + "/missing.exe").c_str()));

    // guest main thread: runs a registered guest function with is_main_thread true
    uint32_t out = guest_heap_alloc(64, true);
    CHECK(fnv_posix_run_main(kMainFn, out) == 5 && M32(out) == 1);
    CHECK(fnv_host_is_main_thread());               // this OS thread stays the guest main thread

    // guest threads through CreateThread: own TEB and stack, joined with WaitForSingleObject
    FnvCpu cpu;
    memset((void*)&cpu, 0, sizeof cpu);
    uint32_t stack = guest_vm_alloc(0, 1u << 20, GH_MEM_RESERVE | GH_MEM_COMMIT);
    cpu.esp = stack + (1u << 20) - 64;
    uint32_t buf = guest_heap_alloc(256, true), tid = buf + 128;
    uint32_t h = pal_call(cpu, "CreateThread", {0, 0, kWorkerFn, buf, 0, tid});
    CHECK(h != 0 && M32(tid) != 0);
    CHECK(pal_call(cpu, "SetThreadPriority", {h, 2}) == 1);
    CHECK(pal_call(cpu, "WaitForSingleObject", {h, 10000}) == 0);
    CHECK(pal_call(cpu, "GetExitCodeThread", {h, buf + 160}) == 1 && M32(buf + 160) == 77);
    CHECK(M32(buf) != 0 && M32(buf + 4) == 1 && M32(buf + 8) == 1 && M32(buf + 12) == 0);
    CHECK(M32(buf) >= kPosixHeapLo && FNV_H2G(FNV_G2H(M32(buf))) == M32(buf));
    pal_call(cpu, "CloseHandle", {h});
    for (int i = 0; i < 8; ++i)                  // several short threads in a row: their guest memory is given back
    {
        uint32_t t = pal_call(cpu, "CreateThread", {0, 0, kWorkerFn, buf, 0, 0});
        CHECK(pal_call(cpu, "WaitForSingleObject", {t, 10000}) == 0);
        pal_call(cpu, "CloseHandle", {t});
    }

    // spawn: the native stack is big enough for deep recursion, and the priority call is accepted
    struct Deep { static int rec(int n) { volatile char pad[2048]; pad[0] = (char)n; return n ? rec(n - 1) + (pad[0] & 0) + 1 : 0; } };
    static volatile int s_depth;
    uintptr_t tok = fnv_host_spawn([](void*) { s_depth = Deep::rec(20000); }, nullptr);   // ~40 MB of stack
    CHECK(tok != 0);
    fnv_host_set_priority(tok, -3);
    fnv_host_set_priority(0, 0);
    for (int i = 0; i < 500 && s_depth != 20000; ++i) usleep(10000);
    CHECK(s_depth == 20000);

    // files through the std backend
    std::string file = dir + "/a.txt";
    PalFile* f = pal_fs_open(file.c_str(), "w+b");
    CHECK(f && pal_fs_write("hello", 5, f) == 5);
    pal_fs_close(f);
    PalStat st;
    CHECK(pal_fs_stat(file.c_str(), &st) && !st.dir && st.size == 5);
    CHECK(pal_fs_stat(dir.c_str(), &st) && st.dir);
    f = pal_fs_open(file.c_str(), "rb");
    char rb[8] = {};
    CHECK(f && pal_fs_read(rb, 8, f) == 5 && !strcmp(rb, "hello") && pal_fs_seek(f, 1, 0) == 0 && pal_fs_tell(f) == 1);
    pal_fs_close(f);
    CHECK(pal_fs_rename(file.c_str(), (dir + "/b.txt").c_str()) && !pal_fs_stat(file.c_str(), &st));
    std::vector<std::string> names;
    CHECK(pal_fs_list(dir.c_str(), names) && std::find(names.begin(), names.end(), "b.txt") != names.end());
    // ... and through the Win32 file calls the game uses
    uint32_t path = gstring((dir + "/c.bin").c_str()), n = buf + 200;
    uint32_t fh = pal_call(cpu, "CreateFileA", {path, 0xc0000000u, 0, 0, 2, 0x80, 0});   // GENERIC_RW, CREATE_ALWAYS
    CHECK(fh != 0 && fh != 0xffffffffu);
    uint32_t data = gstring("0123456789");
    CHECK(pal_call(cpu, "WriteFile", {fh, data, 10, n, 0}) == 1 && M32(n) == 10);
    CHECK(pal_call(cpu, "SetFilePointer", {fh, 2, 0, 0}) == 2);
    uint32_t rbuf = guest_heap_alloc(16, true);
    CHECK(pal_call(cpu, "ReadFile", {fh, rbuf, 4, n, 0}) == 1 && M32(n) == 4 && !memcmp((void*)FNV_G2H(rbuf), "2345", 4));
    CHECK(pal_call(cpu, "GetFileSize", {fh, 0}) == 10);
    pal_call(cpu, "CloseHandle", {fh});
    CHECK(pal_fs_remove((dir + "/c.bin").c_str()) && pal_fs_remove((dir + "/b.txt").c_str()) && pal_fs_remove(exe.c_str()));
    CHECK(pal_fs_remove(dir.c_str()));

    // guest heap in the window: blocks are inside it, offsets round-trip, addresses above 2 GB work
    uint32_t p = guest_heap_alloc(100, true);
    CHECK(p >= kPosixHeapLo && p < kPosixHostBase && !(p & 15));
    CHECK((uintptr_t)FNV_G2H(p) == fnv_guest_base + p && FNV_H2G(fnv_guest_base + p) == p);
    memset((void*)FNV_G2H(p), 0xab, 100);
    guest_heap_free(p);
    uint32_t hi = guest_vm_alloc(0x90000000u, 0x10000, GH_MEM_RESERVE | GH_MEM_COMMIT);
    CHECK(hi == 0x90000000u);
    M32(hi) = 0xfeedbeefu;
    CHECK(*(uint32_t*)(fnv_guest_base + 0x90000000ull) == 0xfeedbeefu);
    CHECK(guest_vm_free(hi, 0, GH_MEM_RELEASE));

    printf(g_fail ? "pal_host_test: %d FAILED\n" : "pal_host_test: ok\n", g_fail);
    return g_fail != 0;
}
