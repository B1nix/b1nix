#!/bin/sh
# Build a shared libstdc++.so.6 for b1nix from the cross toolchain's static
# libstdc++.a, and stage it into the cross sysroot + rootfs so C++ binaries can
# link the C++ standard library DYNAMICALLY instead of folding ~15 MB of it into
# every executable.
#
# Why this works without a GCC rebuild: b1nix's GCC target is classified
# newlib/elf, so libstdc++'s own configure forces enable_shared=no and ships only
# libstdc++.a. But tools/toolchain/build-toolchain.sh deliberately builds that
# archive with -fPIC (so it can fold into libLLVM.so / any C++ .so), which means
# its objects are position-independent and can be linked straight into a shared
# object here. libstdc++.a already bundles the libsupc++ objects, so we fold only
# it (folding libsupc++.a too would duplicate __cxa_demangle et al.).
#
# Symbols are exported UNVERSIONED. b1nix C++ binaries are compiled against the
# static archive's unversioned symbols (no .symver in the headers), so an
# unversioned .so resolves them 1:1 — no GLIBCXX_3.4.x version map needed.
#
# M75 prerequisite: libstdc++.so's static constructors (std::ios_base::Init for
# cout/cerr, locale facets) live in its .init_array; the kernel now runs shared-
# library init_arrays, so they actually fire — without M75 a shared libstdc++
# would leave iostream uninitialised.
set -eu

ARCH="${ARCH:-${B1NIX_ARCH:-x86_64}}"
TRIPLET="${B1NIX_TRIPLET:-x86_64-b1nix}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
# Use the cross binutils ld, NOT ld.lld, to link the shared object. The b1nix
# GCC target emits its global constructors into the legacy .ctors section
# (initfini-array is disabled for this newlib target). ld.lld's default -shared
# layout leaves .ctors as an orphan section with NO DT_INIT_ARRAY and NO DT_INIT,
# so nothing ever runs them — libstdc++'s ios_base::Init (std::cout/cin/cerr) and
# the EH/RTTI global state stay unconstructed, and every C++ binary that links
# the .so crashes (null std::cout vtable / uncaught-exception abort). The binutils
# default linker script merges .ctors -> .init_array and emits DT_INIT_ARRAY, so
# the M69 loader's M75 path runs them. (Static C++ binaries are unaffected:
# linker-cxx.ld already folds .ctors into the executable's own .init_array.)
XLD="$ROOT/build/toolchain_build/$TRIPLET/cross/bin/$TRIPLET-ld"
LD="$(command -v "$XLD" 2>/dev/null || command -v "$TRIPLET-ld" 2>/dev/null || echo "$XLD")"

LIBDIR="$ROOT/build/toolchain_build/$TRIPLET/cross/$TRIPLET/lib"
STATIC="$LIBDIR/libstdc++.a"
[ -f "$STATIC" ] || { echo "missing $STATIC — run tools/toolchain/build-toolchain.sh first" >&2; exit 1; }
# Ensure the cross sysroot has an up-to-date shared libc to link against.
[ -f "$LIBDIR/libc.so.1" ] || ARCH="$ARCH" sh "$ROOT/tools/toolchain/stage-shared-libc.sh" >/dev/null 2>&1 || true
[ -f "$LIBDIR/libc.so.1" ] || { echo "missing libc.so.1 in $LIBDIR — run tools/toolchain/stage-shared-libc.sh first" >&2; exit 1; }

OUT="$LIBDIR/libstdc++.so.6"
echo "[libstdc++-shared] linking $OUT from $(basename "$STATIC")"
# -L + -lgcc_s (not a full path) so the NEEDED entry records the soname
# 'libgcc_s.so.1', not a build-tree path. --allow-shlib-undefined: libc symbols
# resolve from libc.so.1 at load time via the M69 loader.
# --eh-frame-hdr emits .eh_frame_hdr + a PT_GNU_EH_FRAME program header, which the
# in-kernel loader exposes via dl_iterate_phdr. __b1nix_run_dso_init decodes that
# to the .eh_frame start and calls __register_frame so the libgcc_s.so DWARF
# unwinder (classic FDE registry) can find THIS object's FDEs when a C++ exception
# is thrown from inside it (e.g. __cxa_throw) and unwinds back across the .so/exe
# boundary. eh_frame_end.o appends the zero terminator __register_frame scans to
# (normally crtendS.o's job, but this newlib target has no crt files).
EHEND="$LIBDIR/b1nix_eh_frame_end.o"
"$(command -v "$TRIPLET-gcc" 2>/dev/null || echo "$ROOT/build/toolchain_build/$TRIPLET/cross/bin/$TRIPLET-gcc")" \
    -c "$(dirname "$0")/eh_frame_end.S" -o "$EHEND"
"$LD" -shared -soname libstdc++.so.6 --eh-frame-hdr -o "$OUT" \
    --whole-archive "$STATIC" --no-whole-archive \
    -L"$LIBDIR" "$LIBDIR/libc.so.1" -lgcc_s \
    --allow-shlib-undefined \
    "$EHEND"

# Normalise the NEEDED libgcc_s entry to 'libgcc_s.so' — the exact name the
# initramfs serves it under (/lib/libgcc_s.so, the soname the --enable-shared
# cross GCC emits) and that the M69 loader searches for. binutils ld already
# records the bare 'libgcc_s.so' from -lgcc_s, so this only rewrites a stray
# build-tree path back to that soname; do NOT rewrite to 'libgcc_s.so.1' (no
# such file exists in the initramfs → the loader would fail the DT_NEEDED).
if command -v patchelf >/dev/null 2>&1; then
    for n in $(patchelf --print-needed "$OUT" 2>/dev/null); do
        case "$n" in
            libgcc_s.so) : ;;                  # already correct
            */libgcc_s.so|*/libgcc_s.so.*|libgcc_s.so.*)
                patchelf --replace-needed "$n" libgcc_s.so "$OUT" 2>/dev/null || true ;;
        esac
    done
fi

# Provide the linker-name symlink libstdc++.so -> libstdc++.so.6 so `-lstdc++`
# resolves to the shared object (in the cross sysroot, for the cross toolchain).
ln -sf libstdc++.so.6 "$LIBDIR/libstdc++.so"

echo "[libstdc++-shared] done: $OUT ($(du -m "$OUT" | cut -f1) MB)"
echo "  soname  : $(patchelf --print-soname "$OUT" 2>/dev/null || echo libstdc++.so.6)"
echo "  needed  : $(patchelf --print-needed "$OUT" 2>/dev/null | tr '\n' ' ')"
echo "$OUT"
