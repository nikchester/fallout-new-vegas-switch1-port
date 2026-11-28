#!/bin/sh
# Runs a command (default: make) in a directory of this repository, inside the Switch 1 toolchain container
# (tools/switch1/Containerfile, image fnv-switch1). Uses podman if it is installed, otherwise docker; set ENGINE to
# choose. The image is built on first use. The repository is mounted at /src.
#   tools/switch1/build.sh port/switch1/hello            # make
#   tools/switch1/build.sh port/switch1/hello make clean
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
engine=${ENGINE:-$(command -v podman >/dev/null 2>&1 && echo podman || echo docker)}
dir=${1:-.}
shift || true
[ $# -gt 0 ] || set -- make -j"$(nproc)"
"$engine" image inspect fnv-switch1 >/dev/null 2>&1 ||
    "$engine" build -t fnv-switch1 -f "$root/tools/switch1/Containerfile" "$root/tools/switch1"
# docker runs as root inside the container: pass the caller's ids so built files are not owned by root.
# Rootless podman already maps root to the caller.
user=
[ "$(basename "$engine")" = podman ] || user="--user $(id -u):$(id -g)"
# Compiler cache: if CCACHE_DIR names a host folder, mount it at the same path and pass it on (ccache is used when present
# in the image).
cc=
if [ -n "${CCACHE_DIR:-}" ]; then mkdir -p "$CCACHE_DIR"; cc="-v $CCACHE_DIR:$CCACHE_DIR -e CCACHE_DIR=$CCACHE_DIR"; fi
# shellcheck disable=SC2086
exec "$engine" run --rm $user $cc -v "$root:/src" -w "/src/$dir" -e DEVKITPRO=/opt/devkitpro fnv-switch1 \
    sh -c 'export PATH=/opt/devkitpro/tools/bin:/opt/devkitpro/devkitA64/bin:$PATH; exec "$@"' sh "$@"
