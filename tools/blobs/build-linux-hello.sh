#!/bin/sh
# M40 — assemble + link tools/blobs/linux_hello.S into a static Linux x86_64 ELF
# blob, then set EI_OSABI = ELFOSABI_LINUX (3) so the binary advertises a Linux
# personality two ways (EI_OSABI and the embedded .note.ABI-tag).
#
# Output: tools/blobs/linux_hello.bin (committed; consumed by the initramfs build).
# The blob is regenerated only when this script is run by hand — the kernel build
# does not depend on a Linux assembler being present on the build host.
set -e

DIR="$(cd "$(dirname "$0")" && pwd)"
CC="${CC:-clang}"
LD="${LD:-ld.lld}"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

build_one() {
	target="$1"; src="$2"; out="$3"
	# See build-linux-abi-test.sh: 0x200000 is the shared kernel half on aarch64.
	case "$target" in
	aarch64*) LDEXTRA="--image-base=0x500000000000" ;;
	*)        LDEXTRA="" ;;
	esac
	"$CC" -target "$target" -nostdlib -c "$src" -o "$TMP/linux_hello.o"
	"$LD" -static -e _start $LDEXTRA "$TMP/linux_hello.o" -o "$out"
	# Stamp EI_OSABI (e_ident[7]) = 3 (ELFOSABI_LINUX). printf writes one raw byte.
	printf '\003' | dd of="$out" bs=1 seek=7 count=1 conv=notrunc status=none
	echo "wrote $out ($(wc -c < "$out") bytes)"
}

# One blob per architecture. The two sources are separate files rather than one
# #ifdef'd file: they are pure assembly, and the aarch64 one does not merely
# renumber the syscalls — asm-generic has no open/getdents and no arch_prctl, so
# several steps drive different calls to check the same property.
build_one x86_64-linux-gnu  "$DIR/linux_hello.S"         "$DIR/linux_hello.bin"
build_one aarch64-linux-gnu "$DIR/linux_hello_aarch64.S" "$DIR/linux_hello_aarch64.bin"
