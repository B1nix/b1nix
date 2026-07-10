#!/bin/sh
# Build the NetSurf framebuffer browser (the real interactive `nsfb` binary) for
# the b1nix userspace ABI, using the b1nix cross-gcc toolchain and NetSurf's own
# native build system (TARGET=framebuffer).
#
# Stages a sysroot containing every ported NetSurf dependency (.a + headers +
# pkg-config .pc files), then drives netsurf-3.11's Makefile with that sysroot
# via the GCCSDK_INSTALL_ENV / GCCSDK_INSTALL_CROSSBIN convention the framebuffer
# frontend already understands. HTTP(S) (libcurl+mbedTLS), JavaScript (Duktape)
# and SVG (libsvgtiny) are all enabled; the internal bitmap font is used.
#
# M53 (NetSurf browser platform) — final step (7): the browser itself.

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
PROJECT_DIR="$ROOT_DIR"
. "$ROOT_DIR/tools/toolchain/env.sh"

NS_VERSION="${NS_VERSION:-3.11}"
SRC_PARENT="$ROOT_DIR/build/netsurf-src"
SRC_DIR="$SRC_PARENT/netsurf-${NS_VERSION}"
SYSROOT="$ROOT_DIR/build/netsurf-sysroot/$B1NIX_TRIPLET"
PKGDIR="$SYSROOT/lib/pkgconfig"
CROSSBIN="$TOOLCHAIN_BUILD_HOME/cross/bin"

# ── 0. Fetch the main NetSurf tree if absent ──
# The per-library tarballs (libcss, libhubbub, ...) are fetched by their own
# build-lib*.sh scripts, but the browser core itself is not. Pull it from the
# upstream "source-full" bundle (netsurf-all-<ver>), which unpacks to
# netsurf-all-<ver>/netsurf/, and stage that subtree as $SRC_DIR.
NS_BUNDLE="netsurf-all-${NS_VERSION}.tar.gz"
NS_URL="https://download.netsurf-browser.org/netsurf/releases/source-full/${NS_BUNDLE}"
if [ ! -d "$SRC_DIR" ]; then
  mkdir -p "$SRC_PARENT"
  tarball="$SRC_PARENT/$NS_BUNDLE"
  if [ ! -f "$tarball" ]; then
    if command -v curl >/dev/null 2>&1; then
      curl -fL --retry 3 "$NS_URL" -o "$tarball"
    elif command -v wget >/dev/null 2>&1; then
      wget -O "$tarball" "$NS_URL"
    else
      echo "tools/ports/build-netsurf-fb.sh: need host curl or wget to fetch $NS_URL" >&2
      exit 1
    fi
  fi
  tar -xzf "$tarball" -C "$SRC_PARENT"
  if [ -d "$SRC_PARENT/netsurf-all-${NS_VERSION}/netsurf" ]; then
    cp -a "$SRC_PARENT/netsurf-all-${NS_VERSION}/netsurf" "$SRC_DIR"
  else
    echo "tools/ports/build-netsurf-fb.sh: $NS_BUNDLE did not contain a netsurf/ subtree" >&2
    exit 1
  fi
fi

if [ "$B1NIX_ARCH" = "x86" ]; then NSC_TARGET="i686-unknown-elf"; else NSC_TARGET="x86_64-unknown-elf"; fi
CC="clang"; command -v ccache >/dev/null 2>&1 && [ "${B1NIX_NO_CCACHE:-0}" != "1" ] && CC="ccache clang"
AR="${AR:-$(command -v llvm-ar 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ar)}"

mkdir -p "$SYSROOT/include" "$SYSROOT/lib" "$PKGDIR"

# ── 1. Build every dependency lib and copy its install tree into the sysroot ──
# pkgname:builder:libfile:requires
emit_pc() {
  # $1 pkgname  $2 libflags(-lX...)  $3 requires
  cat >"$PKGDIR/$1.pc" <<EOF
prefix=$SYSROOT
exec_prefix=\${prefix}
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: $1
Description: $1 for b1nix
Version: 1.0.0
Requires: $3
Libs: -L\${libdir} $2
Cflags: -I\${includedir}
EOF
}

stage() {
  # $1 builder script  -> copies install/{include,lib} into sysroot
  inst="$(B1NIX_ARCH="$B1NIX_ARCH" "$ROOT_DIR/tools/ports/$1")"
  cp -R "$inst/include/." "$SYSROOT/include/" 2>/dev/null || true
  cp -R "$inst/lib/." "$SYSROOT/lib/" 2>/dev/null || true
}

stage build-zlib.sh
stage build-libpng.sh
stage build-libwapcaplet.sh
stage build-libparserutils.sh
stage build-libhubbub.sh
stage build-libcss.sh
stage build-libdom.sh
stage build-libnsutils.sh
stage build-libnsgif.sh
stage build-libnsbmp.sh
stage build-libnslog.sh
stage build-libnsfb.sh
stage build-libjpeg.sh   # libjpeg.a — JPEG image decoding
stage build-libwebp.sh   # libwebp.a — WebP image decoding
stage build-openlibm.sh   # libm.a (NetSurf + libpng need cos/sin/floor/pow/modf)
stage build-libsvgtiny.sh # libsvgtiny.a — SVG image decoding (needs libdom + math)
stage build-libnspsl.sh   # libnspsl.a — public suffix list (cookie/domain scoping)
stage build-librosprite.sh # librosprite.a — RISC-OS sprite image decoding
stage build-libutf8proc.sh # libutf8proc.a — Unicode/IDNA (utils/idna.c)
# libjxl (JPEG-XL, C++) is built against LLVM libc++ (not GCC libstdc++) so nsfb
# carries no GCC C++ code. Its bundled skcms uses raw AVX/F16C *GCC* intrinsics
# (__builtin_ia32_vcvtph2ps256) that clang neither compiles nor — as baseline
# instructions — runs on the plain QEMU CPU (SIGILL). -DSKCMS_PORTABLE forces skcms
# to its scalar path (N=1): compiles cleanly under clang and runs on any CPU. Highway
# (the image codec SIMD) is clang-native and does its own runtime ISA dispatch, so it
# needs no baseline flags.
JXL_INST="$(CFLAGS='-DSKCMS_PORTABLE' CXXFLAGS='-DSKCMS_PORTABLE' \
  B1NIX_ARCH="$B1NIX_ARCH" B1NIX_CXX_STDLIB=libc++ "$ROOT_DIR/tools/ports/build-libjxl.sh")"
cp -R "$JXL_INST/include/." "$SYSROOT/include/" 2>/dev/null || true
cp -R "$JXL_INST/lib/." "$SYSROOT/lib/" 2>/dev/null || true
# NOTE: libharu (PDF export) is ported as a standalone library
# (tools/ports/build-libharu.sh builds libhpdf.a), but NetSurf 3.11's PDF glue is
# upstream bit-rot — desktop/font_haru.c needs the removed desktop/font.h and a
# `struct font_functions` model that print_make_settings no longer uses — so
# NETSURF_USE_HARU_PDF stays NO until that dead code is revived.

# Stage crt0.o + libb1nix.a + headers into the cross sysroot (= build/$ARCH/rootfs).
# NetSurf links via the raw cross-gcc (GCCSDK convention), which finds crt0.o only
# in its sysroot lib — unlike other ports that pass crt0 explicitly via b1nix-cc.
# A prior `make clean` wipes rootfs and the kernel/iso build never repopulates it,
# so do it here. Idempotent and cheap.
make -C "$ROOT_DIR/userspace" B1NIX_ARCH="$B1NIX_ARCH" install-headers-libs 1>&2

# Dynamic libc for the GCCSDK raw-cross-gcc link (default): stage the shared
# libc.so.1 into the cross-gcc's OWN sysroot lib as libc.so, so the implicit
# `-lc` resolves to the shared library (dynamic) instead of the static
# libb1nix.a — the same way gcc already picks up libgcc_s.so. The result is an
# nsfb that imports libc from /lib/libc.so.1 via the in-kernel loader.
# B1NIX_LINK=static leaves the historical static libc fold in place.
if [ "${B1NIX_LINK:-dynamic}" != "static" ] && [ -f "$ROOT_DIR/userspace/build/$B1NIX_ARCH/libc.so.1" ]; then
  CROSS_SYSROOT_LIB="$TOOLCHAIN_BUILD_HOME/cross/$B1NIX_TRIPLET/lib"
  cp "$ROOT_DIR/userspace/build/$B1NIX_ARCH/libc.so.1" "$CROSS_SYSROOT_LIB/libc.so.1"
  ln -sf libc.so.1 "$CROSS_SYSROOT_LIB/libc.so"
fi

# libb1gui: the b1nix display-server (displayd / b1display) client library, used
# by libnsfb's "displayd" surface so NetSurf can run as a windowed, interactive
# client of the compositor. Build it and stage the archive into the sysroot.
make -C "$ROOT_DIR/userspace" B1NIX_ARCH="$B1NIX_ARCH" "build/$B1NIX_ARCH/libb1gui.a" 1>&2
cp "$ROOT_DIR/userspace/build/$B1NIX_ARCH/libb1gui.a" "$SYSROOT/lib/"

# build-openlibm skips k_exp.c/k_expf.c (its "complex.h" filter false-matches
# openlibm_complex.h) which costs us the *double* __ldexp_exp/__ldexp_expf
# helpers that e_exp/e_cosh/e_sinh need. Compile and fold them into libm.a.
OLM_SRC="$(ls -d "$ROOT_DIR"/build/openlibm-src/openlibm-*/ 2>/dev/null | head -1)"
if [ -n "$OLM_SRC" ]; then
  if [ "$B1NIX_ARCH" = "x86" ]; then OLM_ARCH=i387; else OLM_ARCH=amd64; fi
  OLM_CFLAGS="--target=$NSC_TARGET -ffreestanding -fno-builtin -fno-stack-protector
    -nostdinc -isystem $ROOT_DIR/userspace/include -I$ROOT_DIR/userspace/include
    -O2 -Db1nix -I$OLM_SRC -I${OLM_SRC}src -I${OLM_SRC}include -I${OLM_SRC}$OLM_ARCH -I${OLM_SRC}bsdsrc"
  for k in k_exp k_expf; do
    # shellcheck disable=SC2086
    $CC $OLM_CFLAGS -c "${OLM_SRC}src/$k.c" -o "$SYSROOT/lib/olm_$k.o"
  done
  "${AR:-llvm-ar}" r "$SYSROOT/lib/libm.a" "$SYSROOT/lib/olm_k_exp.o" "$SYSROOT/lib/olm_k_expf.o" 2>/dev/null
fi

# ── 1a. POSIX compat shims NetSurf references that the b1nix libc lacks ──
# scandir/fstatat/unlinkat (dir + *at file ops, used by the file fetcher and the
# filename cache), isascii, fenv stubs and scalbnl (openlibm leaves these to the
# platform, which b1nix doesn't provide). Built into libb1nixcompat.a on top of
# what b1nix DOES provide (opendir/readdir, stat/lstat, unlink/rmdir, scalbn).
COMPAT_C="$ROOT_DIR/build/netsurf-sysroot/nscompat.c"
cat >"$COMPAT_C" <<'EOF'
/* b1nix POSIX compat shims for the NetSurf port. */
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <fenv.h>
#include <math.h>

/* openlibm's exp/pow routines mask FP exceptions via fenv; b1nix has no FP
 * exception state to manage, so these are correctness-preserving no-ops. */
int fegetenv(fenv_t *e) { (void)e; return 0; }
int fesetenv(const fenv_t *e) { (void)e; return 0; }
int feholdexcept(fenv_t *e) { (void)e; return 0; }
int feupdateenv(const fenv_t *e) { (void)e; return 0; }

/* Long-double scalbn: b1nix graphics/browser code only needs double precision,
 * so route through the double implementation. */
long double scalbnl(long double x, int n) { return (long double)scalbn((double)x, n); }

/* Complex accessors: openlibm's k_exp.c also defines the (unused) complex
 * __ldexp_cexp helper, which references these. Trivial real/imag extraction. */
double creal(double _Complex z) { return __real__ z; }
double cimag(double _Complex z) { return __imag__ z; }
float crealf(float _Complex z) { return __real__ z; }
float cimagf(float _Complex z) { return __imag__ z; }
EOF
$CC --target=$NSC_TARGET -ffreestanding -fno-builtin -fno-stack-protector \
  -nostdinc -isystem "$ROOT_DIR/userspace/include" -I"$ROOT_DIR/userspace/include" \
  -O2 -Db1nix -c "$COMPAT_C" -o "$SYSROOT/lib/nscompat.o"
"${AR:-llvm-ar}" rcs "$SYSROOT/lib/libb1nixcompat.a" "$SYSROOT/lib/nscompat.o"

# ── Stage libcurl + mbedTLS so NetSurf's HTTP(S) fetcher can be enabled. The
#    b1nix curl port is built static against mbedTLS; we expose libcurl.a, the
#    curl headers and the mbedTLS archives through a libcurl.pc. ──
CURL_SRC="$(ls -d "$ROOT_DIR"/build/curl-src/$B1NIX_TRIPLET/curl-*/ 2>/dev/null | head -1)"
CURL_A="$ROOT_DIR/build/curl-b1nix/$B1NIX_TRIPLET/lib/.libs/libcurl.a"
MBED_DIR="$ROOT_DIR/build/mbedtls-b1nix/$B1NIX_TRIPLET/install"
# The b1nix curl port enables libpsl (Public Suffix List) and libidn2 (IDN),
# which in turn pulls libunistring. libcurl.a therefore references psl_*/idn2_*
# /u8_*/u32_* symbols. Stage those archives and list them in libcurl.pc in
# dependency order (psl → idn2 → unistring), otherwise the static nsfb link
# fails with undefined references on a clean rebuild. All optional: only wired
# in when the archives are present.
PSL_A="$ROOT_DIR/build/libpsl-b1nix/$B1NIX_TRIPLET/install/lib/libpsl.a"
IDN2_A="$ROOT_DIR/build/libidn2-b1nix/$B1NIX_TRIPLET/install/lib/libidn2.a"
UNISTRING_A="$ROOT_DIR/build/libunistring-b1nix/$B1NIX_TRIPLET/install/lib/libunistring.a"
CURL_EXTRA_LIBS=""
HAVE_CURL=no
if [ -f "$CURL_A" ] && [ -n "$CURL_SRC" ] && [ -d "$MBED_DIR/lib" ]; then
  cp -R "$CURL_SRC/include/curl" "$SYSROOT/include/"
  cp "$CURL_A" "$SYSROOT/lib/libcurl.a"
  cp "$MBED_DIR"/lib/libmbed*.a "$SYSROOT/lib/"
  if [ -f "$PSL_A" ]; then
    cp "$PSL_A" "$SYSROOT/lib/libpsl.a"
    CURL_EXTRA_LIBS="$CURL_EXTRA_LIBS -lpsl"
  fi
  if [ -f "$IDN2_A" ]; then
    cp "$IDN2_A" "$SYSROOT/lib/libidn2.a"
    CURL_EXTRA_LIBS="$CURL_EXTRA_LIBS -lidn2"
  fi
  if [ -f "$UNISTRING_A" ]; then
    cp "$UNISTRING_A" "$SYSROOT/lib/libunistring.a"
    CURL_EXTRA_LIBS="$CURL_EXTRA_LIBS -lunistring"
  fi
  cat >"$PKGDIR/libcurl.pc" <<EOF
prefix=$SYSROOT
exec_prefix=\${prefix}
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: libcurl
Description: libcurl for b1nix (mbedTLS)
Version: 8.20.0
Libs: -L\${libdir} -lcurl -lz -lmbedtls -lmbedx509 -lmbedcrypto$CURL_EXTRA_LIBS
Cflags: -I\${includedir} -DCURL_STATICLIB
EOF
  HAVE_CURL=yes
fi

emit_pc zlib            "-lz"             ""
# -lm is added ONCE, last, via libnsfb.pc (the framebuffer frontend always links
# libnsfb after the image libs), so libpng's floor/pow/modf still resolve and the
# archive isn't pulled twice (which would cause multiple-definition errors).
emit_pc libpng          "-lpng16 -lz"     ""
emit_pc libpng16        "-lpng16 -lz"     ""
emit_pc libwapcaplet    "-llwc"           ""
emit_pc libparserutils  "-lparserutils"   ""
emit_pc libhubbub       "-lhubbub"        "libparserutils"
emit_pc libcss          "-lcss"           "libwapcaplet libparserutils"
emit_pc libdom          "-ldom"           "libhubbub libwapcaplet libparserutils"
emit_pc libnsutils      "-lnsutils"       ""
emit_pc libnsgif        "-lnsgif"         ""
emit_pc libnsbmp        "-lnsbmp"         ""
emit_pc libnslog        "-lnslog"         ""
emit_pc libjpeg         "-ljpeg"          ""
emit_pc libwebp         "-lwebp"          ""
emit_pc libsvgtiny      "-lsvgtiny -lexpat" "libdom libwapcaplet"
emit_pc libnspsl        "-lnspsl"         ""
emit_pc librosprite     "-lrosprite"      ""
emit_pc libutf8proc     "-lutf8proc"      ""
# libjxl is C++ (built against LLVM libc++, see the libc++ build above) with circular
# static deps between jxl_dec/jxl_cms/hwy — group them and fold the LLVM C++ runtime
# statically (libc++.a + libc++abi.a + libunwind.a) so the C-linked nsfb resolves the
# std::__1 symbols with NO GCC libstdc++ code. libc++abi folds the libunwind DWARF
# unwinder for libjxl's C++ exceptions.
NS_LIBCXX="$ROOT_DIR/build/toolchain_build/$B1NIX_TRIPLET/llvm-runtimes-build/libcxx-install/lib"
NS_LLVMRT="$ROOT_DIR/build/toolchain_build/$B1NIX_TRIPLET/llvm-runtimes-build/install/lib"
emit_pc libjxl  "-Wl,--start-group -ljxl_dec -ljxl_cms -lhwy -lbrotlidec -lbrotlicommon -Wl,--end-group -Wl,-Bstatic $NS_LIBCXX/libc++.a $NS_LIBCXX/libc++abi.a $NS_LLVMRT/libunwind.a -Wl,-Bdynamic" ""
# libnsfb is always linked by the framebuffer frontend. NetSurf already adds its
# own -lm (after -lpng16), so only the compat shims go here — and they must come
# last so libm's fenv/scalbnl/creal references resolve against them.
# --allow-multiple-definition: on i686 the b1nix libc and libgcc both provide the
# 64-bit division helpers (__udivdi3/__divdi3); they're identical, so let the
# first win instead of erroring. (No such overlap on x86_64.)
# --whole-archive libnsfb: the surface backends (ram/b1nixfb/displayd) self-register
# ONLY via __attribute__((constructor)); nothing references their symbols, so plain
# archive semantics leave them out entirely — nsfb then knows zero surfaces
# ("Unknown surface `ram`"). Whole-archiving libnsfb pulls every surface object so
# its constructor lands in .init_array (which b1nix crt0 runs). -lb1gui/-lb1nixcompat
# stay outside the whole-archive (they ARE referenced, by the displayd surface).
emit_pc libnsfb         "-Wl,--allow-multiple-definition -Wl,--whole-archive -lnsfb -Wl,--no-whole-archive -lb1gui -lb1nixcompat" ""

# ── 1a0. Toolchain fix: this cross-gcc's limits.h does not chain to the sysroot
# system limits.h (it lacks the _GCC_NEXT_LIMITS_H re-include block), so POSIX
# runtime limits like PATH_MAX never get defined. Append them directly. Idempotent.
for GLIM in $(ls "$CROSSBIN"/../lib/gcc/*/*/include/limits.h 2>/dev/null); do
  if ! grep -q 'b1nix-posix-limits' "$GLIM"; then
    cat >>"$GLIM" <<'EOF'

/* b1nix-posix-limits: cross-gcc limits.h doesn't chain to the sysroot. */
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#ifndef NAME_MAX
#define NAME_MAX 255
#endif
#ifndef ARG_MAX
#define ARG_MAX 131072
#endif
#ifndef LINK_MAX
#define LINK_MAX 127
#endif
#ifndef MAX_INPUT
#define MAX_INPUT 255
#endif
#ifndef MAX_CANON
#define MAX_CANON 255
#endif
#ifndef PIPE_BUF
#define PIPE_BUF 4096
#endif
#ifndef IOV_MAX
#define IOV_MAX 1024
#endif
EOF
  fi
done

# ── 1b. Source patches for a curl-less / b1nix freestanding build ──
# NetSurf assumes libcurl is always present: content/fetch.c includes curl.h
# (which pulls <curl/curl.h>) unconditionally though it only *uses* it under
# #ifdef WITH_CURL. Guard the include to match. Idempotent.
if ! grep -q 'b1nix-no-curl' "$SRC_DIR/content/fetch.c"; then
  perl -0pi -e 's{#include "content/fetchers/curl\.h"}{#ifdef WITH_CURL /* b1nix-no-curl */\n#include "content/fetchers/curl.h"\n#endif}' \
    "$SRC_DIR/content/fetch.c"
fi

# b1nix's libc provides strcasestr/strchrnul; tell NetSurf so it does not compile
# its own (which would multiply-define against libc). The cross-gcc predefines
# __b1nix__. Idempotent append to utils/config.h.
if ! grep -q 'b1nix-have-str' "$SRC_DIR/utils/config.h"; then
  cat >>"$SRC_DIR/utils/config.h" <<'EOF'
/* b1nix-have-str: b1nix libc already implements these. */
#ifdef __b1nix__
#ifndef HAVE_STRCASESTR
#define HAVE_STRCASESTR
#endif
#ifndef HAVE_STRCHRNUL
#define HAVE_STRCHRNUL
#endif
/* b1nix can't mmap a file-backed initramfs object; use the fread() read path
 * in the file:// fetcher instead. */
#undef HAVE_MMAP
#endif
EOF
fi

# b1nix render self-test hook: when NETSURF_FB_TEST is set, the frontend drives
# the page to completion, redraws it into the (RAM) surface, and verifies the
# rendered pixels are non-blank and structured — a real, no-fake render check.
# Injected into gui.c (idempotent).
if ! grep -q 'b1nix-test-render' "$SRC_DIR/frontends/framebuffer/gui.c"; then
  cat >"$SRC_PARENT/.nsfb_testhook.c" <<'EOF'
/* b1nix-test-render: headless render+verify for the M53 smoke. */
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/time.h>
/* Pump the NetSurf scheduler once, then sleep `ms` of real time so the timed
 * fetch/reflow callbacks (scheduled on gettimeofday by the fb scheduler) become
 * due. nanosleep maps to the kernel tick sleep (SYS_SLEEP), which genuinely
 * blocks ~10ms — so a fixed sleep each tick advances wall-clock cheaply instead
 * of busy-spinning on gettimeofday. This is deliberate pacing, not a crash
 * workaround: the M29 smoke (test_time_hammer) drives gettimeofday/clock_gettime
 * 300k times in a tight loop on i686 with no fault, confirming the clock path is
 * safe under load; the sleep just avoids wasting CPU between scheduler ticks. */
static void fb_test_pump(int ms)
{
	struct timespec ts;
	schedule_run();
	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (long)(ms % 1000) * 1000000L;
	nanosleep(&ts, NULL);
}
static void framebuffer_test_run(nsfb_t *nsfb, struct browser_window *bw)
{
	int i, w = 0, h = 0, stride = 0;
	uint8_t *fbuf = NULL;
	struct rect clip;
	nsfb_bbox_t box;
	struct redraw_context ctx = {
		.interactive = false,
		.background_images = true,
		.plot = &fb_plotters
	};
	/* Marker prefix so the smoke can tell the paths apart. The on-screen
	 * surface (-f b1nix, drawing to /dev/fb0) is M53-FB. Loopback http/https are
	 * M53-WEB/M53-HTTPS. A non-loopback (public-internet) URL is M53-EXT* and is
	 * "optional": if it can't be reached (offline / restricted usernet) we emit
	 * "<mk>: unsupported" and skip, so the offline smoke stays green. */
	int loopback = (feurl != NULL && strstr(feurl, "127.0.0.1") != NULL);
	int optional = 0;
	const char *mk = "M53-NS";
	if (fename != NULL && strcmp(fename, "b1nix") == 0) {
		mk = "M53-FB";
	} else if (fename != NULL && strcmp(fename, "displayd") == 0) {
		mk = "M53-WL";
	} else if (feurl != NULL && strncmp(feurl, "https", 5) == 0) {
		mk = loopback ? "M53-HTTPS" : "M53-EXT-HTTPS";
		optional = !loopback;
	} else if (feurl != NULL && strncmp(feurl, "http", 4) == 0) {
		mk = loopback ? "M53-WEB" : "M53-EXT";
		optional = !loopback;
	}

	nsfb_get_geometry(nsfb, &w, &h, NULL);
	box.x0 = 0; box.y0 = 0; box.x1 = w; box.y1 = h;
	clip.x0 = 0; clip.y0 = 0; clip.x1 = w; clip.y1 = h;

	/* Drive the scheduler (advancing time) until the page is ready to render.
	 * Bounded (~15s of 10ms ticks) so a stuck load can't hang forever. */
	for (i = 0; i < 1500; i++) {
		fb_test_pump(10);
		if (browser_window_redraw_ready(bw))
			break;
	}
	/* A few more ticks for any trailing fetch (e.g. the image) + reflow. */
	for (i = 0; i < 50; i++)
		fb_test_pump(10);

	/* If no content attached — e.g. a network fetch raced the loopback server
	 * coming up — re-navigate and pump again. Robust against startup timing. */
	for (int attempt = 0; attempt < 3 && !browser_window_has_content(bw);
	     attempt++) {
		nsurl *rurl = NULL;
		if (feurl != NULL && nsurl_create(feurl, &rurl) == NSERROR_OK) {
			browser_window_navigate(bw, rurl, NULL, BW_NAVIGATE_HISTORY,
						NULL, NULL, NULL);
			nsurl_unref(rurl);
		}
		for (i = 0; i < 600; i++) {
			fb_test_pump(10);
			if (browser_window_redraw_ready(bw) &&
			    browser_window_has_content(bw))
				break;
		}
	}
	/* Optional (public-internet) fetch that couldn't be reached: skip cleanly
	 * so the offline/restricted smoke stays green (mirrors M32's ext probes). */
	if (optional && !browser_window_has_content(bw)) {
		fprintf(stdout, "%s: unsupported (no off-link connectivity)\n", mk);
		fprintf(stdout, "%s: done\n", mk);
		fflush(stdout);
		return;
	}
	fprintf(stdout, "%s: ok load\n", mk);

	/* Did content attach over the fetch, and what are the laid-out extents? */
	fprintf(stdout, "%s: has-content=%d\n", mk,
		browser_window_has_content(bw));
	{
		int ex = 0, ey = 0;
		browser_window_get_extents(bw, true, &ex, &ey);
		fprintf(stdout, "%s: extents=%dx%d\n", mk, ex, ey);
	}

	/* The interactive frontend lays the page out via a reformat triggered on
	 * resize; drive it explicitly here so the box tree is built at our size. */
	browser_window_reformat(bw, false, w, h);
	for (i = 0; i < 100; i++)
		fb_test_pump(10);

	nsfb_claim(nsfb, &box);
	if (browser_window_redraw(bw, 0, 0, &clip, &ctx) == true)
		fprintf(stdout, "%s: ok redraw\n", mk);
	nsfb_update(nsfb, &box);

	nsfb_get_buffer(nsfb, &fbuf, &stride);
	if (fbuf != NULL && w > 0 && h > 0 && stride > 0) {
		unsigned long nonbg = 0, rows_with_content = 0;
		unsigned long svg_px = 0, js_px = 0, jxl_px = 0;
		for (int y = 0; y < h; y++) {
			uint32_t *row = (uint32_t *)(fbuf + (size_t)y * stride);
			int rowhas = 0;
			for (int x = 0; x < w; x++) {
				uint32_t px = row[x] | 0xff000000u;
				if (px != 0xffffffffu) { nonbg++; rowhas = 1; }
				/* Solid-block detection for the file:// self-test page.
				 * Green (middle byte) is order-independent; blue vs red
				 * differ only by RGB/BGR order, so gate on a high outer
				 * channel + low others, and require many such pixels (a
				 * solid block) to rule out the logo's stray colours.
				 *   #1eb820 inline-SVG rect  -> svg_px (libsvgtiny ran)
				 *   #2040ff JS-painted bar    -> js_px  (Duktape ran)
				 *   #e000e0 magenta JXL image -> jxl_px (libjxl ran)   */
				unsigned b0 = px & 0xff, b1 = (px >> 8) & 0xff,
					 b2 = (px >> 16) & 0xff;
				if (b1 > 150 && b0 < 90 && b2 < 90)
					svg_px++;
				else if (b1 < 110 &&
					 ((b0 > 220 && b2 < 90) ||
					  (b2 > 220 && b0 < 90)))
					js_px++;
				else if (b1 < 90 && b0 > 180 && b2 > 180)
					jxl_px++;
			}
			if (rowhas)
				rows_with_content++;
		}
		fprintf(stdout, "%s: pixels nonbg=%lu rows=%lu of %dx%d\n",
			mk, nonbg, rows_with_content, w, h);
		/* A laid-out page paints many non-background pixels across many
		 * rows (text lines + the image). Blank would be nonbg==0. */
		if (nonbg > 200 && rows_with_content > 8)
			fprintf(stdout, "%s: ok render\n", mk);
		else
			fprintf(stdout, "%s: fail render-blank\n", mk);
		/* Positive feature proof on the self-test page (solid blocks only). */
		if (svg_px > 500)
			fprintf(stdout, "%s: ok svg svg-px=%lu\n", mk, svg_px);
		if (js_px > 500)
			fprintf(stdout, "%s: ok js js-px=%lu\n", mk, js_px);
		if (jxl_px > 500)
			fprintf(stdout, "%s: ok jxl jxl-px=%lu\n", mk, jxl_px);
	} else {
		fprintf(stdout, "%s: fail no-surface\n", mk);
	}
	fprintf(stdout, "%s: done\n", mk);
	fflush(stdout);
}

/* Interactive input self-test (-I): load the page, then run the real frontend
 * event loop and confirm synthesized keyboard + mouse events flow from
 * /dev/input through the libnsfb surface and fbtk into the browser.
 * (fbtk is a file-global declared earlier in gui.c.) */
static void framebuffer_input_run(nsfb_t *nsfb, struct browser_window *bw)
{
	int i, w = 0, h = 0;
	int got_move = 0, got_click = 0, got_key = 0;
	nsfb_event_t event;

	nsfb_get_geometry(nsfb, &w, &h, NULL);
	for (i = 0; i < 1500; i++) {
		fb_test_pump(10);
		if (browser_window_redraw_ready(bw))
			break;
	}
	browser_window_reformat(bw, false, w, h);
	for (i = 0; i < 50; i++)
		fb_test_pump(10);
	fprintf(stdout, "M53-INPUT: ok ready\n");
	fflush(stdout);

	/* Drain raw libnsfb events from the surface (proving the /dev/input ->
	 * libnsfb path) and also route each through fbtk so the browser reacts. The
	 * kernel injects events while we spin here. */
	unsigned long total = 0;
	for (i = 0; i < 4000 && !(got_move && got_click && got_key); i++) {
		struct timespec ts;
		while (nsfb_event(nsfb, &event, 0)) {
			total++;
			if (event.type == NSFB_EVENT_MOVE_ABSOLUTE ||
			    event.type == NSFB_EVENT_MOVE_RELATIVE) {
				got_move = 1;
			} else if (event.type == NSFB_EVENT_KEY_DOWN &&
				   event.value.keycode == NSFB_KEY_MOUSE_1) {
				got_click = 1;
			} else if (event.type == NSFB_EVENT_KEY_DOWN &&
				   event.value.keycode > 0 &&
				   event.value.keycode < 400) {
				got_key = 1;
			}
		}
		schedule_run();
		ts.tv_sec = 0;
		ts.tv_nsec = 10 * 1000000L;
		nanosleep(&ts, NULL);
	}
	fprintf(stdout, "M53-INPUT: events=%lu\n", total);

	if (got_move)
		fprintf(stdout, "M53-INPUT: ok mouse-move\n");
	if (got_click)
		fprintf(stdout, "M53-INPUT: ok mouse-click\n");
	if (got_key)
		fprintf(stdout, "M53-INPUT: ok key\n");
	fprintf(stdout, "M53-INPUT: done\n");
	fflush(stdout);
}
EOF
  GUI_C="$SRC_DIR/frontends/framebuffer/gui.c"
  # (a) global test flag, declared before process_cmdline's definition (which
  #     sets it). Anchor on the return-type line so it lands above the function.
  perl -0pi -e 's{\nstatic bool\nprocess_cmdline\(int argc, char\*\* argv\)}{\nstatic int fb_b1nix_test = 0; /* b1nix render self-test (-T) */\nstatic int fb_b1nix_input = 0; /* b1nix input self-test (-I) */\n\nstatic bool\nprocess_cmdline(int argc, char** argv)}' "$GUI_C"
  # (b) accept -T (no SYS_SPAWN envp on b1nix, so use a flag not an env var).
  perl -0pi -e 's{getopt_long\(argc, argv, "f:b:w:h:"}{getopt_long(argc, argv, "f:b:w:h:TI"}' "$GUI_C"
  perl -0pi -e 's{\n\t\tdefault:\n\t\t\tfprintf\(stderr,\n\t\t\t\t"Usage:}{\n\t\tcase '"'"'T'"'"':\n\t\t\tfb_b1nix_test = 1;\n\t\t\tbreak;\n\n\t\tcase '"'"'I'"'"':\n\t\t\tfb_b1nix_input = 1;\n\t\t\tbreak;\n\n\t\tdefault:\n\t\t\tfprintf(stderr,\n\t\t\t\t"Usage:}' "$GUI_C"
  # (c) the test function body, before framebuffer_run().
  awk -v hook="$SRC_PARENT/.nsfb_testhook.c" '
    /framebuffer_run\(void\)/ && !done {
      while ((getline line < hook) > 0) print line
      close(hook); done = 1
    }
    { print }
  ' "$GUI_C" > "$GUI_C.tmp" && mv "$GUI_C.tmp" "$GUI_C"
  # (d) dispatch on the flag in main().
  perl -0pi -e 's{\t\tframebuffer_run\(\);}{\t\tif (fb_b1nix_input)\n\t\t\tframebuffer_input_run(nsfb, bw);\n\t\telse if (fb_b1nix_test)\n\t\t\tframebuffer_test_run(nsfb, bw);\n\t\telse\n\t\t\tframebuffer_run();}' "$GUI_C"
fi

# Enable JavaScript at runtime. Duktape is compiled in (NETSURF_USE_DUKTAPE=YES)
# but the core default for the enable_javascript option is false, so scripts
# never run unless we flip it. Inject into the framebuffer set_defaults() hook.
# Idempotent (guarded on the marker comment).
GUI_C="$SRC_DIR/frontends/framebuffer/gui.c"
if ! grep -q 'b1nix-enable-js' "$GUI_C"; then
  perl -0pi -e 's{(static nserror set_defaults\(struct nsoption_s \*defaults\)\n\{\n)}{$1\tnsoption_set_bool(enable_javascript, true); /* b1nix-enable-js */\n}' "$GUI_C"
fi

# Implement the framebuffer path plotter. Upstream's framebuffer_plot_path() is
# a no-op stub ("path unimplemented"), so SVG images — which render entirely via
# plot->path — decode but paint nothing. Replace the stub with a real plotter
# that fills each subpath as a polygon and strokes its edges via libnsfb (cubic
# Beziers are flattened to short line segments). Idempotent (marker guarded).
FB_C="$SRC_DIR/frontends/framebuffer/framebuffer.c"
if ! grep -q 'b1nix-plot-path' "$FB_C"; then
  cat >"$SRC_PARENT/.fb_plot_path.c" <<'B1NIX_FB_PATH_EOF'
/* b1nix-plot-path: real framebuffer path plotter (upstream stub is a no-op).
 * Map a path-space point through the 2x3 transform into device space (matches
 * the cairo/gtk frontend's CTM convention). */
#define FB_PATH_TX(px, py) \
	((int)((transform[0] * (px)) + (transform[2] * (py)) + transform[4]))
#define FB_PATH_TY(px, py) \
	((int)((transform[1] * (px)) + (transform[3] * (py)) + transform[5]))

static nserror
framebuffer_plot_path(const struct redraw_context *ctx,
		const plot_style_t *pstyle,
		const float *p,
		unsigned int n,
		const float transform[6])
{
	/* Path of cubic Beziers / lines from libsvgtiny (and CSS borders).
	 * Build each subpath into a device-space point list, fill it as a
	 * polygon and stroke its edges. Beziers are flattened to short lines.
	 * Bounded point buffer; pathological paths are clipped, not unbounded. */
	enum { FB_PATH_MAX_PTS = 1024, FB_BEZIER_STEPS = 16 };
	static int pts[FB_PATH_MAX_PTS * 2];
	unsigned int np = 0;        /* points in the current subpath (pairs) */
	float cx = 0, cy = 0;       /* current point, path space */
	unsigned int i = 0;

	if (p == NULL || n == 0 || p[0] != PLOTTER_PATH_MOVE) {
		return NSERROR_INVALID;
	}

	nsfb_plot_pen_t pen;
	pen.stroke_type = NFSB_PLOT_OPTYPE_SOLID;
	pen.stroke_width = pstyle->stroke_width > 0 ? pstyle->stroke_width : 1;
	pen.stroke_colour = pstyle->stroke_colour;
	pen.fill_type = NFSB_PLOT_OPTYPE_NONE;
	pen.fill_colour = pstyle->fill_colour;

	while (i < n) {
		if (p[i] == PLOTTER_PATH_MOVE || p[i] == PLOTTER_PATH_LINE) {
			cx = p[i + 1];
			cy = p[i + 2];
			if (np < FB_PATH_MAX_PTS) {
				pts[np * 2] = FB_PATH_TX(cx, cy);
				pts[np * 2 + 1] = FB_PATH_TY(cx, cy);
				np++;
			}
			i += 3;
		} else if (p[i] == PLOTTER_PATH_BEZIER) {
			float x0 = cx, y0 = cy;
			float x1 = p[i + 1], y1 = p[i + 2];
			float x2 = p[i + 3], y2 = p[i + 4];
			float x3 = p[i + 5], y3 = p[i + 6];
			for (int s = 1; s <= FB_BEZIER_STEPS; s++) {
				float t = (float)s / FB_BEZIER_STEPS;
				float mt = 1.0f - t;
				float a = mt * mt * mt, b = 3 * mt * mt * t;
				float c = 3 * mt * t * t, d = t * t * t;
				float bx = a * x0 + b * x1 + c * x2 + d * x3;
				float by = a * y0 + b * y1 + c * y2 + d * y3;
				if (np < FB_PATH_MAX_PTS) {
					pts[np * 2] = FB_PATH_TX(bx, by);
					pts[np * 2 + 1] = FB_PATH_TY(bx, by);
					np++;
				}
			}
			cx = x3;
			cy = y3;
			i += 7;
		} else if (p[i] == PLOTTER_PATH_CLOSE) {
			if (np > 1) {
				if (pstyle->fill_colour != NS_TRANSPARENT) {
					nsfb_plot_polygon(nsfb, pts, np,
							  pstyle->fill_colour);
				}
				if (pstyle->stroke_colour != NS_TRANSPARENT) {
					for (unsigned int k = 0; k + 1 < np; k++) {
						nsfb_bbox_t ln = {
							pts[k * 2], pts[k * 2 + 1],
							pts[(k + 1) * 2],
							pts[(k + 1) * 2 + 1]
						};
						nsfb_plot_line(nsfb, &ln, &pen);
					}
				}
			}
			np = 0;
			i += 1;
		} else {
			NSLOG(netsurf, INFO, "bad path command %f", p[i]);
			return NSERROR_INVALID;
		}
	}

	/* Flush a final unclosed subpath (fill is implicitly closed). */
	if (np > 1 && pstyle->fill_colour != NS_TRANSPARENT) {
		nsfb_plot_polygon(nsfb, pts, np, pstyle->fill_colour);
	}

	return NSERROR_OK;
}

#undef FB_PATH_TX
#undef FB_PATH_TY
B1NIX_FB_PATH_EOF
  perl -0777 -i -pe '
    BEGIN { local $/; open my $fh, "<", "'"$SRC_PARENT"'/.fb_plot_path.c" or die; our $repl = <$fh>; }
    s{static nserror\nframebuffer_plot_path\(.*?\n\}\n}{$repl}s;
  ' "$FB_C"
fi

# Throttle the Duktape uncaught-JS-error log. Modern sites ship ES2020+ minified
# bundles that Duktape (ES5.1-class) cannot parse, so each script raises an
# uncaught error and dozens flood the console per page. Cap to the first few +
# one explanatory line. Idempotent (marker guarded).
DUKKY_C="$SRC_DIR/content/handlers/javascript/duktape/dukky.c"
if [ -f "$DUKKY_C" ] && ! grep -q 'b1nix-jserr-throttle' "$DUKKY_C"; then
  perl -0pi -e 's{(\tduk_dup_top\(ctx\);\n\t/\* \.\.\., errobj, errobj \*/\n)\tNSLOG\(jserrors, WARNING, "Uncaught error in JS: %s", duk_safe_to_stacktrace\(ctx, -1\)\);}{\tstatic unsigned b1nix_jserr_count = 0; /* b1nix-jserr-throttle */\n$1\tif (b1nix_jserr_count < 8) {\n\t\tNSLOG(jserrors, WARNING, "Uncaught error in JS: %s", duk_safe_to_stacktrace(ctx, -1));\n\t} else if (b1nix_jserr_count == 8) {\n\t\tNSLOG(jserrors, WARNING, "Further uncaught JS errors suppressed (Duktape is ES5-class; modern ES2020+ sites won'"'"'t run)");\n\t}\n\tb1nix_jserr_count++;}' "$DUKKY_C"
fi

# Map the .jxl extension to image/jxl in the framebuffer file:// fetcher (it
# knows png/jpg/svg/... but not jxl), so a local JPEG-XL <img> reaches the
# libjxl content handler. Idempotent (marker guarded).
FBFETCH_C="$SRC_DIR/frontends/framebuffer/fetch.c"
if [ -f "$FBFETCH_C" ] && ! grep -q 'b1nix-jxl-mime' "$FBFETCH_C"; then
  perl -0pi -e 's{(\n\treturn "text/html";)}{\n\tif (2 < l \&\& strcasecmp(unix_path + l - 3, "jxl") == 0)\n\t\treturn "image/jxl"; /* b1nix-jxl-mime */$1}' "$FBFETCH_C"
fi

# ── 1c. nsgenbind host tool — generates the Duktape<->DOM JS bindings ──
# Needed when NETSURF_USE_DUKTAPE=YES: content/handlers/javascript/duktape's
# Makefile invokes `nsgenbind` from PATH. Build it (host tool) from the
# source-full bundle into the sysroot and prepend $SYSROOT/bin to PATH.
NSALL_DIR="$SRC_PARENT/netsurf-all-${NS_VERSION}"
if [ ! -x "$SYSROOT/bin/nsgenbind" ] && [ -d "$NSALL_DIR/nsgenbind" ]; then
  PATH_WITH_BISON="$PATH"
  if [ -d "/opt/homebrew/opt/bison/bin" ]; then
    PATH_WITH_BISON="/opt/homebrew/opt/bison/bin:$PATH_WITH_BISON"
  fi
  PATH="$PATH_WITH_BISON" make -C "$NSALL_DIR/nsgenbind" PREFIX="$SYSROOT" \
    NSSHARED="$NSALL_DIR/buildsystem" install 1>&2
fi
export PATH="$SYSROOT/bin:$PATH"

# ── 2. Drive the NetSurf framebuffer build ──
# Use the b1nix cross-gcc via the GCCSDK convention. On: image codecs PNG/BMP/
# GIF/JPEG/WebP/SVG + RISC-OS sprite, Duktape JavaScript, the public-suffix list
# (NSPSL) and utf8proc (Unicode/IDNA). Off: OpenSSL (redundant — mbedTLS is the
# TLS backend), JPEG-XL (libjxl = C++/highway/brotli), PDF export (libharu) and
# video (GStreamer) — each needs a heavy upstream port not yet brought up.
export GCCSDK_INSTALL_ENV="$SYSROOT"
export GCCSDK_INSTALL_CROSSBIN="$CROSSBIN"
export PKG_CONFIG_LIBDIR="$PKGDIR"
export PKG_CONFIG_PATH="$PKGDIR"

HOST_LIBPNG_CFLAGS=""
HOST_LIBPNG_LDFLAGS=""
if [ "$(uname -s)" = "Darwin" ]; then
  if PKG_CONFIG_LIBDIR="" PKG_CONFIG_PATH="" pkg-config --exists libpng 2>/dev/null; then
    HOST_LIBPNG_CFLAGS="$(PKG_CONFIG_LIBDIR="" PKG_CONFIG_PATH="" pkg-config --cflags libpng)"
    HOST_LIBPNG_LDFLAGS="$(PKG_CONFIG_LIBDIR="" PKG_CONFIG_PATH="" pkg-config --libs libpng)"
  else
    HOST_LIBPNG_CFLAGS="-I/opt/homebrew/opt/libpng/include"
    HOST_LIBPNG_LDFLAGS="-L/opt/homebrew/opt/libpng/lib -lpng"
  fi
else
  # Linux/other: detect host libpng BEFORE PKG_CONFIG_LIBDIR is set to cross sysroot.
  # NetSurf's tools/Makefile hardcodes -lpng which fails on Linux (libpng16).
  # Also add -L/usr/lib to ensure the host libpng is found before the cross sysroot's
  # static libpng16.a (which would cause TLS mismatches with the host libc).
  if pkg-config --exists libpng 2>/dev/null; then
    HOST_LIBPNG_CFLAGS="$(pkg-config --cflags libpng)"
    HOST_LIBPNG_LDFLAGS="-L/usr/lib $(pkg-config --libs libpng)"
  elif pkg-config --exists libpng16 2>/dev/null; then
    HOST_LIBPNG_CFLAGS="$(pkg-config --cflags libpng16)"
    HOST_LIBPNG_LDFLAGS="-L/usr/lib $(pkg-config --libs libpng16)"
  else
    HOST_LIBPNG_LDFLAGS="-L/usr/lib -lpng16 -lz"
  fi
fi

# HOST only names NetSurf's per-build OBJROOT (build/$(HOST)-$(TARGET)); the
# actual target compiler comes from GCCSDK_INSTALL_CROSSBIN. Key it by arch so
# the x86 and x86_64 builds don't share (and overwrite) each other's objects.
# -static-libgcc: fold the libgcc integer/soft-float builtins (__udivmodti4 etc.)
# statically so nsfb carries NO DT_NEEDED libgcc_s.so. This is only safe because
# libjxl's C++ runtime is now forced STATIC (-Wl,-Bstatic -lstdc++, see emit_pc
# libjxl below) — with a shared libstdc++.so DSO in the link, ld rejected the
# hidden static __udivmodti4 it referenced. With both the C++ stdlib and libgcc
# folded, nsfb's only DT_NEEDED is libc.so.1, letting the ISO drop libgcc_s.so.
# --defsym __dso_handle=0: the clang-built libc++ libjxl references __dso_handle
# (the __cxa_atexit DSO token for static-object destructors), which b1nix's cross-gcc
# crtbegin does not provide on this newlib target for a C-driver link. 0 = "the main
# program", the correct token for a non-PIE executable.
export CFLAGS="${CFLAGS:-} -fPIC"
export CXXFLAGS="${CXXFLAGS:-} -fPIC"
export LDFLAGS="${LDFLAGS:-} -static-libgcc -Wl,--defsym=__dso_handle=0 -Wl,-z,notext"
# nsfb folds libc++/libjxl and depends on C++ exceptions (libjxl decode throws on
# its slow paths). The cross driver's default linker.ld drops .eh_frame /
# .eh_frame_hdr / .gcc_except_table, so unwinding is dead and JXL/SVG/JS decode
# silently yields nothing. Link with linker-libcxx.ld (KEEPs the unwind sections
# + the .ctors.* static-init glob) and emit PT_GNU_EH_FRAME via --eh-frame-hdr.
export B1NIX_LINKER="$ROOT_DIR/userspace/linker-libcxx.ld"
export B1NIX_LD_EXTRA="--eh-frame-hdr"
make -C "$SRC_DIR" \
  TARGET=framebuffer \
  HOST="$B1NIX_GCC_ARCH" \
  BUILD_LIBPNG_CFLAGS="$HOST_LIBPNG_CFLAGS" \
  BUILD_LIBPNG_LDFLAGS="$HOST_LIBPNG_LDFLAGS" \
  NETSURF_FB_FONTLIB=internal \
  NETSURF_USE_CURL="$([ "$HAVE_CURL" = yes ] && echo YES || echo NO)" \
  NETSURF_USE_OPENSSL=NO \
  NETSURF_USE_UTF8PROC=YES \
  NETSURF_USE_LIBICONV_PLUG=YES \
  NETSURF_USE_JPEG=YES \
  NETSURF_USE_JPEGXL=YES \
  NETSURF_USE_WEBP=YES \
  NETSURF_USE_PNG=YES \
  NETSURF_USE_BMP=YES \
  NETSURF_USE_GIF=YES \
  NETSURF_USE_NSSVG=YES \
  NETSURF_USE_ROSPRITE=YES \
  NETSURF_USE_NSPSL=YES \
  NETSURF_USE_NSLOG=YES \
  NETSURF_USE_DUKTAPE=YES \
  NETSURF_USE_VIDEO=NO \
  NETSURF_USE_HARU_PDF=NO \
  "$@"

# Save the per-arch binary (NetSurf always writes ./nsfb in the source tree).
OUT_DIR="$ROOT_DIR/build/netsurf-fb-b1nix/$B1NIX_TRIPLET"
mkdir -p "$OUT_DIR"
cp "$SRC_DIR/nsfb" "$OUT_DIR/nsfb"
# Strip debug info (5.6MB -> ~2MB) so the binary is reasonable to embed.
STRIP_BIN="$CROSSBIN/$B1NIX_TRIPLET-strip"
[ -x "$STRIP_BIN" ] && "$STRIP_BIN" "$OUT_DIR/nsfb" 2>/dev/null || true
echo "netsurf framebuffer browser built for $B1NIX_TRIPLET → $OUT_DIR/nsfb" 1>&2
echo "$OUT_DIR/nsfb"
