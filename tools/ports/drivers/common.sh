#!/bin/sh
# tools/ports/drivers/common.sh
#
# Shared helpers for the consolidated b1nix port build drivers. This file is
# *sourced* (never executed) by the per-category drivers in this directory
# (nslib.sh, cport.sh, autotools.sh, meson.sh, cmake.sh), which are in turn
# sourced by the per-port manifest scripts tools/ports/build-<port>.sh.
#
# Contract for the manifest scripts:
#   * set -eu
#   * define ROOT_DIR  ("$(cd "$(dirname "$0")/../.." && pwd)")
#   * set the driver's manifest variables, then source the category driver.
# The manifest's only stdout output is the final install dir printed by the
# driver; all build noise must go to stderr (helpers below honour this).
#
# Requires ROOT_DIR to be set before sourcing.

if [ -z "${ROOT_DIR:-}" ]; then
  echo "drivers/common.sh: ROOT_DIR must be set before sourcing" >&2
  exit 1
fi

# Resolve the per-architecture build identity (B1NIX_ARCH/TRIPLET/ROOTFS/...).
. "$ROOT_DIR/tools/toolchain/env.sh"

# ---------------------------------------------------------------------------
# ccache
#
# Every driver compile path runs the compiler through ccache when it is present
# on the host (b1nix CI uses /usr/bin/ccache). ccache returns the *exact* object
# the compiler would have produced, so wiring it in changes build speed only,
# never the emitted bytes. Set B1NIX_NO_CCACHE=1 to bypass (e.g. for an
# apples-to-apples equivalence diff against a pre-ccache build).
# ---------------------------------------------------------------------------
port_ccache_prefix() {
  if [ "${B1NIX_NO_CCACHE:-0}" != "1" ] && command -v ccache >/dev/null 2>&1; then
    printf 'ccache '
  fi
}

# llvm-ar is deterministic by default (zeroed timestamps/uid/gid).
port_ar() {
  printf '%s' "${AR:-$(command -v llvm-ar 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ar)}"
}

# Freestanding clang target triplet for the active arch.
port_clang_target() {
  if [ "$B1NIX_ARCH" = "x86" ]; then printf 'i686-unknown-elf'; else printf 'x86_64-unknown-elf'; fi
}

# Download + extract a source tarball once. Idempotent: skips when $4 (a marker
# path: the expected src dir, or a sentinel file inside it) already exists.
# Picks the tar decompressor from the tarball extension (.gz/.tgz vs .xz).
#   $1 url  $2 tarball-path  $3 dest-parent  $4 existence marker
port_fetch_tarball() {
  _url="$1"; _tar="$2"; _parent="$3"; _marker="$4"
  mkdir -p "$_parent"
  (
    flock -x 9
    if [ -n "$_marker" ] && [ -e "$_marker" ]; then
      : # Already extracted
    else
      if [ ! -f "$_tar" ]; then
        curl -fL "$_url" -o "$_tar" 1>&2 || wget -O "$_tar" "$_url" 1>&2
      fi
      case "$_tar" in
      *.xz)        tar -xJf "$_tar" -C "$_parent" 1>&2 || { rm -f "$_tar"; exit 1; } ;;
      *.gz|*.tgz)  tar -xzf "$_tar" -C "$_parent" 1>&2 || { rm -f "$_tar"; exit 1; } ;;
      *)           tar -xf  "$_tar" -C "$_parent" 1>&2 || { rm -f "$_tar"; exit 1; } ;;
      esac
    fi
  ) 9>"$_tar.lock"
}

# Apply patch / sed-snippet files from tools/patches/<port>/ idempotently.
# Each entry in PATCHES is a path relative to tools/patches. A *.patch entry is
# applied with `patch -p1` (guarded by --dry-run so re-application is a no-op);
# a *.sed entry is a list of `s/old/new/` style sed scripts applied in-place to
# the files named by `# files: a b c` header lines inside the snippet (see the
# snippet format in tools/patches/<port>/*.sed).
#   $1 source dir,  remaining args: patch entries (relative to tools/patches)
port_apply_patches() {
  _srcdir="$1"; shift
  for _entry in "$@"; do
    [ -n "$_entry" ] || continue
    _pf="$ROOT_DIR/tools/patches/$_entry"
    if [ ! -f "$_pf" ]; then
      echo "drivers: patch not found: $_pf" >&2
      exit 1
    fi
    case "$_entry" in
    *.patch)
      # Idempotent: only apply when the forward patch still applies cleanly.
      if patch -d "$_srcdir" -p1 --dry-run --silent <"$_pf" >/dev/null 2>&1; then
        patch -d "$_srcdir" -p1 <"$_pf" 1>&2
      fi
      ;;
    *.sh)
      # Executable patch hook: invoked as `<hook> <srcdir>`; must be idempotent.
      sh "$_pf" "$_srcdir" 1>&2
      ;;
    *)
      echo "drivers: unknown patch type: $_entry" >&2
      exit 1
      ;;
    esac
  done
}
