#!/bin/sh
# Build musl as a static + shared libc for the b1nix userspace ABI.
#
# Produces:
#   - build/musl-b1nix/<triplet>/usr/lib/libc.a   (static musl)
#   - build/musl-b1nix/<triplet>/usr/lib/libc.so   (shared musl / ld.so)
#   - build/musl-b1nix/<triplet>/usr/include/      (musl headers)
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

SRC_PARENT="$ROOT_DIR/build/musl-src"
SRC_DIR="$SRC_PARENT/$B1NIX_TRIPLET/$MUSL_SRCNAME"
BUILD_DIR="$ROOT_DIR/build/musl-b1nix/$B1NIX_TRIPLET"
INSTALL_DIR="$BUILD_DIR/install"
mkdir -p "$SRC_PARENT/$B1NIX_TRIPLET" "$BUILD_DIR" "$INSTALL_DIR"

# --- clean -------------------------------------------------------------------
if [ "${1:-}" = "--clean" ]; then
  echo "build-musl.sh: cleaning $BUILD_DIR and $SRC_PARENT" >&2
  rm -rf "$BUILD_DIR" "$SRC_PARENT"
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
    CC="$MUSL_CC --target=$B1NIX_TRIPLET $MUSL_BASE_CFLAGS" \
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
  # leaving edits in the generated build/musl-src tree as the source of truth.
  # --forward skips already-applied hunks but then exits non-zero, which under
  # `set -e` would abort an incremental rebuild of an already-patched tree — so
  # tolerate that (idempotent re-apply).
  patch -d "$SRC_DIR" -p1 --forward --batch \
    < "$ROOT_DIR/tools/patches/musl/b1nix-dynamic-loader.patch" || true

  # --- post-configure fixups for freestanding LLVM cross-toolchain ---
  if [ -f "$SRC_DIR/config.mak" ]; then
    # 1. musl's configure detects -lgcc/-lgcc_eh via the host compiler and adds
    #    them to LIBCC. We don't have libgcc — replace with compiler-rt builtins
    #    which provides __mulxc3/__muldc3/__mulsc3 (complex math helpers musl needs).
    CRT_BUILTINS=$(ls /usr/lib/clang/*/lib/linux/libclang_rt.builtins-x86_64.a 2>/dev/null | tail -1)
    if [ -n "$CRT_BUILTINS" ]; then
      sed -i "s|^LIBCC = .*|LIBCC = $CRT_BUILTINS|" "$SRC_DIR/config.mak"
    else
      sed -i 's/^LIBCC = .*/LIBCC =/' "$SRC_DIR/config.mak"
    fi

    # 2. -Wa,--noexecstack is a GNU assembler flag; clang's integrated assembler
    #    rejects it. Remove from CFLAGS_C99FSE.
    sed -i 's/-Wa,--noexecstack//' "$SRC_DIR/config.mak"

    # 3. Force clang to use lld as the linker driver.
    #    musl Makefile: $(CC) $(CFLAGS_ALL) $(LDFLAGS_ALL) -nostdlib -shared ...
    #    -fuse-ld=lld must be in LDFLAGS_ALL so it reaches the link step.
    #    clang + lld auto-links compiler-rt for __mulxc3/__muldc3/__mulsc3.
    sed -i 's/^LDFLAGS_AUTO = /LDFLAGS_AUTO = -Wl,-z,now -fuse-ld=lld /' "$SRC_DIR/config.mak"

    echo "build-musl.sh: patched config.mak (LIBCC cleared, noexecstack removed, lld linker, eager PLT)" >&2
  fi
fi

# --- build --------------------------------------------------------------------
echo "build-musl.sh: building musl (static + shared)" >&2

# musl's Makefile builds both .o (for libc.a) and .lo (PIC, for libc.so).
# With LIBCC empty and -fuse-ld=lld, the libc.so link uses ld.lld which
# auto-resolves compiler-rt builtins. This is the clean path: no manual ld.lld.
make -C "$SRC_DIR" -j"$(nproc 2>/dev/null || echo 4)" 2>&1 | tail -5

# --- install (manual, reliable) -----------------------------------------------
echo "build-musl.sh: installing musl to $INSTALL_DIR" >&2
mkdir -p "$INSTALL_DIR/usr/lib" "$INSTALL_DIR/usr/include"

cp "$SRC_DIR/lib/libc.a" "$INSTALL_DIR/usr/lib/libc.a"
cp "$SRC_DIR/lib/libc.so" "$INSTALL_DIR/usr/lib/libc.so"

# libc.so carries the whole implementation — math, threads, timers, dlopen,
# crypt, resolver. The per-facility archives the build emits are deliberately
# empty: they exist so that `-lm`, `-lpthread`, `-lrt`, `-ldl` … keep resolving
# at link time while every symbol comes from libc.so at run time. Installing
# them is what keeps a single libc blob on the image instead of one shared
# object per facility.
for compat in libm.a libpthread.a librt.a libdl.a libcrypt.a \
              libutil.a libresolv.a libxnet.a; do
  if [ -f "$SRC_DIR/lib/$compat" ]; then
    cp "$SRC_DIR/lib/$compat" "$INSTALL_DIR/usr/lib/$compat"
  fi
done

# Any per-facility shared object in the install tree would pull a second copy of
# code that already lives in libc.so, and would add a DT_NEEDED that resolves to
# nothing at run time. Drop them.
rm -f "$INSTALL_DIR/usr/lib"/libm.so* "$INSTALL_DIR/usr/lib"/libpthread.so* \
      "$INSTALL_DIR/usr/lib"/librt.so* "$INSTALL_DIR/usr/lib"/libdl.so* \
      "$INSTALL_DIR/usr/lib"/libcrypt.so* "$INSTALL_DIR/usr/lib"/libutil.so* \
      "$INSTALL_DIR/usr/lib"/libresolv.so* "$INSTALL_DIR/usr/lib"/libxnet.so*

for crt in crt1.o crti.o crtn.o Scrt1.o rcrt1.o; do
  [ -f "$SRC_DIR/lib/$crt" ] && cp "$SRC_DIR/lib/$crt" "$INSTALL_DIR/usr/lib/$crt"
done

# Install headers via musl's own make (handles arch bits symlink + generated files)
make -C "$SRC_DIR" install-headers DESTDIR="$INSTALL_DIR" 2>&1 || true
# Fallback: manual copy if install-headers target doesn't exist
if [ ! -f "$INSTALL_DIR/usr/include/stdlib.h" ]; then
  cp -r "$SRC_DIR"/include/* "$INSTALL_DIR/usr/include/"
fi
# musl's arch-specific "bits" headers — ensure generated alltypes.h is present
if [ -f "$SRC_DIR/obj/include/bits/alltypes.h" ]; then
  mkdir -p "$INSTALL_DIR/usr/include/bits"
  cp "$SRC_DIR/obj/include/bits/alltypes.h" "$INSTALL_DIR/usr/include/bits/alltypes.h"
fi
if [ -d "$SRC_DIR/arch/x86_64/bits" ]; then
  mkdir -p "$INSTALL_DIR/usr/include/bits"
  cp -r "$SRC_DIR"/arch/x86_64/bits/* "$INSTALL_DIR/usr/include/bits/"
fi
if [ -d "$SRC_DIR/arch/generic/bits" ]; then
  mkdir -p "$INSTALL_DIR/usr/include/bits"
  for f in "$SRC_DIR"/arch/generic/bits/*; do
    [ -f "$INSTALL_DIR/usr/include/bits/$(basename "$f")" ] || cp "$f" "$INSTALL_DIR/usr/include/bits/"
  done
fi

# Verify
if [ ! -f "$INSTALL_DIR/usr/lib/libc.a" ]; then
  echo "build-musl.sh: ERROR — lib/libc.a not found" >&2
  exit 1
fi
if [ ! -f "$INSTALL_DIR/usr/lib/libc.so" ]; then
  echo "build-musl.sh: ERROR — lib/libc.so not found" >&2
  exit 1
fi

readelf -h "$INSTALL_DIR/usr/lib/libc.so" 2>/dev/null | grep -q "Entry" \
  && echo "build-musl.sh: libc.so entry point OK" >&2

echo "build-musl.sh: musl built successfully" >&2
echo "  static: $INSTALL_DIR/usr/lib/libc.a" >&2
echo "  shared: $INSTALL_DIR/usr/lib/libc.so" >&2
echo "  include: $INSTALL_DIR/usr/include/" >&2

echo "$INSTALL_DIR/usr"
