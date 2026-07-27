#!/bin/sh
# Build runit (http://smarden.org/runit/) for b1nix.
#
# runit is a three-stage init plus a process supervisor: `runit-init` is PID 1,
# it execs `runit`, which runs /etc/runit/1 (one-time setup), /etc/runit/2 (the
# supervisor, normally `runsvdir`) and /etc/runit/3 (shutdown). Porting it gives
# b1nix a second real init system next to OpenRC, which is the point: "any init
# system" has to mean more than one.
#
# Upstream builds through its own hand-rolled package/compile scripts, which
# probe the host with try*.c programs and then link -static. Neither is usable
# here — the probes would run host binaries, and a static ET_EXEC is rejected by
# tools/check-dynamic.sh. So this script does what the other b1nix ports do:
# pick the generated config headers by hand (we know the target), compile the
# curated source list with clang against the musl sysroot, and link musl PIE.
#
# Usage: sh tools/ports/build-runit.sh
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
ARCH="${ARCH:-x86_64}"
BUILD_DIR="$ROOT_DIR/build/$ARCH"
PREFIX="$BUILD_DIR/rootfs"

VER="${RUNIT_VERSION:-2.1.2}"
TARBALL="runit-${VER}.tar.gz"
URL="http://smarden.org/runit/${TARBALL}"
DIST_DIR="$ROOT_DIR/build/dist"
SRC_PARENT="$ROOT_DIR/build/src/runit"
SRC_DIR="$SRC_PARENT/admin/runit-${VER}/src"

PORT_DIR="$BUILD_DIR/ports/runit"
OBJ_DIR="$PORT_DIR/obj"
GEN_DIR="$PORT_DIR/gen"
LIB_DIR="$PORT_DIR/lib"
BIN_DIR="$PREFIX/sbin"

MUSL_INSTALL="$BUILD_DIR/ports/musl/install/usr"
MUSL_INCLUDE="$MUSL_INSTALL/include"
MUSL_LIB="$MUSL_INSTALL/lib"

export PROJECT_DIR="$ROOT_DIR"
. "$ROOT_DIR/tools/toolchain/env.sh" 2>/dev/null || true
CLANG="${CLANG:-clang}"
B1NIX_TRIPLET="${B1NIX_TRIPLET:-x86_64-b1nix}"
AR="${AR:-llvm-ar}"

echo "=== Building runit $VER for b1nix ($ARCH) ==="

if [ ! -f "$MUSL_LIB/libc.so" ]; then
    echo "musl libc not built. Run: tools/ports/build-musl.sh" >&2; exit 1
fi

# ───────────────────────────────────────────────────────────────────
# Phase 1: fetch + unpack
# ───────────────────────────────────────────────────────────────────
mkdir -p "$DIST_DIR" "$SRC_PARENT"
if [ ! -f "$DIST_DIR/$TARBALL" ]; then
    echo "--- Fetching $URL"
    curl -sSfL -o "$DIST_DIR/$TARBALL" "$URL"
fi
if [ ! -d "$SRC_DIR" ]; then
    echo "--- Unpacking into $SRC_PARENT"
    tar -xzf "$DIST_DIR/$TARBALL" -C "$SRC_PARENT"
fi
[ -f "$SRC_DIR/runit.c" ] || { echo "runit source not found at $SRC_DIR" >&2; exit 1; }

rm -rf "$OBJ_DIR" "$GEN_DIR" "$LIB_DIR"
mkdir -p "$OBJ_DIR" "$GEN_DIR" "$LIB_DIR" "$BIN_DIR"

# ───────────────────────────────────────────────────────────────────
# Phase 2: the config headers upstream's ./configure-by-compiling picks
#
# Every one of these is a straight copy of the .h1/.h2 variant that matches the
# target. We know the target (x86_64, musl, Linux ABI), so probing it by running
# test programs — which under a cross build would run *host* programs and answer
# for the wrong system — buys nothing.
# ───────────────────────────────────────────────────────────────────
echo ""
echo "--- Phase 2: generating sysdeps headers ---"
pick() {  # $1 = header name, $2 = variant suffix (1|2), $3 = why
    cp "$SRC_DIR/$1$2" "$GEN_DIR/$1"
    printf '  %-18s <- %s%s  (%s)\n' "$1" "$1" "$2" "$3"
}
pick direntry.h      2 "musl has <dirent.h>/struct dirent"
pick hasflock.h      2 "kernel implements SYS_FLOCK (240)"
pick hasmkffo.h      2 "mkfifo(3) present; b1nix has real FIFOs"
pick hassgact.h      2 "sigaction(2) present"
pick hassgprm.h      2 "sigprocmask(2) present"
pick hasshsgr.h      1 "musl setgroups takes gid_t*, not short"
pick haswaitp.h      2 "waitpid(2) present"
pick iopause.h       2 "poll(2) present — same choice as on Linux"
pick uint64.h        2 "LP64: unsigned long is 64-bit"
pick select.h        2 "<sys/select.h> present"
pick reboot_system.h 2 "musl declares reboot(int), not glibc's reboot(int, arg)"
pick uw_tmp.h        1 "musl exposes <utmp.h>; there is no struct futmpx"

# ───────────────────────────────────────────────────────────────────
# Phase 3: compile
# ───────────────────────────────────────────────────────────────────
CLANG_RES="$($CLANG -print-resource-dir 2>/dev/null || true)"
INC_FLAGS="-I$GEN_DIR -I$SRC_DIR -nostdinc -isystem $MUSL_INCLUDE"
[ -n "$CLANG_RES" ] && INC_FLAGS="$INC_FLAGS -isystem $CLANG_RES/include"

# runit is 1990s-vintage C: empty parameter lists, K&R-isms, assignments in
# conditionals. clang 22 makes several of those hard errors by default, so they
# are turned back into what upstream expects. -D_GNU_SOURCE for TIOCSCTTY and
# the RB_* reboot constants.
CFLAGS="--target=$B1NIX_TRIPLET -ffreestanding -fno-builtin -fno-stack-protector \
-O2 -fPIC -Db1nix -D__b1nix__ -D__linux__ -D_GNU_SOURCE $INC_FLAGS \
-Wno-implicit-function-declaration -Wno-int-conversion \
-Wno-deprecated-non-prototype -Wno-strict-prototypes \
-Wno-unused-but-set-variable -Wno-unused-variable -Wno-parentheses"

cc_file() {
    $CLANG $CFLAGS -c "$1" -o "$2"
}

# Everything that is not a program entry point nor a configure probe goes into
# one support archive. Upstream splits it into unix.a/byte.a/time.a purely to
# keep its own hand-written Makefile short; the link set is the same.
MAINS="runit runit-init runsv runsvdir runsvchdir runsvstat runsvctrl sv \
svlogd chpst utmpset svwaitup svwaitdown"

is_main() {
    for m in $MAINS; do [ "$1" = "$m" ] && return 0; done
    return 1
}

echo ""
echo "--- Phase 3: compiling support objects ---"
SUPPORT_OBJS=""
SUPPORT_FAIL=0
for src in "$SRC_DIR"/*.c; do
    name=$(basename "$src" .c)
    case "$name" in
        try*|chkshsgr|x86cpuid) continue ;;   # configure probes / host tools
    esac
    is_main "$name" && continue
    obj="$OBJ_DIR/$name.o"
    if cc_file "$src" "$obj"; then
        SUPPORT_OBJS="$SUPPORT_OBJS $obj"
    else
        echo "  COMPILE FAIL: $name.c"
        SUPPORT_FAIL=$((SUPPORT_FAIL + 1))
    fi
done
echo "  $(echo $SUPPORT_OBJS | wc -w) objects, $SUPPORT_FAIL failed"
[ "$SUPPORT_FAIL" -eq 0 ] || { echo "support objects failed to build" >&2; exit 1; }
$AR rcs "$LIB_DIR/librunit.a" $SUPPORT_OBJS
echo "  -> librunit.a"

# ───────────────────────────────────────────────────────────────────
# Phase 4: link the programs — musl PIE, like every other b1nix binary
# ───────────────────────────────────────────────────────────────────
link_bin() {
    out="$1"; shift
    # Scrt1.o is the PIE-capable crt1; the explicit -dynamic-linker gives the
    # binary a PT_INTERP, which is also what tells the b1nix loader this is a
    # Linux-ABI image. Upstream links runit and runit-init -static; that is
    # rejected by tools/check-dynamic.sh and would leave PID 1 without a
    # PT_INTERP for the loader to read.
    $CLANG --target="$B1NIX_TRIPLET" -nostdlib -pie -fuse-ld=lld \
        -Wl,-dynamic-linker,/lib/ld-musl-x86_64.so.1 \
        "$MUSL_LIB/Scrt1.o" "$MUSL_LIB/crti.o" "$@" \
        -L"$MUSL_LIB" -lc "$MUSL_LIB/crtn.o" -o "$out"
}

echo ""
echo "--- Phase 4: linking programs ---"
OK_COUNT=0
FAIL_COUNT=0
for name in $MAINS; do
    printf '  %-12s ... ' "$name"
    obj="$OBJ_DIR/$name.o"
    if ! cc_file "$SRC_DIR/$name.c" "$obj" >/dev/null 2>&1; then
        # Re-run visibly so the failure is diagnosable, not just counted.
        cc_file "$SRC_DIR/$name.c" "$obj" 2>&1 | sed 's/^/      /' || true
        echo "COMPILE FAIL"; FAIL_COUNT=$((FAIL_COUNT + 1)); continue
    fi
    if ! link_bin "$BIN_DIR/$name" "$obj" "$LIB_DIR/librunit.a" >/dev/null 2>&1; then
        link_bin "$BIN_DIR/$name" "$obj" "$LIB_DIR/librunit.a" 2>&1 | sed 's/^/      /' || true
        echo "LINK FAIL"; FAIL_COUNT=$((FAIL_COUNT + 1)); continue
    fi
    echo "ok"; OK_COUNT=$((OK_COUNT + 1))
done

# ───────────────────────────────────────────────────────────────────
# Phase 5: runtime layout
#
# runit's stage scripts are deliberately distribution-specific — upstream ships
# etc/debian/1, etc/freebsd/1 and so on, none of which apply here. These are the
# b1nix equivalents, kept to what the system actually has.
# ───────────────────────────────────────────────────────────────────
echo ""
echo "--- Phase 5: installing runtime layout ---"
mkdir -p "$PREFIX/etc/runit" "$PREFIX/etc/sv" "$PREFIX/service" "$PREFIX/var/log"

# Stage 1: one-time system setup. Mounts the kernel filesystems, then hands back
# to runit. Exit 100 here would make runit skip stage 2 entirely.
cat > "$PREFIX/etc/runit/1" <<'STAGE1'
#!/bin/sh
# runit stage 1 — one-time system initialisation.
PATH=/sbin:/bin:/usr/sbin:/usr/bin:/opt/busybox/bin
export PATH

echo "M95-RUNIT: stage1 start"

mount -t proc proc /proc 2>/dev/null
mount -t sysfs sysfs /sys 2>/dev/null

# runsvdir needs the service directory to exist before stage 2 starts; a missing
# one makes it log "unable to open" once a second forever.
mkdir -p /service

echo "M95-RUNIT: ok stage1"
exit 0
STAGE1

# Stage 2: the supervisor. This is the process runit keeps alive for the whole
# uptime of the system; when it exits, runit moves to stage 3.
cat > "$PREFIX/etc/runit/2" <<'STAGE2'
#!/bin/sh
# runit stage 2 — process supervision.
PATH=/sbin:/bin:/usr/sbin:/usr/bin:/opt/busybox/bin
export PATH

echo "M95-RUNIT: stage2 start"
exec runsvdir -P /service
STAGE2

# Stage 3: shutdown. Runs after stage 2 exits; runit halts or reboots the
# machine when this returns, driven by /etc/runit/reboot.
cat > "$PREFIX/etc/runit/3" <<'STAGE3'
#!/bin/sh
# runit stage 3 — shutdown tasks.
PATH=/sbin:/bin:/usr/sbin:/usr/bin:/opt/busybox/bin
export PATH

echo "M95-RUNIT: stage3 start"
sync
echo "M95-RUNIT: ok stage3"
exit 0
STAGE3

chmod +x "$PREFIX/etc/runit/1" "$PREFIX/etc/runit/2" "$PREFIX/etc/runit/3"
echo "  -> /etc/runit/{1,2,3}"

# A supervised service, so stage 2 is proving something rather than idling: runsv
# starts it, it announces itself, and `sv` can be pointed at it.
mkdir -p "$PREFIX/etc/sv/heartbeat"
cat > "$PREFIX/etc/sv/heartbeat/run" <<'HEARTBEAT'
#!/bin/sh
# Minimal supervised service: proves runsvdir started a runsv, which started us.
exec 2>&1
echo "M95-RUNIT: ok supervised-service"
while :; do
	sleep 30
done
HEARTBEAT
chmod +x "$PREFIX/etc/sv/heartbeat/run"
# /service/<name> is a real directory, not a symlink into /etc/sv: runsv creates
# supervise/ and its control FIFOs inside it, and the whole tree has to survive
# the copy into root.ext4 unambiguously.
rm -rf "$PREFIX/service/heartbeat"
mkdir -p "$PREFIX/service/heartbeat"
cp "$PREFIX/etc/sv/heartbeat/run" "$PREFIX/service/heartbeat/run"
chmod +x "$PREFIX/service/heartbeat/run"
echo "  -> /etc/sv/heartbeat, staged into /service"

# runit-init writes these to ask PID 1 to halt or reboot; ship them
# non-executable: runit tests them with st_mode & S_IXUSR, so a non-executable
# file is the "no request pending" state. Upstream uses mode 0 for that; 0644
# reads the same to runit and, unlike 0, lets mke2fs copy the file into
# root.ext4 when the image is populated.
for f in stopit reboot; do
    [ -e "$PREFIX/etc/runit/$f" ] && chmod 0644 "$PREFIX/etc/runit/$f"
    : > "$PREFIX/etc/runit/$f"
    chmod 0644 "$PREFIX/etc/runit/$f"
done

echo ""
echo "=== runit build complete ==="
echo "  Built: $OK_COUNT  Failed: $FAIL_COUNT"
echo ""
echo "Programs in $BIN_DIR:"
for name in $MAINS; do
    [ -f "$BIN_DIR/$name" ] && ls -l "$BIN_DIR/$name" | sed 's/^/  /'
done
[ "$FAIL_COUNT" -eq 0 ] || exit 1
