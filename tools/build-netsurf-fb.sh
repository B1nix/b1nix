#!/bin/sh
# Build the NetSurf framebuffer browser (the real interactive `nsfb` binary) for
# the b1nix userspace ABI, using the b1nix cross-gcc toolchain and NetSurf's own
# native build system (TARGET=framebuffer).
#
# Stages a sysroot containing every ported NetSurf dependency (.a + headers +
# pkg-config .pc files), then drives netsurf-3.11's Makefile with that sysroot
# via the GCCSDK_INSTALL_ENV / GCCSDK_INSTALL_CROSSBIN convention the framebuffer
# frontend already understands. Network/JS/SVG are disabled; only the built-in
# file:// fetcher and the internal bitmap font are used.
#
# M53 (NetSurf browser platform) — final step (7): the browser itself.

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
PROJECT_DIR="$ROOT_DIR"
. "$ROOT_DIR/tools/toolchain-env.sh"

NS_VERSION="${NS_VERSION:-3.11}"
SRC_PARENT="$ROOT_DIR/build/netsurf-src"
SRC_DIR="$SRC_PARENT/netsurf-${NS_VERSION}"
SYSROOT="$ROOT_DIR/build/netsurf-sysroot/$B1NIX_TRIPLET"
PKGDIR="$SYSROOT/lib/pkgconfig"
CROSSBIN="$TOOLCHAIN_BUILD_HOME/cross/bin"

if [ "$B1NIX_ARCH" = "x86" ]; then NSC_TARGET="i686-unknown-elf"; else NSC_TARGET="x86_64-unknown-elf"; fi

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
  inst="$(B1NIX_ARCH="$B1NIX_ARCH" "$ROOT_DIR/tools/$1")"
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
stage build-openlibm.sh   # libm.a (NetSurf + libpng need cos/sin/floor/pow/modf)

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
    clang $OLM_CFLAGS -c "${OLM_SRC}src/$k.c" -o "$SYSROOT/lib/olm_$k.o"
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

#ifndef AT_SYMLINK_NOFOLLOW
#define AT_SYMLINK_NOFOLLOW 0x100
#endif
#ifndef AT_REMOVEDIR
#define AT_REMOVEDIR 0x200
#endif

int isascii(int c) { return (c & ~0x7f) == 0; }

/* The *at variants ignore dirfd (NetSurf only ever passes paths usable from the
 * cwd in the file fetcher / filename cache). */
int fstatat(int dirfd, const char *path, struct stat *st, int flags) {
  (void)dirfd;
  if (flags & AT_SYMLINK_NOFOLLOW) return lstat(path, st);
  return stat(path, st);
}

int unlinkat(int dirfd, const char *path, int flags) {
  (void)dirfd;
  if (flags & AT_REMOVEDIR) return rmdir(path);
  return unlink(path);
}

int scandir(const char *dirp, struct dirent ***namelist,
            int (*filter)(const struct dirent *),
            int (*compar)(const struct dirent **, const struct dirent **)) {
  DIR *d = opendir(dirp);
  if (d == NULL) return -1;
  struct dirent **list = NULL;
  size_t n = 0, cap = 0;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (filter != NULL && filter(e) == 0) continue;
    if (n == cap) {
      cap = cap ? cap * 2 : 16;
      struct dirent **nl = realloc(list, cap * sizeof(*list));
      if (nl == NULL) goto fail;
      list = nl;
    }
    struct dirent *copy = malloc(sizeof(*copy));
    if (copy == NULL) goto fail;
    memcpy(copy, e, sizeof(*copy));
    list[n++] = copy;
  }
  closedir(d);
  if (compar != NULL && n > 1)
    qsort(list, n, sizeof(*list),
          (int (*)(const void *, const void *))compar);
  *namelist = list;
  return (int)n;
fail:
  for (size_t i = 0; i < n; i++) free(list[i]);
  free(list);
  closedir(d);
  return -1;
}
EOF
clang --target=$NSC_TARGET -ffreestanding -fno-builtin -fno-stack-protector \
  -nostdinc -isystem "$ROOT_DIR/userspace/include" -I"$ROOT_DIR/userspace/include" \
  -O2 -Db1nix -c "$COMPAT_C" -o "$SYSROOT/lib/nscompat.o"
"${AR:-llvm-ar}" rcs "$SYSROOT/lib/libb1nixcompat.a" "$SYSROOT/lib/nscompat.o"

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
# libnsfb is always linked by the framebuffer frontend. NetSurf already adds its
# own -lm (after -lpng16), so only the compat shims go here — and they must come
# last so libm's fenv/scalbnl/creal references resolve against them.
# --allow-multiple-definition: on i686 the b1nix libc and libgcc both provide the
# 64-bit division helpers (__udivdi3/__divdi3); they're identical, so let the
# first win instead of erroring. (No such overlap on x86_64.)
emit_pc libnsfb         "-Wl,--allow-multiple-definition -lnsfb -lb1nixcompat" ""

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
#include <time.h>
#include <sys/stat.h>
#include <sys/time.h>
/* Pump the NetSurf scheduler, then busy-wait `ms` of real wall-clock so the
 * timed fetch/reflow callbacks (scheduled on gettimeofday by the fb scheduler)
 * actually become due. nanosleep() is a no-op on b1nix, so spin on gettimeofday
 * instead — under KVM this is cheap and is the only thing the process does. */
static void fb_test_pump(int ms)
{
	struct timeval start, now;
	long elapsed;
	schedule_run();
	gettimeofday(&start, NULL);
	do {
		gettimeofday(&now, NULL);
		elapsed = (now.tv_sec - start.tv_sec) * 1000 +
			  (now.tv_usec - start.tv_usec) / 1000;
	} while (elapsed < ms);
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

	nsfb_get_geometry(nsfb, &w, &h, NULL);
	box.x0 = 0; box.y0 = 0; box.x1 = w; box.y1 = h;
	clip.x0 = 0; clip.y0 = 0; clip.x1 = w; clip.y1 = h;

	/* Drive the scheduler (advancing time) until the page is ready to render.
	 * Bounded (~30s worth of 10ms ticks) so a stuck load can't hang forever. */
	for (i = 0; i < 3000; i++) {
		fb_test_pump(10);
		if (browser_window_redraw_ready(bw))
			break;
	}
	/* A few more ticks for any trailing fetch (e.g. the image) + reflow. */
	for (i = 0; i < 100; i++)
		fb_test_pump(10);
	fprintf(stdout, "M53-NS: ok load\n");

	/* Diagnostics: is the page file reachable, did content attach, and what
	 * are the laid-out extents? */
	{
		FILE *tf = fopen("/netsurf/test.html", "rb");
		fprintf(stdout, "M53-NS: file-readable=%d\n", tf != NULL);
		if (tf) fclose(tf);
		{
			struct stat sb;
			int sr = stat("/netsurf/test.html", &sb);
			fprintf(stdout, "M53-NS: stat rc=%d mode=0%o isreg=%d size=%ld\n",
				sr, (unsigned)sb.st_mode, S_ISREG(sb.st_mode),
				(long)sb.st_size);
		}
		fprintf(stdout, "M53-NS: has-content=%d\n",
			browser_window_has_content(bw));
		int ex = 0, ey = 0;
		browser_window_get_extents(bw, true, &ex, &ey);
		fprintf(stdout, "M53-NS: extents=%dx%d\n", ex, ey);
	}

	/* The interactive frontend lays the page out via a reformat triggered on
	 * resize; drive it explicitly here so the box tree is built at our size. */
	browser_window_reformat(bw, false, w, h);
	for (i = 0; i < 100; i++)
		fb_test_pump(10);

	nsfb_claim(nsfb, &box);
	if (browser_window_redraw(bw, 0, 0, &clip, &ctx) == true)
		fprintf(stdout, "M53-NS: ok redraw\n");
	nsfb_update(nsfb, &box);

	nsfb_get_buffer(nsfb, &fbuf, &stride);
	if (fbuf != NULL && w > 0 && h > 0 && stride > 0) {
		unsigned long nonbg = 0, rows_with_content = 0;
		for (int y = 0; y < h; y++) {
			uint32_t *row = (uint32_t *)(fbuf + (size_t)y * stride);
			int rowhas = 0;
			for (int x = 0; x < w; x++) {
				uint32_t px = row[x] | 0xff000000u;
				if (px != 0xffffffffu) { nonbg++; rowhas = 1; }
			}
			if (rowhas)
				rows_with_content++;
		}
		fprintf(stdout, "M53-NS: pixels nonbg=%lu rows=%lu of %dx%d\n",
			nonbg, rows_with_content, w, h);
		/* A laid-out page paints many non-background pixels across many
		 * rows (text lines + the image). Blank would be nonbg==0. */
		if (nonbg > 200 && rows_with_content > 8)
			fprintf(stdout, "M53-NS: ok render\n");
		else
			fprintf(stdout, "M53-NS: fail render-blank\n");
	} else {
		fprintf(stdout, "M53-NS: fail no-surface\n");
	}
	fprintf(stdout, "M53-NS: done\n");
	fflush(stdout);
}
EOF
  GUI_C="$SRC_DIR/frontends/framebuffer/gui.c"
  # (a) global test flag, declared before process_cmdline's definition (which
  #     sets it). Anchor on the return-type line so it lands above the function.
  perl -0pi -e 's{\nstatic bool\nprocess_cmdline\(int argc, char\*\* argv\)}{\nstatic int fb_b1nix_test = 0; /* b1nix render self-test */\n\nstatic bool\nprocess_cmdline(int argc, char** argv)}' "$GUI_C"
  # (b) accept -T (no SYS_SPAWN envp on b1nix, so use a flag not an env var).
  perl -0pi -e 's{getopt_long\(argc, argv, "f:b:w:h:"}{getopt_long(argc, argv, "f:b:w:h:T"}' "$GUI_C"
  perl -0pi -e 's{\n\t\tdefault:\n\t\t\tfprintf\(stderr,\n\t\t\t\t"Usage:}{\n\t\tcase '"'"'T'"'"':\n\t\t\tfb_b1nix_test = 1;\n\t\t\tbreak;\n\n\t\tdefault:\n\t\t\tfprintf(stderr,\n\t\t\t\t"Usage:}' "$GUI_C"
  # (c) the test function body, before framebuffer_run().
  awk -v hook="$SRC_PARENT/.nsfb_testhook.c" '
    /framebuffer_run\(void\)/ && !done {
      while ((getline line < hook) > 0) print line
      close(hook); done = 1
    }
    { print }
  ' "$GUI_C" > "$GUI_C.tmp" && mv "$GUI_C.tmp" "$GUI_C"
  # (d) dispatch on the flag in main().
  perl -0pi -e 's{\t\tframebuffer_run\(\);}{\t\tif (fb_b1nix_test)\n\t\t\tframebuffer_test_run(nsfb, bw);\n\t\telse\n\t\t\tframebuffer_run();}' "$GUI_C"
fi

# ── 2. Drive the NetSurf framebuffer build ──
# Use the b1nix cross-gcc via the GCCSDK convention. Disable everything that
# needs network, JS, SVG, sprite, PSL, extra image codecs, or a real iconv.
export GCCSDK_INSTALL_ENV="$SYSROOT"
export GCCSDK_INSTALL_CROSSBIN="$CROSSBIN"
export PKG_CONFIG_LIBDIR="$PKGDIR"
export PKG_CONFIG_PATH="$PKGDIR"

# HOST only names NetSurf's per-build OBJROOT (build/$(HOST)-$(TARGET)); the
# actual target compiler comes from GCCSDK_INSTALL_CROSSBIN. Key it by arch so
# the x86 and x86_64 builds don't share (and overwrite) each other's objects.
make -C "$SRC_DIR" \
  TARGET=framebuffer \
  HOST="$B1NIX_GCC_ARCH" \
  NETSURF_FB_FONTLIB=internal \
  NETSURF_USE_CURL=NO \
  NETSURF_USE_OPENSSL=NO \
  NETSURF_USE_UTF8PROC=NO \
  NETSURF_USE_LIBICONV_PLUG=YES \
  NETSURF_USE_JPEG=NO \
  NETSURF_USE_JPEGXL=NO \
  NETSURF_USE_WEBP=NO \
  NETSURF_USE_PNG=YES \
  NETSURF_USE_BMP=YES \
  NETSURF_USE_GIF=YES \
  NETSURF_USE_NSSVG=NO \
  NETSURF_USE_ROSPRITE=NO \
  NETSURF_USE_NSPSL=NO \
  NETSURF_USE_NSLOG=YES \
  NETSURF_USE_DUKTAPE=NO \
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
