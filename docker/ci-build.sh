#!/bin/bash
# Builds GBabe and packages it, then runs the PC-side checks. This is what the
# workflows run inside the build image, and it works the same in the
# devcontainer:
#
#   bash docker/ci-build.sh
#
# GBABE_TARGET limits the packaging: 3dsx (just the .3dsx + .smdh), cia (also
# the installable .cia), or both (the default).
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

target="${GBABE_TARGET:-both}"
case "$target" in
  3dsx) want_cia=0 ;;
  cia)  want_cia=1 ;;
  both) want_cia=1 ;;
  *) echo "GBABE_TARGET must be 3dsx, cia or both (got '$target')" >&2; exit 2 ;;
esac

jobs="$(nproc 2>/dev/null || echo 4)"

echo "=== toolchain ==="
echo "DEVKITARM=${DEVKITARM:-<unset>}"
arm-none-eabi-gcc --version | head -n1

# The stock devkitPro image has the compiler but not the packaging tools; the
# image from docker/gbabe-devcontainer.Dockerfile has both. Installing them
# takes a few seconds and keeps this script usable in either.
if [ "$want_cia" = 1 ] && { ! command -v makerom >/dev/null ||
                            ! command -v bannertool >/dev/null; }; then
  echo "=== packaging tools ==="
  sh .github/scripts/install-3ds-packaging-tools.sh
fi

echo "=== 3dsx ==="
make -j"$jobs"

if [ "$want_cia" = 1 ]; then
  echo "=== cia ==="
  make cia
fi

echo "=== host build + render check ==="
make -C tools/host -j"$jobs"
make -C tools/host check
tools/host/gbabe-host test/dmg-acid2.gb 300 "$ROOT/build/acid2.ppm" 0
test -s "$ROOT/build/acid2.ppm"

echo "=== ui preview ==="
make -C tools/uipreview -j"$jobs"
mkdir -p "$ROOT/build/ui-preview"
tools/uipreview/uipreview "$ROOT/build/ui-preview"

ls -l gbabe.3dsx gbabe.smdh gbabe.cia 2>/dev/null || true
echo "=== done ==="
