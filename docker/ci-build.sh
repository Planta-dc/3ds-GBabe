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
# On failure, say which command failed and where, so the log does not just stop.
trap 'rc=$?; echo "ci-build.sh: line $LINENO exited $rc" >&2' ERR

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

# One emulation core: Gambatte 0.5.0. The Walnut-CGB build that used to be made
# alongside it was removed - it was 1.6x-3.4x slower per frame here, and two
# backends meant every front-end change had to be checked twice.
echo "=== 3dsx ==="
make -j"$jobs"

if [ "$want_cia" = 1 ]; then
  echo "=== cia ==="
  make cia
fi

mkdir -p artifacts
cp gbabe.3dsx gbabe.smdh artifacts/ 2>/dev/null || true
[ "$want_cia" = 1 ] && cp gbabe.cia artifacts/

# The binary has to agree with the header, or an artifact gets uploaded that
# reports a different version from the one in the release notes. .github/
# scripts/check-version.sh compares the two version *sources*; this checks the
# thing that was actually built, and says in the log which version was packaged.
echo "=== version stamp ==="
ver="$(sed -n 's/^#define GBABE_VERSION[[:space:]]*"\([^"]*\)".*/\1/p' include/gb3ds.h)"
[ -n "$ver" ] || { echo "could not read GBABE_VERSION from include/gb3ds.h" >&2; exit 1; }
grep -a -q -- "$ver" gbabe.elf ||
  { echo "gbabe.elf does not contain its version string '$ver'" >&2; exit 1; }
echo "packaged GBabe $ver"

echo "=== host build + render check ==="
make -C tools/host -j"$jobs"
make -C tools/host check
make -C tools/host acid2
make -C tools/host logtest
make -C tools/host pacetest
make -C tools/host presenttest
make -C tools/host drawtest
make -C tools/host resamptest
make -C tools/host dectest

# Real-time: two seconds for the stereo run, two for the mono fallback.
echo "=== sound path keeps CSND fed ==="
make -C tools/host audiotest

# Not `make asan` directly: a container that cannot turn address randomisation
# off makes AddressSanitizer die before main(), which is not a result. The
# script says which of the two happened; 77 means "no sanitizer result here",
# and only that is tolerated. The host job runs the same test with
# GBABE_ASAN_REQUIRED=1, so the check is not lost by this.
echo "=== ui stress test ==="
make -C tools/uipreview uistress
tools/uipreview/run-sanitized.sh || {
  rc=$?
  [ "$rc" -eq 77 ] || exit "$rc"
  echo "ui stress: skipped - this container cannot run the sanitizer"
}

# The libretro core, host build and render check (same reference frame as
# `make acid2`), and then the 3DS archive - the only part of this repository's
# 3DS code that is compiled with the real toolchain from sources that do not
# need libctru.
echo "=== libretro core ==="
make -C libretro test
make -C libretro ctr

echo "=== ui preview ==="
make -C tools/uipreview -j"$jobs"
mkdir -p "$ROOT/build/ui-preview"
tools/uipreview/uipreview "$ROOT/build/ui-preview"

ls -l artifacts/ 2>/dev/null || true
echo "=== done ==="
