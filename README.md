# Fallout: New Vegas for Switch 2

Fallout: New Vegas running natively on the Nintendo Switch 2 as homebrew.

The original PC `FalloutNV.exe` is statically recompiled to native ARM64 code: no emulator, no JIT. A platform layer
stands in for Windows, Direct3D 9 (rendered through Vulkan) and DirectSound.

**Status:** playable, work in progress. Runs at full 1080p; frame rate depends on the scene (60 fps in open areas,
lower in busy ones).

You need your own PC copy of the game. This project contains no game files.

## Setup

### What you need

- A Switch 2 that can run homebrew (hbmenu through kaiHEN).
- An SD card with about 10 GB free (FAT32 or exFAT).
- Your own PC copy of Fallout: New Vegas.
- `fnv.nro` from a release, or one you built (see below).

### Install

1. On the SD card, make the folder `switch/fnv/`.
2. Copy these into it:
   - `fnv.nro`
   - `FalloutNV.exe` from your game folder
   - `Fallout_default.ini` from your game folder
   - the whole `Data` folder from your game folder
3. You should end up with:

   ```
   sdmc:/switch/fnv/fnv.nro
   sdmc:/switch/fnv/FalloutNV.exe
   sdmc:/switch/fnv/Fallout_default.ini
   sdmc:/switch/fnv/Data/
   ```

A card reader is much faster than FTP for the 9 GB of game data.

### Run

Start hbmenu through kaiHEN (hold R while starting a game), then pick **Fallout New Vegas**. The screen stays dark for
a few seconds while it loads. Close it from the HOME menu.

### Settings

`sdmc:/switch/fnv/fnv_settings.ini` is created on first start. Edit it and restart the game:

| Option | Meaning |
| --- | --- |
| `graphics=high` | `low`, `medium`, `high` (default) or `ultra` |
| `show_fps=0` | `1` shows an fps counter in the corner |
| `threads=1` | runs AI, particles and animation on extra cores (`0` turns it off) |
| `profile=1` | writes performance info to `fnv.log` (`0` turns it off) |

Saves are in `sdmc:/switch/fnv/docs/My Games/FalloutNV/`. To update, replace only `fnv.nro`; your saves and data stay.

If something goes wrong, `sdmc:/switch/fnv/fnv.log` has the log of the last run.

## Building from source

Needs Linux with Python 3 (with `capstone`) and podman.

```bash
podman build -t fnv-ounce -f tools/switch2/Containerfile tools/switch2   # once: the Switch toolchain
python3 tools/recomp.py --exe /path/to/FalloutNV.exe                      # recompile the game code
tools/switch2/build.sh port/switch2/fnv_fast make -j8                    # build the NRO
```

The result is `port/switch2/fnv_fast/fnv.nro`. The compiler needs 1–2 GB of RAM per job.

### Switch 1

Needs docker or podman. `tools/switch1/build.sh` builds the toolchain image (`tools/switch1/Containerfile`) on first
use and runs `make` in the given directory:

```bash
tools/switch1/build.sh port/switch1/hello   # toolchain check: port/switch1/hello/hello.nro
```

How the recompiler works is in [docs/RECOMP.md](docs/RECOMP.md). The browser build is described in
[docs/README_browser.md](docs/README_browser.md).

## Credits

Built on [fnv_decomp](https://github.com/momusdigital/fnv_decomp), the community decompilation of the game. Full list
in [CREDITS.md](CREDITS.md).

Fallout: New Vegas belongs to its owners: Obsidian Entertainment developed it and Bethesda Softworks published it. This
project is not affiliated with or endorsed by them.

## License

[GPL-3.0](LICENSE), for this project's own code only. It grants nothing in the game or its files.
