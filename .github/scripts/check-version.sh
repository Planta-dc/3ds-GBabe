#!/bin/sh
#
# The version lives in two places, and they have to agree:
#
#   include/gb3ds.h   GBABE_VERSION, shown in the menu and the status readout
#   Makefile          VERSION_MAJOR/MINOR/MICRO, which makerom stamps into the
#                     .cia that the 3DS itself displays
#
# If they drift, a release installs under one number and reports another. When
# this runs on a tag push the tag (v0.1.0) is checked as well.
#
#   sh .github/scripts/check-version.sh
set -eu

hdr_version() {
	sed -n 's/^#define[[:space:]]*GBABE_VERSION[[:space:]]*"\([^"]*\)".*/\1/p' \
		include/gb3ds.h | head -n1
}

mk_version() {
	major=$(sed -n 's/^VERSION_MAJOR[[:space:]]*:=[[:space:]]*\([0-9][0-9]*\).*/\1/p' Makefile)
	minor=$(sed -n 's/^VERSION_MINOR[[:space:]]*:=[[:space:]]*\([0-9][0-9]*\).*/\1/p' Makefile)
	micro=$(sed -n 's/^VERSION_MICRO[[:space:]]*:=[[:space:]]*\([0-9][0-9]*\).*/\1/p' Makefile)
	if [ -z "$major" ] || [ -z "$minor" ] || [ -z "$micro" ]; then
		echo ""
		return
	fi
	echo "$major.$minor.$micro"
}

app="$(hdr_version)"
cia="$(mk_version)"
tag=""
case "${GITHUB_REF_NAME:-}" in
v*) tag="${GITHUB_REF_NAME#v}" ;;
esac

echo "include/gb3ds.h GBABE_VERSION: ${app:-<not found>}"
echo "Makefile VERSION_*:            ${cia:-<not found>}"
[ -n "$tag" ] && echo "release tag:                   $tag"

fail=0

if [ -z "$app" ]; then
	echo "::error file=include/gb3ds.h::GBABE_VERSION is missing."
	fail=1
fi

if [ -z "$cia" ]; then
	echo "::error file=Makefile::VERSION_MAJOR/VERSION_MINOR/VERSION_MICRO are missing."
	fail=1
fi

if [ -n "$app" ] && [ -n "$cia" ] && [ "$app" != "$cia" ]; then
	echo "::error file=Makefile::VERSION_* in the Makefile ($cia) does not match GBABE_VERSION in include/gb3ds.h ($app)."
	fail=1
fi

if [ -n "$tag" ] && [ "$tag" != "$app" ]; then
	echo "::error file=include/gb3ds.h::The release tag (${GITHUB_REF_NAME}) does not match GBABE_VERSION ($app)."
	fail=1
fi

# And nothing may spell the version out itself. The UI preview kept showing
# 0.1.0 long after the application said 0.2.0, because tools/uipreview passed the
# string in by hand instead of using GBABE_VERSION - which meant the picture CI
# publishes as "what the bottom screen looks like" was of a version that no
# longer existed. Only GBABE_VERSION may contain a version number.
literals="$(grep -rnE '[0-9]+\.[0-9]+\.[0-9]+' source include tools 2>/dev/null |
	grep -v 'GBABE_VERSION' | grep -vi 'gambatte' || true)"

if [ -n "$literals" ]; then
	printf '%s\n' "$literals"
	echo "::error::A version number is typed into the source. It goes stale with the next bump - use GBABE_VERSION from include/gb3ds.h."
	fail=1
fi

if [ "$fail" -eq 0 ]; then
	echo "versions agree: $app, and no source spells a version out itself"
fi

exit $fail
