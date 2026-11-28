# Owner cycle: from your FalloutNV.exe to a Switch 1 build

One script runs the whole loop on your own PC. The exe, `Data/` and everything generated from them (`out/`, `local/`,
objects, NRO) stay on that PC; only text logs are shared.

## Requirements

- Windows 11 with WSL and Docker Desktop (WSL integration on), or Linux with Docker. Podman is not needed.
- Python 3 with `capstone` (`pip install capstone==5.0.7`) inside the shell that runs the script.
- Clone of `momusdigital/fnv_decomp` next to this repository (`git clone https://github.com/momusdigital/fnv_decomp ../fnv_decomp`) or `FNV_DECOMP`.
- `FalloutNV.exe` from GOG 1.4.0.525, outside the repository, e.g. `Documents/My_Projects/fnv-private/FalloutNV.exe`. Steam or third-party exes are not supported.
- About 8 GB RAM is enough with the default `-j2`; the Docker image `devkitpro/devkita64:20260219` is pulled on first use.
- The Linux host build (`out/recomp/posix`) is built only if `clang++` is available in the shell; otherwise that step is reported as skipped.
- The Switch 1 game build runs only when `port/switch1/game/Makefile` exists (not yet; the step is reported as skipped until then).

## Run

WSL / Linux:

    tools/owner_cycle.sh --exe /mnt/c/Users/<you>/Documents/My_Projects/fnv-private/FalloutNV.exe

Windows PowerShell (calls WSL):

    powershell -File tools\owner_cycle.ps1 -Exe C:\Users\<you>\Documents\My_Projects\fnv-private\FalloutNV.exe

Options: `-j N` (default 2, compilers need 1-2 GB each), `--opt -O1|-O2` (-O1 for iterations, -O2 for speed measurements),
`--ccache DIR` (default `../fnv-private/ccache`), `--no-update` (skip `git pull`), `--skip-recomp` (rebuild only),
`--check-only` (stop after the exe check), `--nxlink HOST` (send the NRO with `nxlink -a HOST -s`; start the homebrew menu
and press Y first). Data on the SD card belongs in `sdmc:/switch/fnv1/`.

Steps: refuse to run unless `.gitignore` covers the outputs; `git pull --ff-only`; `tools/exe_check.py` (at least 12 of 16
probes, else stop); `tools/recomp.py`, `gen_import_table.py`, `gen_com_tables.py`, `gen_d3d9_caps.py`, `data_image.py`;
builds; summary. Unchanged shards and objects are not rebuilt, so a repeat run without changes is fast; a full build is
expected to take 15-30 minutes.

If a compiler process is killed (out of memory in Docker/WSL), the script says so: re-run with `-j1`.

## Linux run without graphics (stage 2.2)

    tools/owner_cycle.sh --exe /path/FalloutNV.exe --run-linux 180

After the Linux build the host runs headless (no window or renderer is linked; the D3D command ring stays off and packets are
dropped) in the exe's folder, so `Data/` must sit next to the exe. `summary.txt` then lists the `[boot]` stage lines
(`kBootMain`, `kBootMenu`, `kBootWorld`, `kBootPorch`) and any `[posix] stopping:` line (`fnv_host_fatal`). The goal of 2.2
is `kBootMenu` with no `stopping:` line. Post `summary.txt` and the last ~100 lines of `run_linux.log` (no game data in them).

## What to send back

Everything goes in `out/owner_cycle/<timestamp>/`. Post only the text files: `summary.txt` (commit, probes matched,
sizes, lifted/unsupported counts, guest heap high-water mark) and the failing step's `*.log`. A guest address
at or above `0x80000000` is reported as an error: the port runs without LAA. Do not send the exe, `out/`, `local/`,
the NRO, or snippets of generated code.

## Without an exe

`tools/owner_cycle.sh --exe /nonexistent` stops with "exe not found". The Linux host with an empty function table
(no game files) is covered by `tools/test_pal_posix.sh` (CI job `unit-tests`).

## Notes

- ccache: `--ccache DIR` is passed to the container by `tools/switch1/build.sh` (`CCACHE_DIR`, mounted at the same path). It helps only if ccache is installed in the image.
- The `lifted/unsupported` and `guest_heap_high_water` lines of `summary.txt` are placeholders: they appear only once `recomp.py` writes `out/recomp/report.json` and the host prints `guest_heap_high_water`. Until then the summary says "not in the logs" for the address check.
