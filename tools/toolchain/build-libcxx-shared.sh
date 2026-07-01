#!/bin/sh
# Build the shared LLVM C++ runtime for b1nix — libc++abi.so.1 + libc++.so.1 —
# from the PIC static archives produced by tools/toolchain/build-libcxx.sh, and
# stage them into the cross sysroot + rootfs so C++ binaries link the C++ stdlib
# DYNAMICALLY instead of folding ~10 MB of libc++ into every executable.
#
# Layout (mirrors the GCC side's libstdc++.so.6 + libgcc_s.so, but using LLVM):
#   libc++abi.so.1  — folds libc++abi.a AND libunwind.a, so it exports both the
#                     Itanium C++ ABI (__cxa_*, vtables, type_info) and the DWARF
#                     unwinder (_Unwind_*). NEEDS libc.so.1.
#   libc++.so.1     — the C++ standard library. NEEDS libc++abi.so.1 + libc.so.1.
#
# Why fold libunwind INTO libc++abi.so rather than ship it separately: there must
# be exactly one unwinder instance in the process. libc++abi calls _Unwind_*, the
# executable's landing pads call _Unwind_Resume, and both resolve to this single
# copy. Cross-DSO exceptions then work because libunwind locates each object's
# FDEs through dl_iterate_phdr + its PT_GNU_EH_FRAME (.eh_frame_hdr) segment — the
# standard ELF mechanism, NOT libgcc's __register_frame registry. So unlike the
# GCC libstdc++ path, NO kernel-side __register_frame call and NO .eh_frame zero
# terminator are needed: --eh-frame-hdr on every object + the in-kernel
# dl_iterate_phdr (SYS_DL_PHDR_INFO) table are sufficient.
#
# We link with ld.lld (not the cross binutils ld): libc++ is built by clang and
# emits its static constructors into .init_array natively, so lld's default
# -shared layout produces a proper DT_INIT_ARRAY that the M69 loader's M75 path
# runs. (The libstdc++ side needs binutils ld only because GCC emits legacy
# .ctors that lld would orphan.)
set -eu

ARCH="${ARCH:-${B1NIX_ARCH:-x86_64}}"
TRIPLET="${B1NIX_TRIPLET:-x86_64-b1nix}"
[ "$ARCH" = "x86" ] && TRIPLET="i686-b1nix"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

LD="$(command -v ld.lld 2>/dev/null || echo ld.lld)"

RT_INSTALL="$ROOT/build/toolchain_build/$TRIPLET/llvm-runtimes-build/libcxx-install/lib"
CXXABI_A="$RT_INSTALL/libc++abi.a"
UNWIND_A="$RT_INSTALL/libunwind.a"
CXX_A="$RT_INSTALL/libc++.a"
# compiler-rt builtins (__divti3, __multi3, …): libc++.so / libc++abi.so reference
# a few of these, so resolve them into the .so (NOT whole-archive — only the
# referenced builtins are pulled) to keep each .so self-contained at load time.
CRT_A="$ROOT/build/toolchain_build/$TRIPLET/llvm-runtimes-build/install/lib/libcompiler_rt.a"
for f in "$CXXABI_A" "$UNWIND_A" "$CXX_A" "$CRT_A"; do
    [ -f "$f" ] || { echo "missing $f — run tools/toolchain/build-libcxx.sh first" >&2; exit 1; }
done

CROSSLIB="$ROOT/build/toolchain_build/$TRIPLET/cross/$TRIPLET/lib"
LIBC_SO="$CROSSLIB/libc.so.1"
[ -f "$LIBC_SO" ] || ARCH="$ARCH" sh "$ROOT/tools/toolchain/stage-shared-libc.sh" >/dev/null 2>&1 || true
[ -f "$LIBC_SO" ] || { echo "missing libc.so.1 in $CROSSLIB — run stage-shared-libc.sh first" >&2; exit 1; }

ABI_SO="$CROSSLIB/libc++abi.so.1"
CXX_SO="$CROSSLIB/libc++.so.1"

# ── 1. libc++abi.so.1 (libunwind is already folded into libc++abi.a) ──────────
# build-libcxx.sh builds libc++abi with LIBCXXABI_ENABLE_STATIC_UNWINDER=ON, so
# libc++abi.a already contains the libunwind objects (the _Unwind_* DWARF
# unwinder). Whole-archiving libunwind.a as well would duplicate every _Unwind_*
# symbol, so fold ONLY libc++abi.a here.
# Hide libunwind's libgcc-incompatible frame-registration API from the dynamic
# symbol table. b1nix's crt0 (the shared libgcc model) does, for every dynamic
# binary, `if (&__register_frame) __register_frame(__EH_FRAME_BEGIN__)` — passing
# the .eh_frame SECTION start, which is libgcc's __register_frame contract. LLVM
# libunwind's __register_frame expects a SINGLE FDE instead, so it misreads the
# leading CIE ("FDE is really a CIE"), corrupts its dynamic-FDE registry, and the
# next unwind segfaults in findUnwindSectionsByPhdr. libc++ binaries don't need it
# at all — libunwind finds every object's FDEs through dl_iterate_phdr + its
# PT_GNU_EH_FRAME (.eh_frame_hdr). A version script localizes these symbols so they
# leave the .dynsym entirely: crt0's weak __register_frame reference then resolves
# to 0 (no loaded .so exports it), crt0 skips the bogus registration, and
# exceptions unwind via the .eh_frame_hdr path. (None are called internally by
# libc++abi/libunwind — they are the public JIT frame API only.) A version script
# at link time is required; llvm-objcopy --localize-symbol does not drop an entry
# already present in a shared object's .dynsym.
VSCRIPT="$(mktemp)"
cat > "$VSCRIPT" <<'EOF'
{
  global: *;
  local:
    __register_frame;
    __deregister_frame;
};
EOF
echo "[libc++-shared] linking $ABI_SO from libc++abi.a (libunwind folded in)"
"$LD" -shared -soname libc++abi.so.1 --eh-frame-hdr -o "$ABI_SO" \
    --whole-archive "$CXXABI_A" --no-whole-archive \
    -L"$CROSSLIB" "$LIBC_SO" "$CRT_A" \
    --version-script "$VSCRIPT" \
    --allow-shlib-undefined
rm -f "$VSCRIPT"
ln -sf libc++abi.so.1 "$CROSSLIB/libc++abi.so"

# ── 2. libc++.so.1 (NEEDS libc++abi.so.1 + libc.so.1) ─────────────────────────
echo "[libc++-shared] linking $CXX_SO from libc++.a"
"$LD" -shared -soname libc++.so.1 --eh-frame-hdr -o "$CXX_SO" \
    --whole-archive "$CXX_A" --no-whole-archive \
    -L"$CROSSLIB" "$ABI_SO" "$LIBC_SO" "$CRT_A" \
    --allow-shlib-undefined
ln -sf libc++.so.1 "$CROSSLIB/libc++.so"

# Normalise NEEDED entries to bare sonames (the names the initramfs serves under
# /lib), in case lld recorded a build-tree path for libc.so.1.
if command -v patchelf >/dev/null 2>&1; then
    for so in "$ABI_SO" "$CXX_SO"; do
        for n in $(patchelf --print-needed "$so" 2>/dev/null); do
            case "$n" in
                */libc.so.1) patchelf --replace-needed "$n" libc.so.1 "$so" 2>/dev/null || true ;;
                */libc++abi.so.1) patchelf --replace-needed "$n" libc++abi.so.1 "$so" 2>/dev/null || true ;;
            esac
        done
    done
fi

# ── 3. Stage to rootfs /lib (loader search path) + rootfs-as-sysroot lib ──────
for d in "$ROOT/build/$ARCH/rootfs/lib" "$ROOT/build/$ARCH/rootfs/usr/lib"; do
    [ -d "$d" ] || mkdir -p "$d"
    cp -f "$ABI_SO" "$d/libc++abi.so.1"; ln -sf libc++abi.so.1 "$d/libc++abi.so"
    cp -f "$CXX_SO"  "$d/libc++.so.1";   ln -sf libc++.so.1    "$d/libc++.so"
done

echo "[libc++-shared] done:"
for so in "$ABI_SO" "$CXX_SO"; do
    echo "  $(basename "$so") ($(du -m "$so" | cut -f1) MB)"
    echo "    soname: $(patchelf --print-soname "$so" 2>/dev/null || echo '?')"
    echo "    needed: $(patchelf --print-needed "$so" 2>/dev/null | tr '\n' ' ')"
done
echo "$CXX_SO"
