#!/bin/sh
# Build upstream GNU bash for the b1nix userspace ABI.
#
# Bash is the second real interactive shell on b1nix (after BusyBox ash) and is
# promoted to the default /bin/sh once it boots cleanly. It is an autotools
# project, so it follows the same cross-build pattern as tools/ports/build-dropbear.sh
# and tools/ports/build-curl.sh: the b1nix-autotools-cc wrapper compiles+links against
# the freestanding libb1nix.a, and a preseeded config.cache answers the configure
# run-time probes that a real cross-compile cannot execute.
#
# Output (stdout, last line): the bash source/build dir. The built binary is
#   $SRC_DIR/bash
# which the caller (Makefile) converts to initramfs_bash.inc via xxd -i.

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
BASH_VERSION_NUM="${BASH_VERSION_NUM:-5.2.37}"
BASH_TARBALL="bash-${BASH_VERSION_NUM}.tar.gz"
BASH_URL="https://ftp.gnu.org/gnu/bash/${BASH_TARBALL}"
CCACHE=""
command -v ccache >/dev/null 2>&1 && [ "${B1NIX_NO_CCACHE:-0}" != "1" ] && CCACHE="ccache "
WRAP="${CCACHE}$ROOT_DIR/tools/toolchain/bin/b1nix-musl-autotools-cc"
AR_BIN="${AR:-$(command -v llvm-ar 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ar)}"
RANLIB_BIN="${RANLIB:-$(command -v llvm-ranlib 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ranlib)}"

# Per-architecture build identity (B1NIX_ARCH -> triplet).
. "$ROOT_DIR/tools/toolchain/env.sh"
HOST_TRIPLET="$B1NIX_TRIPLET"
case "$HOST_TRIPLET" in
  i686*) B1NIX_ARCH=x86 ;;
  *)     B1NIX_ARCH=x86_64 ;;
esac
export B1NIX_ARCH

# Per-triplet source tree so x86 and x86_64 never share objects.
SRC_PARENT="$ROOT_DIR/build/src/bash"
SRC_DIR="$SRC_PARENT/$HOST_TRIPLET/bash-${BASH_VERSION_NUM}"

mkdir -p "$SRC_PARENT/$HOST_TRIPLET"

if [ ! -d "$SRC_DIR" ]; then
  tmp="$SRC_PARENT/${BASH_TARBALL}"
  if [ ! -f "$tmp" ]; then
    if command -v curl >/dev/null 2>&1; then
      curl -L "$BASH_URL" -o "$tmp"
    elif command -v wget >/dev/null 2>&1; then
      wget -O "$tmp" "$BASH_URL"
    else
      echo "tools/ports/build-bash.sh: need host curl or wget to fetch $BASH_URL" >&2
      exit 1
    fi
  fi
  tar -xzf "$tmp" -C "$SRC_PARENT/$HOST_TRIPLET"
fi

# Patch config.sub (top-level + bundled lib copies) to accept the b1nix host
# triplet. Modern config.sub validate the OS against a case list ending in
# "| fiwix* | mlibc* )".
for sub in "$SRC_DIR/support/config.sub" "$SRC_DIR/config.sub"; do
  if [ -f "$sub" ] && ! grep -q 'b1nix' "$sub"; then
    tmp_sub="$sub.new"
    # config.sub ends its OS allow-list either with "| fiwix* | mlibc* )" (newer)
    # or "| fiwix* )" (bash 5.2's 2021 copy). Handle both.
    if grep -q '| fiwix\* | mlibc\* )' "$sub"; then
      sed 's/| fiwix\* | mlibc\* )/| fiwix* | mlibc* | b1nix* )/' "$sub" > "$tmp_sub"
    else
      sed 's/| fiwix\* )/| fiwix* | b1nix* )/' "$sub" > "$tmp_sub"
    fi
    mv "$tmp_sub" "$sub"
  fi
done

# Upstream bug exposed only in non-multibyte builds: parse.y assigns to
# shell_input_line_property[] inside an `#if ALIAS||DPAREN` block, but the array
# is declared only under `#if HANDLE_MULTIBYTE`. b1nix's libc lacks the wide-char
# functions, so configure leaves HANDLE_MULTIBYTE off and the reference is
# undeclared. Guard that lone statement (in both parse.y and the shipped,
# actually-compiled y.tab.c) so the no-multibyte build compiles.
for pf in "$SRC_DIR/parse.y" "$SRC_DIR/y.tab.c"; do
  if [ -f "$pf" ] && ! grep -q 'B1NIX_MBPROP_GUARD' "$pf"; then
    perl -0pi -e 's/(\n[ \t]*)(shell_input_line_property\[shell_input_line_index - 1\] = 1;)/${1}#if defined (HANDLE_MULTIBYTE) \/* B1NIX_MBPROP_GUARD *\/${1}${2}${1}#endif/' "$pf"
  fi
done

# Preseed the configure cache for the run-time feature probes bash performs.
# A real cross-compile cannot run target binaries, so configure would either
# guess wrong or abort. These answers describe the b1nix libc/kernel:
#  - job control, named pipes (FIFOs), /dev/fd, sigsetjmp, POSIX signals all work
#    (ash already exercises them);
#  - rt signals are usable, getcwd(NULL) mallocs, mktime works;
#  - bash uses its *bundled* termcap (we ship no terminfo/termcap library).
CACHE="$SRC_DIR/config.cache"
cat > "$CACHE" <<'EOF'
ac_cv_func_setvbuf_reversed=no
ac_cv_func_working_mktime=yes
# b1nix libc has strerror() and does NOT export the legacy sys_errlist/sys_nerr
# (removed from modern host glibc too). Without these, cross-configure guesses
# HAVE_SYS_ERRLIST=1 + HAVE_STRERROR undefined, so bash's host build tools
# (mksyntax/mksignames) compile a strerror replacement that references the
# absent sys_errlist and fail to link on the build host.
ac_cv_func_strerror=yes
bash_cv_sys_errlist=no
# Cross-configure cannot run probes and guesses these libc functions absent, so
# bash compiles its own replacements / K&R redeclarations that then conflict
# with b1nix's string.h prototypes (e.g. "conflicting types for 'strpbrk'").
# b1nix libc provides all of these, so mark them present.
ac_cv_func_strpbrk=yes
ac_cv_func_strstr=yes
ac_cv_func_strchr=yes
ac_cv_func_strcasecmp=yes
ac_cv_func_strncasecmp=yes
ac_cv_func_strchrnul=yes
ac_cv_func_strnlen=yes
ac_cv_func_strdup=yes
ac_cv_func_mempcpy=yes
ac_cv_func_getpgrp_void=yes
ac_cv_sys_restartable_syscalls=yes
ac_cv_func_strcoll_works=yes
ac_cv_c_stack_direction=-1
ac_cv_func_lstat_dereferences_slashed_symlink=yes
ac_cv_func_lstat_empty_string_bug=no
ac_cv_func_stat_empty_string_bug=no
ac_cv_func_realloc_0_nonnull=yes
ac_cv_func_malloc_0_nonnull=yes
gt_cv_int_divbyzero_sigfpe=no
bash_cv_sys_named_pipes=present
bash_cv_func_sigsetjmp=present
bash_cv_func_ctype_nonascii=yes
bash_cv_must_reinstall_sighandlers=no
bash_cv_func_strcoll_broken=no
bash_cv_dup2_broken=no
bash_cv_pgrp_pipe=no
bash_cv_signal_vintage=posix
bash_cv_sys_siglist=yes
bash_cv_under_sys_siglist=yes
bash_cv_unusable_rtsigs=no
bash_cv_getcwd_malloc=yes
bash_cv_job_control_missing=present
bash_cv_printf_a_format=yes
bash_cv_wcontinued_broken=no
bash_cv_termcap_lib=gnutermcap
bash_cv_dev_fd=standard
bash_cv_dev_stdin=present
bash_cv_fnmatch_equiv_fallback=work
bash_cv_unbuffered_read=yes
bash_cv_type_rlimit=rlim_t
bash_cv_have_mbstate_t=yes
bash_cv_func_snprintf=yes
bash_cv_func_vsnprintf=yes
EOF

# Configure once (regenerating config.h non-deterministically has bitten other
# autotools ports here).
if [ ! -f "$SRC_DIR/config.h" ]; then
(
  cd "$SRC_DIR"
  BUILD_TRIPLET="$("./support/config.guess" 2>/dev/null || echo "$(uname -m)-apple-darwin")"
  export cross_compiling=yes
  # Host build tools (mkbuiltins, etc.) are K&R C. GCC 15+ defaults to gnu23
  # where `()` means `(void)`, breaking them ("too many arguments to xmalloc").
  # Pin the host build-tool compile to gnu17.
  export CC_FOR_BUILD="cc -std=gnu17"
  ./configure \
    --host="$HOST_TRIPLET" \
    --build="$BUILD_TRIPLET" \
    --cache-file=config.cache \
    --without-bash-malloc \
    --disable-nls \
    --enable-readline \
    --enable-history \
    --enable-job-control \
    --enable-net-redirections \
    --disable-rpath \
    CC="$WRAP" AR="$AR_BIN" RANLIB="$RANLIB_BIN" \
    CFLAGS_FOR_BUILD="-O2 -std=gnu17" \
    1>&2
)
fi

# bash bundles a lot of pre-ANSI K&R code (lib/termcap, lib/sh). Modern clang
# promotes implicit function declarations and int<->pointer mismatches to hard
# errors; the referenced functions (write, etc.) do exist in libb1nix, so demote
# those back to warnings just for this legacy tree.
# -fcommon: termcap's BC/PC/UP and readline's copies are tentative (common)
# definitions that modern clang's -fno-common turns into clashing strong
# symbols; -fcommon restores the merge-into-one-common behaviour they expect.
# -D_POSIX_VERSION: makes bash's posixwait.h pick `int` over the BSD `union wait`
# process-status type. Scoped to this build, not the global libc headers
# (defining it globally flips OpenSSL's secure-memory path to mlock we lack).
# UTF-8 is now the libc-wide default (MB_CUR_MAX=4 in <stdlib.h>), so no
# per-build multibyte flag is needed.
LEGACY_CFLAGS="-g -O2 -fcommon -D_POSIX_VERSION=200809L -Wno-implicit-function-declaration -Wno-int-conversion -Wno-implicit-int -fuse-ld=lld"
# bash provides its own getenv/setenv/putenv/unsetenv (lib/sh/getenv.c) that
# deliberately override libb1nix's; they appear first in the link line, so
# --allow-multiple-definition makes the linker keep bash's set.
export B1NIX_LD_EXTRA="--allow-multiple-definition -Wl,-dynamic-linker,/lib/ld-musl-x86_64.so.1 -L$ROOT_DIR/build/x86_64/ports/musl/install/lib -lc"
# bash links dynamically against libc.so.1 via the shared recipe's default
# (B1NIX_LINK=dynamic). bash's own getenv/setenv/putenv set manipulates `environ`
# directly, so the link emits an R_X86_64_COPY for `environ` (+ stdin/stdout/
# stderr/errno) into bash's .bss plus a paired GLOB_DAT for `environ`; the 4-byte
# `errno` COPY lands in the final bytes of the RW segment. The in-kernel M69
# loader handles that COPY-at-.bss-tail layout (see kernel/user/process.c —
# R_X86_64_COPY is exempt from the fixed 8-byte target probe).
make -C "$SRC_DIR" -j"${JOBS:-4}" \
  CC="$WRAP" AR="$AR_BIN" RANLIB="$RANLIB_BIN" \
  CFLAGS="$LEGACY_CFLAGS" \
  bash 1>&2

# Strip debug info to shrink the initramfs payload (~3 MB -> ~1 MB); the kernel
# loader only needs the ELF program headers, not symbols/DWARF.
STRIP_BIN="${STRIP:-$(command -v llvm-strip 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-strip)}"
if [ -x "$STRIP_BIN" ] || command -v "$STRIP_BIN" >/dev/null 2>&1; then
  "$STRIP_BIN" -S "$SRC_DIR/bash" 2>/dev/null || true
fi

echo "$SRC_DIR"
