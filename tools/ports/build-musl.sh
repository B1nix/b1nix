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

LOCKFILE="$BUILD_DIR/locks/build.lock"
mkdir -p "$(dirname "$LOCKFILE")"

(
  flock -x 9
  if [ -f "$INSTALL_DIR/lib/libc.a" ] && [ -f "$INSTALL_DIR/lib/libc.so" ]; then
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
if [ ! -f "$INSTALL_DIR/include/linux/futex.h" ]; then
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
if [ ! -f "$INSTALL_DIR/include/linux/auxvec.h" ]; then
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
if [ ! -f "$INSTALL_DIR/include/linux/unistd.h" ]; then
  echo "/* b1nix compat: empty — musl provides SYS_* via sys/syscall.h; callers guard with #if defined(__NR_*) */" \
    > "$INSTALL_DIR/include/linux/unistd.h"
fi

# linux/vt.h: BusyBox's init applet includes it unconditionally on Linux for the
# virtual-console ioctls. b1nix has one console and no VT switching, so the
# struct and the ioctl numbers are enough to compile; the ioctls themselves
# return ENOTTY at runtime, which is what init already handles ("not a VT").
if [ ! -f "$INSTALL_DIR/include/linux/vt.h" ]; then
  cat > "$INSTALL_DIR/include/linux/vt.h" <<'EOF'
/* b1nix compat: minimal uapi <linux/vt.h>. b1nix has no virtual terminals; the
 * numbers match Linux so a caller's ioctl() is a well-formed request that the
 * kernel simply rejects. */
#ifndef _B1NIX_LINUX_VT_H
#define _B1NIX_LINUX_VT_H

#define MIN_NR_CONSOLES 1
#define MAX_NR_CONSOLES 63

#define VT_OPENQRY   0x5600 /* find an available VT */
#define VT_GETMODE   0x5601
#define VT_SETMODE   0x5602
#define VT_GETSTATE  0x5603
#define VT_ACTIVATE  0x5606
#define VT_WAITACTIVE 0x5607
#define VT_DISALLOCATE 0x5608

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

#endif /* _B1NIX_LINUX_VT_H */
EOF
fi

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
