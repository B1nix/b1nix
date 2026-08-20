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

# update_passwd() opens the file it is about to rewrite BEFORE it takes the
# "<file>+" O_EXCL lock that serialises passwd/chpasswd/adduser/deluser against
# each other. A process that loses that race therefore holds a descriptor on the
# pre-rename inode: it waits, wins the lock, and then copies stale content over
# the winner's update — losing a password change whenever two of these applets
# run at once. The old descriptor also survives the winner's rename() because
# the winner hardlinks the old inode to "<file>-" first, so the stale read never
# even fails.
#
# Move that open to after the O_EXCL create succeeds. Holding "<file>+" is what
# guarantees nobody else is mid-rename, so the content read there is the current
# file, and the read-modify-rename cycle becomes properly serialised. The new
# failure path has to drop the lock file it just created.
UP="$SRC_DIR/libbb/update_passwd.c"
if [ -f "$UP" ] && ! grep -q "__b1nix__" "$UP"; then
  python3 - "$UP" <<'PY'
import sys
path = sys.argv[1]
src = open(path).read()

open_block = (
    '\tif (shadow)\n'
    '\t\told_fp = fopen(filename, "r+");\n'
    '\telse\n'
    '\t\told_fp = fopen_or_warn(filename, "r+");\n'
    '\tif (!old_fp) {\n'
    '\t\tif (shadow)\n'
    '\t\t\tret = 0; /* missing shadow is not an error */\n'
    '\t\tgoto free_mem;\n'
    '\t}\n'
    '\told_fd = fileno(old_fp);\n'
    '\n'
    '\tselinux_preserve_fcontext(old_fd);\n'
    '\n'
)
assert src.count(open_block) == 1, "update_passwd.c: open block not found"
src = src.replace(open_block, '', 1)

lock_tail = (
    '\tbb_perror_msg("can\'t create \'%s\'", fnamesfx);\n'
    '\tgoto close_old_fp;\n'
    '\n'
    ' created:\n'
    '\tif (fstat(old_fd, &sb) == 0) {\n'
)
assert src.count(lock_tail) == 1, "update_passwd.c: lock tail not found"
src = src.replace(lock_tail, (
    '\tbb_perror_msg("can\'t create \'%s\'", fnamesfx);\n'
    '\tgoto free_mem;\n'
    '\n'
    ' created:\n'
    '\t/* __b1nix__: open the file only now, holding "<file>+". Opening it\n'
    '\t * before the O_EXCL create above left every loser of that race\n'
    '\t * reading the inode the winner had already replaced, so the loser\n'
    '\t * wrote the winner\'s change back out of existence. Nothing else can\n'
    '\t * be renaming the file while we hold the lock, so what is read here\n'
    '\t * is the current file. */\n'
    '\tif (shadow)\n'
    '\t\told_fp = fopen(filename, "r+");\n'
    '\telse\n'
    '\t\told_fp = fopen_or_warn(filename, "r+");\n'
    '\tif (!old_fp) {\n'
    '\t\tif (shadow)\n'
    '\t\t\tret = 0; /* missing shadow is not an error */\n'
    '\t\tclose(new_fd);\n'
    '\t\tunlink(fnamesfx); /* release the lock we are abandoning */\n'
    '\t\tgoto free_mem;\n'
    '\t}\n'
    '\told_fd = fileno(old_fp);\n'
    '\n'
    '\tselinux_preserve_fcontext(old_fd);\n'
    '\n'
    '\tif (fstat(old_fd, &sb) == 0) {\n'
), 1)

# The label is now unreachable by any goto; the fclose below it is still
# reached by falling through from unlink_new.
label = '\n close_old_fp:\n\tfclose(old_fp);\n'
assert src.count(label) == 1, "update_passwd.c: close_old_fp label not found"
src = src.replace(label, '\n\tfclose(old_fp);\n', 1)

open(path, "w").write(src)
PY
fi
