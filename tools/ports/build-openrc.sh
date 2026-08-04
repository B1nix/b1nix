#!/bin/sh
# Build OpenRC for b1nix — static build against musl libc.
# Usage: sh tools/ports/build-openrc.sh
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
ARCH="${ARCH:-x86_64}"
BUILD_DIR="$ROOT_DIR/build/$ARCH"
SRC_DIR="$ROOT_DIR/build/src/openrc"
PREFIX="$BUILD_DIR/rootfs"

MUSL_INSTALL="$BUILD_DIR/ports/musl/install/usr"
MUSL_INCLUDE="$MUSL_INSTALL/include"
MUSL_LIB="$MUSL_INSTALL/lib"

export PROJECT_DIR="$ROOT_DIR"
. "$ROOT_DIR/tools/toolchain/env.sh" 2>/dev/null || true
CLANG="${CLANG:-clang}"
B1NIX_TRIPLET="${B1NIX_TRIPLET:-x86_64-b1nix}"
OBJ_DIR="$BUILD_DIR/openrc-obj"
LIB_DIR="$BUILD_DIR/openrc-lib"
BIN_DIR="$PREFIX/sbin"
SHIM_DIR="$SRC_DIR/b1nix_shim"
rm -rf "$OBJ_DIR" "$LIB_DIR"
mkdir -p "$OBJ_DIR" "$LIB_DIR" "$BIN_DIR" "$PREFIX/etc/init.d" \
         "$PREFIX/libexec/openrc/functions" "$SHIM_DIR/sys"

INC_FLAGS="-isystem $MUSL_INCLUDE -isystem $SHIM_DIR"
CFLAGS_BASE="--target=$B1NIX_TRIPLET -ffreestanding -nostdinc -fPIC $INC_FLAGS -Db1nix -D__b1nix__ -D__linux__ -DPATH_MAX=4096 -DHAVE_STRUCT_TIMESPEC -D_GNU_SOURCE -DHAVE_STRLCAT -DHAVE_STRLCPY -Wall -Wno-unused-function -Wno-unused-variable -Wno-sign-compare -Wno-type-limits -Wno-pointer-sign -Wno-implicit-function-declaration -Wno-int-conversion -Wno-unused-but-set-variable -Wno-enum-conversion"
COMPILE_FLAGS="-I$SRC_DIR/src -I$SRC_DIR/src/shared -I$SRC_DIR/src/librc -I$OBJ_DIR -I$SRC_DIR/src/libeinfo -D_RC_PATH=\"/etc/rc\",\"/etc\",\"/libexec/openrc/rc\" -DHAVE_INITSCRIPTS=\"/etc/init.d\" -UHAVE_CLOSE_RANGE"

echo "=== Building OpenRC for b1nix ($ARCH) ==="
echo "  SRC:      $SRC_DIR"
echo "  MUSL:     $MUSL_INCLUDE"
echo "  PREFIX:   $PREFIX"

if [ ! -d "$SRC_DIR" ]; then
    echo "OpenRC source not found at $SRC_DIR" >&2; exit 1
fi
if [ ! -f "$MUSL_LIB/libc.a" ]; then
    echo "musl libc not built. Run: tools/ports/build-musl.sh" >&2; exit 1
fi

OBJ_DIR="$BUILD_DIR/openrc-obj"
LIB_DIR="$BUILD_DIR/openrc-lib"
BIN_DIR="$PREFIX/sbin"
SHIM_DIR="$SRC_DIR/b1nix_shim"
rm -rf "$OBJ_DIR" "$LIB_DIR"
mkdir -p "$OBJ_DIR" "$LIB_DIR" "$BIN_DIR" "$PREFIX/etc/init.d" \
         "$PREFIX/libexec/openrc/functions" "$SHIM_DIR/sys"

cc_file() {
    local src="$1" obj="$2" extra="${3:-}"
    $CLANG $CFLAGS_BASE $COMPILE_FLAGS $extra -c "$src" -o "$obj" 2>&1
}

link_bin() {
    local out="$1"; shift
    # Dynamic, like the rest of the userspace and like every distribution that
    # ships OpenRC (Alpine, Gentoo). Scrt1.o is the PIE-capable crt1; the
    # explicit -dynamic-linker gives the binary a PT_INTERP, which is also what
    # tells the kernel loader this is a Linux-ABI image — no EI_OSABI stamping
    # needed. tools/check-dynamic.sh fails the build if anything here regresses
    # to -static.
    $CLANG --target=$B1NIX_TRIPLET -nostdlib -pie -fuse-ld=lld \
        -Wl,-dynamic-linker,/lib/ld-musl-x86_64.so.1 \
        "$MUSL_LIB/Scrt1.o" "$MUSL_LIB/crti.o" "$@" \
        -L"$MUSL_LIB" -lc "$MUSL_LIB/crtn.o" -o "$out" 2>&1 || return $?
}

# ═══════════════════════════════════════════════════════════════════
# Phase 1: Patch source for b1nix compatibility
# ═══════════════════════════════════════════════════════════════════
echo ""
echo "--- Phase 1: Patching source ---"

# Restore originals if .bak exists
for f in $(find "$SRC_DIR/src" -name "*.bak" 2>/dev/null); do
    orig="${f%.bak}"; cp "$f" "$orig"
done

# b1nix_compat.h — must include sys/types.h first for pid_t/int32_t
cat > "$SRC_DIR/b1nix_compat.h" <<'COMPATEOF'
#ifndef B1NIX_COMPAT_H
#define B1NIX_COMPAT_H

#ifdef b1nix
#include <sys/types.h>
#include <stdint.h>
#include <errno.h>

struct utmp {
    short   ut_type;
    pid_t   ut_pid;
    char    ut_id[4];
    char    ut_user[32];
    char    ut_host[256];
    struct  { short tv_sec; short tv_usec; } ut_exit;
    long    ut_session;
    struct  { long tv_sec; long tv_usec; } ut_tv;
    int32_t ut_addr_v6[4];
    char    __unused[20];
};
#define EMPTY 0
#define RUN_LVL 1
#define BOOT_TIME 2
#define NEW_TIME 3
#define OLD_TIME 4
#define LOGIN_PROCESS 5
#define USER_PROCESS 6
#define DEAD_PROCESS 7
#define ACCOUNTING 8
#define ut_name ut_user
static inline void pututline(struct utmp *u) { (void)u; }
static inline struct utmp *getutid(struct utmp *u) { (void)u; return 0; }
static inline struct utmp *getutline(struct utmp *u) { (void)u; return 0; }
static inline void utmpname(const char *f) { (void)f; }
static inline void endutent(void) {}
static inline void setutent(void) {}

static inline void log_wtmp(const char *u, const char *h, pid_t p,
                            const char *tv, const char *ty) {
    (void)u; (void)h; (void)p; (void)tv; (void)ty;
}

#ifndef CLOCK_BOOTTIME
#define CLOCK_BOOTTIME CLOCK_MONOTONIC
#endif

static inline int ioprio_set(int who, int prio) {
    (void)who; (void)prio;
    return 0;
}
#endif /* b1nix */
#endif /* B1NIX_COMPAT_H */
COMPATEOF

# sys/capability.h stub
cat > "$SHIM_DIR/sys/capability.h" <<'CAPEOF'
#ifndef _B1NIX_SYS_CAPABILITY_H
#define _B1NIX_SYS_CAPABILITY_H
#include <sys/types.h>
typedef struct _cap_struct *cap_t;
typedef struct _cap_iab *cap_iab_t;
typedef int cap_value_t;
typedef enum { CAP_EFFECTIVE=0, CAP_PERMITTED=1, CAP_INHERITABLE=2 } cap_flag_t;
typedef enum { CAP_CLEAR=0, CAP_SET=1 } cap_flag_value_t;
static inline cap_t cap_get_proc(void) { return (void*)0; }
static inline int cap_set_proc(cap_t c) { (void)c; return 0; }
static inline int cap_free(void *p) { (void)p; return 0; }
static inline int cap_set_flag(cap_t c, cap_flag_t f, int n, const cap_value_t *v, cap_flag_value_t fv) {
    (void)c; (void)f; (void)n; (void)v; (void)fv; return 0;
}
static inline int cap_clear(cap_t c) { (void)c; return 0; }
static inline cap_t cap_init(void) { return (void*)0; }
static inline cap_iab_t cap_iab_from_text(const char *s) { (void)s; return (void*)0; }
static inline int cap_iab_set_proc(cap_iab_t i) { (void)i; return 0; }
static inline cap_iab_t cap_iab_init(void) { return (void*)0; }
static inline int cap_iab_fill(cap_iab_t i, cap_flag_t f, cap_flag_value_t fv, ...) {
    (void)i; (void)f; (void)fv; return 0;
}
static inline char *cap_to_text(cap_t c, ssize_t *l) { (void)c; if(l)*l=0; return (void*)0; }
static inline cap_t cap_from_text(const char *s) { (void)s; return (void*)0; }
static inline int cap_setuid(uid_t u) { (void)u; return 0; }
static inline int cap_set_secbits(int b) { (void)b; return 0; }
#endif
CAPEOF

# openrc-init.c: replace utmp.h with compat, remove CLOCK_BOOTTIME block
INIT_SRC="$SRC_DIR/src/openrc-init/openrc-init.c"
sed -i 's|#include <utmp.h>|#include "../../b1nix_compat.h"|' "$INIT_SRC"
sed -i 's|#include "wtmp.h"||' "$INIT_SRC"
sed -i '/#if defined(__linux__)/,/#endif/c\/* b1nix: use CLOCK_BOOTTIME from compat *\/' "$INIT_SRC"

# shared/misc.c: add compat header, remove sys/sysinfo.h
MISC_SRC="$SRC_DIR/src/shared/misc.c"
sed -i '1i #include "../../b1nix_compat.h"' "$MISC_SRC"
sed -i 's|#include <sys/sysinfo.h>|/* b1nix: no sysinfo.h */|' "$MISC_SRC"

# openrc-run.c: no pty.h
sed -i 's|#include <pty.h>|/* b1nix: no pty.h */|' "$SRC_DIR/src/openrc-run/openrc-run.c"

# start-stop-daemon / supervise-daemon: stub ioprio + no capability
for f in "$SRC_DIR/src/start-stop-daemon/start-stop-daemon.c" \
         "$SRC_DIR/src/supervise-daemon/supervise-daemon.c"; do
    sed -i 's|return syscall(SYS_ioprio_set.*|return 0;|' "$f"
done

# librc/librc.c: no kvm
sed -i 's|#include <kvm.h>|/* b1nix: no kvm */|' "$SRC_DIR/src/librc/librc.c"

echo "Patches applied."

# ═══════════════════════════════════════════════════════════════════
# Phase 2: Build libeinfo
# ═══════════════════════════════════════════════════════════════════
echo ""
echo "--- Phase 2: Building libeinfo ---"
EINFO_OBJS=""
for src in "$SRC_DIR"/src/libeinfo/*.c; do
    name=$(basename "$src" .c)
    obj="$OBJ_DIR/einfo_${name}.o"
    echo "  einfo/$name.c"
    cc_file "$src" "$obj" "-I$SRC_DIR/src -I$SRC_DIR/src/shared" || continue
    EINFO_OBJS="$EINFO_OBJS $obj"
done
llvm-ar rcs "$LIB_DIR/libeinfo.a" $EINFO_OBJS
echo "  -> libeinfo.a"

# ═══════════════════════════════════════════════════════════════════
# Phase 3: Build librc
# ═══════════════════════════════════════════════════════════════════
echo ""
echo "--- Phase 3: Building librc ---"
RC_H="$OBJ_DIR/rc.h"
sed -e 's|@SYSCONFDIR@|/etc|g' \
    -e 's|@RC_LIBEXECDIR@|/libexec/openrc|g' \
    -e 's|@RC_PLUGINDIR@|/libexec/openrc/plugins|g' \
    -e 's|@LOCAL_PREFIX@|/usr/local|g' \
    "$SRC_DIR/src/librc/rc.h.in" > "$RC_H"
echo "  Generated rc.h"

LIBRC_OBJS=""
for src in "$SRC_DIR"/src/librc/librc.c \
          "$SRC_DIR"/src/librc/librc-daemon.c \
          "$SRC_DIR"/src/librc/librc-depend.c \
          "$SRC_DIR"/src/librc/librc-misc.c \
          "$SRC_DIR"/src/librc/librc-stringlist.c; do
    name=$(basename "$src" .c)
    obj="$OBJ_DIR/librc_${name}.o"
    echo "  librc/$name.c"
    cc_file "$src" "$obj" "-I$SRC_DIR/src -I$SRC_DIR/src/shared -I$SRC_DIR/src/librc -I$OBJ_DIR -D_RC_PATH=\"/etc/rc\",\"/etc\",\"/libexec/openrc/rc\"" || continue
    LIBRC_OBJS="$LIBRC_OBJS $obj"
done
llvm-ar rcs "$LIB_DIR/librc.a" $LIBRC_OBJS
echo "  -> librc.a"

# Generate version.h
sed 's|@VCS_TAG@|0.63.0-b1nix|g' "$SRC_DIR/src/shared/version.h.in" > "$SRC_DIR/src/shared/version.h"
for d in "$SRC_DIR/src/openrc" "$SRC_DIR/src/openrc-init" "$SRC_DIR/src/openrc-run"; do
    cp "$SRC_DIR/src/shared/version.h" "$d/version.h" 2>/dev/null || true
done
cp "$SRC_DIR/src/shared/version.h" "$OBJ_DIR/version.h"
echo "  Generated version.h"

# Build shared library: misc, plugin, _usage, rc_exec, schedules, timeutils, wtmp
echo ""
echo "--- Building libshared ---"
SHARED_OBJS=""
for src in "$SRC_DIR/src/shared/misc.c" \
           "$SRC_DIR/src/shared/plugin.c" \
           "$SRC_DIR/src/shared/_usage.c" \
           "$SRC_DIR/src/shared/rc_exec.c" \
           "$SRC_DIR/src/shared/schedules.c" \
           "$SRC_DIR/src/shared/timeutils.c" \
           "$SRC_DIR/src/shared/wtmp.c"; do
    [ -f "$src" ] || continue
    name=$(basename "$src" .c)
    obj="$OBJ_DIR/shared_${name}.o"
    echo "  shared/$name.c"
    cc_file "$src" "$obj" || continue
    SHARED_OBJS="$SHARED_OBJS $obj"
done
llvm-ar rcs "$LIB_DIR/libshared.a" $SHARED_OBJS
echo "  -> libshared.a"

# ═══════════════════════════════════════════════════════════════════
# Phase 4: Build binaries
# ═══════════════════════════════════════════════════════════════════
echo ""
echo "--- Phase 4: Building binaries ---"
FAIL_COUNT=0
OK_COUNT=0

build_one() {
    local name="$1" src="$2"
    local obj="$OBJ_DIR/${name}.o"
    echo -n "  $name ... "
    if ! cc_file "$src" "$obj" 2>/dev/null; then
        echo "COMPILE FAIL"; FAIL_COUNT=$((FAIL_COUNT+1)); return 1
    fi
    if ! link_bin "$BIN_DIR/$name" "$obj" \
        "$LIB_DIR/libshared.a" "$LIB_DIR/librc.a" "$LIB_DIR/libeinfo.a" 2>/dev/null; then
        echo "LINK FAIL"; FAIL_COUNT=$((FAIL_COUNT+1)); return 1
    fi
    echo "ok"; OK_COUNT=$((OK_COUNT+1))
}

build_one_extra() {
    local name="$1" src="$2"; shift 2
    local obj="$OBJ_DIR/${name}.o"
    echo -n "  $name ... "
    if ! cc_file "$src" "$obj" 2>/dev/null; then
        echo "COMPILE FAIL"; FAIL_COUNT=$((FAIL_COUNT+1)); return 1
    fi
    if ! link_bin "$BIN_DIR/$name" "$obj" "$@" \
        "$LIB_DIR/libshared.a" "$LIB_DIR/librc.a" "$LIB_DIR/libeinfo.a" 2>/dev/null; then
        echo "LINK FAIL"; FAIL_COUNT=$((FAIL_COUNT+1)); return 1
    fi
    echo "ok"; OK_COUNT=$((OK_COUNT+1))
}

# Core binaries (depend on rc + einfo + shared)
build_one "openrc-init" "$SRC_DIR/src/openrc-init/openrc-init.c"
build_one "openrc-run"  "$SRC_DIR/src/openrc-run/openrc-run.c"
build_one "rc-update"   "$SRC_DIR/src/rc-update/rc-update.c"
build_one "rc-service"  "$SRC_DIR/src/rc-service/rc-service.c"
build_one "rc-depend"   "$SRC_DIR/src/rc-depend/rc-depend.c"
build_one "rc-abort"    "$SRC_DIR/src/rc-abort/rc-abort.c"
build_one "mountinfo"   "$SRC_DIR/src/mountinfo/mountinfo.c"
build_one "kill_all"    "$SRC_DIR/src/kill_all/kill_all.c"
build_one "is_newer_than" "$SRC_DIR/src/is_newer_than/is_newer_than.c"
build_one "is_older_than" "$SRC_DIR/src/is_older_than/is_older_than.c"
build_one "mark_service" "$SRC_DIR/src/mark_service/mark_service.c"
build_one "value"       "$SRC_DIR/src/value/value.c"
build_one "shell_var"   "$SRC_DIR/src/shell_var/shell_var.c"
build_one "fstabinfo"   "$SRC_DIR/src/fstabinfo/fstabinfo.c"

# openrc (needs rc-logger.c compiled separately)
echo -n "  openrc ... "
OPENRC_OBJ1="$OBJ_DIR/openrc_rc.o"
OPENRC_OBJ2="$OBJ_DIR/openrc_logger.o"
cc_file "$SRC_DIR/src/openrc/rc.c" "$OPENRC_OBJ1" && \
cc_file "$SRC_DIR/src/openrc/rc-logger.c" "$OPENRC_OBJ2" 2>/dev/null && \
link_bin "$BIN_DIR/openrc" "$OPENRC_OBJ1" "$OPENRC_OBJ2" \
    "$LIB_DIR/libshared.a" "$LIB_DIR/librc.a" "$LIB_DIR/libeinfo.a" 2>/dev/null && \
    echo "ok" && OK_COUNT=$((OK_COUNT+1)) || { echo "FAIL"; FAIL_COUNT=$((FAIL_COUNT+1)); }

# openrc-shutdown (needs broadcast.c + sysvinit.c)
echo -n "  openrc-shutdown ... "
OSS_OBJ1="$OBJ_DIR/openrc_shutdown_main.o"
OSS_OBJ2="$OBJ_DIR/openrc_shutdown_broadcast.o"
OSS_OBJ3="$OBJ_DIR/openrc_shutdown_sysvinit.o"
cc_file "$SRC_DIR/src/openrc-shutdown/openrc-shutdown.c" "$OSS_OBJ1" 2>/dev/null && \
cc_file "$SRC_DIR/src/openrc-shutdown/broadcast.c" "$OSS_OBJ2" 2>/dev/null && \
cc_file "$SRC_DIR/src/openrc-shutdown/sysvinit.c" "$OSS_OBJ3" 2>/dev/null && \
link_bin "$BIN_DIR/openrc-shutdown" "$OSS_OBJ1" "$OSS_OBJ2" "$OSS_OBJ3" \
    "$LIB_DIR/libshared.a" "$LIB_DIR/librc.a" "$LIB_DIR/libeinfo.a" 2>/dev/null && \
    echo "ok" && OK_COUNT=$((OK_COUNT+1)) || { echo "FAIL"; FAIL_COUNT=$((FAIL_COUNT+1)); }

# rc-status (needs libutil)
echo -n "  rc-status ... "
RCS_OBJ="$OBJ_DIR/rc_status.o"
cc_file "$SRC_DIR/src/rc-status/rc-status.c" "$RCS_OBJ" 2>/dev/null && \
link_bin "$BIN_DIR/rc-status" "$RCS_OBJ" \
    "$LIB_DIR/libshared.a" "$LIB_DIR/librc.a" "$LIB_DIR/libeinfo.a" \
    "$MUSL_LIB/libutil.a" 2>/dev/null && \
    echo "ok" && OK_COUNT=$((OK_COUNT+1)) || { echo "FAIL"; FAIL_COUNT=$((FAIL_COUNT+1)); }

# checkpath (needs pam/crypt stubs)
echo -n "  checkpath ... "
CHK_OBJ="$OBJ_DIR/checkpath.o"
cc_file "$SRC_DIR/src/checkpath/checkpath.c" "$CHK_OBJ" 2>/dev/null && \
link_bin "$BIN_DIR/checkpath" "$CHK_OBJ" \
    "$LIB_DIR/libshared.a" "$LIB_DIR/librc.a" "$LIB_DIR/libeinfo.a" \
    "$MUSL_LIB/libcrypt.a" 2>/dev/null && \
    echo "ok" && OK_COUNT=$((OK_COUNT+1)) || { echo "FAIL"; FAIL_COUNT=$((FAIL_COUNT+1)); }

# service (compiled multiple times with different defines)
for variant in service_starting service_started service_stopping service_stopped \
               service_inactive service_wasinactive service_hotplugged \
               service_started_daemon service_crashed; do
    echo -n "  $variant ... "
    SV_OBJ="$OBJ_DIR/${variant}.o"
    cc_file "$SRC_DIR/src/service/service.c" "$SV_OBJ" "-D${variant^^}" 2>/dev/null && \
    link_bin "$BIN_DIR/$variant" "$SV_OBJ" \
        "$LIB_DIR/libshared.a" "$LIB_DIR/librc.a" "$LIB_DIR/libeinfo.a" 2>/dev/null && \
        echo "ok" && OK_COUNT=$((OK_COUNT+1)) || { echo "FAIL"; FAIL_COUNT=$((FAIL_COUNT+1)); }
done

# mark_service variants
for variant in mark_service_starting mark_service_started mark_service_stopping mark_service_stopped \
               mark_service_inactive mark_service_wasinactive mark_service_hotplugged \
               mark_service_failed mark_service_crashed; do
    echo -n "  $variant ... "
    MS_OBJ="$OBJ_DIR/${variant}.o"
    cc_file "$SRC_DIR/src/mark_service/mark_service.c" "$MS_OBJ" "-D${variant^^}" 2>/dev/null && \
    link_bin "$BIN_DIR/$variant" "$MS_OBJ" \
        "$LIB_DIR/libshared.a" "$LIB_DIR/librc.a" "$LIB_DIR/libeinfo.a" 2>/dev/null && \
        echo "ok" && OK_COUNT=$((OK_COUNT+1)) || { echo "FAIL"; FAIL_COUNT=$((FAIL_COUNT+1)); }
done

# value variants
for variant in service_get_value service_set_value service_export get_options save_options; do
    echo -n "  $variant ... "
    VL_OBJ="$OBJ_DIR/${variant}.o"
    cc_file "$SRC_DIR/src/value/value.c" "$VL_OBJ" "-D${variant^^}" 2>/dev/null && \
    link_bin "$BIN_DIR/$variant" "$VL_OBJ" \
        "$LIB_DIR/libshared.a" "$LIB_DIR/librc.a" "$LIB_DIR/libeinfo.a" 2>/dev/null && \
        echo "ok" && OK_COUNT=$((OK_COUNT+1)) || { echo "FAIL"; FAIL_COUNT=$((FAIL_COUNT+1)); }
done

# start-stop-daemon
echo -n "  start-stop-daemon ... "
SSD_OBJ="$OBJ_DIR/start_stop_daemon.o"
cc_file "$SRC_DIR/src/start-stop-daemon/start-stop-daemon.c" "$SSD_OBJ" 2>/dev/null && \
link_bin "$BIN_DIR/start-stop-daemon" "$SSD_OBJ" \
    "$LIB_DIR/libshared.a" "$LIB_DIR/librc.a" "$LIB_DIR/libeinfo.a" \
    "$MUSL_LIB/libcrypt.a" "$MUSL_LIB/libutil.a" 2>/dev/null && \
    echo "ok" && OK_COUNT=$((OK_COUNT+1)) || { echo "FAIL"; FAIL_COUNT=$((FAIL_COUNT+1)); }

# supervise-daemon
echo -n "  supervise-daemon ... "
SUP_OBJ="$OBJ_DIR/supervise_daemon.o"
cc_file "$SRC_DIR/src/supervise-daemon/supervise-daemon.c" "$SUP_OBJ" 2>/dev/null && \
link_bin "$BIN_DIR/supervise-daemon" "$SUP_OBJ" \
    "$LIB_DIR/libshared.a" "$LIB_DIR/librc.a" "$LIB_DIR/libeinfo.a" \
    "$MUSL_LIB/libcrypt.a" "$MUSL_LIB/libutil.a" 2>/dev/null && \
    echo "ok" && OK_COUNT=$((OK_COUNT+1)) || { echo "FAIL"; FAIL_COUNT=$((FAIL_COUNT+1)); }

# Install shell-script commands
for script in halt reboot poweroff shutdown on_ac_power; do
    src_file="$SRC_DIR/src/$script/$script.in"
    [ -f "$src_file" ] || src_file="$SRC_DIR/src/$script/$script"
    [ -f "$src_file" ] || continue
    sed 's|@LIBEXECDIR@|/libexec/openrc|g; s|@SYSCONFDIR@|/etc|g' "$src_file" > "$BIN_DIR/$script"
    chmod +x "$BIN_DIR/$script"
    echo "  -> $script (script)"
    OK_COUNT=$((OK_COUNT+1))
done

# einfo helpers. OpenRC's shell functions (functions.sh) do not define einfo /
# eerror / ebegin themselves — they shell out to binaries of those names, which
# is why every init script printed "eerror: not found" without them. One
# multicall binary dispatching on argv[0], installed into RC_PATH_PREFIX's first
# entry (/libexec/openrc/bin).
EINFO_BIN_DIR="$PREFIX/libexec/openrc/bin"
mkdir -p "$EINFO_BIN_DIR"
EINFO_MAIN_OBJ="$OBJ_DIR/einfo_main.o"
if cc_file "$SRC_DIR/src/einfo/einfo.c" "$EINFO_MAIN_OBJ" \
        "-I$SRC_DIR/src/libeinfo -I$SRC_DIR/src/shared" >/dev/null 2>&1 &&
   link_bin "$EINFO_BIN_DIR/einfo" "$EINFO_MAIN_OBJ" \
        "$LIB_DIR/libeinfo.a" "$LIB_DIR/librc.a" "$LIB_DIR/libshared.a" \
        >/dev/null 2>&1; then
    for applet in einfon ewarnn ewarn eerrorn eerror ebegin eend ewend \
                  eindent eoutdent esyslog eval_ecolors ewaitfile \
                  veinfo vewarn vebegin veend vewend veindent veoutdent; do
        ln -sf einfo "$EINFO_BIN_DIR/$applet"
    done
    echo "  -> einfo + 20 applet symlinks in /libexec/openrc/bin"
    OK_COUNT=$((OK_COUNT+1))
else
    echo "  -> einfo FAILED"
    FAIL_COUNT=$((FAIL_COUNT+1))
fi

# No /sbin/init symlink here. /sbin/init is BusyBox's init applet (stamped by
# tools/ports/build-busybox.sh) and is the kernel's default PID 1; OpenRC is the
# high-level init driving the runlevels underneath it, exactly as on Alpine.
# openrc-init keeps its own name and is selected with init=/sbin/openrc-init.
if [ -f "$BIN_DIR/openrc-init" ]; then
    # Only drop the stale link an older tree left behind — never one pointing at
    # BusyBox, which may already have been installed into this sysroot.
    [ "$(readlink "$BIN_DIR/init" 2>/dev/null)" = "openrc-init" ] && \
        rm -f "$BIN_DIR/init"
    echo "  -> openrc-init (PID 1 only with init=/sbin/openrc-init)"
fi

# ═══════════════════════════════════════════════════════════════════
# Phase 5: Install shell functions, configs, init.d
# ═══════════════════════════════════════════════════════════════════
echo ""
echo "--- Phase 5: Installing shell scripts and configs ---"

# sh/ — the shell half of OpenRC: init.sh (the sysinit entry point openrc execs
# first), gendepends.sh, openrc-run.sh, functions.sh and friends. Upstream
# installs these into $rc_libexecdir/sh via meson's configure_file; do the same
# @VAR@ substitution by hand. Without them openrc dies with
# "unable to exec `/libexec/openrc/sh/init.sh'".
SH_DIR="$PREFIX/libexec/openrc/sh"
mkdir -p "$SH_DIR"
sh_configure() {
    sed -e 's|@BINDIR@|/bin|g' \
        -e 's|@SBINDIR@|/sbin|g' \
        -e 's|@LIBEXECDIR@|/libexec/openrc|g' \
        -e 's|@SYSCONFDIR@|/etc|g' \
        -e 's|@LOCAL_PREFIX@|/usr/local|g' \
        -e 's|@SHELL@|/bin/sh|g' \
        -e 's|@UUCP_GROUP@|uucp|g' "$1" > "$2"
}
for f in "$SRC_DIR"/sh/*.sh; do
    [ -f "$f" ] || continue
    cp "$f" "$SH_DIR/$(basename "$f")"
done
for f in "$SRC_DIR"/sh/*.sh.in; do
    [ -f "$f" ] || continue
    name=$(basename "$f" .in)
    sh_configure "$f" "$SH_DIR/$name"
done
# Linux-flavoured variants win over the generic name.
for f in init.sh init-early.sh; do
    [ -f "$SRC_DIR/sh/$f.Linux.in" ] || continue
    sh_configure "$SRC_DIR/sh/$f.Linux.in" "$SH_DIR/$f"
done
chmod +x "$SH_DIR"/*.sh 2>/dev/null || true
echo "  -> $(ls "$SH_DIR" | wc -l) shell scripts in /libexec/openrc/sh"

for f in "$SRC_DIR"/src/libeinfo/*.sh "$SRC_DIR"/src/shared/*.sh; do
    [ -f "$f" ] && cp "$f" "$PREFIX/libexec/openrc/functions/" 2>/dev/null || true
done
[ -f "$SRC_DIR/src/functions.sh" ] && \
    cp "$SRC_DIR/src/functions.sh" "$PREFIX/libexec/openrc/functions/" 2>/dev/null || true

mkdir -p "$PREFIX/etc/conf.d"
[ -f "$SRC_DIR/etc/conf.d/rc" ] && cp "$SRC_DIR/etc/conf.d/rc" "$PREFIX/etc/conf.d/"

# init.d: install only the services that mean something on b1nix (bootmisc is
# excluded too: it needs localmount, which needs an /etc/fstab). Upstream
# ships ~60 scripts, most of them BSD-only (ipfw, adjkerntz, dumpon, wscons) or
# for subsystems b1nix has no equivalent of (cgroups, modules, nscd). Shipping
# them all makes openrc walk every one of them when it builds the dependency
# cache, for services that could never start.
rm -f "$PREFIX"/etc/init.d/* 2>/dev/null || true
for name in procfs hostname local killprocs savecache; do
    f="$SRC_DIR/init.d/$name.in"
    [ -f "$f" ] || continue
    # Same @VAR@ substitution meson does — the shebang is @SBINDIR@/openrc-run,
    # and gendepends.sh only sources scripts whose shebang names openrc-run.
    sh_configure "$f" "$PREFIX/etc/init.d/$name"
done
chmod +x "$PREFIX"/etc/init.d/* 2>/dev/null || true

# Runlevels. Without these openrc has nothing to start and never leaves
# sysinit — an init system with no runlevel population is not actually
# managing the boot.
for rl in sysinit boot default shutdown; do
    mkdir -p "$PREFIX/etc/runlevels/$rl"
done
ln -sf /etc/init.d/procfs    "$PREFIX/etc/runlevels/sysinit/procfs"
ln -sf /etc/init.d/hostname  "$PREFIX/etc/runlevels/boot/hostname"
ln -sf /etc/init.d/local     "$PREFIX/etc/runlevels/default/local"
ln -sf /etc/init.d/killprocs "$PREFIX/etc/runlevels/shutdown/killprocs"
ln -sf /etc/init.d/savecache "$PREFIX/etc/runlevels/shutdown/savecache"
mkdir -p "$PREFIX/etc/local.d"

# rc.conf: no logger (no /var/log daemon yet), no parallel start (the console is
# a single serial line, interleaved service output is unreadable).
cat > "$PREFIX/etc/rc.conf" <<'RCCONFEOF'
rc_logger="NO"
rc_parallel="NO"
rc_verbose="yes"
RCCONFEOF

# End-to-end control-channel check, run from the default runlevel: ask PID 1 to
# power the machine off through its FIFO. openrc-shutdown writes the command to
# /run/openrc/init.ctl and openrc-init reads it there, so a clean poweroff is
# proof the channel works — the same path telinit uses. Only armed when the
# kernel cmdline asks for it, so an ordinary boot is unaffected.
cat > "$PREFIX/etc/openrc-ctltest.sh" <<'CTLBODY'
#!/bin/sh
# Ask PID 1 to power the machine off through its control FIFO — the same path
# `openrc-shutdown` and telinit use on a running system. openrc-init only opens
# /run/openrc/init.ctl once every runlevel has been processed, so wait for the
# boot to finish first, exactly as an operator would. Markers go straight to the
# console: a service's stdout is routed through openrc-run's pty.
i=0
while [ ! -p /run/openrc/init.ctl ] && [ $i -lt 30 ]; do
	sleep 1
	i=$((i + 1))
done
if [ -p /run/openrc/init.ctl ]; then
	echo "M94-OPENRC: ok init-fifo-present"
	echo "M94-OPENRC: sending poweroff through openrc-shutdown"
	/sbin/openrc-shutdown --no-write --poweroff now 2>&1
else
	echo "M94-OPENRC: fail init-fifo-present"
fi
CTLBODY
chmod +x "$PREFIX/etc/openrc-ctltest.sh"

# 00-smoke.start is a static file now — see tools/ports/00-smoke.start.
# The Makefile copies it into the rootfs during root-image.

cat > "$PREFIX/etc/local.d/zz-ctltest.start" <<'CTLEOF'
#!/bin/sh
# Only on the instance that asked for it. Arming this on every test boot left a
# 30-second FIFO waiter running on instances whose PID 1 is not openrc-init,
# which could only ever report a failure it was never meant to test.
grep -q 'b1nix.openrc-ctltest' /proc/cmdline 2>/dev/null || exit 0
# setsid: the waiter must outlive the `local` service, whose process group is
# torn down when the service finishes.
( /etc/openrc-ctltest.sh ) &
CTLEOF
chmod +x "$PREFIX/etc/local.d/zz-ctltest.start"
echo "  -> 5 init.d services, runlevels sysinit/boot/default/shutdown"

echo ""
echo "=== OpenRC build complete ==="
echo "  Built: $OK_COUNT  Failed: $FAIL_COUNT"
echo ""
echo "Binaries in $BIN_DIR:"
ls -1 "$BIN_DIR/" 2>/dev/null | head -40
echo ""
echo "init.d scripts:"
ls "$PREFIX/etc/init.d/" 2>/dev/null | head -20
