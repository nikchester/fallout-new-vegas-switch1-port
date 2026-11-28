# Whole-game recompilation (Phase 3 path, chosen 2026-11-07)

Author: Claude Opus 5.5. The user chose to build the browser version by lifting the whole original executable,
rather than compiling decomp C++ to WebAssembly. clang has no Microsoft C++ ABI on wasm, so decomp objects could not
share vtables and layouts with the game's data. The decomp stays the reference for names,
understanding and debugging.

## Model

- **One CPU state per guest thread** (`FnvCpu`: eight registers, flags, x87 stack, XMM, MMX, `fs` base), passed to
  every function. Each original function `0xXXXXXXXX` becomes `void f_XXXXXXXX(FnvCpu& s)`.
- **Real guest stack.** Each guest thread has a stack in guest memory, and `esp` points into it. `call` pushes the
  original return address (the next instruction's address) and calls the target's host function. `ret N` does
  `esp += 4 + N` and returns to the host caller. Because the return address is on the stack, code that reads it
  works: `__SEH_prolog4`, `_chkstk` / `_alloca_probe`, `_EH_prolog`, `call $+5; pop`. Helpers that leave `esp`
  unbalanced also work, because callers continue from the shared `esp`.
- **Guest memory = host memory at the original addresses.** The exe's sections are mapped at their virtual
  addresses (0x00400000…0x0140c9e4), and heaps and stacks are allocated in the same 32-bit space. `M32(a)` is a direct
  load. On native x86 this is the process address space (the host program is based above 0x10000000); in wasm32 it
  is linear memory starting at address 0.
- **Direct calls** to a known function compile to a direct C++ call. **Indirect calls and jumps** (vtables,
  callbacks, `jmp eax`) go through `fnv_dispatch(s, addr)`: a table from original address to host function, built
  from every lifted entry. A jump into the middle of another function is lifted as an entry of its own.
- **Tail jumps** call the target without pushing; the target's `ret` returns for both.
- **Imports.** At start-up each IAT slot gets an address in a reserved *host range*. The dispatcher sends those to
  host implementations (`h_CreateFileA(s)` reads its arguments at `esp+4`, sets `eax`, pops like the real stdcall
  function). COM objects the host hands out (Direct3D 9, DirectInput, DirectSound) are guest-memory objects whose
  vtables hold host-range addresses.
- **Callbacks** from the host into guest code (window procedure, thread start, timers, enumeration callbacks) go
  through `fnv_guest_call(s, addr, args...)`: push the arguments and a sentinel return address, then dispatch.
- **`fs:`** is a per-thread TEB block in guest memory: `[0]` SEH head, `[4]`/`[8]` stack bounds, `[0x18]` self,
  `[0x2c]` TLS array. The original static CRT keeps working on it, including its own TLS (`__tls_index`).
- **Threads.** Native: real OS threads, each with its own `FnvCpu` and guest stack. Browser: pthreads on
  SharedArrayBuffer (needs COOP/COEP headers), or one-at-a-time coroutines, as Phase 2's thread host planned.
- **x87** stays the current double-based emulation. It now lives in the shared state, so float results cross
  calls without the old `fpush` plumbing.
- **SEH / C++ exceptions** (later milestone). `RaiseException` (host) walks the guest SEH chain and calls the
  original handlers (`__CxxFrameHandler3`, lifted). `_JumpToContinuation` resumes at a catch continuation: functions
  with try blocks get a host `setjmp` landing pad keyed by their guest frame, and the continuation becomes a
  `longjmp` plus a `switch` to the label. Until this exists an exception stops the run with a report.

## Milestones

1. **Native full-lift build.** Every function is lifted, and no decomp code is compiled. A small host program maps
   the player's exe, installs host imports that call real Win32 directly (guest pointers are host pointers here),
   and runs the original entry point `mainCRTStartup`, so the original CRT start-up runs too. Exit: main menu.
2. **Emscripten build of the same code, headless in node.** Host imports are the browser `pal_win32` (files through
   the OPFS layer in `web/`). Exit: load `FalloutNV.esm`, `coc GSDocMitchellHouse`, tick for
   60 s, exit cleanly.
3. **Browser.** The game in a worker (PROXY_TO_PTHREAD), files from the local dev server (`server/serve.mjs`,
   `pal_fs_web.cpp`), the D3D9 model sending a command ring that `web/recomp/renderer.js` draws with WebGPU and the
   translated shaders, input through `fnv_key`/`fnv_mouse`, audio still to do. Exit: the main menu in a browser
   (reached 2026-11-07, `out/recomp/browser_main_menu.png`), then the opening.

## Performance notes (to measure, not assumed)

The lifter computes flags eagerly. Each body keeps the registers and flags it uses in locals (`FnvLocal`, written
back at calls, returns and traps), so clang drops the dead flag work and keeps registers out of memory. Memory accesses through a
distinct struct type let clang's type-based alias analysis keep `FnvCpu` fields in registers across guest stores.

Measured on PC (2026-11-20, `posix` build at -O1, the Goodsprings save, idle, vsync off; main-thread profile in
docs/PROFILE_MAIN_THREAD.md):

- **Flags stay behind at calls and returns** (`FnvLocal::out_call`). Storing all five flags at every call kept the
  flag work of the last flag-setting instruction before each call alive. Compiled code never reads a flag across a
  call, and `tools/flags_scan.py` checks that for every call site and entry of the exe (tail jumps still store
  them). 160 -> 176 fps.
- **Trivial imports inline** (`tools/recomp.py INLINE_IMPORTS`): `call [InterlockedIncrement]` and friends become an
  atomic on guest memory, `TlsGetValue` a read of the thread's `FnvCpu.tls`. Calls through a register still
  dispatch.
- **Small leaves inline across shards** (`LEAF_MAX`): every shard gets a private copy of each leaf function of up to
  20 instructions that it calls directly, so the compiler can inline getters and empty constructors (27% of all
  direct call sites).
- **Native replacements** (`NATIVE_FUNCS`, port/recomp/native_math.cpp): hand-written guest-layout versions of hot
  functions, with the lifted original kept as `fo_XXXXXXXX` and compared on live data under `FNV_NATIVE_CHECK=1`.
  So far NiMatrix3/NiTransform multiply and the culling plane tests (Dot, Distance, WhichSide, ClassifyPoint):
  33-134 million live calls each compared, 0 differences.
  **Opt-in:** `tools/recomp.py` uses the lifted code for all of them unless `--native` is given (then it also writes
  `out/recomp/native_math.on`, and the build scripts and the Switch 2 Makefiles compile native_math.cpp with
  `-DFNV_NATIVE_MATH`; without the flag the file is empty). Re-run the build after switching (the Makefiles need
  `rm build/native_math.o`). `tools/test_native_math.sh` unit-tests each function against its reference formula on
  Linux (CI: `unit-tests`); `python tools/native_check.py <FalloutNV.exe>` is the owner's one-command comparison against
  the lifted originals on live data (FNV_NATIVE_CHECK=1). The versions are written from the public layouts (fnv_decomp
  headers), so every layout, calling convention and rounding point is an assumption until that check reports 0
  differences; keep `--native` off until then.
- **x87 pass sees unconditional jumps** (tools/x87_static.py `code_of`): `goto L;  // jmp ...` was not taken as
  unconditional because of its trailing comment, so the code after every jmp looked reachable by falling through,
  depths clashed and optimized functions kept the x87 stack in memory. 853 more functions in locals (16,006), among
  them the particle and bound updates (NiPSysBombModifier::Update 5.4% -> 0.9% of the main thread).
- Inline imports + small leaves + the two multiplies together: 176.4 -> 202.2 fps; the culling tests: -> 209.7 fps;
  the x87 fix: -> 237.5 fps. With the dispatcher rewrite before them (137 -> 159 fps) the PC build went from 137
  to 237 fps on this save.
- **Platform layer for the Switch** (a console profile showed these): D3D9 state the renderer already has is no
  longer re-sent (render/sampler states, textures, streams, indices, shaders, declarations, unchanged shader
  constants; the device's state is sent once at creation so the renderer's copy matches); the sound mixer runs the
  music and movie decoders outside the lock the game's buffer calls wait on; a missing file under the cached
  directory listings is answered without touching the SD card (the game probes for loose Data\ files hundreds of
  times, 20-60 ms each on the console). PC: 234.6 -> 238.0 fps; the console is where these cost.
- **Frame slots in locals** (tools/stack_slots.py, after the x87 pass): in functions whose frame nothing else can
  reach, each [ebp +/- N] slot becomes a C++ local (20,559 functions, 42% of the console's guest time). Two traps
  found on the way, both now handled: a slot read before it is written holds what `push ecx` put there (locals are
  loaded from the stack once the frame is set up), and the CRT's x87 error path fills its caller's frame through
  ebp (callers of a function that reads ebp before setting it keep their frame in memory). `-DFNV_SLOT_CHECK`
  writes every slot through and reports a slot that memory disagrees with; FNV_SLOT_WT=lo-hi limits write-through
  to a function range, for bisecting. PC: 238.0 -> 242.6 fps (clang -O1 on x86 reloads stack slots cheaply).
- **Callee-saved registers across calls** (tools/callee_saved.py, `RCALLK`/`RCALLIK`): after a call to a function that
  keeps ebx/esi/edi/ebp (proven for direct callees by a whole-program scan, assumed for calls through a pointer per
  the x86 convention) those registers are not reloaded, so they stay in host registers. `-DFNV_CALLEE_CHECK`
  verifies every such call at run time; it found the CRT's hand-written assembly breaking the convention, so calls
  made from CRT code are left alone. The stores before the call stay (skipping them on the scan's say-so crashed:
  its input scan is linear). PC, -O2, Ultra 1080p with 12 spawned NPCs: 148.4 -> 156.6 fps.
- **Inline cache for indirect calls** (`FNV_IC`, on by default): each indirect call site keeps its last target's table
  slot in one atomic word and skips the function table's hash on a hit. 153.0 -> 158.2 fps.
- Tried and dropped: engine threading switches in FALLOUT.INI (bUseThreadedAI and friends: the main thread waits on
  the workers, 160 -> 154 fps); clang PGO (+1%, so no cross-toolchain GCC PGO for the Switch); -O3 (= -O2);
  [esp + N] slot promotion for optimized code (tools/esp_slots.py, written but parked: it reaches 13% of console
  guest time, where registers already hold most values).
- Build level matters: -O2 vs -O1 is +25% (160 -> 200 fps under the crowd), and ThinLTO adds +6% (148.6 -> 157.5).
  port/switch2/fnv_fast is the Switch build to play with: -O2, GCC LTO, no profiler.

- **Renderer per-draw cost** (renderer_vk.cpp): at Ultra (shadows, MSAA) the Vulkan renderer thread was busy 56% of
  each frame on PC, which would have capped the Switch near 47 fps. Now 23%: each draw copies only the constant
  registers its shaders read (d3d9_const_bytes parses the D3D9 bytecode; relative addressing gets the full block),
  the per-texture format-filter query is cached, unchanged dynamic state and pipeline binds are skipped, and
  pipelines and vertex layouts are found by binary key instead of a text key built per draw. Skipping identical
  descriptor pushes and buffer binds as well gained nothing on PC and was taken out.
- Native ShadowSceneLight::TestSphereFrustum (shadow-light culling, 3.6% at Ultra): exact, +0.5% on PC.
- Native ShadowSceneLight::ParseObjectAddFrustum 0x00b9f5b0 (the per-light scene walk, 6.1% at Ultra): the recursion
  is a native call with no guest-stack traffic, and its two NiRTTI virtual calls are answered inline when the target
  is "mov eax, ecx; ret" or "xor eax, eax; ret" (FNV_NATIVE_CHECK makes the real call too: 0 mismatches in 262144).
  Ultra crowd A/B, 4 pairs: 154.0 -> 160.8 fps (+4.4%), every pair faster. Screenshots show the crowd drawn.
- Trivial virtual targets game-wide (-DFNV_IC_STATS counts them): of 2.6 billion indirect calls in a 2-minute Ultra
  crowd run (about 130,000 a frame), 92% go to real functions; "return this/0/constant/field" bodies are 8%. Answering
  those at the call site would save well under 1%: not done.
- Parallel shadow-light culling (native_math.cpp poaf_parallel): the walk only reads the scene, and its virtual
  calls are all "return this"/"return 0" bodies, so subtrees are walked on 3 worker threads (FNV_CULL_WORKERS) and the
  0x00b9f480 adds stay on the game thread in the original order; unknown targets fall back to the original walk.
  FNV_NATIVE_CHECK: 32768 walks, identical sequences. Ultra crowd A/B, 3 pairs: 156.2 -> 166.6 fps (+6.6%).
  FNV_PAR_CULL=0 turns it off.

The Switch build writes the launcher's highest preset (VeryHigh.ini) at 1080p into FalloutPrefs.ini; PC measurements
since 2026-11-21 use the same preset (run/docs_ultra) with 12 NCR troopers spawned beside the player (FNV_CONSOLE).
