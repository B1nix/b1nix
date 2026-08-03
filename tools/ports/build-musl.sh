#!/bin/sh
# Build musl as a static + shared libc for the b1nix userspace ABI.
#
# Produces:
#   - build/<arch>/ports/musl/usr/lib/libc.a   (static musl)
#   - build/<arch>/ports/musl/usr/lib/libc.so   (shared musl / ld.so)
#   - build/<arch>/ports/musl/usr/include/      (musl headers)
#
# Usage:
#   tools/ports/build-musl.sh              # build musl
#   tools/ports/build-musl.sh --clean      # clean build dir
#
# The install prefix (last stdout line) can be used by downstream scripts.
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
. "$ROOT_DIR/tools/toolchain/env.sh"
. "$ROOT_DIR/tools/ports/drivers/common.sh"

MUSL_VERSION="${MUSL_VERSION:-1.2.5}"
MUSL_URL="https://musl.libc.org/releases/musl-${MUSL_VERSION}.tar.gz"
MUSL_TARBALL="musl-${MUSL_VERSION}.tar.gz"
MUSL_SRCNAME="musl-${MUSL_VERSION}"

SRC_PARENT="$ROOT_DIR/build/src/musl"
SRC_DIR="$SRC_PARENT/$B1NIX_TRIPLET/$MUSL_SRCNAME"
# Primary install: flat layout matching all other ports (no usr/ subdir)
BUILD_DIR="$ROOT_DIR/build/$B1NIX_ARCH/ports/musl"
INSTALL_DIR="$BUILD_DIR/install"
# Dedicated top-level alias: build/<arch>/libc/ — same tree, symlinked
LIBC_DIR="$ROOT_DIR/build/$B1NIX_ARCH/libc"
mkdir -p "$SRC_PARENT/$B1NIX_TRIPLET" "$BUILD_DIR" "$INSTALL_DIR"

# --- clean -------------------------------------------------------------------
if [ "${1:-}" = "--clean" ]; then
  echo "build-musl.sh: cleaning $BUILD_DIR and $SRC_PARENT" >&2
  rm -rf "$BUILD_DIR" "$SRC_PARENT"
  echo "$INSTALL_DIR"
  exit 0
fi

# Bump when any generated uapi header below changes: the fast path stores this
# number in the sysroot and only skips the rebuild when it matches, so a tree
# built before a header grew a constant refreshes instead of staying stale.
UAPI_HEADERS_VERSION=2
UAPI_STAMP="$INSTALL_DIR/include/.b1nix-uapi-version"

LOCKFILE="$BUILD_DIR/locks/build.lock"
mkdir -p "$(dirname "$LOCKFILE")"

(
  flock -x 9
  # The uapi headers this script emits (linux/{kd,keyboard,rtc,watchdog,i2c,
  # i2c-dev,vt}.h) are written further down, AFTER this fast path, so "already
  # built" has to account for them: a stamp carrying UAPI_HEADERS_VERSION.
  # Checking for a header's mere existence was not enough — vt.h kept the text
  # it was first written with (VT_RELDISP and VT_PROCESS missing) long after the
  # generator gained them, and the BusyBox applets that include it failed to
  # compile with no hint that the sysroot was simply stale.
  if [ -f "$INSTALL_DIR/lib/libc.a" ] && [ -f "$INSTALL_DIR/lib/libc.so" ] &&
     [ -f "$UAPI_STAMP" ] &&
     [ "$(cat "$UAPI_STAMP" 2>/dev/null)" = "$UAPI_HEADERS_VERSION" ]; then
    echo "$INSTALL_DIR"
    exit 0
  fi

  # --- fetch + extract ----------------------------------------------------------
  if [ ! -d "$SRC_DIR" ]; then
    echo "build-musl.sh: fetching musl $MUSL_VERSION" >&2
    port_fetch_tarball "$MUSL_URL" "$SRC_PARENT/$MUSL_TARBALL" \
      "$SRC_PARENT/$B1NIX_TRIPLET" "$SRC_DIR"
  fi

# --- provide __cxa_thread_atexit_impl ----------------------------------------
# musl 1.2.5 deliberately omits __cxa_thread_atexit_impl (a glibc-ABI primitive),
# but the LLVM libc++abi we build against references it for C++11 thread_local
# destructors — any C++ port that constructs a thread_local with a non-trivial
# dtor (e.g. libjxl inside NetSurf) then fails to relocate libc++abi.so.1 at load
# under b1nix's strict in-kernel dynamic linker. Add it to libc where it belongs:
# register each destructor on a pthread_key so they run at thread exit. musl's
# Makefile compiles every src/**/*.c, so dropping the file in is enough.
if [ -d "$SRC_DIR/src/thread" ] && [ ! -f "$SRC_DIR/src/thread/__cxa_thread_atexit_impl.c" ]; then
  cat > "$SRC_DIR/src/thread/__cxa_thread_atexit_impl.c" <<'EOF'
#include <pthread.h>
#include <stdlib.h>

/* One registered thread_local destructor. */
struct __cxa_tls_dtor {
	void (*func)(void *);
	void *obj;
	struct __cxa_tls_dtor *next;
};

static pthread_key_t __cxa_tls_dtor_key;
static pthread_once_t __cxa_tls_dtor_once = PTHREAD_ONCE_INIT;

/* pthread_key destructor: run the thread's registered dtors, most-recent first
 * (reverse registration order), as C++ requires. */
static void __cxa_tls_dtor_run(void *head)
{
	struct __cxa_tls_dtor *d = head;
	while (d) {
		struct __cxa_tls_dtor *next = d->next;
		d->func(d->obj);
		free(d);
		d = next;
	}
}

static void __cxa_tls_dtor_init(void)
{
	pthread_key_create(&__cxa_tls_dtor_key, __cxa_tls_dtor_run);
}

/* Itanium C++ ABI: register a destructor to run when the current thread exits.
 * The dso handle is unused (b1nix does not dlclose live TLS owners). */
int __cxa_thread_atexit_impl(void (*func)(void *), void *obj, void *dso)
{
	(void)dso;
	pthread_once(&__cxa_tls_dtor_once, __cxa_tls_dtor_init);
	struct __cxa_tls_dtor *d = malloc(sizeof *d);
	if (!d)
		return -1;
	d->func = func;
	d->obj = obj;
	d->next = pthread_getspecific(__cxa_tls_dtor_key);
	pthread_setspecific(__cxa_tls_dtor_key, d);
	return 0;
}
EOF
  echo "build-musl.sh: added __cxa_thread_atexit_impl.c" >&2
fi

# --- patch config.sub to accept b1nix ----------------------------------------
if [ -f "$SRC_DIR/config.sub" ] && ! grep -q 'b1nix' "$SRC_DIR/config.sub"; then
  echo "build-musl.sh: patching config.sub for b1nix" >&2
  tmp_sub="$SRC_DIR/config.sub.new"
  cp "$SRC_DIR/config.sub" "$tmp_sub"
  sed -e 's/| fiwix\* /| fiwix* | b1nix* /' \
      -e 's/| -mint\*/| -mint* | -b1nix*/' \
      -e 's/| -none\*/| -none* | -b1nix*/' \
      -e 's/| -elf\*/| -elf* | -b1nix*/' \
      -e 's/| -limine\*/| -limine* | -b1nix*/' \
      -e 's/| -os2\*/| -os2* | -b1nix*/' \
      "$tmp_sub" > "$SRC_DIR/config.sub"
  rm -f "$tmp_sub"
fi

if [ -f "$SRC_DIR/config.sub" ] && ! grep -q "$B1NIX_TRIPLET" "$SRC_DIR/config.sub"; then
  tmp_sub="$SRC_DIR/config.sub.new"
  cp "$SRC_DIR/config.sub" "$tmp_sub"
  sed -e 's/| -b1nix\*/| -b1nix* | '"$B1NIX_TRIPLET"'/' \
      "$tmp_sub" > "$SRC_DIR/config.sub"
  rm -f "$tmp_sub"
fi

# --- configure ----------------------------------------------------------------
CCACHE=""
if [ "${B1NIX_NO_CCACHE:-0}" != "1" ] && command -v ccache >/dev/null 2>&1; then
  CCACHE="ccache"
fi

MUSL_BASE_CFLAGS="-ffreestanding -nostdinc -fno-builtin -fno-stack-protector -msoft-float -mno-implicit-float -O2 -Wall -D__linux__ -D__b1nix__ -Db1nix"
MUSL_CC="${CCACHE} clang"
MUSL_AR="$(port_ar)"

if [ ! -f "$BUILD_DIR/Makefile" ]; then
  echo "build-musl.sh: configuring musl" >&2
  (
    cd "$SRC_DIR"
    CC="$MUSL_CC --target=x86_64-unknown-linux-gnu $MUSL_BASE_CFLAGS" \
    AR="$MUSL_AR" \
    RANLIB="${RANLIB:-$(command -v llvm-ranlib 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ranlib)}" \
    CFLAGS="$MUSL_BASE_CFLAGS" \
    ./configure \
      --host="$B1NIX_TRIPLET" \
      --prefix=/usr \
      --enable-shared \
      --enable-static \
      2>&1
  )

  # Apply the b1nix-specific dynamic-loader fixes to the clean musl source.
  # Keeping them as a repository patch makes rebuilds reproducible and avoids
  # leaving edits in the generated build/src/musl tree as the source of truth.
  # --forward skips already-applied hunks but then exits non-zero, which under
  # `set -e` would abort an incremental rebuild of an already-patched tree — so
  # tolerate that (idempotent re-apply).
  patch -d "$SRC_DIR" -p1 --forward --batch \
    < "$ROOT_DIR/tools/patches/musl/b1nix-dynamic-loader.patch" || true

  # --- post-configure fixups for freestanding LLVM cross-toolchain ---
  if [ -f "$SRC_DIR/config.mak" ]; then
    sed -i.bak "s|^CC = .*|CC = $MUSL_CC --target=x86_64-unknown-linux-gnu|" "$SRC_DIR/config.mak"
    # 1. musl's configure detects -lgcc/-lgcc_eh via the host compiler and adds
    #    them to LIBCC. We don't have libgcc — replace with compiler-rt builtins
    #    which provides __mulxc3/__muldc3/__mulsc3 (complex math helpers musl needs).
    #    /usr/lib/clang/*/lib/linux/... is where a Linux clang installs this —
    #    it never exists on macOS, so this glob always missed here, silently
    #    clearing LIBCC (see build-llvm-runtimes.sh for the sibling bug: the
    #    same class of "assumed a Linux host" path miss). Use our own built
    #    cross compiler-rt archive instead.
    CRT_BUILTINS="$TOOLCHAIN_BUILD_HOME/llvm-runtimes-build/install/lib/libcompiler_rt.a"
    [ -f "$CRT_BUILTINS" ] || CRT_BUILTINS=$(ls /usr/lib/clang/*/lib/linux/libclang_rt.builtins-x86_64.a 2>/dev/null | tail -1)
    if [ -n "$CRT_BUILTINS" ]; then
      sed -i.bak "s|^LIBCC = .*|LIBCC = $CRT_BUILTINS|" "$SRC_DIR/config.mak"
    else
      sed -i.bak 's/^LIBCC = .*/LIBCC =/' "$SRC_DIR/config.mak"
    fi

    # 2. -Wa,--noexecstack is a GNU assembler flag; clang's integrated assembler
    #    rejects it. Remove from CFLAGS_C99FSE.
    sed -i.bak 's/-Wa,--noexecstack//' "$SRC_DIR/config.mak"

    # 3. Force clang to use lld as the linker driver.
    #    musl Makefile: $(CC) $(CFLAGS_ALL) $(LDFLAGS_ALL) -nostdlib -shared ...
    #    -fuse-ld=lld must be in LDFLAGS_ALL so it reaches the link step.
    #    clang + lld auto-links compiler-rt for __mulxc3/__muldc3/__mulsc3.
    #    Also set a DT_SONAME on libc.so: without it, consumers (libc++abi.so.1,
    #    libc++.so.1) linked with `-l:libc.so` record the *absolute* install path
    #    as their DT_NEEDED, which breaks load-time resolution under b1nix's
    #    musl ld.so (it tries the host path verbatim -> ENOENT). A soname makes
    #    lld emit the bare basename instead.
    LLD_PATH="$(command -v ld.lld || echo /opt/homebrew/bin/ld.lld)"
    sed -i.bak "s|^LDFLAGS_AUTO = |LDFLAGS_AUTO = -Wl,-z,now -fuse-ld=$LLD_PATH -Wl,-soname,libc.so |" "$SRC_DIR/config.mak"

    echo "build-musl.sh: patched config.mak (LIBCC cleared, noexecstack removed, lld linker, eager PLT, libc.so soname)" >&2
  fi
fi

# --- build --------------------------------------------------------------------
echo "build-musl.sh: building musl (static + shared)" >&2

# musl's Makefile builds both .o (for libc.a) and .lo (PIC, for libc.so).
# With LIBCC empty and -fuse-ld=lld, the libc.so link uses ld.lld which
# auto-resolves compiler-rt builtins. This is the clean path: no manual ld.lld.
make -C "$SRC_DIR" -j"$(nproc 2>/dev/null || echo 4)" 2>&1 | tail -5

# --- install (manual, reliable) -----------------------------------------------
# Flat layout: install/lib/ + install/include/ — same as every other port.
echo "build-musl.sh: installing musl to $INSTALL_DIR" >&2
mkdir -p "$INSTALL_DIR/lib" "$INSTALL_DIR/include"

cp "$SRC_DIR/lib/libc.a"  "$INSTALL_DIR/lib/libc.a"
cp "$SRC_DIR/lib/libc.so" "$INSTALL_DIR/lib/libc.so"

# libc.so carries the whole implementation — math, threads, timers, dlopen,
# crypt, resolver. The per-facility archives are deliberately empty: they exist
# so that -lm/-lpthread/-lrt/-ldl keep resolving at link time while every symbol
# comes from libc.so at run time.
for compat in libm.a libpthread.a librt.a libdl.a libcrypt.a \
              libutil.a libresolv.a libxnet.a; do
  [ -f "$SRC_DIR/lib/$compat" ] && cp "$SRC_DIR/lib/$compat" "$INSTALL_DIR/lib/$compat"
done

# Drop per-facility .so — they would duplicate code already in libc.so.
rm -f "$INSTALL_DIR/lib"/libm.so* "$INSTALL_DIR/lib"/libpthread.so* \
      "$INSTALL_DIR/lib"/librt.so* "$INSTALL_DIR/lib"/libdl.so* \
      "$INSTALL_DIR/lib"/libcrypt.so* "$INSTALL_DIR/lib"/libutil.so* \
      "$INSTALL_DIR/lib"/libresolv.so* "$INSTALL_DIR/lib"/libxnet.so*

for crt in crt1.o crti.o crtn.o Scrt1.o rcrt1.o; do
  [ -f "$SRC_DIR/lib/$crt" ] && cp "$SRC_DIR/lib/$crt" "$INSTALL_DIR/lib/$crt"
done

# Headers — use musl's own target, fall back to manual copy.
make -C "$SRC_DIR" install-headers DESTDIR="$INSTALL_DIR" prefix= 2>&1 || true
if [ ! -f "$INSTALL_DIR/include/stdlib.h" ]; then
  cp -r "$SRC_DIR"/include/* "$INSTALL_DIR/include/"
fi
# Generated bits/alltypes.h
if [ -f "$SRC_DIR/obj/include/bits/alltypes.h" ]; then
  mkdir -p "$INSTALL_DIR/include/bits"
  cp "$SRC_DIR/obj/include/bits/alltypes.h" "$INSTALL_DIR/include/bits/alltypes.h"
fi
if [ -d "$SRC_DIR/arch/x86_64/bits" ]; then
  mkdir -p "$INSTALL_DIR/include/bits"
  cp -r "$SRC_DIR"/arch/x86_64/bits/* "$INSTALL_DIR/include/bits/"
fi
if [ -d "$SRC_DIR/arch/generic/bits" ]; then
  mkdir -p "$INSTALL_DIR/include/bits"
  for f in "$SRC_DIR"/arch/generic/bits/*; do
    [ -f "$INSTALL_DIR/include/bits/$(basename "$f")" ] || cp "$f" "$INSTALL_DIR/include/bits/"
  done
fi

# linux/futex.h: musl deliberately ships no kernel-uapi headers (it never
# needs the FUTEX_* op constants as macros — its own futex calls are internal
# and hardcode the numbers), but plenty of ported software (e.g. Chromium's
# PartitionAlloc SpinningMutex, in the V8 port) #includes it directly for
# those constants. b1nix's own futex(2) (kernel/sched/futex.c) implements the
# same numeric ops via the Linux-ABI syscall layer, so only the header is
# missing, not the kernel feature — provide the standard uapi values.
mkdir -p "$INSTALL_DIR/include/linux"
# Regenerated every run: the guard that skipped an existing copy left a
# sysroot stuck with whatever this script wrote the first time — /linux/vt.h
# still said "b1nix has no virtual terminals" three milestones after it did.
if true; then
  cat > "$INSTALL_DIR/include/linux/futex.h" <<'EOF'
/* b1nix compat: standard Linux uapi/linux/futex.h FUTEX_* constants.
 * musl does not ship this header; b1nix's futex(2) implements the same
 * numeric ops, so only the constants (not the syscall) were missing. */
#ifndef _LINUX_FUTEX_H
#define _LINUX_FUTEX_H

#define FUTEX_WAIT              0
#define FUTEX_WAKE              1
#define FUTEX_FD                2
#define FUTEX_REQUEUE           3
#define FUTEX_CMP_REQUEUE       4
#define FUTEX_WAKE_OP           5
#define FUTEX_LOCK_PI           6
#define FUTEX_UNLOCK_PI         7
#define FUTEX_TRYLOCK_PI        8
#define FUTEX_WAIT_BITSET       9
#define FUTEX_WAKE_BITSET       10
#define FUTEX_WAIT_REQUEUE_PI   11
#define FUTEX_CMP_REQUEUE_PI    12
#define FUTEX_LOCK_PI2          13

#define FUTEX_PRIVATE_FLAG      128
#define FUTEX_CLOCK_REALTIME    256
#define FUTEX_CMD_MASK          (~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME))
#define FUTEX_BITSET_MATCH_ANY  0xffffffff

#define FUTEX_WAIT_PRIVATE      (FUTEX_WAIT | FUTEX_PRIVATE_FLAG)
#define FUTEX_WAKE_PRIVATE      (FUTEX_WAKE | FUTEX_PRIVATE_FLAG)
#define FUTEX_REQUEUE_PRIVATE   (FUTEX_REQUEUE | FUTEX_PRIVATE_FLAG)
#define FUTEX_CMP_REQUEUE_PRIVATE (FUTEX_CMP_REQUEUE | FUTEX_PRIVATE_FLAG)
#define FUTEX_WAKE_OP_PRIVATE   (FUTEX_WAKE_OP | FUTEX_PRIVATE_FLAG)
#define FUTEX_LOCK_PI_PRIVATE   (FUTEX_LOCK_PI | FUTEX_PRIVATE_FLAG)
#define FUTEX_LOCK_PI2_PRIVATE  (FUTEX_LOCK_PI2 | FUTEX_PRIVATE_FLAG)
#define FUTEX_UNLOCK_PI_PRIVATE (FUTEX_UNLOCK_PI | FUTEX_PRIVATE_FLAG)
#define FUTEX_TRYLOCK_PI_PRIVATE (FUTEX_TRYLOCK_PI | FUTEX_PRIVATE_FLAG)
#define FUTEX_WAIT_BITSET_PRIVATE (FUTEX_WAIT_BITSET | FUTEX_PRIVATE_FLAG)
#define FUTEX_WAKE_BITSET_PRIVATE (FUTEX_WAKE_BITSET | FUTEX_PRIVATE_FLAG)
#define FUTEX_WAIT_REQUEUE_PI_PRIVATE (FUTEX_WAIT_REQUEUE_PI | FUTEX_PRIVATE_FLAG)
#define FUTEX_CMP_REQUEUE_PI_PRIVATE (FUTEX_CMP_REQUEUE_PI | FUTEX_PRIVATE_FLAG)

struct futex_waitv {
	unsigned int val;
	unsigned int flags;
	unsigned int __reserved;
	unsigned long long uaddr;
};

#endif /* _LINUX_FUTEX_H */
EOF
fi

# linux/auxvec.h: same story as futex.h above — ported software (V8's
# src/base/cpu.cc reads AT_HWCAP) expects the kernel-uapi AT_* constants that
# musl does not ship as a public header.
if true; then
  cat > "$INSTALL_DIR/include/linux/auxvec.h" <<'EOF'
#ifndef _LINUX_AUXVEC_H
#define _LINUX_AUXVEC_H

#define AT_NULL   0
#define AT_IGNORE 1
#define AT_EXECFD 2
#define AT_PHDR   3
#define AT_PHENT  4
#define AT_PHNUM  5
#define AT_BASE   6
#define AT_FLAGS  7
#define AT_ENTRY  9
#define AT_NOTELF 10
#define AT_UID    11
#define AT_EUID   12
#define AT_GID    13
#define AT_EGID   14
#define AT_PLATFORM 15
#define AT_HWCAP  16
#define AT_CLKTCK 17
#define AT_SECURE 23
#define AT_BASE_PLATFORM 24
#define AT_RANDOM 25
#define AT_HWCAP2 26
#define AT_EXECFN 31

#endif /* _LINUX_AUXVEC_H */
EOF
fi

# linux/unistd.h: ported software (e.g. abseil's direct_mmap.h) includes this
# for __NR_mmap/__NR_mmap2 as an old-kernel fallback, always behind
# `#if defined(__NR_mmap)` — so an empty header is a correct, complete answer
# (musl's own sys/syscall.h already provides SYS_mmap unconditionally).
if true; then
  echo "/* b1nix compat: empty — musl provides SYS_* via sys/syscall.h; callers guard with #if defined(__NR_*) */" \
    > "$INSTALL_DIR/include/linux/unistd.h"
fi

# M107 kernel UAPI headers. musl ships no linux/ tree, so every applet that
# talks to a character device through its uapi header (setfont/loadkmap and the
# other console tools, hwclock, rtcwake, watchdog, the i2c-tools family) has
# nothing to compile against. These are the real Linux numbers and structs —
# kernel/dev/{vt,rtc_dev,watchdog,i2c}.c implement them.
if true; then
  mkdir -p "$INSTALL_DIR/include/linux"
  cat > "$INSTALL_DIR/include/linux/kd.h" <<'B1NIX_EOF'
/* b1nix: uapi <linux/kd.h>. The console-tools applets (setfont, loadkmap,
 * dumpkmap, chvt, openvt, deallocvt) compile against this; the numbers are
 * Linux's and kernel/dev/vt.c implements them. */
#ifndef _B1NIX_LINUX_KD_H
#define _B1NIX_LINUX_KD_H

#include <linux/types.h>

#define GIO_FONT       0x4B60  /* gets font in expanded form */
#define PIO_FONT       0x4B61  /* use font in expanded form */
#define GIO_FONTX      0x4B6B  /* get font using struct consolefontdesc */
#define PIO_FONTX      0x4B6C  /* set font using struct consolefontdesc */
#define PIO_FONTRESET  0x4B6D  /* reset to default font */

struct consolefontdesc {
	unsigned short charcount;   /* characters in font (256 or 512) */
	unsigned short charheight;  /* scan lines per character (1-32) */
	char *chardata;             /* font data in expanded form */
};

#define GIO_CMAP  0x4B70
#define PIO_CMAP  0x4B71

#define KIOCSOUND 0x4B2F
#define KDMKTONE  0x4B30

#define KDGETLED  0x4B31
#define KDSETLED  0x4B32
#define LED_SCR   0x01
#define LED_NUM   0x02
#define LED_CAP   0x04

#define KDGKBTYPE 0x4B33
#define KB_84     0x01
#define KB_101    0x02
#define KB_OTHER  0x03

#define KDADDIO   0x4B34
#define KDDELIO   0x4B35
#define KDENABIO  0x4B36
#define KDDISABIO 0x4B37

#define KDSETMODE 0x4B3A
#define KDGETMODE 0x4B3B
#define KD_TEXT       0x00
#define KD_GRAPHICS   0x01
#define KD_TEXT0      0x02
#define KD_TEXT1      0x03

#define KDMAPDISP 0x4B3C
#define KDUNMAPDISP 0x4B3D

typedef char scrnmap_t;
#define E_TABSZ 256
#define GIO_SCRNMAP  0x4B40
#define PIO_SCRNMAP  0x4B41
#define GIO_UNISCRNMAP 0x4B69
#define PIO_UNISCRNMAP 0x4B6A

#define GIO_UNIMAP 0x4B66
struct unipair {
	unsigned short unicode;
	unsigned short fontpos;
};
struct unimapdesc {
	unsigned short entry_ct;
	struct unipair *entries;
};
#define PIO_UNIMAP    0x4B67
#define PIO_UNIMAPCLR 0x4B68
struct unimapinit {
	unsigned short advised_hashsize;
	unsigned short advised_hashstep;
	unsigned short advised_hashlevel;
};

#define UNI_DIRECT_BASE 0xF000
#define UNI_DIRECT_MASK 0x01FF

#define K_RAW       0x00
#define K_XLATE     0x01
#define K_MEDIUMRAW 0x02
#define K_UNICODE   0x03
#define K_OFF       0x04
#define KDGKBMODE   0x4B44
#define KDSKBMODE   0x4B45

#define K_METABIT   0x03
#define K_ESCPREFIX 0x04
#define KDGKBMETA   0x4B62
#define KDSKBMETA   0x4B63

#define K_SCROLLLOCK 0x01
#define K_NUMLOCK    0x02
#define K_CAPSLOCK   0x04
#define KDGKBLED 0x4B64
#define KDSKBLED 0x4B65

struct kbentry {
	unsigned char kb_table;
	unsigned char kb_index;
	unsigned short kb_value;
};
#define K_NORMTAB 0x00
#define K_SHIFTTAB 0x01
#define K_ALTTAB 0x02
#define K_ALTSHIFTTAB 0x03

#define KDGKBENT 0x4B46
#define KDSKBENT 0x4B47

struct kbsentry {
	unsigned char kb_func;
	unsigned char kb_string[512];
};
#define KDGKBSENT 0x4B48
#define KDSKBSENT 0x4B49

struct kbdiacr {
	unsigned char diacr, base, result;
};
struct kbdiacrs {
	unsigned int kb_cnt;
	struct kbdiacr kbdiacr[256];
};
#define KDGKBDIACR 0x4B4A
#define KDSKBDIACR 0x4B4B

struct kbkeycode {
	unsigned int scancode, keycode;
};
#define KDGETKEYCODE 0x4B4C
#define KDSETKEYCODE 0x4B4D

#define KDSIGACCEPT 0x4B4E

struct kbd_repeat {
	int delay;
	int period;
};
#define KDKBDREP 0x4B52

#define KDFONTOP 0x4B72
struct console_font_op {
	unsigned int op;
	unsigned int flags;
	unsigned int width, height;
	unsigned int charcount;
	unsigned char *data;
};
struct console_font {
	unsigned int width, height;
	unsigned int charcount;
	unsigned char *data;
};
#define KD_FONT_OP_SET          0
#define KD_FONT_OP_GET          1
#define KD_FONT_OP_SET_DEFAULT  2
#define KD_FONT_OP_COPY         3
#define KD_FONT_FLAG_DONT_RECALC 1

#endif /* _B1NIX_LINUX_KD_H */
B1NIX_EOF
fi

if true; then
  mkdir -p "$INSTALL_DIR/include/linux"
  cat > "$INSTALL_DIR/include/linux/keyboard.h" <<'B1NIX_EOF'
/* b1nix: uapi <linux/keyboard.h> — the keysym encoding the VT keymap uses. */
#ifndef _B1NIX_LINUX_KEYBOARD_H
#define _B1NIX_LINUX_KEYBOARD_H

/* kernel/dev/vt.c stores 16 real modifier tables of 128 keys; tables beyond
 * that read back as K_HOLE, exactly as an unallocated Linux keymap does. */
#define NR_KEYS         128
#define MAX_NR_KEYMAPS  256
#define MAX_NR_FUNC     256

#define KG_SHIFT   0
#define KG_CTRL    2
#define KG_ALT     3
#define KG_ALTGR   1
#define KG_SHIFTL  4
#define KG_KANASHIFT 4
#define KG_SHIFTR  5
#define KG_CTRLL   6
#define KG_CTRLR   7
#define KG_CAPSSHIFT 8

#define NR_SHIFT 9

#define K(t,v)     (((t) << 8) | (v))
#define KTYP(x)    ((x) >> 8)
#define KVAL(x)    ((x) & 0xff)

#define KT_LATIN   0
#define KT_LETTER  11
#define KT_FN      1
#define KT_SPEC    2
#define KT_PAD     3
#define KT_DEAD    4
#define KT_CONS    5
#define KT_CUR     6
#define KT_SHIFT   7
#define KT_META    8
#define KT_ASCII   9
#define KT_LOCK    10
#define KT_SLOCK   12
#define KT_BRL     14

#define K_HOLE     K(KT_SPEC, 0)
#define K_ENTER    K(KT_SPEC, 1)

#endif /* _B1NIX_LINUX_KEYBOARD_H */
B1NIX_EOF
fi

if true; then
  mkdir -p "$INSTALL_DIR/include/linux"
  cat > "$INSTALL_DIR/include/linux/rtc.h" <<'B1NIX_EOF'
/* b1nix: uapi <linux/rtc.h> — /dev/rtc0, implemented in kernel/dev/rtc_dev.c. */
#ifndef _B1NIX_LINUX_RTC_H
#define _B1NIX_LINUX_RTC_H

#include <linux/types.h>
#include <linux/ioctl.h>

struct rtc_time {
	int tm_sec;
	int tm_min;
	int tm_hour;
	int tm_mday;
	int tm_mon;
	int tm_year;
	int tm_wday;
	int tm_yday;
	int tm_isdst;
};

struct rtc_wkalrm {
	unsigned char enabled;
	unsigned char pending;
	struct rtc_time time;
};

struct rtc_pll_info {
	int pll_ctrl;
	int pll_value;
	int pll_max;
	int pll_min;
	int pll_posmult;
	int pll_negmult;
	long pll_clock;
};

#define RTC_AIE_ON  _IO('p', 0x01)
#define RTC_AIE_OFF _IO('p', 0x02)
#define RTC_UIE_ON  _IO('p', 0x03)
#define RTC_UIE_OFF _IO('p', 0x04)
#define RTC_PIE_ON  _IO('p', 0x05)
#define RTC_PIE_OFF _IO('p', 0x06)
#define RTC_WIE_ON  _IO('p', 0x0f)
#define RTC_WIE_OFF _IO('p', 0x10)

#define RTC_ALM_SET  _IOW('p', 0x07, struct rtc_time)
#define RTC_ALM_READ _IOR('p', 0x08, struct rtc_time)
#define RTC_RD_TIME  _IOR('p', 0x09, struct rtc_time)
#define RTC_SET_TIME _IOW('p', 0x0a, struct rtc_time)
#define RTC_IRQP_READ  _IOR('p', 0x0b, unsigned long)
#define RTC_IRQP_SET   _IOW('p', 0x0c, unsigned long)
#define RTC_EPOCH_READ _IOR('p', 0x0d, unsigned long)
#define RTC_EPOCH_SET  _IOW('p', 0x0e, unsigned long)

#define RTC_WKALM_SET _IOW('p', 0x0f, struct rtc_wkalrm)
#define RTC_WKALM_RD  _IOR('p', 0x10, struct rtc_wkalrm)

#define RTC_PLL_GET _IOR('p', 0x11, struct rtc_pll_info)
#define RTC_PLL_SET _IOW('p', 0x12, struct rtc_pll_info)

#define RTC_VL_READ  _IOR('p', 0x13, int)
#define RTC_VL_CLR   _IO('p', 0x14)

#define RTC_IRQF 0x80
#define RTC_PF   0x40
#define RTC_AF   0x20
#define RTC_UF   0x10

#endif /* _B1NIX_LINUX_RTC_H */
B1NIX_EOF
fi

if true; then
  mkdir -p "$INSTALL_DIR/include/linux"
  cat > "$INSTALL_DIR/include/linux/watchdog.h" <<'B1NIX_EOF'
/* b1nix: uapi <linux/watchdog.h> — /dev/watchdog, kernel/dev/watchdog.c. */
#ifndef _B1NIX_LINUX_WATCHDOG_H
#define _B1NIX_LINUX_WATCHDOG_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define WATCHDOG_IOCTL_BASE 'W'

struct watchdog_info {
	__u32 options;
	__u32 firmware_version;
	__u8 identity[32];
};

#define WDIOC_GETSUPPORT    _IOR(WATCHDOG_IOCTL_BASE, 0, struct watchdog_info)
#define WDIOC_GETSTATUS     _IOR(WATCHDOG_IOCTL_BASE, 1, int)
#define WDIOC_GETBOOTSTATUS _IOR(WATCHDOG_IOCTL_BASE, 2, int)
#define WDIOC_GETTEMP       _IOR(WATCHDOG_IOCTL_BASE, 3, int)
#define WDIOC_SETOPTIONS    _IOR(WATCHDOG_IOCTL_BASE, 4, int)
#define WDIOC_KEEPALIVE     _IOR(WATCHDOG_IOCTL_BASE, 5, int)
#define WDIOC_SETTIMEOUT    _IOWR(WATCHDOG_IOCTL_BASE, 6, int)
#define WDIOC_GETTIMEOUT    _IOR(WATCHDOG_IOCTL_BASE, 7, int)
#define WDIOC_SETPRETIMEOUT _IOWR(WATCHDOG_IOCTL_BASE, 8, int)
#define WDIOC_GETPRETIMEOUT _IOR(WATCHDOG_IOCTL_BASE, 9, int)
#define WDIOC_GETTIMELEFT   _IOR(WATCHDOG_IOCTL_BASE, 10, int)

#define WDIOF_UNKNOWN      -1
#define WDIOS_UNKNOWN      -1

#define WDIOF_OVERHEAT      0x0001
#define WDIOF_FANFAULT      0x0002
#define WDIOF_EXTERN1       0x0004
#define WDIOF_EXTERN2       0x0008
#define WDIOF_POWERUNDER    0x0010
#define WDIOF_CARDRESET     0x0020
#define WDIOF_POWEROVER     0x0040
#define WDIOF_SETTIMEOUT    0x0080
#define WDIOF_MAGICCLOSE    0x0100
#define WDIOF_PRETIMEOUT    0x0200
#define WDIOF_ALARMONLY     0x0400
#define WDIOF_KEEPALIVEPING 0x8000

#define WDIOS_DISABLECARD 0x0001
#define WDIOS_ENABLECARD  0x0002
#define WDIOS_TEMPPANIC   0x0004

#endif /* _B1NIX_LINUX_WATCHDOG_H */
B1NIX_EOF
fi

if true; then
  mkdir -p "$INSTALL_DIR/include/linux"
  cat > "$INSTALL_DIR/include/linux/i2c.h" <<'B1NIX_EOF'
/* b1nix: uapi <linux/i2c.h> — the SMBus transaction shapes /dev/i2c-N accepts
 * (kernel/dev/i2c.c drives a PIIX4/ICH9 SMBus host controller). */
#ifndef _B1NIX_LINUX_I2C_H
#define _B1NIX_LINUX_I2C_H

#include <linux/types.h>

struct i2c_msg {
	__u16 addr;
	__u16 flags;
#define I2C_M_RD        0x0001
#define I2C_M_TEN       0x0010
#define I2C_M_RECV_LEN  0x0400
#define I2C_M_NO_RD_ACK 0x0800
#define I2C_M_IGNORE_NAK 0x1000
#define I2C_M_REV_DIR_ADDR 0x2000
#define I2C_M_NOSTART   0x4000
#define I2C_M_STOP      0x8000
	__u16 len;
	__u8 *buf;
};

#define I2C_FUNC_I2C                    0x00000001
#define I2C_FUNC_10BIT_ADDR             0x00000002
#define I2C_FUNC_PROTOCOL_MANGLING      0x00000004
#define I2C_FUNC_SMBUS_PEC              0x00000008
#define I2C_FUNC_NOSTART                0x00000010
#define I2C_FUNC_SLAVE                  0x00000020
#define I2C_FUNC_SMBUS_BLOCK_PROC_CALL  0x00008000
#define I2C_FUNC_SMBUS_QUICK            0x00010000
#define I2C_FUNC_SMBUS_READ_BYTE        0x00020000
#define I2C_FUNC_SMBUS_WRITE_BYTE       0x00040000
#define I2C_FUNC_SMBUS_READ_BYTE_DATA   0x00080000
#define I2C_FUNC_SMBUS_WRITE_BYTE_DATA  0x00100000
#define I2C_FUNC_SMBUS_READ_WORD_DATA   0x00200000
#define I2C_FUNC_SMBUS_WRITE_WORD_DATA  0x00400000
#define I2C_FUNC_SMBUS_PROC_CALL        0x00800000
#define I2C_FUNC_SMBUS_READ_BLOCK_DATA  0x01000000
#define I2C_FUNC_SMBUS_WRITE_BLOCK_DATA 0x02000000
#define I2C_FUNC_SMBUS_READ_I2C_BLOCK   0x04000000
#define I2C_FUNC_SMBUS_WRITE_I2C_BLOCK  0x08000000
#define I2C_FUNC_SMBUS_HOST_NOTIFY      0x10000000

#define I2C_FUNC_SMBUS_BYTE \
	(I2C_FUNC_SMBUS_READ_BYTE | I2C_FUNC_SMBUS_WRITE_BYTE)
#define I2C_FUNC_SMBUS_BYTE_DATA \
	(I2C_FUNC_SMBUS_READ_BYTE_DATA | I2C_FUNC_SMBUS_WRITE_BYTE_DATA)
#define I2C_FUNC_SMBUS_WORD_DATA \
	(I2C_FUNC_SMBUS_READ_WORD_DATA | I2C_FUNC_SMBUS_WRITE_WORD_DATA)
#define I2C_FUNC_SMBUS_BLOCK_DATA \
	(I2C_FUNC_SMBUS_READ_BLOCK_DATA | I2C_FUNC_SMBUS_WRITE_BLOCK_DATA)
#define I2C_FUNC_SMBUS_I2C_BLOCK \
	(I2C_FUNC_SMBUS_READ_I2C_BLOCK | I2C_FUNC_SMBUS_WRITE_I2C_BLOCK)
#define I2C_FUNC_SMBUS_EMUL \
	(I2C_FUNC_SMBUS_QUICK | I2C_FUNC_SMBUS_BYTE | \
	 I2C_FUNC_SMBUS_BYTE_DATA | I2C_FUNC_SMBUS_WORD_DATA | \
	 I2C_FUNC_SMBUS_PROC_CALL | I2C_FUNC_SMBUS_WRITE_BLOCK_DATA | \
	 I2C_FUNC_SMBUS_I2C_BLOCK | I2C_FUNC_SMBUS_PEC)

#define I2C_SMBUS_BLOCK_MAX 32
union i2c_smbus_data {
	__u8 byte;
	__u16 word;
	__u8 block[I2C_SMBUS_BLOCK_MAX + 2];
};

#define I2C_SMBUS_READ  1
#define I2C_SMBUS_WRITE 0

#define I2C_SMBUS_QUICK            0
#define I2C_SMBUS_BYTE             1
#define I2C_SMBUS_BYTE_DATA        2
#define I2C_SMBUS_WORD_DATA        3
#define I2C_SMBUS_PROC_CALL        4
#define I2C_SMBUS_BLOCK_DATA       5
#define I2C_SMBUS_I2C_BLOCK_BROKEN 6
#define I2C_SMBUS_BLOCK_PROC_CALL  7
#define I2C_SMBUS_I2C_BLOCK_DATA   8

#endif /* _B1NIX_LINUX_I2C_H */
B1NIX_EOF
fi

if true; then
  mkdir -p "$INSTALL_DIR/include/linux"
  cat > "$INSTALL_DIR/include/linux/i2c-dev.h" <<'B1NIX_EOF'
/* b1nix: uapi <linux/i2c-dev.h> — the /dev/i2c-N ioctl surface. */
#ifndef _B1NIX_LINUX_I2C_DEV_H
#define _B1NIX_LINUX_I2C_DEV_H

#include <linux/types.h>
#include <linux/i2c.h>

#define I2C_RETRIES     0x0701
#define I2C_TIMEOUT     0x0702
#define I2C_SLAVE       0x0703
#define I2C_SLAVE_FORCE 0x0706
#define I2C_TENBIT      0x0704
#define I2C_FUNCS       0x0705
#define I2C_RDWR        0x0707
#define I2C_PEC         0x0708
#define I2C_SMBUS       0x0720

struct i2c_smbus_ioctl_data {
	__u8 read_write;
	__u8 command;
	__u32 size;
	union i2c_smbus_data *data;
};

struct i2c_rdwr_ioctl_data {
	struct i2c_msg *msgs;
	__u32 nmsgs;
};

#define I2C_RDWR_IOCTL_MAX_MSGS 42

#endif /* _B1NIX_LINUX_I2C_DEV_H */
B1NIX_EOF
fi

if true; then
  mkdir -p "$INSTALL_DIR/include/linux"
  cat > "$INSTALL_DIR/include/linux/vt.h" <<'B1NIX_EOF'
/* b1nix: uapi <linux/vt.h>. b1nix has six virtual terminals (M107,
 * kernel/dev/vt.c) reachable as /dev/tty1../dev/tty6, with /dev/tty0 as the
 * "currently active" alias; these ioctls are implemented, not stubs. */
#ifndef _B1NIX_LINUX_VT_H
#define _B1NIX_LINUX_VT_H

#define MIN_NR_CONSOLES 1
#define MAX_NR_CONSOLES 63
#define MAX_NR_USER_CONSOLES 63

#define VT_OPENQRY     0x5600 /* find an available VT */
#define VT_GETMODE     0x5601
#define VT_SETMODE     0x5602
#define VT_GETSTATE    0x5603
#define VT_SENDSIG     0x5604
#define VT_RELDISP     0x5605
#define VT_ACTIVATE    0x5606
#define VT_WAITACTIVE  0x5607
#define VT_DISALLOCATE 0x5608
#define VT_RESIZE      0x5609
#define VT_RESIZEX     0x560A
#define VT_LOCKSWITCH  0x560B
#define VT_UNLOCKSWITCH 0x560C
#define VT_GETHIFONTMASK 0x560D
#define VT_WAITEVENT   0x560E

#define VT_AUTO    0x00
#define VT_PROCESS 0x01
#define VT_ACKACQ  0x02

struct vt_mode {
	char mode;
	char waitv;
	short relsig;
	short acqsig;
	short frsig;
};

struct vt_stat {
	unsigned short v_active;  /* active vt */
	unsigned short v_signal;  /* signal to send */
	unsigned short v_state;   /* vt bitmask */
};

struct vt_sizes {
	unsigned short v_rows;
	unsigned short v_cols;
	unsigned short v_scrollsize;
};

struct vt_consize {
	unsigned short v_rows;
	unsigned short v_cols;
	unsigned short v_vlin;
	unsigned short v_clin;
	unsigned short v_vcol;
	unsigned short v_ccol;
};

#endif /* _B1NIX_LINUX_VT_H */
B1NIX_EOF
fi

# Record which generation of the uapi headers this sysroot carries, so the fast
# path above can tell a current tree from one that predates a header change.
printf '%s\n' "$UAPI_HEADERS_VERSION" > "$UAPI_STAMP"

# Expose as build/<arch>/libc/ — dedicated top-level alias for the system libc
ln -sfn "$INSTALL_DIR" "$LIBC_DIR"

# Verify
if [ ! -f "$INSTALL_DIR/lib/libc.a" ]; then
  echo "build-musl.sh: ERROR — lib/libc.a not found" >&2
  exit 1
fi
if [ ! -f "$INSTALL_DIR/lib/libc.so" ]; then
  echo "build-musl.sh: ERROR — lib/libc.so not found" >&2
  exit 1
fi

readelf -h "$INSTALL_DIR/lib/libc.so" 2>/dev/null | grep -q "Entry" \
  && echo "build-musl.sh: libc.so entry point OK" >&2

echo "build-musl.sh: musl built successfully" >&2
echo "  static:  $INSTALL_DIR/lib/libc.a" >&2
echo "  shared:  $INSTALL_DIR/lib/libc.so" >&2
echo "  include: $INSTALL_DIR/include/" >&2
echo "  alias:   $LIBC_DIR" >&2

echo "$INSTALL_DIR"
) 9>"$LOCKFILE"
