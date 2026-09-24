#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# tools/toolchain/kernel/gen_btf.sh <kernel.elf.stage1> <blob-out>
#
# Encode the first-pass kernel's DWARF as BTF with pahole and print an
# assembler file that places the blob in the .BTF section (see the linker
# scripts). pahole comes from tools/toolchain/build-pahole.sh; when it cannot be
# had, the blob is empty and the kernel reports that it has no BTF.
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../../.." && pwd)"
ELF="$1"
BLOB="$2"

rm -f "$BLOB"
if PAHOLE="$(sh "$ROOT_DIR/tools/toolchain/build-pahole.sh" 2>/dev/null)"; then
	LIBDIR="$(dirname "$(dirname "$PAHOLE")")/lib"
	# pahole wants somewhere to write; -j runs its CUs in parallel.
	if ! LD_LIBRARY_PATH="$LIBDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
		"$PAHOLE" --btf_encode_detached="$BLOB" -j "$ELF" >&2; then
		echo "gen_btf: pahole failed -- the kernel will have no BTF" >&2
		rm -f "$BLOB"
	fi
else
	echo "gen_btf: no pahole -- the kernel will have no BTF" >&2
fi
[ -f "$BLOB" ] || : >"$BLOB"

printf '.section .BTF, "a"\n'
printf '.balign 8\n'
printf '.incbin "%s"\n' "$BLOB"
