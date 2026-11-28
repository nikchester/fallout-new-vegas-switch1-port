// Nintendo Switch (libnx) host for the whole-game recompiler, same shape as host_posix.h. The hooks of fnv_recomp_host.h /
// pal.h live in host_switch1.cpp; this is what the program's main calls around them.
#pragma once
#include <stddef.h>
#include <stdint.h>

// Guest memory: a dense block at the start of the process heap, 2048 MiB, addresses [0x10000, 0x80000000) (NIK-342).
static const size_t kSwitch1GuestBytes = (size_t)2048 << 20;
// What the host and renderer need besides the guest window; the NRO refuses to start with less heap than guest + this.
static const size_t kSwitch1HostMin = (size_t)512 << 20;
static const uint32_t kSwitch1HeapLo = 0x00010000u;
static const uint32_t kSwitch1HeapHi = 0x80000000u;
// First guest address of host functions (fnv_host_base): dispatch keys, never dereferenced.
static const uint32_t kSwitch1HostBase = 0xf0000000u;

// Open the log on the SD card, check the heap __libnx_initheap set up and start the guest allocator. False (and a log
// line) if the process did not get enough memory.
bool fnv_switch1_init();
// Copy the PE image at `path` into the guest window at its original addresses. False and a log line if the file is
// missing or not a 32-bit PE.
bool fnv_switch1_load_exe(const char* path);
uint32_t fnv_switch1_entry();
// Make the calling thread the guest's main thread and run `fn(arg)` on it; returns eax.
uint32_t fnv_switch1_run_main(uint32_t fn, uint32_t arg);
