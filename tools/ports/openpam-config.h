/* Hand-written config.h for the b1nix/musl cross-port of OpenPAM.
 * OpenPAM ships no `configure` script (autogen.sh must run autoconf/automake
 * on the build host, and autoconf's runtime feature probes cannot execute a
 * cross-target binary anyway). musl on b1nix provides every POSIX feature
 * OpenPAM's config.h.in checks for, so instead of fighting autoconf's cache
 * preseeding for a one-off unknown host triplet, the answers are just
 * written out directly here. Keep this in sync with any config.h.in delta
 * on OpenPAM upstream bumps. */
#ifndef OPENPAM_B1NIX_CONFIG_H
#define OPENPAM_B1NIX_CONFIG_H

/* autoconf convention: code tests these with #ifdef/#ifndef, never #if — so
 * a "no" answer must be a genuinely UNDEFINED macro, not `#define FOO 0`
 * (which is still defined and would flip every #ifdef the wrong way). Only
 * the true features get a #define below; false ones are left out entirely
 * and documented in comments so the "no" answer is still auditable. */

#define HAVE_ASPRINTF 1
/* HAVE_CRYB_TEST: cryb-test is an optional external unit-test harness — unused. */
#define HAVE_CRYPT_H 1
#define HAVE_DLFCN_H 1
/* HAVE_DLFUNC: BSD-only dlfunc(3); openpam_dlfunc.h supplies a dlsym()-based
 * fallback when this is undefined. */
/* HAVE_FDLOPEN: BSD-only fd-based dlopen variant; openpam_dynamic.c falls
 * back to path-based dlopen() (with its own fd-race guard) when undefined. */
/* HAVE_FPURGE: BSD-only stdio purge; openpam_ttyconv.c has a fallback. */
#define HAVE_INTTYPES_H 1
/* HAVE_LIBDL: musl folds dlopen/dlsym into libc — no separate -ldl needed. */
/* HAVE_LIBPAM: WITH_SYSTEM_LIBPAM path (build su against host PAM) — n/a here. */
/* HAVE_MINIX_CONFIG_H: not MINIX. */
#define HAVE_SETLOGMASK 1
#define HAVE_STDINT_H 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRINGS_H 1
#define HAVE_STRING_H 1
#define HAVE_STRLCAT 1
/* HAVE_STRLCMP: not a real libc function anywhere; OpenPAM always supplies it
 * itself (openpam_strlcmp.h has no HAVE_STRLCMP-guarded declaration path used
 * by any .c file that isn't openpam_strlcmp.c itself — verified below). */
#define HAVE_STRLCPY 1
/* HAVE_STRLSET: OpenPAM-only helper, never provided by libc; always built
 * from openpam_strlset.c. */
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_UNISTD_H 1
#define HAVE_VASPRINTF 1

#define LIB_MAJ 2
#define PACKAGE "openpam"
#define PACKAGE_BUGREPORT "des@des.dev"
#define PACKAGE_NAME "OpenPAM"
#define PACKAGE_STRING "OpenPAM 20250531"
#define PACKAGE_TARNAME "openpam"
#define PACKAGE_URL "https://openpam.org/"
#define PACKAGE_VERSION "20250531"
#define VERSION "20250531"

/* b1nix ships musl's crypt(3), not FreeBSD's -lcrypt; nothing extra needed. */
#define STDC_HEADERS 1

/* Where OpenPAM looks for policy modules — matches the rootfs layout staged
 * by tools/ports/build-openpam.sh (see port_install: modules go to
 * $ROOTFS/lib/security/). */
#define OPENPAM_MODULES_DIRECTORY "/lib/security"

#endif /* OPENPAM_B1NIX_CONFIG_H */
