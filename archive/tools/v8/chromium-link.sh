#!/bin/sh
# Relink Chromium content_shell (and its b1nix-target graphics .so's) as b1nix
# ELFs from the gn-compiled objects. Mirrors tools/v8/v8-link-d8.sh: gn's own
# link drives clang++ with host defaults (links /usr/lib/libc.so.6) — wrong for
# b1nix — so we relink the exact object set gn computed (the .rsp files) with the
# b1nix recipe: crt0 + b1nix linker script + libc++/libgcc/libm + whole-archived
# libb1nix, in one --start-group. The 4 graphics .so's are runtime-dlopen'd, so
# content_shell links independently of them.
#
# Run AFTER: ninja -k 0 -C out/b1nix content_shell  (compiles everything; the .so
# links + the final content_shell link fail under the gn toolchain — expected;
# the .o/.rsp are what we use). Needs out/b1nix/content_shell.rsp to exist.
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
TC="$ROOT_DIR/build/toolchain_build/x86_64-b1nix/cross/bin"
OUT="$ROOT_DIR/build/toolchain_build/chromium/src/out/b1nix"
GXX="$TC/x86_64-b1nix-g++"
LD="$TC/x86_64-b1nix-ld"
RANLIB="$TC/x86_64-b1nix-ranlib"

CRT0="$ROOT_DIR/userspace/build/x86_64/crt/crt0.o"
LINKER_EXE="$ROOT_DIR/userspace/linker-cxx.ld"
LINKER_SO="$ROOT_DIR/userspace/linker_shared_cxx.ld"
LIBB1="$ROOT_DIR/userspace/build/x86_64/libb1nix.a"
LIBC_SO="$ROOT_DIR/userspace/build/x86_64/libc.so.1"
LIBM="$ROOT_DIR/build/openlibm-b1nix/x86_64-b1nix/install/lib/libm.a"
LIBGCC="$("$GXX" -print-libgcc-file-name)"

[ -f "$OUT/content_shell.rsp" ] || { echo "missing $OUT/content_shell.rsp — run 'ninja -k 0 -C out/b1nix content_shell' first"; exit 1; }
[ -f "$CRT0" ] || { echo "missing crt0.o — build userspace (B1NIX_ARCH=x86_64)"; exit 1; }
[ -f "$LIBC_SO" ] || { echo "missing libc.so.1 — build userspace (B1NIX_ARCH=x86_64)"; exit 1; }

# powl/sincosf etc.: long-double + GNU math the freestanding libb1nix can't carry
# (it's whole-archived into binaries that don't link libm). Compile the thin
# wrapper here; it resolves against libm which the big binaries link.
MATHL_O="$OUT/mathl-b1nix.o"
"$GXX" -x c -c "$ROOT_DIR/userspace/libc/mathl.c" -o "$MATHL_O"

# gn emits thin archives (ar -D) with no symbol index; ld needs one.
echo "ranlib'ing archives (this is slow — ~2k thin .a)..."
find "$OUT" -name '*.a' -exec "$RANLIB" {} \; 2>/dev/null || true

cd "$OUT"

# Strip the clang-driver-only bits from a gn .rsp so the raw ld can consume it:
#  - drop -Wl, prefixes
#  - drop the host runtime libs the driver injects (-lc/-lgcc_s/-lrt/-latomic/-lm)
# Keeps the in-rsp libc++.a/libc++abi.a paths + the --whole-archive stub objects.
clean_rsp() {
	sed -e 's/-Wl,//g' \
	    -e 's/\(^\| \)-l\(c\|gcc_s\|rt\|atomic\|m\|dl\|pthread\) / /g' "$1"
}

# --- the b1nix-target graphics .so's (runtime-dlopen'd Vulkan/GL drivers) ---
for so in libEGL.so libGLESv2.so libvulkan.so.1 libvk_swiftshader.so; do
	[ -f "$OUT/$so.rsp" ] || { echo "  skip $so (no .rsp)"; continue; }
	echo "linking $so ..."
	clean_rsp "$OUT/$so.rsp" > "$OUT/$so.b1nix.rsp"
	"$LD" -m elf_x86_64 -shared -z norelro --hash-style=sysv -soname "$so" \
		-T "$LINKER_SO" -o "$OUT/$so" \
		--start-group @"$OUT/$so.b1nix.rsp" "$LIBGCC" "$LIBM" \
		--whole-archive "$LIBB1" --no-whole-archive "$LIBC_SO" --end-group
	"$TC/x86_64-b1nix-readelf" -hd "$OUT/$so" | grep -E 'Class|Machine|Type|SONAME|NEEDED' || true
done

# --- content_shell as a static b1nix ELF (d8-style; independent of the .so's) ---
echo "linking content_shell ..."
clean_rsp "$OUT/content_shell.rsp" > "$OUT/content_shell.b1nix.rsp"
# Rust rlibs/std, like v8-link-d8.sh (GN binds them outside the .rsp).
RUST_RLIBS=""
[ -f obj/content/shell/content_shell.ninja ] && RUST_RLIBS="$(grep -E '^[[:space:]]*rlibs =' obj/content/shell/content_shell.ninja | head -1 | sed 's/^[[:space:]]*rlibs = //')"
RUST_STD=""
[ -d prebuilt_rustc_sysroot/lib/rustlib/x86_64-unknown-b1nix/lib ] && \
	RUST_STD="$(ls prebuilt_rustc_sysroot/lib/rustlib/x86_64-unknown-b1nix/lib/*.rlib 2>/dev/null | tr '\n' ' ')"

"$LD" -m elf_x86_64 -T "$LINKER_EXE" --gc-sections --allow-multiple-definition \
	-o content_shell.b1nix \
	"$CRT0" \
	--start-group @content_shell.b1nix.rsp $RUST_RLIBS $RUST_STD \
	"$MATHL_O" "$LIBGCC" "$LIBM" \
	--whole-archive "$LIBB1" --no-whole-archive --end-group

echo "=== linked: $OUT/content_shell.b1nix ==="
"$TC/x86_64-b1nix-readelf" -h content_shell.b1nix | grep -E 'Class|Machine|Type|Entry'
ls -la content_shell.b1nix
