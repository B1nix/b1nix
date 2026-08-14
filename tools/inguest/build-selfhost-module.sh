#!/bin/sh
# M26 native-Clang kernel self-host: build the ext4 ram0 module that lets b1nix
# compile its OWN kernel, in-guest, with its own clang + ld.lld.
#
# Unlike the legacy run-build.py path (which needs a full bootable rootfs with
# busybox/getty/login/make), this is driven SHELL-FREE by a kernel handler
# (b1nix.selfhostbuild in kernel/main.c): the kernel mounts this module as
# /mnt/build, spawns `clang -c` once per kernel TU, then links with `ld.lld @rsp`
# and checks the produced kernel.elf — mirroring the proven tools/inguest/clang-proof.sh.
#
# The module carries: /bin/clang (+ resource headers), /bin/ld.lld, /lib/libc.so.1,
# the kernel source tree + the generated .inc inputs, a flat TU list (srcs.txt:
# "src-relpath  obj-abspath" per line) and the linker response file (kernel.rsp).
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
# Prefer the DYNAMIC native toolchain (44 MB clang + 5.5 MB lld, both backed by a
# demand-paged libLLVM-22.so) over the static 94 MB clang — the whole reason for
# the self-host module is to keep the resident footprint low. When the dynamic
# prefix is present, libLLVM-22.so is staged to /lib so the M69 loader resolves it.
USR="$ROOT_DIR/build/native-clang/b1nix-dyn/usr"
[ -d "$USR/bin" ] || USR="$ROOT_DIR/build/native-clang/b1nix/usr"
LIBLLVM="$USR/lib/libLLVM-22.so"   # present only for the dynamic toolchain
LIBC="$ROOT_DIR/userspace/build/x86_64/libc.so.1"
RSP_SRC="$ROOT_DIR/tools/inguest/kernel-make.rsp"
OUT="$ROOT_DIR/build/selfhost-out"
IMG="$OUT/selfhost.img"

CLANG="$USR/bin/clang-22"
LLD="$USR/bin/lld"
RESDIR="$(ls -d "$USR"/lib/clang/* 2>/dev/null | head -1)"

for f in "$CLANG" "$LLD" "$LIBC" "$RSP_SRC"; do
	[ -f "$f" ] || { echo "missing: $f"; exit 1; }
done
[ -d "$RESDIR/include" ] || { echo "missing clang resource headers"; exit 1; }
STRIP="${STRIP:-strip}"

echo "=== [1/4] stage toolchain + kernel source ($([ -f "$LIBLLVM" ] && echo dynamic || echo static)) ==="
STAGE="$OUT/stage"
rm -rf "$STAGE"; mkdir -p "$STAGE/bin" "$STAGE/lib" "$STAGE/src" "$STAGE/obj"
cp "$CLANG" "$STAGE/bin/clang";  "$STRIP" --strip-unneeded "$STAGE/bin/clang" 2>/dev/null || true
cp "$LLD"   "$STAGE/bin/ld.lld"; "$STRIP" --strip-unneeded "$STAGE/bin/ld.lld" 2>/dev/null || true
cp "$LIBC"  "$STAGE/lib/libc.so.1"
# M93: userspace self-host builder (replaces in-kernel run_selfhost_build).
SELFBUILD="$ROOT_DIR/userspace/build/x86_64/bin/selfhost_build"
if [ -f "$SELFBUILD" ]; then
	cp "$SELFBUILD" "$STAGE/bin/selfhost-build"
	"$STRIP" --strip-unneeded "$STAGE/bin/selfhost-build" 2>/dev/null || true
fi
# Dynamic toolchain: ship the shared LLVM so clang/lld resolve it (soname) at /lib.
[ -f "$LIBLLVM" ] && cp "$LIBLLVM" "$STAGE/lib/libLLVM-22.so"
RESV="$(basename "$RESDIR")"
mkdir -p "$STAGE/bin-lib/clang/$RESV"
cp -R "$RESDIR/include" "$STAGE/bin-lib/clang/$RESV/"   # -> /lib/clang/<v>/include relative to /bin/clang

# Kernel source tree (+ generated .inc the kernel TUs #include from build/x86_64).
tar -C "$ROOT_DIR" -cf - --exclude='*.o' --exclude='*.a' --exclude='.git' kernel | tar -C "$STAGE/src" -xf -
mkdir -p "$STAGE/src/build/x86_64"
# This in-guest build compiles standalone (no host Makefile CFLAGS), and only the
# minimal .inc set is staged below. Force the staged initramfs.c into minimal
# mode by prepending the define (the committed source no longer hardcodes it, so
# normal host builds get the full initramfs).
if ! head -1 "$STAGE/src/kernel/fs/initramfs.c" | grep -q "define MINIMAL_INITRAMFS"; then
	printf '#define MINIMAL_INITRAMFS 1\n' | cat - "$STAGE/src/kernel/fs/initramfs.c" > "$STAGE/src/kernel/fs/initramfs.c.tmp"
	mv "$STAGE/src/kernel/fs/initramfs.c.tmp" "$STAGE/src/kernel/fs/initramfs.c"
fi
# The staged kernel/fs/initramfs.c is in MINIMAL_INITRAMFS mode, so it only
# #includes the minimal .inc set; lapic.c needs ap_trampoline.inc. Staging only
# these (not all ~531MB of port-binary .inc) keeps the module small.
for inc in ap_trampoline initramfs_applet_registration initramfs_applet_symlinks \
           initramfs_native_smoke initramfs_return_42 \
           initramfs_b1cc_hello initramfs_b1cc_argv initramfs_b1cc_file_write \
           initramfs_b1cc_stderr_exit initramfs_b1cc_better_c; do
	cp "$ROOT_DIR/build/x86_64/$inc.inc" "$STAGE/src/build/x86_64/" 2>/dev/null || true
done

echo "=== [2/4] generate flat TU list (srcs.txt) + response file (kernel.rsp) ==="
# Authoritative object set = exactly what the host links into kernel.elf: every
# kernel/**/*.o (minus the ap_trampoline_tmp intermediate, which becomes a .inc,
# not a link input) plus the generated kallsyms.o. Derived from the host build
# tree so it never drifts from the real KERNEL_SOURCES (the old kernel-make.rsp
# was a stale 92-TU snapshot, missing ~22 current TUs). .c TUs compile in-guest;
# the 6 .S stubs and kallsyms.o are pre-staged from the host build.
: > "$STAGE/srcs.txt"; : > "$STAGE/kernel.rsp"
OBJDIRS=""
for hostobj in $(find "$ROOT_DIR/build/x86_64/kernel" -name '*.o' | sort); do
	rel="${hostobj#$ROOT_DIR/build/x86_64/}"; rel="${rel%.o}"   # kernel/arch/x86_64/arch
	case "$rel" in *ap_trampoline_tmp) continue ;; esac
	obj="/mnt/build/obj/$rel.o"
	d="$(dirname "obj/$rel")"
	case " $OBJDIRS " in *" $d "*) ;; *) OBJDIRS="$OBJDIRS $d" ;; esac
	if [ -f "$STAGE/src/$rel.c" ]; then
		printf '%s\t%s\n' "$rel.c" "$obj" >> "$STAGE/srcs.txt"
		printf '%s\n' "$obj" >> "$STAGE/kernel.rsp"
	else
		# .S stubs and generated objects (e.g. kallsyms) are pre-staged from the
		# host build: clang assembles .S via a separate cc1as process whose
		# re-exec can't resolve clang's own path on b1nix (empty InstalledDir;
		# canonical-prefix/dladdr resolution fails, -no-canonical-prefixes hangs).
		# The .c TUs — all real codegen — compile in-guest. TODO: assemble .S too.
		mkdir -p "$STAGE/obj/$(dirname "$rel")"
		cp "$hostobj" "$STAGE/obj/$rel.o" && printf '%s\n' "$obj" >> "$STAGE/kernel.rsp" \
			|| echo "WARN could not stage host obj $rel.o"
	fi
done
mkdir -p "$STAGE/tmp"   # writable TMPDIR for any in-guest clang intermediates
# kallsyms.o: produced by the host 2-stage link (gen_kallsyms.sh). Pre-staged so
# the in-guest single-stage ld.lld link resolves the kallsyms symbol table.
if [ -f "$ROOT_DIR/build/x86_64/kallsyms.o" ]; then
	cp "$ROOT_DIR/build/x86_64/kallsyms.o" "$STAGE/obj/kallsyms.o"
	printf '/mnt/build/obj/kallsyms.o\n' >> "$STAGE/kernel.rsp"
fi
echo "  $(wc -l < "$STAGE/srcs.txt") C TUs compiled in-guest; $(grep -c '\.o$' "$STAGE/kernel.rsp") total link objects"

echo "=== [3/4] build ext4 module ==="
SZ=$(du -sm "$STAGE" | cut -f1)
mkdir -p "$OUT"
IMG_MB=$(( SZ + 96 ))
rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count="$IMG_MB" status=none
# mke2fs -d populates the whole tree (incl. the pre-created obj subdir skeleton)
# in one shot, so the kernel handler never needs mkdir.
mkdir -p $(for d in $OBJDIRS; do echo "$STAGE/$d"; done) 2>/dev/null || true
# place resource headers where clang looks (../lib relative to /bin/clang = /lib)
cp -R "$STAGE/bin-lib/clang" "$STAGE/lib/clang"; rm -rf "$STAGE/bin-lib"
mke2fs -F -q -t ext4 -O ^metadata_csum,^64bit,^flex_bg,^huge_file -b 4096 -d "$STAGE" "$IMG"
echo "  selfhost.img = ${IMG_MB}MB ($(wc -l < "$STAGE/kernel.rsp") objs)"

echo "=== [4/4] done -> $IMG ==="
echo "Boot a kernel built with the b1nix.selfhostbuild handler + this module as ram0:"
echo "  (see tools/inguest/selfhost-proof.sh)"
