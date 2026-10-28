#!/bin/sh
#
# Install the two tools needed to turn a 3DS ELF into a .cia:
#
#   makerom     https://github.com/3DSGuy/Project_CTR
#   bannertool  https://github.com/carstene1ns/3ds-bannertool (maintained fork
#               of Steveice10's original)
#
# Neither is part of devkitPro's pacman repositories, which is why the
# devkitpro/devkitarm container does not have them.
#
# Both are built from source at a pinned tag instead of being downloaded as
# prebuilt binaries, because the published Linux binaries are linked against a
# newer glibc than the build container ships (they are produced on Ubuntu 24.04;
# devkitpro/devkitarm is Debian bookworm). Running one of those fails with
# "version `GLIBC_2.3x' not found" as soon as the CIA is packaged. Building from
# source costs a few seconds and removes the whole class of problem.
#
# Override with MAKEROM_VERSION / BANNERTOOL_VERSION, or point MAKEROM /
# BANNERTOOL at existing binaries to skip a build.
set -eu

MAKEROM_VERSION="${MAKEROM_VERSION:-0.19.0}"
BANNERTOOL_VERSION="${BANNERTOOL_VERSION:-1.2.3}"
DEST="${DEST:-/usr/local/bin}"

MAKEROM_REPO="${MAKEROM_REPO:-https://github.com/3DSGuy/Project_CTR}"
BANNERTOOL_REPO="${BANNERTOOL_REPO:-https://github.com/carstene1ns/3ds-bannertool}"

mkdir -p "$DEST"

# Fail loudly instead of leaving a binary behind that cannot execute: a broken
# tool must break *this* step, not the packaging step much later.
verify() {
	local name="$1" bin="$2" out
	shift 2
	# `makerom -help` exits non-zero, so only the output is checked here.
	out="$("$bin" "$@" 2>&1 | head -n1 || true)"
	if [ -z "$out" ]; then
		echo "::error::$name at $bin does not run in this image"
		"$bin" "$@" 2>&1 | head -n5 || true
		echo "::error::$(ldd "$bin" 2>&1 | head -n20 | tr '\n' ' ')"
		return 1
	fi
	echo "$name: $out"
}

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# ---------------------------------------------------------------------------
# makerom
# ---------------------------------------------------------------------------
if [ -n "${MAKEROM:-}" ]; then
	echo "using makerom from MAKEROM=$MAKEROM"
else
	echo "building makerom ${MAKEROM_VERSION} from source"
	git clone -q --depth 1 --branch "makerom-v${MAKEROM_VERSION}" \
		"$MAKEROM_REPO" "$work/Project_CTR"
	(
		cd "$work/Project_CTR/makerom"
		# deps (mbedtls, blz, yaml) are vendored in the repository, so this
		# needs no network access.
		make --no-print-directory deps
		make --no-print-directory -j"$(nproc 2>/dev/null || echo 2)"
	)
	install -m 0755 "$work/Project_CTR/makerom/bin/makerom" "$DEST/makerom"
	MAKEROM="$DEST/makerom"
fi

# ---------------------------------------------------------------------------
# bannertool
# ---------------------------------------------------------------------------
if [ -n "${BANNERTOOL:-}" ]; then
	echo "using bannertool from BANNERTOOL=$BANNERTOOL"
else
	echo "building bannertool ${BANNERTOOL_VERSION} from source"
	git clone -q --depth 1 --branch "$BANNERTOOL_VERSION" \
		"$BANNERTOOL_REPO" "$work/3ds-bannertool"
	# The project is a plain C++17 program with only vendored single-header
	# dependencies, so it is compiled directly: that avoids depending on the
	# cmake version inside the container.
	(
		cd "$work/3ds-bannertool"
		"${CXX:-g++}" -std=c++17 -O2 \
			-DVERSION="\"${BANNERTOOL_VERSION}\"" \
			-Isource -Isource/pc \
			-DSTBI_ONLY_PNG -DSTBI_NO_LINEAR -DSTBI_NO_STDIO \
			-DDR_WAV_NO_STDIO \
			-DSTB_VORBIS_NO_PUSHDATA_API -DSTB_VORBIS_NO_STDIO \
			-o "$work/bannertool" \
			source/main.cpp source/log.cpp source/types.cpp \
			source/utils.cpp source/3ds/cbmd.cpp source/3ds/cwav.cpp \
			source/3ds/lz11.cpp source/pc/stb_image.cpp \
			source/pc/stb_vorbis.cpp source/pc/dr_wav.cpp
	)
	install -m 0755 "$work/bannertool" "$DEST/bannertool"
	BANNERTOOL="$DEST/bannertool"
fi

echo "--- installed"
ls -l "$MAKEROM" "$BANNERTOOL"
verify makerom "$MAKEROM" -help
verify bannertool "$BANNERTOOL" --version

# The packaging Makefile looks the tools up on PATH.
case ":$PATH:" in
*":$DEST:"*) ;;
*)
	echo "::warning::$DEST is not on PATH; pass MAKEROM=$MAKEROM BANNERTOOL=$BANNERTOOL to make"
	;;
esac
