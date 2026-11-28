r"""One-command check of the hand-written native_math functions against the recompiled originals.

    python tools/native_check.py C:\Games\FNV\FalloutNV.exe

Needs the player's FalloutNV.exe, so it runs on the owner's machine, never in CI. It lifts the exe with --native (the
originals stay as fo_XXXXXXXX), builds (tools/recomp_build.py on Windows, recomp_build_posix.py elsewhere) and runs
the game with FNV_NATIVE_CHECK=1: every call goes to both versions, and the log gets "[native-check] <addr> differs
..." for a difference (the first 16 per function) and a progress line every 1,048,576 calls. Play a few minutes in a
scene with NPCs and shadows: 0 differences on every address is the pass. 0x00b9f5b0 (ParseObjectAddFrustum) runs the
original in this mode (it has side effects), so it is checked by playing: shadows must match a build without --native.
Back to the lifted code: python tools/recomp.py --exe <exe>   (no --native), then rebuild.
"""
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / 'out/recomp'


def run(cmd, **kw):
    print('>', ' '.join(str(c) for c in cmd), flush=True)
    subprocess.run([str(c) for c in cmd], check=True, **kw)


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    exe = sys.argv[1]
    run([sys.executable, ROOT / 'tools/recomp.py', '--exe', exe, '--native'])
    if os.name == 'nt':
        run([sys.executable, ROOT / 'tools/recomp_build.py'])
        game = OUT / 'fnv_recomp.exe'
    else:
        run([sys.executable, ROOT / 'tools/recomp_build_posix.py'])
        game = OUT / 'posix/fnv_posix'
    print('\nbuilt %s with the native versions. Starting it with FNV_NATIVE_CHECK=1 (run it the usual way if it needs '
          'arguments: set FNV_NATIVE_CHECK=1 in the environment).' % game, flush=True)
    run([game], env=dict(os.environ, FNV_NATIVE_CHECK='1'), cwd=str(Path(exe).resolve().parent))


if __name__ == '__main__':
    main()
