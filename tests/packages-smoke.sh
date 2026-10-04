#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# PKG-SMOKE: the overlay packages build, publish and install.
#
#   sh tests/packages-smoke.sh
#
# This is the phase A lane of docs/distro/roadmap.md, written to the contract
# in docs/distro/lanes.md: every marker is printed only after the operation it
# names really happened, and the four failure modes -- build, install, stage
# and check -- are told apart.
#
# It needs a kernel binary per architecture: build/x86_64/kernel.elf and
# build/aarch64/kernel.elf (KERNEL_ELF / ARM64_KERNEL_ELF override them).
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
ARCH="${ARCH:-x86_64}"
DEB_ARCH="${DEB_ARCH:-amd64}"
SUITE="${SUITE:-trixie}"
LANE="PKG-SMOKE"
OUT_DIR="$ROOT_DIR/smoke_run"
LOG="$OUT_DIR/packages-smoke.log"
REPO="$ROOT_DIR/build/packages/repo"
CHROOT="$ROOT_DIR/tools/deb/debian-chroot.sh"
# A chroot of its own: installing into the one that builds the packages would
# prove nothing about a clean machine.
TEST_BASE="$ROOT_DIR/build/packages/test"
# binutils is not a nicety: without readelf the ELF checks below silently
# succeed, which is exactly the kind of fake pass this suite forbids.
TEST_DEPS="${TEST_DEPS:-ca-certificates binutils}"
ARM64_KERNEL_ELF="${ARM64_KERNEL_ELF:-$ROOT_DIR/build/aarch64/kernel.elf}"
# A throwaway keyring: the lane signs what it publishes, and proves apt both
# accepts that signature and refuses one made by another key.
GNUPG_DIR="$OUT_DIR/pkg-smoke-gnupg"

GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[1;33m'; NC='\033[0m'
pass=0
fail=0

ok()   { printf "${GREEN}%s: ok %s${NC}\n" "$LANE" "$1"; pass=$((pass + 1)); }
bad()  { printf "${RED}%s: FAIL %s${NC} -- %s\n" "$LANE" "$1" "$2"; fail=$((fail + 1)); }
info() { printf "${YELLOW}%s${NC}\n" "$*"; }
# A stage that did not run is not a failed check: it is a different kind of
# failure, and saying so is the difference between "the kernel is broken" and
# "the build never happened".
stage_die() { printf "${RED}%s: STAGE-FAILED %s${NC} -- see %s\n" "$LANE" "$1" "$LOG"; exit 2; }

mkdir -p "$OUT_DIR"
: >"$LOG"

in_test_chroot() { # command...
	CHROOT_BASE="$TEST_BASE" BUILD_DEPS="$TEST_DEPS" sh "$CHROOT" run "$@"
}

# ── stage 1: build ──────────────────────────────────────────────────────────
info "building the overlay packages"
sh "$ROOT_DIR/tools/deb/build-deb.sh" >>"$LOG" 2>&1 || stage_die build
for p in b1nix-kernel b1nix-base-files b1nix-desktop b1nix-tools b1cc; do
	ls "$ROOT_DIR"/build/packages/out/"$p"_*.deb >/dev/null 2>&1 ||
		bad "built-$p" "no .deb for $p in build/packages/out"
done
[ "$fail" -eq 0 ] && ok "packages-built"

# The kernel for arm64, cross-packaged from the aarch64 build (phase F boots
# it; this lane proves the package).
[ -f "$ARM64_KERNEL_ELF" ] || stage_die no-arm64-kernel
info "building the arm64 kernel package"
ARCH=aarch64 DEB_ARCH=arm64 KERNEL_ELF="$ARM64_KERNEL_ELF" \
	sh "$ROOT_DIR/tools/deb/build-deb.sh" b1nix-kernel >>"$LOG" 2>&1 || stage_die build-arm64
if ls "$ROOT_DIR"/build/packages/out/b1nix-kernel_*_arm64.deb >/dev/null 2>&1; then
	ok "built-arm64-kernel"
else
	bad "built-arm64-kernel" "no arm64 b1nix-kernel .deb in build/packages/out"
fi

# ── stage 2: publish ────────────────────────────────────────────────────────
info "creating a throwaway signing key"
rm -rf "$GNUPG_DIR"
mkdir -p "$GNUPG_DIR"
chmod 700 "$GNUPG_DIR"
export GNUPGHOME="$GNUPG_DIR"
gpg --batch --passphrase '' --quick-gen-key 'b1nix PKG-SMOKE <pkg-smoke@b1nix.invalid>' \
	ed25519 sign 1d >>"$LOG" 2>&1 || stage_die gen-key
gpg --batch --passphrase '' --quick-gen-key 'b1nix PKG-SMOKE other <other@b1nix.invalid>' \
	ed25519 sign 1d >>"$LOG" 2>&1 || stage_die gen-other-key
SIGN_FPR=$(gpg --batch --with-colons --list-keys pkg-smoke@b1nix.invalid | awk -F: '$1 == "fpr" {print $10; exit}')
OTHER_FPR=$(gpg --batch --with-colons --list-keys other@b1nix.invalid | awk -F: '$1 == "fpr" {print $10; exit}')
[ -n "$SIGN_FPR" ] && [ -n "$OTHER_FPR" ] || stage_die key-fingerprint
gpg --batch --yes --armor --export "$OTHER_FPR" >"$OUT_DIR/pkg-smoke-other.asc"

info "publishing the repository, signed"
SIGN_KEY="$SIGN_FPR" sh "$ROOT_DIR/tools/deb/publish-repo.sh" >>"$LOG" 2>&1 || stage_die publish
if [ -f "$REPO/dists/$SUITE/Release" ] &&
   [ -f "$REPO/dists/$SUITE/main/binary-$DEB_ARCH/Packages" ]; then
	ok "repo-indices"
else
	bad "repo-indices" "Release or Packages is missing under $REPO"
fi
if grep -q '^Architectures:.*amd64' "$REPO/dists/$SUITE/Release" &&
   grep -q '^Architectures:.*arm64' "$REPO/dists/$SUITE/Release" &&
   awk '$1 == "Package:" {p = $2} $1 == "Architecture:" && p == "b1nix-kernel" && $2 == "arm64" {f = 1} END {exit !f}' \
	"$REPO/dists/$SUITE/main/binary-arm64/Packages"; then
	ok "repo-arm64-index"
else
	bad "repo-arm64-index" "Release does not list both architectures, or binary-arm64 has no arm64 b1nix-kernel"
fi
if [ -f "$REPO/dists/$SUITE/InRelease" ] && [ -f "$REPO/dists/$SUITE/Release.gpg" ] &&
   gpg --batch --verify "$REPO/dists/$SUITE/Release.gpg" "$REPO/dists/$SUITE/Release" >>"$LOG" 2>&1; then
	ok "repo-signed"
else
	bad "repo-signed" "Release carries no signature that verifies with the key it was signed with"
fi

# ── stage 3: install into a clean chroot ────────────────────────────────────
info "installing from the repository into a clean $SUITE chroot"
CHROOT_BASE="$TEST_BASE" BUILD_DEPS="$TEST_DEPS" sh "$CHROOT" create >>"$LOG" 2>&1 ||
	stage_die create-test-chroot
# Prove the tools the checks depend on are really there. A missing readelf
# turns "! readelf ... | grep" into a pass and the lane into a liar.
in_test_chroot "command -v readelf >/dev/null" >>"$LOG" 2>&1 || stage_die test-chroot-tools

# Signed-By, as an installed machine has it: no trusted=yes anywhere.
repo_rel="${REPO#"$ROOT_DIR"/}"
in_test_chroot "mkdir -p /etc/apt/keyrings && cp /src/$repo_rel/b1nix-archive.asc /etc/apt/keyrings/b1nix-archive.asc && printf 'deb [signed-by=/etc/apt/keyrings/b1nix-archive.asc] file:/src/$repo_rel $SUITE main\n' >/etc/apt/sources.list.d/b1nix-test.list" >>"$LOG" 2>&1 ||
	stage_die add-source
if in_test_chroot "apt-get update -qq" >>"$LOG" 2>&1; then
	ok "apt-update-signed"
else
	bad "apt-update-signed" "apt refused the overlay signed with the key it was given"
fi

# The same repository against a key that did not sign it must be refused.
other_rel="${OUT_DIR#"$ROOT_DIR"/}/pkg-smoke-other.asc"
wrong_key='
printf "deb [signed-by=/src/'"$other_rel"'] file:/src/'"$repo_rel $SUITE"' main\n" >/tmp/wrong-key.list
mkdir -p /tmp/wrong-key-lists/partial
apt-get update -o Dir::Etc::sourcelist=/tmp/wrong-key.list -o Dir::Etc::sourceparts=/dev/null \
	-o Dir::State::lists=/tmp/wrong-key-lists -o APT::Get::List-Cleanup=0 >/tmp/wrong-key.out 2>&1 || true
cat /tmp/wrong-key.out
grep -q "NO_PUBKEY\|is not signed\|BADSIG" /tmp/wrong-key.out
'
if in_test_chroot "$wrong_key" >>"$LOG" 2>&1; then
	ok "apt-refuses-wrong-key"
else
	bad "apt-refuses-wrong-key" "apt accepted the overlay against a key that did not sign it"
fi

if in_test_chroot "DEBIAN_FRONTEND=noninteractive apt-get install -y b1nix-kernel b1nix-base-files b1nix-tools b1cc" >>"$LOG" 2>&1; then
	ok "apt-install"
else
	bad "apt-install" "apt could not install the overlay packages"
fi

# b1cc compiles, links and runs a program against the system C library, with
# no --target: the package's default is this machine's Linux target.
b1cc_script='
set -e
mkdir -p /tmp/b1cc && cd /tmp/b1cc
printf "%s\n" "#include <stdio.h>" "#include <string.h>" \
	"static int sum(const int *v, int n) { int s = 0; for (int i = 0; i < n; i++) s += v[i]; return s; }" \
	"int main(void) {" \
	"	int v[] = {3, 4, 5};" \
	"	char buf[32];" \
	"	snprintf(buf, sizeof buf, \"b1cc %d %d\", sum(v, 3), (int)strlen(\"debian\"));" \
	"	puts(buf);" \
	"	return 0;" \
	"}" >hello.c
b1cc hello.c -o hello
./hello
./hello | grep -qx "b1cc 12 6"
'
if in_test_chroot "$b1cc_script" >>"$LOG" 2>&1; then
	ok "b1cc-compiles"
else
	bad "b1cc-compiles" "the packaged b1cc could not build and run a C program"
fi

# The arm64 kernel as apt sees it, without enabling arm64 for the whole
# machine: only the overlay is asked for both architectures.
arm64_apt="-o APT::Architectures::=amd64 -o APT::Architectures::=arm64 -o Dir::Etc::sourcelist=/etc/apt/sources.list.d/b1nix-test.list -o Dir::Etc::sourceparts=/dev/null -o Dir::State::lists=/tmp/arm64-lists -o APT::Get::List-Cleanup=0"
if in_test_chroot "mkdir -p /tmp/arm64-lists/partial && apt-get update -qq $arm64_apt && apt-cache $arm64_apt show b1nix-kernel:arm64 | grep -q '^Architecture: arm64'" >>"$LOG" 2>&1; then
	ok "apt-sees-arm64-kernel"
else
	bad "apt-sees-arm64-kernel" "apt does not find b1nix-kernel for arm64 in the overlay"
fi

# ── stage 4: checks on what was installed ───────────────────────────────────
release=$(sed -n 's/^#define B1NIX_LINUX_ABI_RELEASE[ \t]*"\([^"]*\)".*/\1/p' \
	"$ROOT_DIR/kernel/include/b1nix/version.h" | head -1)-b1nix-$(
	sed -n 's/^#define B1NIX_VERSION_STR[ \t]*"\([^"]*\)".*/\1/p' \
	"$ROOT_DIR/kernel/include/b1nix/version.h" | head -1)

if in_test_chroot "test -f /boot/b1nix-$release" >>"$LOG" 2>&1; then
	ok "kernel-on-boot"
else
	bad "kernel-on-boot" "/boot/b1nix-$release is not there after the install"
fi

# The shipped kernel must be stripped -- and must still carry the blob the
# panic path symbolises from.
if in_test_chroot "readelf -S /boot/b1nix-$release >/tmp/sections && ! grep -q ' .symtab' /tmp/sections" >>"$LOG" 2>&1; then
	ok "kernel-stripped"
else
	bad "kernel-stripped" "the installed kernel still carries a symbol table"
fi
if in_test_chroot "readelf -S /boot/b1nix-$release >/tmp/sections && grep -q kallsyms /tmp/sections" >>"$LOG" 2>&1; then
	ok "kernel-keeps-kallsyms"
else
	bad "kernel-keeps-kallsyms" "the .kallsyms blob did not survive the strip: panics would print bare addresses"
fi

if in_test_chroot "grep -q '^ID=b1nix' /etc/os-release && grep -q '^ID_LIKE=debian' /etc/os-release" >>"$LOG" 2>&1; then
	ok "os-release"
else
	bad "os-release" "/etc/os-release does not identify b1nix as a Debian derivative"
fi

# The pin is the thing that keeps Debian's kernel out of a dependency
# resolution. -1 means "never install this unless told by name".
if in_test_chroot "apt-cache policy linux-image-amd64 | grep -q ' -1$'" >>"$LOG" 2>&1; then
	ok "debian-kernel-pinned"
else
	bad "debian-kernel-pinned" "apt does not report Debian's linux-image-amd64 at priority -1"
fi

if in_test_chroot "b1nix-report --no-ask -o /tmp/r.txt && grep -q '^schema: 1' /tmp/r.txt && grep -q '^kernel:' /tmp/r.txt" >>"$LOG" 2>&1; then
	ok "b1nix-report"
else
	bad "b1nix-report" "b1nix-report did not produce a report with the documented header"
fi

# ── stage 5: the bootloader generator and the boot state ────────────────────
# Run against a fake ESP, because what is being tested is the state machine,
# not a real disk.
info "exercising b1nix-update-bootloader against a fake ESP"
esp_script='
set -e
esp=/tmp/esp
rm -rf $esp && mkdir -p $esp
: >$esp/b1nix-6.6.0-b1nix-0.123.0
: >$esp/b1nix-6.6.0-b1nix-0.124.0
ESP=$esp ROOT_SPEC=/dev/sda2 /usr/sbin/b1nix-update-bootloader
'
if in_test_chroot "$esp_script" >>"$LOG" 2>&1; then
	ok "bootloader-generated"
else
	bad "bootloader-generated" "b1nix-update-bootloader failed against a fake ESP"
fi

if in_test_chroot "grep -q '^default=6.6.0-b1nix-0.124.0' /tmp/esp/b1nix/boot-state" >>"$LOG" 2>&1; then
	ok "newest-kernel-is-default"
else
	bad "newest-kernel-is-default" "the state file does not make the newest kernel the default"
fi

if in_test_chroot "grep -c '^entry=' /tmp/esp/b1nix/boot-state | grep -q '^2$'" >>"$LOG" 2>&1; then
	ok "both-kernels-in-state"
else
	bad "both-kernels-in-state" "the state file does not carry an entry per installed kernel"
fi

if in_test_chroot "grep -q 'b1nix.entry=6.6.0-b1nix-0.124.0' /tmp/esp/limine.conf && grep -q 'b1nix.entry=6.6.0-b1nix-0.123.0' /tmp/esp/limine.conf" >>"$LOG" 2>&1; then
	ok "both-kernels-bootable"
else
	bad "both-kernels-bootable" "limine.conf does not offer both kernels"
fi

# The rescue entry must exist and must be excluded from counting, or a machine
# that has spent every try has nothing left to boot.
if in_test_chroot "grep -q 'rescue' /tmp/esp/limine.conf && grep -q 'b1nix.no-boot-count' /tmp/esp/limine.conf" >>"$LOG" 2>&1; then
	ok "rescue-entry-uncounted"
else
	bad "rescue-entry-uncounted" "limine.conf has no rescue entry excluded from boot counting"
fi

# b1nix-boot-good marks the entry it was told about, and only that one.
good_script='
set -e
ESP=/tmp/esp SETTLE=0 /usr/sbin/b1nix-boot-good </dev/null >/dev/null 2>&1 || true
# No b1nix.entry= on this chroot cmdline, so nothing may have changed.
grep -q "good=0" /tmp/esp/b1nix/boot-state
'
if in_test_chroot "$good_script" >>"$LOG" 2>&1; then
	ok "boot-good-needs-an-entry"
else
	bad "boot-good-needs-an-entry" "b1nix-boot-good marked something good without knowing which entry booted"
fi

# ── stage 6: the arm64 kernel package ───────────────────────────────────────
# Unpacked and read rather than installed: its postinst builds an initramfs
# with this machine's binaries, which says nothing about an arm64 machine.
info "checking the arm64 kernel package"
arm64_deb=$(ls "$ROOT_DIR"/build/packages/out/b1nix-kernel-"$release"_*_arm64.deb 2>/dev/null | head -1)
arm64_rel="${arm64_deb#"$ROOT_DIR"/}"
if [ -n "$arm64_deb" ] &&
   in_test_chroot "rm -rf /tmp/arm64 && dpkg-deb -x /src/$arm64_rel /tmp/arm64 && test -f /tmp/arm64/boot/b1nix-$release" >>"$LOG" 2>&1; then
	ok "arm64-kernel-packaged"
	if in_test_chroot "readelf -h /tmp/arm64/boot/b1nix-$release | grep -q 'Machine:.*AArch64'" >>"$LOG" 2>&1; then
		ok "arm64-kernel-is-aarch64"
	else
		bad "arm64-kernel-is-aarch64" "the arm64 package does not carry an AArch64 kernel"
	fi
	if in_test_chroot "readelf -S /tmp/arm64/boot/b1nix-$release >/tmp/arm64.sections && ! grep -q ' .symtab' /tmp/arm64.sections && grep -q kallsyms /tmp/arm64.sections" >>"$LOG" 2>&1; then
		ok "arm64-kernel-stripped-keeps-kallsyms"
	else
		bad "arm64-kernel-stripped-keeps-kallsyms" "the arm64 kernel is not stripped, or lost .kallsyms"
	fi
else
	bad "arm64-kernel-packaged" "no b1nix-kernel-$release arm64 package with /boot/b1nix-$release in it"
fi

# ── summary ─────────────────────────────────────────────────────────────────
printf '\n%s: %d passed, %d failed (log: %s)\n' "$LANE" "$pass" "$fail" "$LOG"
[ "$fail" -eq 0 ] || exit 1
