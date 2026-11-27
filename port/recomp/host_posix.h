// POSIX (Linux) host for the whole-game recompiler: the guest's 4 GB window, the exe image at its original addresses,
// guest threads. The hooks of fnv_recomp_host.h / pal.h live in host_posix.cpp; this is what a program (the game's
// main, a unit test) calls around them.
#pragma once
#include <stdint.h>

// First guest address of host functions (fnv_host_base): dispatch keys above everything the guest allocator hands out.
static const uint32_t kPosixHostBase = 0xf0000000u;
// Guest address range handed to guest_heap: above the null guard, below the host function range.
static const uint32_t kPosixHeapLo = 0x00010000u;

// Reserve the window (PROT_NONE) and set up the guest allocator. In a FNV_GUEST_BASE build the window sits at that
// address, with FNV_GUEST_BASE_RUNTIME the host picks one and sets fnv_guest_base. False if the window cannot be mapped.
bool fnv_posix_init();
// Copy the PE image at `path` into the window at its original addresses (headers, sections, zero-filled gaps) and
// remember its entry point and TLS template. False and a log line if the file is not a 32-bit PE or the range is taken.
bool fnv_posix_load_exe(const char* path);
uint32_t fnv_posix_entry();
// Make the calling OS thread the guest's main thread and run `fn(arg)` on it; returns eax.
uint32_t fnv_posix_run_main(uint32_t fn, uint32_t arg);
