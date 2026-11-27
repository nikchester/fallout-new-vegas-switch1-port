# fnv1 probe

Measures what a Switch 1 offers: launch mode, memory, the 4 GiB guest window (two mapping variants, including whether
anything above 2 GiB can be mapped), CPU, GPU memblocks and SD read speed. Output goes to the screen and to
`sdmc:/switch/fnv1/probe.log` (and to the PC when started with `nxlink -s`); nothing else on the SD card is written.

- Build: `tools/switch1/build.sh port/switch1/probe` (produces `probe.nro`).
- Run: copy `probe.nro` to `sdmc:/switch/fnv1/`, start the homebrew menu in Application mode (hold R while launching a
  game), start the probe, press + when it says "Done".
- Run over Wi-Fi: start the homebrew menu as above, press Y (netloader), then on the PC run `nxlink -s probe.nro`; the
  log is printed in the PC terminal and still written to the SD card.
- Host CPU benchmark (e.g. on Linux aarch64): `make -C port/switch1/probe/linux && port/switch1/probe/linux/cpubench`.
