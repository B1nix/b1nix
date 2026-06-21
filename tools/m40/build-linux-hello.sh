#!/bin/sh
# M40 — assemble + link tools/m40/linux_hello.S into a static Linux x86_64 ELF
# blob, then set EI_OSABI = ELFOSABI_LINUX (3) so the binary advertises a Linux
# personality two ways (EI_OSABI and the embedded .note.ABI-tag).
#
# Output: tools/m40/linux_hello.bin (committed; consumed by the initramfs build).
# The blob is regenerated only when this script is run by hand — the kernel build
# does not depend on a Linux assembler being present on the build host.
set -e

DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$DIR/linux_hello.S"
OUT="$DIR/linux_hello.bin"

CC="${CC:-clang}"
LD="${LD:-ld.lld}"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

"$CC" -target x86_64-linux-gnu -nostdlib -c "$SRC" -o "$TMP/linux_hello.o"
"$LD" -static -e _start "$TMP/linux_hello.o" -o "$OUT"

# Stamp EI_OSABI (e_ident[7]) = 3 (ELFOSABI_LINUX). printf writes one raw byte.
printf '\003' | dd of="$OUT" bs=1 seek=7 count=1 conv=notrunc status=none

echo "wrote $OUT ($(wc -c < "$OUT") bytes)"
