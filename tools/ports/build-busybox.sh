#!/usr/bin/env bash
# tools/ports/build-busybox.sh - Porting/building upstream BusyBox 1.38.0 for b1nix
set -euo pipefail

BUSYBOX_VER="1.38.0"
BUSYBOX_TARBALL="busybox-${BUSYBOX_VER}.tar.bz2"
BUSYBOX_URL="https://busybox.net/downloads/${BUSYBOX_TARBALL}"
BUSYBOX_SHA256="34f9ea6ff8636f2c9241153b9114eefa9e65674a45318ae1ef95bb5f31c53bb2"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Per-architecture build identity (B1NIX_ARCH -> triplet, per-triplet paths).
. "$PROJECT_DIR/tools/toolchain/env.sh"
TARGET="$B1NIX_TRIPLET"

OS="$(uname -s)"
if [ "$OS" = "Darwin" ]; then
    NPROC=$(sysctl -n hw.ncpu)
    sed_inplace() { sed -i '' "$@"; }
else
    NPROC=$(nproc)
    sed_inplace() { sed -i "$@"; }
fi

BUILD_HOME="$TOOLCHAIN_BUILD_HOME"
SRC_DIR="$TOOLCHAIN_SRC_DIR/busybox-${BUSYBOX_VER}"
BUILD_DIR="$PROJECT_DIR/build/busybox-b1nix/$B1NIX_TRIPLET"
CROSS_PREFIX="$BUILD_HOME/cross"
SYSROOT="$B1NIX_ROOTFS"
CONFIG_FRAGMENT="$PROJECT_DIR/tools/configs/busybox-${BUSYBOX_VER}.config"
INSTALL_DIR="${BUSYBOX_INSTALL_DIR:-$SYSROOT/opt/busybox/bin}"

if [ ! -f "$CROSS_PREFIX/bin/${TARGET}-gcc" ]; then
    echo "Error: cross-compiler not found at $CROSS_PREFIX/bin/${TARGET}-gcc"
    echo "Run tools/toolchain/build-toolchain.sh first."
    exit 1
fi

export PATH="$CROSS_PREFIX/bin:$PATH"

# ── 0. Build & install userspace libc and headers to sysroot ──
echo "Installing/updating userspace libc and headers in sysroot..."
make -C "$PROJECT_DIR/userspace" install-headers-libs

# Workaround for spaces in path (e.g. "Documents/GitHub"): build tools like
# BusyBox's make / autotools split EXTRA_*FLAGS on whitespace and don't
# tolerate --sysroot=/path/with spaces. Create a symlink at a space-free path.
SPACEFREE_SYSROOT="$TOOLCHAIN_BUILD_HOME/sysroot-$B1NIX_TRIPLET"
rm -f "$SPACEFREE_SYSROOT" 2>/dev/null || true
mkdir -p "$(dirname "$SPACEFREE_SYSROOT")"
ln -sf "$SYSROOT" "$SPACEFREE_SYSROOT"
SYSROOT="$SPACEFREE_SYSROOT"

# ── 1. Fetch (shared cache) + unpack BusyBox ──────────────────
mkdir -p "$TOOLCHAIN_DIST_DIR"
BUSYBOX_TAR="$TOOLCHAIN_DIST_DIR/${BUSYBOX_TARBALL}"
if [ ! -f "$BUSYBOX_TAR" ]; then
    echo "Fetching BusyBox ${BUSYBOX_VER}..."
    if command -v curl >/dev/null 2>&1; then
        curl -fL -o "$BUSYBOX_TAR.tmp" "$BUSYBOX_URL"
    else
        wget -O "$BUSYBOX_TAR.tmp" "$BUSYBOX_URL"
    fi
    mv "$BUSYBOX_TAR.tmp" "$BUSYBOX_TAR"
fi

if command -v sha256sum >/dev/null 2>&1; then
    actual_sha256="$(sha256sum "$BUSYBOX_TAR" | awk '{print $1}')"
else
    actual_sha256="$(shasum -a 256 "$BUSYBOX_TAR" | awk '{print $1}')"
fi
if [ "$actual_sha256" != "$BUSYBOX_SHA256" ]; then
    echo "Error: checksum mismatch for $BUSYBOX_TAR" >&2
    echo "Expected: $BUSYBOX_SHA256" >&2
    echo "Actual:   $actual_sha256" >&2
    exit 1
fi

if [ ! -d "$SRC_DIR" ]; then
    echo "Extracting BusyBox to $SRC_DIR..."
    mkdir -p "$TOOLCHAIN_SRC_DIR"
    tar xjf "$BUSYBOX_TAR" -C "$TOOLCHAIN_SRC_DIR"
fi

# b1nix is a Linux-like target, but the cross GCC predefines __b1nix__/__unix__,
# not __linux__. The procps applets (free/uptime, and the ps uptime helper)
# include <sys/sysinfo.h> and use `struct sysinfo` only under `#ifdef __linux__`,
# so without this they fail to compile ("storage size of 'info' isn't known").
# b1nix ships <sys/sysinfo.h> in its sysroot and implements the SYS_SYSINFO
# syscall, so widen those include guards to also fire for __b1nix__. Idempotent:
# keyed on the rewritten token so a re-extracted tree is patched exactly once.
for bb_src in procps/free.c procps/uptime.c procps/ps.c procps/vmstat.c; do
    if [ -f "$SRC_DIR/$bb_src" ] && ! grep -q "__b1nix__" "$SRC_DIR/$bb_src"; then
        sed_inplace 's/#ifdef __linux__/#if defined(__linux__) || defined(__b1nix__)/' \
            "$SRC_DIR/$bb_src"
    fi
done

# BusyBox 1.38's libbb/alloc_affinity.c calls sched_getaffinity(). b1nix libc
# now implements that (SYS_SCHED_GETAFFINITY, reporting the online-CPU set), so
# the upstream file builds and works as-is — no override needed.

# BusyBox 1.38 miscutils/tree.c uses scandir() + alphasort() which b1nix libc
# does not provide. Replace the scandir/alphasort call with opendir/readdir/
# closedir + manual sorting via a simple strcmp-based insertion sort.
# CRITICAL: keep BusyBox's //config://applet://kbuild://usage: metadata headers
# verbatim — the build system scans them to register the applet, generate
# Config.in/applets.h and emit the kbuild object rule. Without them CONFIG_TREE
# is dropped by `make oldconfig` and tree.o is never compiled (so `busybox tree`
# silently does not exist). The `__b1nix__` marker comment makes this idempotent.
if [ -f "$SRC_DIR/miscutils/tree.c" ] && ! grep -q "__b1nix__" "$SRC_DIR/miscutils/tree.c"; then
    cat > "$SRC_DIR/miscutils/tree.c" << 'TREEPATCH'
/* __b1nix__: scandir/alphasort-free reimplementation of BusyBox tree. */
//config:config TREE
//config:	bool "tree (2.5 kb)"
//config:	default y
//config:	help
//config:	List files and directories in a tree structure.
//config:
//applet:IF_TREE(APPLET(tree, BB_DIR_USR_BIN, BB_SUID_DROP))
//kbuild:lib-$(CONFIG_TREE) += tree.o
//usage:#define tree_trivial_usage NOUSAGE_STR
//usage:#define tree_full_usage ""

#include "libbb.h"
#include "common_bufsiz.h"
#include "unicode.h"
#define prefix_buf bb_common_bufsiz1
static void tree_print(unsigned count[2], const char *directory_name, char *prefix_pos) {
    DIR *d;
    struct dirent **sorted = NULL;
    int n = 0, cap = 0, i;
    const char *bar = "|   ";
    const char *mid = "|-- ";
    const char *end = "`-- ";
    if (ENABLE_UNICODE_SUPPORT && unicode_status == UNICODE_ON) {
        bar = "│   ";
        mid = "├── ";
        end = "└── ";
    }
    d = opendir(directory_name);
    fputs_stdout(directory_name);
    if (!d) { puts(" [error opening dir]"); return; }
    puts("");
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (n >= cap) { cap = cap ? cap * 2 : 64; sorted = xrealloc(sorted, cap * sizeof(sorted[0])); }
        sorted[n] = xstrdup(de->d_name);
        n++;
    }
    closedir(d);
    for (i = 0; i < n; i++) {
        int j;
        for (j = i; j > 0 && strcmp(sorted[j - 1], sorted[j]) > 0; j--) {
            char *tmp = sorted[j]; sorted[j] = sorted[j - 1]; sorted[j - 1] = tmp;
        }
    }
    xchdir(directory_name);
    for (i = 0; i < n; i++) {
        const char *name = sorted[i];
        int is_last = (i == n - 1);
        struct stat statBuf;
        strcpy(prefix_pos, is_last ? end : mid);
        fputs_stdout(prefix_buf);
        int status = lstat(name, &statBuf);
        if (status == 0 && S_ISLNK(statBuf.st_mode)) {
            char *symlink_path = xmalloc_readlink(name);
            printf("%s -> %s\n", name, symlink_path);
            free(symlink_path);
        } else {
            puts(name);
        }
        if (status == 0 && S_ISDIR(statBuf.st_mode)) {
            char *sub_prefix = prefix_pos + strlen(prefix_pos);
            strcpy(sub_prefix, is_last ? "    " : bar);
            tree_print(count, name, sub_prefix);
        }
        free(sorted[i]);
    }
    free(sorted);
    xchdir("..");
}
int tree_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int tree_main(int argc, char **argv) {
    unsigned count[2] = {0, 0};
    const char *dir = ".";
    if (argc > 1) dir = argv[1];
    char *prefix = prefix_buf;
    tree_print(count, dir, prefix);
    return 0;
}
TREEPATCH
fi

# The system password scheme is the libc crypt()'s "$b1$" (b1nix /etc/shadow,
# dropbear, native su/passwd). BusyBox's builtin crypt (CONFIG_USE_BB_CRYPT,
# kept for $1$/$5$/$6$ in cryptpw/chpasswd) dies with "bad salt" on it, which
# broke login on the M39 serial getty. Patch pw_encrypt() to defer "$b1$"
# settings to the libc crypt(). Idempotent via the __b1nix__ marker.
if [ -f "$SRC_DIR/libbb/pw_encrypt.c" ] && ! grep -q "__b1nix__" "$SRC_DIR/libbb/pw_encrypt.c"; then
    python3 - "$SRC_DIR/libbb/pw_encrypt.c" << 'PWCRYPT'
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
PWCRYPT
fi

# ── 2. Configure ────────────────────────────────────────────────────────────
mkdir -p "$BUILD_DIR"

echo "Generating BusyBox configuration from $CONFIG_FRAGMENT..."
make -C "$SRC_DIR" O="$BUILD_DIR" CROSS_COMPILE="${TARGET}-" allnoconfig < /dev/null
while IFS= read -r setting; do
    case "$setting" in
        CONFIG_*=*)
            key="${setting%%=*}"
            grep -v -e "^${key}=" -e "^# ${key} is not set$" \
                "$BUILD_DIR/.config" > "$BUILD_DIR/.config.tmp" || true
            echo "$setting" >> "$BUILD_DIR/.config.tmp"
            mv "$BUILD_DIR/.config.tmp" "$BUILD_DIR/.config"
            ;;
        "# CONFIG_"*" is not set")
            key="${setting#\# }"
            key="${key% is not set}"
            grep -v -e "^${key}=" -e "^# ${key} is not set$" \
                "$BUILD_DIR/.config" > "$BUILD_DIR/.config.tmp" || true
            echo "$setting" >> "$BUILD_DIR/.config.tmp"
            mv "$BUILD_DIR/.config.tmp" "$BUILD_DIR/.config"
            ;;
    esac
done < "$CONFIG_FRAGMENT"
make -C "$SRC_DIR" O="$BUILD_DIR" CROSS_COMPILE="${TARGET}-" oldconfig < /dev/null

# Force a clean rebuild when the b1nix sysroot changed since the last BusyBox
# build. BusyBox's make does not track the sysroot headers / libb1nix.a as
# dependencies, so an incremental build would silently keep object files
# compiled against stale headers — e.g. an inline syscall wrapper or a struct
# layout — and link them with no warning. Keeping the .config (only the objects
# are removed) lets the configure step above stand.
if [ -f "$BUILD_DIR/busybox" ] &&
   [ "$SYSROOT/lib/libb1nix.a" -nt "$BUILD_DIR/busybox" ]; then
    echo "b1nix sysroot changed since last BusyBox build; cleaning objects..."
    make -C "$BUILD_DIR" CROSS_COMPILE="${TARGET}-" clean < /dev/null ||
        rm -f "$BUILD_DIR/busybox"
fi

# ── 3. Build ────────────────────────────────────────────────────────────────
echo "Building BusyBox..."
# Link using userspace library and load at 0x2000000
export EXTRA_CFLAGS="-fcommon --sysroot=$SYSROOT -isystem $SYSROOT/include"
export EXTRA_LDFLAGS="-static -Wl,-Ttext-segment=0x2000000 --sysroot=$SYSROOT"

make -C "$BUILD_DIR" -j"$NPROC" CROSS_COMPILE="${TARGET}-"

# ── 4. Install ──────────────────────────────────────────────────────────────
echo "Installing standalone BusyBox package..."
mkdir -p "$INSTALL_DIR"
cp "$BUILD_DIR/busybox" "$INSTALL_DIR/busybox"

# Remove links created by the earlier integration, but leave unrelated files
# and links in /bin untouched.
for applet in true false yes echo printf pwd basename dirname cat head tail wc \
    mkdir rmdir rm cp mv ln readlink touch chmod chown sync sleep date uname \
    kill test "[" sort uniq; do
    old_path="$SYSROOT/bin/$applet"
    if [ -L "$old_path" ]; then
        old_target="$(readlink "$old_path")"
        if [ "$old_target" = "busybox-real" ] ||
           [ "$old_target" = "/bin/busybox-real" ]; then
            rm -f "$old_path"
        fi
    fi
done
rm -f "$SYSROOT/bin/busybox-real"

echo "BusyBox ${BUSYBOX_VER} installed to $INSTALL_DIR/busybox"
