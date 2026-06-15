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

# ── Stage libcurl + mbedTLS so NetSurf's HTTP(S) fetcher can be enabled. The
#    b1nix curl port is built static against mbedTLS; we expose libcurl.a, the
#    curl headers and the mbedTLS archives through a libcurl.pc. ──
CURL_SRC="$(ls -d "$ROOT_DIR"/build/curl-src/$B1NIX_TRIPLET/curl-*/ 2>/dev/null | head -1)"
CURL_A="$ROOT_DIR/build/curl-b1nix/$B1NIX_TRIPLET/lib/.libs/libcurl.a"
MBED_DIR="$ROOT_DIR/build/mbedtls-b1nix/$B1NIX_TRIPLET/install"
HAVE_CURL=no
if [ -f "$CURL_A" ] && [ -n "$CURL_SRC" ] && [ -d "$MBED_DIR/lib" ]; then
  cp -R "$CURL_SRC/include/curl" "$SYSROOT/include/"
  cp "$CURL_A" "$SYSROOT/lib/libcurl.a"
  cp "$MBED_DIR"/lib/libmbed*.a "$SYSROOT/lib/"
  cat >"$PKGDIR/libcurl.pc" <<EOF
prefix=$SYSROOT
exec_prefix=\${prefix}
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: libcurl
Description: libcurl for b1nix (mbedTLS)
Version: 8.20.0
Libs: -L\${libdir} -lcurl -lz -lmbedtls -lmbedx509 -lmbedcrypto
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
# libnsfb is always linked by the framebuffer frontend. NetSurf already adds its
# own -lm (after -lpng16), so only the compat shims go here — and they must come
# last so libm's fenv/scalbnl/creal references resolve against them.
# --allow-multiple-definition: on i686 the b1nix libc and libgcc both provide the
# 64-bit division helpers (__udivdi3/__divdi3); they're identical, so let the
# first win instead of erroring. (No such overlap on x86_64.)
emit_pc libnsfb         "-Wl,--allow-multiple-definition -lnsfb -lb1gui -lb1nixcompat" ""

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
 * blocks ~10ms — so a fixed sleep each tick advances wall-clock without spinning
 * on gettimeofday (hammering it crashed i686 intermittently). */
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
		fprintf(stdout, "%s: pixels nonbg=%lu rows=%lu of %dx%d\n",
			mk, nonbg, rows_with_content, w, h);
		/* A laid-out page paints many non-background pixels across many
		 * rows (text lines + the image). Blank would be nonbg==0. */
		if (nonbg > 200 && rows_with_content > 8)
			fprintf(stdout, "%s: ok render\n", mk);
		else
			fprintf(stdout, "%s: fail render-blank\n", mk);
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
  NETSURF_USE_CURL="$([ "$HAVE_CURL" = yes ] && echo YES || echo NO)" \
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
