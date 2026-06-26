#!/bin/sh
# Idempotent b1nix source fixes for BusyBox.
set -eu

SRC_DIR="${1:?usage: b1nix-config.sh <busybox-src-dir>}"
PATCH_DIR="$(cd "$(dirname "$0")" && pwd)"

if sed --version >/dev/null 2>&1; then sed_inplace() { sed -i "$@"; }
else sed_inplace() { sed -i '' "$@"; }; fi

# procps applets can use b1nix's sysinfo(2), but BusyBox gates the code on
# __linux__.
for bb_src in procps/free.c procps/uptime.c procps/ps.c procps/vmstat.c; do
  if [ -f "$SRC_DIR/$bb_src" ] && ! grep -q "__b1nix__" "$SRC_DIR/$bb_src"; then
    sed_inplace 's/#ifdef __linux__/#if defined(__linux__) || defined(__b1nix__)/' \
      "$SRC_DIR/$bb_src"
  fi
done

# BusyBox tree uses scandir/alphasort, which b1nix libc does not provide.
# Keep the metadata comments in the replacement source: BusyBox scans them to
# register the applet and generate Config.in/applets.h/kbuild rules.
if [ -f "$SRC_DIR/miscutils/tree.c" ] && ! grep -q "__b1nix__" "$SRC_DIR/miscutils/tree.c"; then
  cp "$PATCH_DIR/tree.c" "$SRC_DIR/miscutils/tree.c"
fi

# The system password scheme is libc crypt()'s "$b1$". BusyBox's builtin crypt
# dies with "bad salt", so defer only those settings to libc.
PW="$SRC_DIR/libbb/pw_encrypt.c"
if [ -f "$PW" ] && ! grep -q "__b1nix__" "$PW"; then
  python3 - "$PW" <<'PY'
import sys
path = sys.argv[1]
src = open(path).read()
anchor = "\tencrypted = my_crypt(clear, salt);"
patch = """\t/* __b1nix__: the system password scheme is the libc crypt()'s "$b1$"
\t * (b1nix /etc/shadow, dropbear, native su/passwd). The builtin crypt
\t * would die with "bad salt" on it, so defer those settings to libc. */
\tif (salt && strncmp(salt, "$b1$", 4) == 0) {
\t\textern char *crypt(const char *key, const char *setting);
\t\tencrypted = crypt(clear, salt);
\t\tif (!encrypted || !encrypted[0])
\t\t\tbb_simple_error_msg_and_die("bad salt");
\t\treturn xstrdup(encrypted);
\t}
"""
assert anchor in src, "pw_encrypt.c anchor not found"
src = src.replace(anchor, patch + anchor, 1)
open(path, "w").write(src)
PY
fi
