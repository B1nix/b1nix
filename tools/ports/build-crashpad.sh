#!/bin/sh
# Build upstream Crashpad (client + handler) for b1nix.
#
# Crashpad is Chromium's crash reporter: a client library that a program links
# in, and a separate `crashpad_handler` process that attaches to a crashing
# process and writes a minidump. It is built with GN + ninja, so this script
#   1. fetches crashpad + mini_chromium (and gn, which it builds from source —
#      no depot_tools, no prebuilt binaries),
#   2. stages a toolchain directory whose bin/clang, bin/clang++ and bin/llvm-ar
#      are the ordinary b1nix wrappers, which is all mini_chromium's GN
#      toolchain asks for on Linux,
#   3. builds the handler and the client library against musl + libc++,
#   4. installs /bin/crashpad_handler and the client headers/libs into the
#      rootfs.
#
# Crashpad's Linux backend runs on the kernel surface M80 provides:
# ptrace (ATTACH/SEIZE, GETREGSET, GETSIGINFO), /proc/<pid>/{task,auxv,mem,maps,
# status}, process_vm_readv, SO_PEERCRED and prctl(PR_SET_PTRACER).
set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ARCH="${B1NIX_ARCH:-x86_64}"
SRC_ROOT="$ROOT/build/src/crashpad-src"
CRASHPAD_SRC="$SRC_ROOT/crashpad"
GN_SRC="$SRC_ROOT/gn"
GN_BIN="$GN_SRC/out/gn"
OUT="$ROOT/build/$ARCH/ports/crashpad"
SHIM="$OUT/toolchain"
BUILD="$OUT/out"
ROOTFS="$ROOT/build/$ARCH/rootfs"

CRASHPAD_URL="https://chromium.googlesource.com/crashpad/crashpad"
MINI_CHROMIUM_URL="https://chromium.googlesource.com/chromium/mini_chromium"
# linux-syscall-support: Crashpad's Linux backend issues raw syscalls through
# LSS so it never depends on libc internals while a process is crashing.
LSS_URL="https://chromium.googlesource.com/linux-syscall-support"
GN_URL="https://gn.googlesource.com/gn"

log() { printf 'crashpad: %s\n' "$*"; }

# ── 1. sources ───────────────────────────────────────────────────────────────
mkdir -p "$SRC_ROOT"
if [ ! -d "$CRASHPAD_SRC/.git" ]; then
    log "cloning crashpad"
    git clone --depth 1 "$CRASHPAD_URL" "$CRASHPAD_SRC"
fi
if [ ! -d "$CRASHPAD_SRC/third_party/mini_chromium/mini_chromium/.git" ]; then
    log "cloning mini_chromium"
    git clone --depth 1 "$MINI_CHROMIUM_URL" \
        "$CRASHPAD_SRC/third_party/mini_chromium/mini_chromium"
fi

if [ ! -d "$CRASHPAD_SRC/third_party/lss/lss/.git" ]; then
    log "cloning linux-syscall-support"
    git clone --depth 1 "$LSS_URL" "$CRASHPAD_SRC/third_party/lss/lss"
fi

# ── 2. gn (built from source; the tree ships no binaries) ────────────────────
if [ ! -x "$GN_BIN" ]; then
    if [ ! -d "$GN_SRC/.git" ]; then
        log "cloning gn"
        git clone --depth 1 "$GN_URL" "$GN_SRC"
    fi
    log "building gn"
    ( cd "$GN_SRC"
      python3 build/gen.py --no-last-commit-position >/dev/null
      # gen.py --no-last-commit-position expects the header to be provided.
      printf '#define LAST_COMMIT_POSITION "b1nix-local"\n#define LAST_COMMIT_POSITION_NUM 0\n' \
          > out/last_commit_position.h
      ninja -C out gn >/dev/null )
fi

# ── 3. toolchain shim ────────────────────────────────────────────────────────
# mini_chromium's Linux toolchain runs "$clang_path/bin/{clang,clang++,llvm-ar}".
# Point each at the wrapper that already knows the b1nix target, musl sysroot
# and libc++ staging — no duplicated flag lists to drift out of sync.
mkdir -p "$SHIM/bin"
cat > "$SHIM/bin/clang" <<EOF
#!/bin/sh
exec "$ROOT/tools/b1nix-musl-cc" "\$@"
EOF
cat > "$SHIM/bin/clang++" <<EOF
#!/bin/sh
exec "$ROOT/tools/b1nix-clang++" "\$@"
EOF
cat > "$SHIM/bin/llvm-ar" <<'EOF'
#!/bin/sh
exec llvm-ar "$@"
EOF
chmod +x "$SHIM/bin/clang" "$SHIM/bin/clang++" "$SHIM/bin/llvm-ar"

# ── 4. configure + build ─────────────────────────────────────────────────────
ZLIB_INC="$ROOT/build/$ARCH/pkg/zlib/include"
ZLIB_LIB="$ROOT/build/$ARCH/pkg/zlib/lib"
CURL_INC="$ROOT/build/$ARCH/ports/curl/install/include"
CURL_LIB="$ROOT/build/$ARCH/ports/curl/install/lib"
STUB_LIB="$ROOT/build/$ARCH/toolchain/sysroot/usr/lib"
if [ ! -f "$ZLIB_INC/zlib.h" ]; then
    echo "crashpad: zlib port missing ($ZLIB_INC) — build it first" >&2
    exit 1
fi

mkdir -p "$BUILD"
cat > "$BUILD/args.gn" <<EOF
target_os = "linux"
target_cpu = "x64"
is_debug = false
clang_path = "$SHIM"
crashpad_dependencies = "standalone"
# Ordinary build flags only — no patched sources, and no private header shims:
# the platform defines (__linux__, _GNU_SOURCE, ...) come from the b1nix
# toolchain wrapper and the system headers from the sysroot, exactly as they
# would for a package built in-guest.
# -Wno-error=sign-compare: musl's own CMSG_NXTHDR compares a size_t against a
# ptrdiff_t, so any -Werror build that walks control messages trips on a libc
# macro rather than on its own code. A build flag, not a patched source.
extra_cflags = "-Wno-error=sign-compare -I$ZLIB_INC -I$CURL_INC"
# musl folds libdl/libpthread/librt into libc; the toolchain's empty stub
# archives are what satisfies a -ldl/-lpthread the upstream build emits.
# -pie: the rootfs gate rejects ET_EXEC (tools/check-dynamic.sh).
extra_ldflags = "-pie -L$ZLIB_LIB -L$CURL_LIB -L$STUB_LIB"
EOF

log "gn gen"
# args.gn is read from the build directory; passing --args as one flattened
# line would let the comments above swallow the rest of it.
( cd "$CRASHPAD_SRC" && "$GN_BIN" gen "$BUILD" >/dev/null )

log "ninja"
ninja -C "$BUILD" crashpad_handler client

# ── 5. install ───────────────────────────────────────────────────────────────
mkdir -p "$ROOTFS/bin" "$ROOTFS/lib" "$ROOTFS/include/crashpad"
install -m 0755 "$BUILD/crashpad_handler" "$ROOTFS/bin/crashpad_handler"
for a in "$BUILD/obj/client/libclient.a" "$BUILD/obj/util/libutil.a" \
         "$BUILD/obj/third_party/mini_chromium/mini_chromium/base/libbase.a"; do
    [ -f "$a" ] && install -m 0644 "$a" "$ROOTFS/lib/"
done
# ── 6. end-to-end smoke client ───────────────────────────────────────────────
# A real Crashpad client: it starts the handler through the upstream API, then
# crashes. Built here (not in userspace/Makefile) because it needs Crashpad's
# own include paths and static libraries.
SMOKE_SRC="$ROOT/userspace/bin/smoke/crashpad_smoke.cpp"
if [ -f "$SMOKE_SRC" ]; then
    log "building crashpad_smoke"
    MC="$CRASHPAD_SRC/third_party/mini_chromium/mini_chromium"
    "$ROOT/tools/b1nix-clang++" \
        -std=c++20 -fPIC \
        -I"$CRASHPAD_SRC" -I"$MC" -I"$BUILD/gen" \
        -c "$SMOKE_SRC" -o "$BUILD/crashpad_smoke.o"
    "$ROOT/tools/b1nix-clang++" -pie \
        "$BUILD/crashpad_smoke.o" \
        "$BUILD/obj/client/libclient.a" \
        "$BUILD/obj/client/libcommon.a" \
        "$BUILD/obj/util/libutil.a" \
        "$BUILD/obj/util/libnet.a" \
        "$BUILD/obj/compat/libcompat.a" \
        "$BUILD/obj/third_party/mini_chromium/mini_chromium/base/libbase.a" \
        -L"$ZLIB_LIB" -L"$CURL_LIB" -L"$STUB_LIB" -lz -ldl -lpthread \
        -o "$BUILD/crashpad_smoke"
    install -m 0755 "$BUILD/crashpad_smoke" "$ROOTFS/bin/crashpad_smoke"
    log "installed /bin/crashpad_smoke"
fi

log "installed /bin/crashpad_handler"
