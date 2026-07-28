#!/bin/sh
# M40 — build tools/m40/linux_abi_test.c into a static Linux x86_64 ELF blob and
# stamp EI_OSABI = ELFOSABI_LINUX (3), so b1nix's loader tags it with the Linux
# personality exactly as it does for a stock Linux binary.
#
# Output: tools/m40/linux_abi_test.bin (committed; staged into the rootfs as
# /bin/m40-linux-abi). Regenerated only when this script is run by hand — the
# kernel build does not depend on a Linux-targeting compiler being installed.
set -e

DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$DIR/linux_abi_test.c"
OUT="$DIR/linux_abi_test.bin"

CC="${CC:-clang}"
LD="${LD:-ld.lld}"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

"$CC" -target x86_64-linux-gnu -ffreestanding -fno-stack-protector -fno-pic -mno-sse -mno-sse2 \
      -fno-builtin -O1 -Wall -Wextra -nostdlib -c "$SRC" -o "$TMP/abi.o"
"$LD" -static -e _start "$TMP/abi.o" -o "$OUT"

# Stamp EI_OSABI (e_ident[7]) = 3 (ELFOSABI_LINUX).
printf '\003' | dd of="$OUT" bs=1 seek=7 count=1 conv=notrunc status=none

echo "wrote $OUT ($(wc -c < "$OUT") bytes)"
