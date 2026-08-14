#!/bin/sh
# Idempotent b1nix source fixes for BusyBox.
set -eu

SRC_DIR="${1:?usage: b1nix-config.sh <busybox-src-dir>}"

# Two edits used to live here and no longer do:
#
#   * procps/{free,uptime,ps,vmstat}.c had every `#ifdef __linux__` widened to
#     also accept __b1nix__. The wrapper that compiles BusyBox already passes
#     -D__linux__ (tools/toolchain/bin/b1nix-musl-autotools-cc), so the rewrite
#     never selected a single line the compiler was not already taking.
#
#   * miscutils/tree.c was replaced wholesale with a scandir-free rewrite,
#     because the old hand-written b1nix libc had no scandir/alphasort. musl
#     has both (src/dirent/{scandir,alphasort}.c, declared in <dirent.h>), so
#     upstream's tree.c builds as shipped.

# /etc/shadow is SHA-512 crypt ("$6$"), which BusyBox hashes itself
# (USE_BB_CRYPT_SHA) and which musl's crypt(3) — the PAM path, M105 — accepts
# unchanged, so su/passwd and pam_unix.so agree on every account by default.
# The legacy b1nix-native scheme "$b1$" (kernel/lib/crypt.c) is not something
# BusyBox's builtin crypt knows: it would die with "bad salt". Defer only those
# settings to libc so an old shadow line still authenticates.
PW="$SRC_DIR/libbb/pw_encrypt.c"
if [ -f "$PW" ] && ! grep -q "__b1nix__" "$PW"; then
  python3 - "$PW" <<'PY'
import sys
path = sys.argv[1]
src = open(path).read()
anchor = "\tencrypted = my_crypt(clear, salt);"
patch = """\t/* __b1nix__: /etc/shadow is "$6$" (SHA-512), which the builtin crypt
\t * below handles and pam_unix.so accepts unchanged. The legacy b1nix
\t * scheme "$b1$" only libc crypt() knows; the builtin would die with
\t * "bad salt" on it, so defer those settings to libc. */
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
