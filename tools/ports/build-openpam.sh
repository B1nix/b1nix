#!/bin/sh
# tools/ports/build-openpam.sh
#
# Build OpenPAM (libpam.so) plus b1nix's own pam_unix.so service module for
# the b1nix/musl userspace ABI.
#
# OpenPAM ships no `configure` script — autogen.sh must run
# autoconf/automake/libtool on the BUILD host to generate one, and even once
# generated, autoconf's runtime feature probes can't execute a cross-target
# binary, and libtool's shared-library link recipe has no case for an
# "x86_64-b1nix"-shaped host_os (every other autotools port in this tree
# sidesteps that exact problem with --disable-shared --enable-static; OpenPAM
# can't, because the whole point of PAM is dlopen()'d modules).
#
# So this port bypasses OpenPAM's build system entirely, the same way
# tools/ports/drivers/cport.sh bypasses upstream Makefiles for zlib/libpng/
# freetype/etc: fetch the source, drop in a hand-written config.h (OpenPAM's
# config.h.in feature checks are all things musl provides — see the comment
# block in the generated config.h below), compile the real libpam_la_SOURCES
# list from lib/libpam/Makefile.am (NOT every *.c in that directory — several
# files there, e.g. pam_authenticate_secondary.c and the pam_sm_*.c XSSO
# stubs, are upstream EXTRA_DIST and are not meant to be built; they don't
# even include openpam_impl.h), and link libpam.so + pam_unix.so directly
# with ld.lld — the same "-shared -z norelro --hash-style=gnu -soname ..."
# recipe userspace/Makefile already uses for userspace/bin/m69_plugin.so.
#
# Usage:
#   tools/ports/build-openpam.sh          # build + print install dir
#
# Output layout (INSTALL_DIR, printed on stdout):
#   include/security/*.h        OpenPAM's public headers, unmodified upstream
#   lib/libpam.so.2              the PAM library (SONAME libpam.so.2)
#   lib/libpam.so                unversioned symlink, for -lpam-style linking
#   lib/security/pam_unix.so     b1nix's pam_unix module (authenticates via
#                                 /etc/shadow + musl crypt(3), same check
#                                 BusyBox's su/passwd applets perform)
#   etc/pam.d/sshd                default policy staged into the rootfs by
#                                 `make install-headers-libs` (see below)
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"

OPENPAM_VERSION="${OPENPAM_VERSION:-20250531}"
OPENPAM_URL="https://git.des.dev/OpenPAM/OpenPAM/archive/openpam-${OPENPAM_VERSION}.tar.gz"
OPENPAM_TARBALL="openpam-${OPENPAM_VERSION}.tar.gz"

. "$ROOT_DIR/tools/ports/drivers/common.sh"

CC="$ROOT_DIR/tools/toolchain/bin/b1nix-musl-autotools-cc"
LD="$(command -v ld.lld 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/ld.lld)"
AR_BIN="$(port_ar)"

SRC_PARENT="$ROOT_DIR/build/src/openpam"
SRC_DIR="$SRC_PARENT/openpam"
BUILD_DIR="$ROOT_DIR/build/$B1NIX_ARCH/ports/openpam"
OBJ_DIR="$BUILD_DIR/obj"
INSTALL_DIR="$BUILD_DIR/install"
LOCKFILE="$BUILD_DIR/locks/build.lock"
mkdir -p "$SRC_PARENT" "$OBJ_DIR" "$INSTALL_DIR" "$(dirname "$LOCKFILE")"

# musl's real, fully-populated libc.so (the installed usr/lib/libc.so is a
# stub) — same resolution b1nix-musl-autotools-cc itself uses.
MUSL_LIBSO="$ROOT_DIR/build/src/musl/$B1NIX_TRIPLET/musl-1.2.5/lib/libc.so"

# The real libpam_la_SOURCES list from upstream lib/libpam/Makefile.am.
# Deliberately excludes EXTRA_DIST-only files (pam_authenticate_secondary.c,
# pam_get_mapped_*.c, pam_set_mapped_*.c, pam_sm_*.c) — those are unused XSSO
# stubs that don't even include openpam_impl.h and fail to compile as-is;
# upstream itself never builds them into libpam.
OPENPAM_SOURCES="openpam_asprintf openpam_borrow_cred openpam_check_owner_perms
openpam_configure openpam_constants openpam_dispatch openpam_dynamic
openpam_features openpam_findenv openpam_free_data openpam_free_envlist
openpam_get_feature openpam_get_option openpam_load openpam_log
openpam_nullconv openpam_readline openpam_readlinev openpam_readword
openpam_restore_cred openpam_set_option openpam_set_feature openpam_static
openpam_straddch openpam_strlcat openpam_strlcpy openpam_strlset
openpam_subst openpam_vasprintf openpam_ttyconv pam_acct_mgmt
pam_authenticate pam_chauthtok pam_close_session pam_end pam_error
pam_get_authtok pam_get_data pam_get_item pam_get_user pam_getenv
pam_getenvlist pam_info pam_open_session pam_prompt pam_putenv pam_set_data
pam_set_item pam_setcred pam_setenv pam_start pam_strerror pam_verror
pam_vinfo pam_vprompt"

(
  flock -x 9
  if [ -f "$INSTALL_DIR/lib/libpam.so" ] && [ -f "$INSTALL_DIR/lib/security/pam_unix.so" ]; then
    echo "$INSTALL_DIR"
    exit 0
  fi

  : > "$BUILD_DIR/build.log"
  exec 2>>"$BUILD_DIR/build.log"

  # --- fetch + extract -------------------------------------------------------
  if [ ! -d "$SRC_DIR" ]; then
    tmp="$SRC_PARENT/$OPENPAM_TARBALL"
    port_fetch_tarball "$OPENPAM_URL" "$tmp" "$SRC_PARENT" "$SRC_DIR/lib/libpam/pam_start.c"
    if [ ! -d "$SRC_DIR" ]; then
      for d in "$SRC_PARENT"/*; do
        if [ -d "$d" ] && [ -f "$d/lib/libpam/pam_start.c" ]; then
          mv "$d" "$SRC_DIR"; break
        fi
      done
    fi
  fi

  # --- ensure musl is built ---------------------------------------------------
  if [ ! -f "$(port_musl_lib)/libc.so" ]; then
    B1NIX_ARCH="$B1NIX_ARCH" sh "$ROOT_DIR/tools/ports/build-musl.sh" 1>&2
  fi

  # --- generate config.h -------------------------------------------------------
  mkdir -p "$BUILD_DIR/gen"
  cp "$ROOT_DIR/tools/ports/openpam-config.h" "$BUILD_DIR/gen/config.h"

  # --- compile lib/libpam -----------------------------------------------------
  OBJS=""
  for _s in $OPENPAM_SOURCES; do
    _obj="$OBJ_DIR/$_s.o"
    "$CC" -fPIC -D_GNU_SOURCE -DHAVE_CONFIG_H \
      -I"$BUILD_DIR/gen" \
      -I"$SRC_DIR/include" -I"$SRC_DIR/include/security" -I"$SRC_DIR/lib/libpam" \
      -c "$SRC_DIR/lib/libpam/$_s.c" -o "$_obj"
    OBJS="$OBJS $_obj"
  done

  # --- link libpam.so ----------------------------------------------------------
  mkdir -p "$INSTALL_DIR/lib" "$INSTALL_DIR/lib/security" "$INSTALL_DIR/include/security"
  # shellcheck disable=SC2086
  "$LD" -shared -z norelro --hash-style=gnu -soname libpam.so.2 \
    -o "$INSTALL_DIR/lib/libpam.so.2" $OBJS \
    -L"$(dirname "$MUSL_LIBSO")" -l:libc.so
  ln -sf libpam.so.2 "$INSTALL_DIR/lib/libpam.so"

  # --- headers -------------------------------------------------------------
  cp "$SRC_DIR"/include/security/*.h "$INSTALL_DIR/include/security/"

  # --- pam_unix.so service module --------------------------------------------
  "$CC" -fPIC -D_GNU_SOURCE -I"$SRC_DIR/include" \
    -c "$ROOT_DIR/tools/ports/openpam-pam_unix.c" -o "$OBJ_DIR/pam_unix.o"
  "$LD" -shared -z norelro --hash-style=gnu -soname pam_unix.so \
    -o "$INSTALL_DIR/lib/security/pam_unix.so" "$OBJ_DIR/pam_unix.o" \
    -L"$INSTALL_DIR/lib" -l:libpam.so.2 \
    -L"$(dirname "$MUSL_LIBSO")" -l:libc.so

  # --- pam.d policy files ------------------------------------------------------
  mkdir -p "$INSTALL_DIR/etc/pam.d"
  cat > "$INSTALL_DIR/etc/pam.d/sshd" <<'EOF'
# b1nix PAM policy for dropbear sshd — pam_unix.so authenticates against
# /etc/shadow via musl crypt(3) "$6$" (same check su(1) performs directly).
auth       required     pam_unix.so
account    required     pam_unix.so
session    required     pam_unix.so
EOF
  cat > "$INSTALL_DIR/etc/pam.d/other" <<'EOF'
# Default policy for services without a specific /etc/pam.d/<service> file.
auth       required     pam_unix.so
account    required     pam_unix.so
session    required     pam_unix.so
EOF
  cat > "$INSTALL_DIR/etc/pam.d/m104-pam-smoke" <<'EOF'
# M104 smoke test policy (userspace/bin/m104_pam_smoke.c) — same policy as
# sshd, kept as its own file rather than relying on the "other" fallback so
# the test still runs unchanged if sshd's policy is edited later.
auth       required     pam_unix.so
account    required     pam_unix.so
session    required     pam_unix.so
EOF

  find "$INSTALL_DIR" -type f | sed "s|^$INSTALL_DIR/||" > "$BUILD_DIR/pkg.manifest"
  touch "$BUILD_DIR/build.stamp"
  echo "$INSTALL_DIR"
) 9>"$LOCKFILE"
_status=$?
if [ "$_status" != 0 ]; then
  echo "build-openpam.sh: FAILED — tail of $BUILD_DIR/build.log:" >&2
  tail -40 "$BUILD_DIR/build.log" 2>/dev/null >&2
fi
exit "$_status"
