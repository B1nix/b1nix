#!/bin/sh
# tools/build-profile.sh - time each phase of the smoke-suite build.
#
# The smoke suite runs one `make -j$(nproc) iso-sys iso-blk ...`, which reports
# a single wall-clock number and says nothing about where it went. This runs the
# same work as an ordered sequence of `make` invocations -- each one still
# parallel -- and prints seconds per phase, so a change can be attributed.
#
#   sh tools/build-profile.sh            # profile an incremental build
#   PROFILE_LABEL=after sh tools/build-profile.sh
#
# The phases are the smoke build's own dependency order; running them separately
# produces the same artifacts as running them together.
set -eu

ARCH="${ARCH:-x86_64}"
J="${J:-$(nproc 2>/dev/null || echo 4)}"
LABEL="${PROFILE_LABEL:-run}"
OUT="${PROFILE_OUT:-smoke_run/build-profile-$LABEL.txt}"
LOG="${PROFILE_LOG:-smoke_run/build-profile-$LABEL.log}"
mkdir -p "$(dirname "$OUT")"
: > "$OUT"
: > "$LOG"

total_start=$(date +%s)

phase() {
	name="$1"; shift
	start=$(date +%s)
	{ echo "### phase: $name"; echo "### cmd: make -j$J $*"; } >> "$LOG"
	if ! make -j"$J" ARCH="$ARCH" "$@" >> "$LOG" 2>&1; then
		echo "FAILED phase $name -- see $LOG" >&2
		exit 1
	fi
	end=$(date +%s)
	printf '%-22s %5d s\n' "$name" "$((end - start))" | tee -a "$OUT"
}

echo "build profile ($LABEL, -j$J, arch $ARCH)" | tee -a "$OUT"
phase userspace-bins   userspace
phase initramfs-incs   $(make -s ARCH="$ARCH" print-GENERATED_INCS)
phase kernel-objects   objects
phase kernel-link      build/"$ARCH"/kernel.elf
phase modules          modules
phase ports-install    install-ports
phase root-image       root-image
phase check-dynamic    check-dynamic
phase iso-sys          iso-sys
phase iso-gfx          iso-gfx
phase iso-posix        iso-posix
phase iso-blk          iso-blk
phase iso-openrc       iso-openrc
phase iso-init         iso-init
phase iso-switchroot   iso-switchroot

total_end=$(date +%s)
printf '%-22s %5d s\n' TOTAL "$((total_end - total_start))" | tee -a "$OUT"
