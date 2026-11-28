#!/bin/bash
# One owner-PC cycle: exe check -> recompile -> generated tables -> build (Linux host, Switch 1) -> optional nxlink -> logs.
# Runs in WSL, Git Bash or Linux with Docker (Docker Desktop or docker engine; podman is not needed). Nothing derived from the
# game (out/, local/, objects, NRO) leaves this machine: the only thing meant for sharing is the text folder printed at the end.
#
#   tools/owner_cycle.sh --exe /mnt/c/Users/me/Documents/My_Projects/fnv-private/FalloutNV.exe [options]
#
#   --exe PATH       FalloutNV.exe from GOG 1.4.0.525 (or FNV_EXE); default ../fnv-private/FalloutNV.exe
#   --decomp DIR     clone of momusdigital/fnv_decomp (or FNV_DECOMP); default ../fnv_decomp
#   -j N             parallel compiler processes, default 2 (1-2 GB each; use -j1 if the build is killed)
#   --opt -O1|-O2    optimisation level, default -O1 (-O2 for speed measurements)
#   --ccache DIR     compiler cache folder (default ../fnv-private/ccache, outside the repository)
#   --no-update      do not run git pull first
#   --skip-recomp    reuse out/recomp (only rebuild)
#   --nxlink HOST    after the Switch 1 build send the NRO with `nxlink -a HOST -s` (homebrew menu netloader, Y)
#   --run-linux [S]  after the Linux build run the headless host for S seconds (default 120) in the exe's folder (Data/ next to it)
#                    and add the [boot] stage lines and any fnv_host_fatal to summary.txt
#   --check-only     stop after the exe check
set -u
cd "$(dirname "$0")/.." || exit 1
ROOT=$PWD
PARENT=$(dirname "$ROOT")
EXE=${FNV_EXE:-$PARENT/fnv-private/FalloutNV.exe}
DECOMP=${FNV_DECOMP:-$PARENT/fnv_decomp}
JOBS=2; OPT=-O1; CCACHE_DIR_ARG=; UPDATE=1; RECOMP=1; NXHOST=; CHECK_ONLY=0; RUNSEC=0
while [ $# -gt 0 ]; do
  case $1 in
    --exe) EXE=$2; shift ;;
    --decomp) DECOMP=$2; shift ;;
    -j) JOBS=$2; shift ;;
    -j[0-9]*) JOBS=${1#-j} ;;
    --opt) OPT=$2; shift ;;
    --ccache) CCACHE_DIR_ARG=$2; shift ;;
    --no-update) UPDATE=0 ;;
    --skip-recomp) RECOMP=0 ;;
    --nxlink) NXHOST=$2; shift ;;
    --run-linux) RUNSEC=120; case ${2:-} in ''|-*) ;; *) RUNSEC=$2; shift ;; esac ;;
    --check-only) CHECK_ONLY=1 ;;
    -h|--help) sed -n 2,21p "$0"; exit 0 ;;
    *) echo "unknown option: $1 (see --help)"; exit 2 ;;
  esac
  shift
done
CCACHE=${CCACHE_DIR_ARG:-$PARENT/fnv-private/ccache}
STAMP=$(date +%Y%m%d-%H%M%S)
LOGS=$ROOT/out/owner_cycle/$STAMP
mkdir -p "$LOGS"
LOG=$LOGS/cycle.log
say() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$LOG"; }
die() { say "STOP: $*"; say "logs: $LOGS"; exit 1; }
# step NAME LOGFILE CMD... : run, keep the full output in LOGFILE, show the last lines on failure
step() {
  local name=$1 f=$LOGS/$2; shift 2
  say "== $name"
  local t=$SECONDS
  "$@" >"$f" 2>&1; local rc=$?
  say "   $name: exit $rc, $((SECONDS - t)) s (log $(basename "$f"))"
  if [ $rc -ne 0 ]; then
    tail -30 "$f" | tee -a "$LOG"
    if grep -qiE 'killed|out of memory|cannot allocate memory|signal 9|exit code 137' "$f" || [ $rc -eq 137 ]; then
      say "The compiler was probably killed for lack of memory. Re-run with -j1, and close other programs."
    fi
    die "$name failed"
  fi
}

# 0. prerequisites -------------------------------------------------------------------------------------------------------
for c in git python3; do command -v $c >/dev/null 2>&1 || die "$c not found in PATH"; done
PY=python3
[ -f "$EXE" ] || die "exe not found: $EXE (pass --exe PATH; FalloutNV.exe from GOG 1.4.0.525, kept outside the repository)"
[ -d "$DECOMP/tools" ] || die "fnv_decomp clone not found at $DECOMP. Run: git clone https://github.com/momusdigital/fnv_decomp $DECOMP   (or pass --decomp DIR)"
[ -f "$DECOMP/build/image_check.json" ] || die "$DECOMP/build/image_check.json missing: build or update the fnv_decomp clone first"
export FNV_DECOMP=$DECOMP
$PY -c 'import capstone' 2>/dev/null || die "python module capstone missing: pip install capstone==5.0.7"
# Outputs must be git-ignored, so game-derived files can not be committed by accident.
for p in out/x local/x build/x port/switch1/game/build/x data_image.bin x.nro; do
  git check-ignore -q "$p" || die ".gitignore does not cover '$p'; update the branch (git pull) before running"
done
git check-ignore -q "$(basename "$EXE")" 2>/dev/null || true
case $(cd "$(dirname "$EXE")" && pwd) in "$ROOT"|"$ROOT"/*) git check-ignore -q "$EXE" || die "the exe is inside the repository and not ignored" ;; esac

# 1. update ------------------------------------------------------------------------------------------------------------
if [ $UPDATE -eq 1 ]; then
  if [ -n "$(git status --porcelain --untracked-files=no)" ]; then say "local changes present: skipping git pull"
  else step "git pull" git_pull.log git pull --ff-only; fi
fi
say "commit: $(git rev-parse --short HEAD) ($(git rev-parse --abbrev-ref HEAD)), jobs -j$JOBS, opt $OPT"

# 2. exe check -----------------------------------------------------------------------------------------------------------
step "exe_check" exe_check.log $PY -B tools/exe_check.py "$EXE"
cat "$LOGS/exe_check.log" | tee -a "$LOG" | grep -E 'probes matched|image base|timestamp'
M=$(sed -n 's/^probes matched: \([0-9]*\)\/.*/\1/p' "$LOGS/exe_check.log")
[ "${M:-0}" -ge 12 ] || die "exe_check matched ${M:-0} probes (need >= 12): this is not the GOG 1.4.0.525 exe"
[ $CHECK_ONLY -eq 1 ] && { say "check only: done"; echo "logs: $LOGS"; exit 0; }

# docker is needed for the Switch 1 build: fail before the long steps, not after
if [ -f port/switch1/game/Makefile ]; then
  command -v docker >/dev/null 2>&1 || command -v podman >/dev/null 2>&1 || die "docker not found (start Docker Desktop, enable WSL integration)"
fi

# 3. recompile and tables ------------------------------------------------------------------------------------------------
if [ $RECOMP -eq 1 ]; then
  step "recomp" recomp.log $PY -B tools/recomp.py --exe "$EXE" --shards 64
  [ -f out/recomp/report.json ] && $PY -c "import json;r=json.load(open('out/recomp/report.json'));print('recomp: %d functions, %d unsupported, %d s'%(r['lifted'],r['unsupported'],r['seconds']))" | tee -a "$LOG"
  step "import table" gen_import_table.log $PY -B tools/gen_import_table.py --exe "$EXE"
  step "com tables" gen_com_tables.log $PY -B tools/gen_com_tables.py
  step "d3d9 caps" gen_d3d9_caps.log $PY -B tools/gen_d3d9_caps.py
  step "data image" data_image.log $PY -B tools/data_image.py --exe "$EXE"
fi

# 4. builds --------------------------------------------------------------------------------------------------------------
if command -v clang++ >/dev/null 2>&1; then
  step "build linux" build_linux.log $PY -B tools/recomp_build_posix.py --jobs "$JOBS" --opt "$OPT"
else
  say "build linux: SKIPPED (clang++ not found in this shell; the Linux host build needs clang, lld, SDL2, vulkan headers)"
fi
if [ $RUNSEC -gt 0 ] && [ -x out/recomp/posix/fnv_posix ]; then
  say "== run linux (headless, $RUNSEC s, cwd $(dirname "$EXE"))"
  ( cd "$(dirname "$EXE")" && timeout "$RUNSEC" "$ROOT/out/recomp/posix/fnv_posix" --exe "$EXE" ) >"$LOGS/run_linux.log" 2>&1
  say "   run linux: exit $? (124 = still running at the time limit; log run_linux.log)"
fi
SW=port/switch1/game
if [ -f $SW/Makefile ]; then
  mkdir -p "$CCACHE"
  export CCACHE_DIR=$CCACHE
  step "build switch1" build_switch1.log env OPT="$OPT" tools/switch1/build.sh $SW make -j"$JOBS" OPT="$OPT"
  NRO=$(ls $SW/*.nro 2>/dev/null | head -1)
  [ -n "$NRO" ] && say "NRO: $NRO ($(stat -c %s "$NRO") bytes) - stays on this PC and the SD card"
  if [ -n "$NXHOST" ] && [ -n "$NRO" ]; then
    command -v nxlink >/dev/null 2>&1 || die "nxlink not found (devkitPro tools); run it from the container or copy $NRO to sdmc:/switch/fnv1/"
    say "== nxlink to $NXHOST (homebrew menu: press Y first); stop with Ctrl-C when done"
    nxlink -a "$NXHOST" -s "$NRO" 2>&1 | tee "$LOGS/nxlink.log"
  fi
else
  say "build switch1: SKIPPED ($SW/Makefile does not exist yet)"
fi

# 5. summary for the task (the lifted/unsupported and guest_heap_high_water lines are placeholders: they appear once recomp.py
#    writes out/recomp/report.json and the host prints guest_heap_high_water; until then they are absent / 'not in the logs') --------------------------------------------------------------------------------------------------
{
  echo "commit: $(git rev-parse --short HEAD) opt=$OPT jobs=$JOBS"
  echo "probes matched: $M"
  if [ -f "$LOGS/run_linux.log" ]; then
    grep -E '^\[boot\]' "$LOGS/run_linux.log" || echo "[boot] no stage reached"
    grep -m3 -E '\[posix\] stopping|fnv_host_fatal' "$LOGS/run_linux.log" || echo "no fatal in run_linux.log"
  fi
  for f in out/recomp/posix/fnv_posix $SW/*.nro; do [ -f "$f" ] && echo "size $(basename "$f"): $(stat -c %s "$f") bytes"; done
  [ -f out/recomp/report.json ] && $PY -c "import json;r=json.load(open('out/recomp/report.json'));print('lifted %d, unsupported %d'%(r['lifted'],r['unsupported']))"
  # guest address check: the port runs without LAA, so a guest address >= 0x80000000 is an error
  hw=$(grep -rhoE 'guest_heap_high_water[^0-9a-fx]*(0x)?[0-9a-fA-F]+' "$LOGS" out/recomp/*.log 2>/dev/null | tail -1)
  if [ -n "$hw" ]; then
    v=$(echo "$hw" | grep -oE '(0x)?[0-9a-fA-F]+$'); case $v in 0x*) ;; *) v=0x$v ;; esac
    echo "$hw"; [ $((v)) -ge $((0x80000000)) ] && echo "ERROR: guest address >= 0x80000000"
  else echo "guest_heap_high_water: not in the logs"; fi
} | tee "$LOGS/summary.txt"
say "done. Send the text files in $LOGS (summary.txt, cycle.log, *.log) - never the exe, out/, local/ or the NRO."
