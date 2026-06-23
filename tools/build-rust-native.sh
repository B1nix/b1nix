#!/bin/sh
# build-rust-native.sh — M68: build a NATIVE rustc + cargo whose HOST is
# x86_64-unknown-b1nix (rustc that RUNS ON b1nix). The Rust analog of M26
# (native GCC self-host).
#
# Strategy (see tools/patches/chromium/M68-NATIVE-RUST-PLAN.md):
#   - build host = x86_64-unknown-linux-gnu (uses prebuilt CI LLVM)
#   - host       = x86_64-unknown-b1nix  (host_tools=true built-in target,
#                  added in compiler/rustc_target/src/spec/targets/
#                  x86_64_unknown_b1nix.rs of the pinned rust source tree)
#   - LLVM for the b1nix host is CROSS-BUILT from the bundled submodule with the
#     b1nix cross g++ (build/toolchain_build/x86_64-b1nix/cross/bin).
#
# Everything lives under build/rust-native (gitignored). The pinned source +
# the b1nix target patch + bootstrap.toml are staged here by the M68 work; this
# script just drives x.py with the right env.
#
# Prereqs:
#   - tools/build-toolchain.sh  (the x86_64-b1nix cross gcc/g++)
#   - tools/build-rust-toolchain.sh  (stages the b1nix libc sysroot for the
#     cross gcc so rustc's link resolves -lc / crt0)
#   - build/rust-native/rust-src-full  (rust-lang/rust @ the nightly commit,
#     with the b1nix target spec + bootstrap.toml applied)
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/build/rust-native/rust-src-full"
CROSS="$ROOT/build/toolchain_build/x86_64-b1nix/cross/bin"

[ -d "$SRC" ] || { echo "error: $SRC missing — clone rust-lang/rust there first"; exit 1; }
[ -x "$CROSS/x86_64-b1nix-g++" ] || { echo "error: cross g++ missing; run tools/build-toolchain.sh"; exit 1; }

# Make sure the b1nix libc + headers are staged into EVERY path the cross g++
# searches. The LLVM cross-build invokes the bare compiler (not the rustc link
# path), so its headers/lib must be current in the toolchain's own
# include/lib dirs, not just the rustc sysroot. Build the worktree libc first.
echo ">> building b1nix userspace libc + crt0 (x86_64)"
# Build the PIC libc objects too (libc.so.1 target): the combined libc is folded
# into LLVM/rustc *shared* objects (libLLVM.so etc.), which require PIC — the
# non-PIC libb1nix.a triggers `R_X86_64_32 ... recompile with -fPIC` in lld.
make -C "$ROOT/userspace" -s B1NIX_ARCH=x86_64 \
    build/x86_64/libb1nix.a build/x86_64/libc.so.1 build/x86_64/crt/crt0.o

TC="$ROOT/build/toolchain_build/x86_64-b1nix"
LIBA="$ROOT/userspace/build/x86_64/libb1nix.a"
PICDIR="$ROOT/userspace/build/x86_64/libc-pic"
CRT0="$ROOT/userspace/build/x86_64/crt/crt0.o"
LIBM="$ROOT/build/openlibm-b1nix/x86_64-b1nix/install/lib/libm.a"
AR="$CROSS/x86_64-b1nix-ar"
[ -f "$LIBM" ] || { echo "error: $LIBM missing — run tools/build-openlibm.sh (LLVM links round/sqrt/pow)"; exit 1; }

# LLVM's APInt/ConstantFolding/JSON reference libm (round, sqrt, pow, sinh, ...).
# The cross g++ driver auto-links -lc but NOT -lm, so fold openlibm into the
# staged libc.a (musl-style: libm IS libc) — then the implicit -lc resolves the
# math symbols on every link line without per-target -lm surgery.
COMBINED="$ROOT/build/rust-native/libc-with-libm.a"
# NOTE: `ar -M addlib` concatenates members by name, so libb1nix.a's string.o and
# any same-named openlibm member COLLIDE — the second wins and silently drops
# libb1nix's mem*/string defs (memset/memcpy/memmove/memcmp). Extract both into
# separate dirs, prefix the libm objects to guarantee unique names, then re-archive.
CDIR="$ROOT/build/rust-native/.libc-combine"
rm -rf "$CDIR" "$COMBINED"; mkdir -p "$CDIR/a" "$CDIR/b"
# Fold the PIC libc objects (not the non-PIC libb1nix.a) so the combined archive
# can be linked into shared objects. openlibm is now built -fPIC too.
if ls "$PICDIR"/*.o >/dev/null 2>&1; then
    cp "$PICDIR"/*.o "$CDIR/a/"
else
    ( cd "$CDIR/a" && "$AR" x "$LIBA" )  # fallback: static (non-shared) toolchain
fi
( cd "$CDIR/b" && "$AR" x "$LIBM" )
for f in "$CDIR"/b/*.o; do mv "$f" "$CDIR/b/libm_$(basename "$f")"; done
"$AR" rcs "$COMBINED" "$CDIR"/a/*.o "$CDIR"/b/*.o
"$CROSS/x86_64-b1nix-ranlib" "$COMBINED"
[ -f "$COMBINED" ] || { echo "error: failed to build combined libc+libm archive"; exit 1; }
# Sanity: the combined libc MUST still define the C mem* funcs (the collision bug).
for s in memset memcpy memmove memcmp malloc free; do
    "$CROSS/x86_64-b1nix-nm" "$COMBINED" 2>/dev/null | grep -qE " T $s\$" || \
        { echo "error: combined libc.a missing definition of $s — fold broke libc"; exit 1; }
done

echo ">> staging libc(+libm) + headers into the cross toolchain (all search paths)"
# Idempotent copy: only overwrite when content differs, so unchanged files keep
# their mtimes. The LLVM cross-build (cmake/ninja) tracks the toolchain
# libs/headers as inputs — blindly `cp`-ing them every run bumps mtimes and
# forces a FULL ~3500-object LLVM recompile on every driver restart. Copying
# only on real change makes restarts resume from the ninja object cache.
stage_file() { # src dst : copy only if missing or differing
    if [ -L "$2" ] || ! cmp -s "$1" "$2" 2>/dev/null; then
        rm -f "$2"; cp "$1" "$2"
    fi
}
for inc in "$TC/sysroot/include" "$TC/sysroot/usr/include" "$TC/cross/x86_64-b1nix/include"; do
    mkdir -p "$inc"
    # Walk every header; stage_file each so unchanged headers keep their mtime.
    ( cd "$ROOT/userspace/include" && find . -type f ) | while IFS= read -r rel; do
        rel="${rel#./}"; mkdir -p "$inc/$(dirname "$rel")"
        stage_file "$ROOT/userspace/include/$rel" "$inc/$rel"
    done
done
for libdir in "$TC/sysroot/lib" "$TC/cross/x86_64-b1nix/lib"; do
    mkdir -p "$libdir"
    # stage_file also handles the pre-existing-SYMLINK case (libc.a -> libb1nix.a):
    # the `-L` test forces a real-file replacement.
    stage_file "$LIBA" "$libdir/libb1nix.a"
    stage_file "$COMBINED" "$libdir/libc.a"
    # Stage the COMBINED archive (libc + libm) as BOTH libc.a AND libm.a.
    # LLVM tools (e.g. llvm-tblgen) link with an explicit `-lm` but NO `-lc`;
    # the cross g++ driver's implicit `-lc` can be dropped under
    # `-Wl,--gc-sections` link ordering, so `-lm` must self-resolve the C
    # mem*/str* funcs too. Plain openlibm libm.a lacks them — use COMBINED.
    stage_file "$COMBINED" "$libdir/libm.a"
    stage_file "$CRT0" "$libdir/crt0.o"
    # Rust's `unwind` crate links `-lunwind` (LLVM libunwind name) on musl+crt-static
    # targets, but b1nix ships gcc's unwinder as libgcc_eh.a (same `_Unwind_*` ABI).
    # The toolchain otherwise has only an EMPTY stub libunwind.a, so the unwinder
    # symbols go unresolved at the final rustc link. Alias the real gcc unwinder.
    _eh="$("$CROSS/x86_64-b1nix-gcc" -print-file-name=libgcc_eh.a)"
    [ -f "$_eh" ] && cp -f "$_eh" "$libdir/libunwind.a"
done

# Verify the staging actually landed in EVERY search path. The shared
# build/toolchain_build tree can be re-staged by a co-tenant build (e.g. the
# Chromium port using the main checkout's older userspace headers), which would
# silently clobber our copies and waste a multi-hour LLVM compile against stale
# headers. Fail fast if any staged unistd.h diverges from the source.
for inc in "$TC/sysroot/include" "$TC/sysroot/usr/include" "$TC/cross/x86_64-b1nix/include"; do
    for h in unistd.h sys/mman.h; do
        if ! cmp -s "$ROOT/userspace/include/$h" "$inc/$h"; then
            echo "error: staged $inc/$h does not match source — staging clobbered" >&2
            exit 1
        fi
    done
done
echo ">> staging verified in all cross g++ include search paths"

export PATH="$CROSS:$PATH"
# Keep parallelism modest so we don't starve a co-tenant build.
JOBS="${RUST_NATIVE_JOBS:-4}"

cd "$SRC"
# Cargo caches the per-target crate-type probe (which crate types rustc can
# emit) in `.rustc_info.json`, keyed on `rustc -vV` output. Editing a built-in
# target spec (e.g. flipping crt_static_allows_dylibs / dynamic_linking) does
# NOT change the rustc version string, so cargo silently reuses a stale probe
# and may wrongly conclude the b1nix target can't produce proc-macro/dylib —
# failing the stage2 build long after the spec was fixed. Nuke these caches each
# run; re-probing rustc is a few milliseconds and removes the landmine entirely.
find "$SRC/build" -name .rustc_info.json -delete 2>/dev/null || true
# rustc_llvm/build.rs cross-LLVM hack: it takes the HOST llvm-config's -I/-L
# paths and blindly string-replaces the host triple with the target triple,
# "praying those dirs are the same" (its words). The host uses a *downloaded*
# `ci-llvm/`, but the b1nix LLVM is built *from source* into `llvm/` — so the
# replaced path `build/x86_64-unknown-b1nix/ci-llvm/{include,lib}` would be
# missing. Make the prayer come true with a symlink. Dangling until x.py builds
# llvm/ on a fresh run; resolves by the time rustc_llvm compiles PassWrapper.cpp.
mkdir -p "$SRC/build/x86_64-unknown-b1nix"
ln -sfn llvm "$SRC/build/x86_64-unknown-b1nix/ci-llvm"
# stage0's prebuilt rustc does not know the built-in b1nix target (only the
# freshly built stage1 rustc_target does), so its --print target-list sanity
# check would reject it. The target IS built-in from stage1 on; skip the check.
export BOOTSTRAP_SKIP_TARGET_SANITY=1
# also expose the JSON spec for any tool that resolves the triple via a path.
export RUST_TARGET_PATH="$ROOT/build/rust/targets"
# Build the COMPILER (rustc + its std sysroot) only — not the full default set
# (rustdoc, clippy, rustfmt, rust-analyzer). M68's deliverable is a native rustc;
# those extra tools are not part of the compiler and their bootstrap "tool" link
# config drops the entire C runtime layer for our unusual host target (every
# libc/builtins symbol shows undefined), a separate non-blocking issue. Override
# by passing explicit paths as "$@" when you want more (e.g. src/tools/cargo).
RUST_BUILD_TARGETS="${RUST_BUILD_TARGETS:-compiler/rustc library/std}"
echo ">> x.py build $RUST_BUILD_TARGETS (host=x86_64-unknown-b1nix), jobs=$JOBS"
# shellcheck disable=SC2086
exec python3 x.py build --stage 2 -j "$JOBS" $RUST_BUILD_TARGETS "$@"
