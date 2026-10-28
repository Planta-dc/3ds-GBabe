#!/usr/bin/env bash
#
# Install the two tools needed to turn a 3DS ELF into a .cia:
#
#   makerom     https://github.com/3DSGuy/Project_CTR
#   bannertool  https://github.com/carstene1ns/3ds-bannertool (maintained fork
#               of Steveice10's original)
#
# Neither is part of devkitPro's pacman repositories, which is why the
# devkitpro/devkitarm container does not have them. Versions are pinned so the
# build stays reproducible; override with MAKEROM_VERSION / BANNERTOOL_VERSION.
set -euo pipefail

MAKEROM_VERSION="${MAKEROM_VERSION:-0.19.0}"
BANNERTOOL_VERSION="${BANNERTOOL_VERSION:-1.2.3}"
DEST="${DEST:-/usr/local/bin}"

MAKEROM_URL="https://github.com/3DSGuy/Project_CTR/releases/download/makerom-v${MAKEROM_VERSION}/makerom-v${MAKEROM_VERSION}-ubuntu_x86_64.zip"
BANNERTOOL_URL="https://github.com/carstene1ns/3ds-bannertool/releases/download/${BANNERTOOL_VERSION}/bannertool-${BANNERTOOL_VERSION}-linux.tar.gz"

if command -v makerom >/dev/null 2>&1 && command -v bannertool >/dev/null 2>&1; then
	echo "makerom and bannertool are already installed"
	exit 0
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

if ! command -v makerom >/dev/null 2>&1; then
	echo "fetching makerom ${MAKEROM_VERSION}"
	curl -sSL -o "$work/makerom.zip" "$MAKEROM_URL"
	unzip -q -o "$work/makerom.zip" -d "$work/makerom"

	# The archive layout has changed between releases, so find the binary
	# rather than hard-coding a path.
	bin="$(find "$work/makerom" -type f -name makerom | head -n1)"
	if [ -z "$bin" ]; then
		echo "error: no makerom binary inside $MAKEROM_URL" >&2
		exit 1
	fi
	install -m 0755 "$bin" "$DEST/makerom"
fi

if ! command -v bannertool >/dev/null 2>&1; then
	echo "fetching bannertool ${BANNERTOOL_VERSION}"
	curl -sSL -o "$work/bannertool.tar.gz" "$BANNERTOOL_URL"
	mkdir -p "$work/bannertool"
	tar xzf "$work/bannertool.tar.gz" -C "$work/bannertool"

	bin="$(find "$work/bannertool" -type f -name bannertool -perm -u+x | head -n1)"
	[ -n "$bin" ] || bin="$(find "$work/bannertool" -type f -name bannertool | head -n1)"
	if [ -z "$bin" ]; then
		echo "error: no bannertool binary inside $BANNERTOOL_URL" >&2
		exit 1
	fi
	install -m 0755 "$bin" "$DEST/bannertool"
fi

echo "--- installed"
ls -l "$DEST/makerom" "$DEST/bannertool"
"$DEST/makerom" -help 2>&1 | head -n1 || true
"$DEST/bannertool" 2>&1 | head -n1 || true

case ":$PATH:" in
  *":$DEST:"*) ;;
  *) echo "warning: $DEST is not on PATH; pass MAKEROM=$DEST/makerom BANNERTOOL=$DEST/bannertool to make" >&2 ;;
esac
