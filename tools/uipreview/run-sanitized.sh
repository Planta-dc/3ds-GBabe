#!/bin/sh
# Runs ./uistress under AddressSanitizer + UBSan, and tells the two ways it can
# die apart:
#
#   "the sanitizer could not start here"  -> not a result: skip (exit 77)
#   "the test ran and complained"         -> a real failure   (exit 1)
#
# The distinction matters because ASan reserves a shadow mapping at fixed
# addresses, and a kernel with high ASLR entropy (vm.mmap_rnd_bits=32, the
# default on newer Ubuntu) hands out addresses it cannot use: the binary dies
# with "AddressSanitizer:DEADLYSIGNAL" before main() runs, twenty times as the
# handler re-crashes, then "Segmentation fault". Nothing of the test has run.
# See the build that failed on exactly that: the sanitizer's own output,
# and nothing else, in a job whose other checks had all passed.
#
# Turning address randomisation off for the process (`setarch -R`) fixes it,
# and that is what this script tries first. It does not always work: Docker's
# default seccomp profile blocks personality(ADDR_NO_RANDOMIZE), so inside a
# container `setarch -R true` exits non-zero and the call falls back to a
# fully randomised run. The container build therefore asks for
# --security-opt seccomp=unconfined (see .github/workflows/build.yml), which
# makes setarch work there too; this script still copes when it does not,
# rather than turning a runner quirk into a red build.
#
# Set GBABE_ASAN_REQUIRED=1 to make an unavailable sanitizer an error instead of
# a skip. The host job sets it, so the check cannot quietly rot: it runs for
# real on the runner (where setarch -R works), and the container build only ever
# skips it visibly.
#
#   ./run-sanitized.sh
#   GBABE_ASAN_REQUIRED=1 ./run-sanitized.sh

set -u

# Next to this script, not next to the caller: docker/ci-build.sh runs it from
# the repository root, where "./uistress" would not exist.
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
binary="$here/uistress"
attempts=5

if [ ! -x "$binary" ]; then
	echo "run-sanitized: $binary is not built (make -C tools/uipreview uistress)" >&2
	exit 2
fi

# Decide how to run once. The probe also covers the case that bit the build:
# setarch is installed and its -R is refused (EPERM inside a container),
# so calling it anyway would put "setarch: failed to set personality" in the
# output and make it look like the test had failed.
if setarch "$(uname -m)" -R true >/dev/null 2>&1; then
	how="setarch $(uname -m) -R"
	run() { setarch "$(uname -m)" -R "$binary" 2>&1; }
else
	how="setarch -R unavailable, address randomisation left on"
	run() { "$binary" 2>&1; }
fi

# uistress prints "ui stress: start" as its first statement in main(). Its
# presence is the only thing that tells a started run from one that died in the
# sanitizer's own startup before main() was reached - and looking for a line
# rather than for the absence of complaints cannot be fooled by the shell
# printing "Segmentation fault" for a killed child.
started()
{
	printf '%s\n' "$1" | grep -qx 'ui stress: start'
}

i=1
while [ "$i" -le "$attempts" ]; do
	out="$(run)"
	rc=$?

	if [ "$rc" -eq 0 ]; then
		printf '%s\n' "$out"
		if printf '%s\n' "$out" | grep -qx 'ui stress: ok'; then
			echo "run-sanitized: ok ($how)"
			exit 0
		fi
		echo "run-sanitized: the test exited 0 without its summary line" >&2
		exit 1
	fi

	if started "$out"; then
		printf '%s\n' "$out" >&2
		echo "run-sanitized: the test failed (exit $rc)" >&2
		exit 1
	fi

	echo "run-sanitized: attempt $i/$attempts: the sanitizer did not start ($how)" >&2
	i=$((i + 1))
done

{
	echo "run-sanitized: AddressSanitizer never started: $attempts attempts,"
	echo "each dying in its own startup with only 'AddressSanitizer:DEADLYSIGNAL'."
	echo "That is the kernel handing out addresses the shadow mapping cannot use"
	echo "(vm.mmap_rnd_bits=32), not a fault in the test."
} >&2

if [ "${GBABE_ASAN_REQUIRED:-0}" = 1 ]; then
	echo "run-sanitized: GBABE_ASAN_REQUIRED=1, so this is an error" >&2
	exit 1
fi

echo "run-sanitized: SKIPPED - no sanitizer result from this environment" >&2
exit 77
