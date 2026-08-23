#!/bin/sh
# M40 — build tools/blobs/linux_abi_test.c into a static Linux x86_64 ELF blob and
# stamp EI_OSABI = ELFOSABI_LINUX (3), so b1nix's loader tags it with the Linux
# personality exactly as it does for a stock Linux binary.
#
# Output: tools/blobs/linux_abi_test.bin (x86_64) and
# tools/blobs/linux_abi_test_aarch64.bin — both committed; the kernel build picks
# the one matching ARCH and stages it as /bin/m40-linux-abi. Regenerated only
# when this script is run by hand, so the kernel build does not depend on a
# Linux-targeting compiler being installed.
#
# The source is one file for both: the syscall numbers are per-arch (asm-generic
# renumbers everything and drops the legacy calls entirely), and the handful of
# calls x86_64 has as dedicated numbers go through the t_* wrappers.
set -e

DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$DIR/linux_abi_test.c"
CC="${CC:-clang}"
LD="${LD:-ld.lld}"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

build_one() {
	target="$1"; out="$2"; extra="$3"
	# aarch64 puts the ELF in the USER half. The default 0x200000 base lands in
	# L0[0] there — the kernel half every address space shares by pointer, which
	# fork() does not clone (only L0[1..511] are copied). A blob linked there
	# crashed in the child the moment it forked.
	case "$target" in
	aarch64*) LDEXTRA="--image-base=0x500000000000" ;;
	*)        LDEXTRA="" ;;
	esac
	# shellcheck disable=SC2086
	"$CC" -target "$target" -ffreestanding -fno-stack-protector -fno-pic $extra \
	      -fno-builtin -O1 -Wall -Wextra -nostdlib -c "$SRC" -o "$TMP/abi.o"
	"$LD" -static -e _start $LDEXTRA "$TMP/abi.o" -o "$out"
	# Stamp EI_OSABI (e_ident[7]) = 3 (ELFOSABI_LINUX).
	printf '\003' | dd of="$out" bs=1 seek=7 count=1 conv=notrunc status=none
	echo "wrote $out ($(wc -c < "$out") bytes)"
}

build_one x86_64-linux-gnu  "$DIR/linux_abi_test.bin"         "-mno-sse -mno-sse2"
build_one aarch64-linux-gnu "$DIR/linux_abi_test_aarch64.bin" ""
