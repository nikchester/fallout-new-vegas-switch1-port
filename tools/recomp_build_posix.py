#!/usr/bin/env python3
"""Build the 64-bit POSIX host of the whole-game recompiler (port/recomp/host_posix.cpp): the lifted shards
(out/recomp/recomp_NN.cpp, tools/recomp.py), the portable runtime and PAL, and the guest-window allocator, with the
system clang. The guest's 4 GB address space is a window at FNV_GUEST_BASE (fnv_recomp.h). A file whose source,
headers and flags are unchanged keeps its object.

usage: python tools/recomp_build_posix.py [--jobs N] [--opt -O1] [--runtime-base] [--profile]
output: out/recomp/posix/fnv_posix
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / 'out/recomp'
BUILD = OUT / 'posix'
GUEST_BASE = '0x100000000000ull'          # 16 TB: far from anything the C library or loader maps
HEADERS = [ROOT / 'port/recomp' / h for h in ('fnv_recomp.h', 'fnv_recomp_host.h', 'pal.h', 'pal_com.h', 'pal_d3d9.h',
                                              'pal_sound.h', 'pal_fs.h', 'guest_heap.h')] + \
          [ROOT / 'port/runtime/fnv_lift_ops.h', OUT / 'import_table.inc', OUT / 'com_tables.inc', OUT / 'd3d9_caps.inc']
PORT = ['fnv_recomp_rt.cpp', 'fnv_intrinsics.cpp', 'pal_kernel.cpp', 'native_math.cpp', 'pal_fs_std.cpp', 'pal_com.cpp', 'pal_user.cpp',
        'pal_input.cpp', 'pal_start.cpp', 'pal_dsound.cpp', 'pal_d3d9.cpp', 'pal_d3dx.cpp', 'pal_dshow.cpp', 'pal_bink.cpp',
        'guest_heap.cpp', 'host_posix.cpp']
GRAPHICS = ['host_window_sdl.cpp', 'renderer_vk.cpp']   # window and Vulkan renderer; without them the D3D ring stays off
VKD3D = ROOT / 'local/dl/vkd3d/vkd3d-1.19'   # patched vkd3d-shader (tools/vkd3d_patches), static


def tool(name):
    for cand in [name] + ['%s-%d' % (name, v) for v in range(25, 13, -1)]:
        if shutil.which(cand):
            return shutil.which(cand)
    raise SystemExit('%s not found' % name)


def compile_one(args):
    cxx, flags, src, obj, key = args
    stamp = Path(str(obj) + '.key')
    if obj.exists() and stamp.exists() and stamp.read_text() == key:
        return src, True, ''
    r = subprocess.run([cxx] + flags + [str(src), '-o', str(obj)], capture_output=True, text=True, errors='replace')
    if r.returncode:
        return src, False, (r.stdout + r.stderr)[-3000:]
    stamp.write_text(key)
    return src, True, ''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 8)
    ap.add_argument('--opt', default=os.environ.get('FNV_POSIX_OPT', '-O1'))
    ap.add_argument('--runtime-base', action='store_true', help='guest window placed at run time (the Switch 2 model)')
    ap.add_argument('--profile', action='store_true',
                    help='shadow call stack for the sampling profiler (FNV_PROFILE_STACK; output out/recomp/posix_prof)')
    ap.add_argument('--define', action='append', default=[], help='extra -D for every file (e.g. FNV_SLOT_CHECK)')
    ap.add_argument('--flag', action='append', default=[], help='extra compiler flag for every file and the link '
                    '(e.g. --flag=-fprofile-instr-generate)')
    ap.add_argument('--build-dir', help='object and binary folder under out/recomp (default posix, posix_rt, posix_prof)')
    a = ap.parse_args()
    graphics = all((ROOT / 'port/recomp' / f).exists() for f in GRAPHICS)
    print('graphics: %s' % ('window + Vulkan renderer' if graphics else 'none (headless: the D3D ring is off, packets are dropped)'))
    cxx = tool('clang++')
    flags = [a.opt, '-g1', '-fno-strict-aliasing', '-fwrapv', '-fno-delete-null-pointer-checks',
             '-fno-strict-float-cast-overflow', '-c', '-w', '-pthread', '-std=c++17', '-I' + str(ROOT / 'port/recomp'), '-I' + str(VKD3D / 'include'),
             ('-DFNV_GUEST_BASE_RUNTIME' if a.runtime_base else '-DFNV_GUEST_BASE=' + GUEST_BASE)] + (['-DFNV_CALL_TRACE'] if os.environ.get('FNV_CALL_TRACE') else []) \
        + (['-DFNV_PROFILE_STACK'] if a.profile else []) + (['-DFNV_NATIVE_MATH'] if (OUT / 'native_math.on').exists() else []) \
        + ['-D' + d for d in a.define] + a.flag
    # native sound decoders (pal_user.cpp Vorbis, pal_dshow.cpp MP3) when their headers are installed
    # (libvorbis-dev, libmpg123-dev); without them voices, ambient loops and music stay silent
    decoders, declibs = [], []
    if os.path.exists('/usr/include/vorbis/vorbisfile.h'):
        decoders.append('-DFNV_HAVE_VORBIS'); declibs += ['-lvorbisfile', '-lvorbis', '-logg']
    if os.path.exists('/usr/include/mpg123.h') or os.path.exists('/usr/include/x86_64-linux-gnu/mpg123.h'):
        decoders.append('-DFNV_HAVE_MPG123'); declibs += ['-lmpg123']
    ffinc = next((d for d in ('/usr/include/x86_64-linux-gnu', '/usr/include') if os.path.exists(d + '/libavcodec/avcodec.h')), None)
    if ffinc:                                   # Bink movies (pal_bink.cpp): libavformat-dev libavcodec-dev libavutil-dev
        decoders.append('-DFNV_HAVE_FFMPEG'); declibs += ['-lavformat', '-lavcodec', '-lavutil']
    flags += decoders
    print('sound decoders: %s' % (' '.join(d[len('-DFNV_HAVE_'):].lower() for d in decoders) or 'none (install libvorbis-dev libmpg123-dev libavformat-dev libavcodec-dev)'))
    global BUILD
    if a.runtime_base: BUILD = OUT / 'posix_rt'
    if a.profile: BUILD = OUT / ('posix_prof_rt' if a.runtime_base else 'posix_prof')
    if a.build_dir: BUILD = OUT / a.build_dir
    BUILD.mkdir(parents=True, exist_ok=True)
    port = PORT + (GRAPHICS if graphics else [])
    if graphics: HEADERS.append(ROOT / 'port/recomp/renderer_vk.h')
    hdr = hashlib.sha1(b''.join(h.read_bytes() for h in HEADERS) + ' '.join(flags).encode()).hexdigest()
    shards = sorted(OUT.glob('recomp_[0-9][0-9].cpp'))
    if not shards:
        raise SystemExit('no lifted shards: run tools/recomp.py first')
    jobs = []
    for src in shards + [ROOT / 'port/recomp' / f for f in port]:
        obj = BUILD / (src.stem + '.o')
        key = hashlib.sha1(src.read_bytes() + hdr.encode()).hexdigest()
        jobs.append((cxx, flags, src, obj, key))
    t0 = time.time()
    failed = 0
    with ThreadPoolExecutor(a.jobs) as ex:
        for src, ok, err in ex.map(compile_one, jobs):
            if not ok:
                failed += 1
                print('FAILED %s\n%s' % (src.name, err))
    print('compiled %d files in %.0f s' % (len(jobs), time.time() - t0))
    if failed:
        raise SystemExit('%d files failed' % failed)
    exe = BUILD / 'fnv_posix'
    libs = ([str(VKD3D / '.libs/libvkd3d-shader.a'), '-lSDL2', '-lvulkan'] if graphics else []) + declibs + ['-lz', '-lm']
    r = subprocess.run([cxx, '-fuse-ld=lld', '-pthread'] + a.flag + ['-o', str(exe)] + [str(j[3]) for j in jobs] + libs,
                       capture_output=True, text=True, errors='replace')
    if r.returncode:
        raise SystemExit('link failed:\n' + r.stderr[-4000:])
    print('linked %s (%.0f MB)' % (exe, exe.stat().st_size / 1e6))


if __name__ == '__main__':
    main()
