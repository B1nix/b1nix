#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Build the b1nix overlay packages.
#
#   sh tools/deb/build-deb.sh              # every source package
#   sh tools/deb/build-deb.sh b1nix-meta   # one of them
#
# The packaging sources in packaging/ are templates: the release string, the
# Debian version and the repository URL are substituted here, once, so that
# every file that mentions a release agrees with every other one. The rendered
# source packages are left under $OUT/src so that a failed build can be
# inspected, and the .debs land in $OUT.
#
# The build runs inside the Debian chroot from tools/deb/debian-chroot.sh:
# an ordinary user, no sudo, no container runtime.
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
ARCH="${ARCH:-x86_64}"
DEB_ARCH="${DEB_ARCH:-amd64}"
SUITE="${SUITE:-trixie}"
OUT="${OUT:-$ROOT_DIR/build/packages/out}"
KERNEL_BUILD_DIR="${KERNEL_BUILD_DIR:-$ROOT_DIR/build/$ARCH}"
KERNEL_ELF="${KERNEL_ELF:-$KERNEL_BUILD_DIR/kernel.elf}"

# The distribution's own identity. Not derived from the kernel version: they
# are different numbers answering different questions (docs/versioning.md).
DISTRO_RELEASE="${DISTRO_RELEASE:-1}"
CODENAME="${CODENAME:-hnylytsi}"
CODENAME_UI="${CODENAME_UI:-Hnylytsi}"
REPO_URL="${REPO_URL:-https://b1nix.github.io/b1nix}"
REPO_SUITE="${REPO_SUITE:-$SUITE}"

CHROOT="$ROOT_DIR/tools/deb/debian-chroot.sh"

log() { printf '\033[1;34m[build-deb]\033[0m %s\n' "$*" >&2; }
die() { printf '\033[1;31m[build-deb] %s\033[0m\n' "$*" >&2; exit 1; }

# ── the version numbers ─────────────────────────────────────────────────────
version_h="$ROOT_DIR/kernel/include/b1nix/version.h"
[ -f "$version_h" ] || die "no $version_h"
# Anchored on the #define and taking the first match: B1NIX_RELEASE_STR
# mentions both macros on its own line, and an unanchored match picks that up
# too, producing a two-line "version" that breaks everything downstream.
KVER=$(sed -n 's/^#define B1NIX_VERSION_STR[ \t]*"\([^"]*\)".*/\1/p' "$version_h" | head -1)
ABIVER=$(sed -n 's/^#define B1NIX_LINUX_ABI_RELEASE[ \t]*"\([^"]*\)".*/\1/p' "$version_h" | head -1)
[ -n "$KVER" ] && [ -n "$ABIVER" ] || die "could not read the version from $version_h"
# The same string the kernel reports through uname(2), composed the same way.
RELEASE="$ABIVER-b1nix-$KVER"

# The Debian version is derived, never typed: on a tag it is the release, and
# between tags it carries the commit so that apt sees a monotonic string that
# still sorts below the next release. docs/versioning.md.
GIT_DESCRIBE=$(git -C "$ROOT_DIR" describe --tags --long --match 'v*' --always --dirty 2>/dev/null || echo unknown)
if git -C "$ROOT_DIR" describe --tags --exact-match --match 'v*' >/dev/null 2>&1; then
	DEB_VERSION="$KVER-1"
else
	_date=$(git -C "$ROOT_DIR" log -1 --format=%cd --date=format:%Y%m%d 2>/dev/null || date -u +%Y%m%d)
	_sha=$(git -C "$ROOT_DIR" rev-parse --short=8 HEAD 2>/dev/null || echo unknown)
	# The commit COUNT, before the hash, is what makes two snapshots from the
	# same day comparable: a hash does not increase, and two builds an hour
	# apart sorted backwards often enough that publish-repo refused the newer
	# one ("8f11c99b sorts below e3534597"). The count only ever grows on a
	# branch, so apt sees what a human means by newer.
	_count=$(git -C "$ROOT_DIR" rev-list --count HEAD 2>/dev/null || echo 0)
	DEB_VERSION="$KVER+git$_date.$_count.$_sha-1"
fi
DATE=$(date -uR)

log "kernel release $RELEASE, package version $DEB_VERSION ($GIT_DESCRIBE)"

# ── rendering a source package ──────────────────────────────────────────────
# A template is a file whose name ends in .in. Its content is substituted, and
# so is its name: pkg.postinst.in becomes b1nix-kernel-<release>.postinst,
# which is how debhelper finds a maintainer script for a package whose name
# contains the release.
render() { # src-file dst-file
	sed -e "s|@RELEASE@|$RELEASE|g" \
	    -e "s|@DEB_ARCH@|$DEB_ARCH|g" \
	    -e "s|@DEB_VERSION@|$DEB_VERSION|g" \
	    -e "s|@SUITE@|$SUITE|g" \
	    -e "s|@GIT_DESCRIBE@|$GIT_DESCRIBE|g" \
	    -e "s|@DATE@|$DATE|g" \
	    -e "s|@DISTRO_RELEASE@|$DISTRO_RELEASE|g" \
	    -e "s|@CODENAME@|$CODENAME|g" \
	    -e "s|@CODENAME_UI@|$CODENAME_UI|g" \
	    -e "s|@KERNEL_RELEASE@|$RELEASE|g" \
	    -e "s|@REPO_URL@|$REPO_URL|g" \
	    -e "s|@REPO_SUITE@|$REPO_SUITE|g" \
	    "$1" >"$2"
}

stage_source() { # source-package-name -> staged directory
	_name="$1"
	_src="$ROOT_DIR/packaging/$_name"
	_dst="$OUT/src/$_name"
	[ -d "$_src" ] || die "no packaging source for $_name"
	rm -rf "$_dst"
	mkdir -p "$_dst"
	cp -a "$_src/." "$_dst/"

	# Templates, content and name alike.
	find "$_dst" -name '*.in' | while read -r t; do
		_rel="${t%.in}"
		case "$(basename "$_rel")" in
		pkg.postinst | pkg.prerm | pkg.postrm)
			_rel="$(dirname "$_rel")/b1nix-kernel-$RELEASE.$(basename "$_rel" | sed 's/^pkg\.//')"
			;;
		esac
		render "$t" "$_rel"
		rm -f "$t"
	done
	# files/*.in are expanded at build time by debian/rules, not here: the
	# entry template ships as a template because b1nix-update-bootloader
	# expands it again on the installed machine, for every kernel it finds.
	if [ -d "$_src/files" ]; then
		rm -rf "$_dst/files"
		cp -a "$_src/files" "$_dst/files"
	fi
	chmod +x "$_dst/debian/rules"
	for s in "$_dst"/debian/*.postinst "$_dst"/debian/*.prerm "$_dst"/debian/*.postrm; do
		[ -f "$s" ] && chmod +x "$s"
	done
	printf '%s' "$_dst"
}

# ── the source packages ─────────────────────────────────────────────────────
build_b1nix_kernel() {
	[ -f "$KERNEL_ELF" ] ||
		die "no kernel at $KERNEL_ELF -- build it first (make ARCH=$ARCH) or set KERNEL_ELF"

	_dst=$(stage_source b1nix-kernel)
	printf '%s\n' "$RELEASE" >"$_dst/debian/b1nix-release"

	mkdir -p "$_dst/artifacts"
	cp "$KERNEL_ELF" "$_dst/artifacts/kernel.elf"
	[ ! -f "$KERNEL_BUILD_DIR/System.map" ] ||
		cp "$KERNEL_BUILD_DIR/System.map" "$_dst/artifacts/System.map"
	# The headers package carries the kernel's own headers: what a module has
	# to compile against.
	mkdir -p "$_dst/artifacts/include"
	cp -a "$ROOT_DIR/kernel/include/." "$_dst/artifacts/include/"

	run_build b1nix-kernel "$_dst"
}

build_b1nix_meta() {
	_dst=$(stage_source b1nix-meta)
	{
		printf 'DISTRO_RELEASE=%s\n' "$DISTRO_RELEASE"
		printf 'CODENAME=%s\n' "$CODENAME"
		printf 'CODENAME_UI=%s\n' "$CODENAME_UI"
		printf 'KERNEL_RELEASE=%s\n' "$RELEASE"
		printf 'REPO_URL=%s\n' "$REPO_URL"
		printf 'REPO_SUITE=%s\n' "$REPO_SUITE"
	} >"$_dst/debian/b1nix-subst"
	run_build b1nix-meta "$_dst"
}

run_build() { # name staged-dir
	_name="$1"
	_dst="$2"
	_rel="${_dst#"$ROOT_DIR"/}"
	# The version string carries the commit, so every rebuild leaves a
	# .changes behind naming .debs that have since been moved to $OUT. lintian
	# then exits non-zero over a missing file rather than over a tag, and the
	# build looks broken when it is not.
	rm -f "$OUT/src/${_name}_"*.changes "$OUT/src/${_name}_"*.buildinfo
	log "building $_name in the $SUITE chroot"
	sh "$CHROOT" run "cd '/src/$_rel' && dpkg-buildpackage -b -us -uc" ||
		die "$_name failed to build -- the rendered source is at $_dst"
	log "lintian on $_name"
	# Errors fail the build; tags below error level are printed and do not.
	# A derivative that ships packages with broken dependencies teaches its
	# users to distrust apt.
	# Only this package's .changes: a glob also picks up the ones left by
	# earlier builds, whose .debs have already been moved to $OUT, and
	# lintian exits non-zero over the missing file rather than over a tag.
	sh "$CHROOT" run "cd '/src/$(dirname "$_rel")' && lintian --fail-on error ${_name}_*.changes" ||
		die "$_name has lintian errors"
}

mkdir -p "$OUT/src"

if [ $# -gt 0 ]; then
	TARGETS="$*"
else
	TARGETS="b1nix-kernel b1nix-meta"
fi

for t in $TARGETS; do
	case "$t" in
	b1nix-kernel) build_b1nix_kernel ;;
	b1nix-meta) build_b1nix_meta ;;
	*) die "unknown source package '$t'" ;;
	esac
done

# dpkg-buildpackage writes beside the source directory, which is $OUT/src.
find "$OUT/src" -maxdepth 1 -name '*.deb' -exec mv -f {} "$OUT/" \;

# Keep one version of each package. Between tags the version carries the
# commit, so every rebuild adds a file rather than replacing one, and
# publish-repo then sees an older build beside the new one and refuses the
# whole publish over a version that sorts backwards. The output directory holds
# what was built now; the repository is the place with history.
for d in "$OUT"/*.deb; do
	[ -f "$d" ] || continue
	_pkg=$(basename "$d" | sed 's/_.*//')
	_ver=$(basename "$d" | sed 's/^[^_]*_//; s/_[^_]*$//')
	[ "$_ver" != "$DEB_VERSION" ] || continue
	log "removing superseded $(basename "$d")"
	rm -f "$d"
done
log "packages in $OUT:"
ls -1 "$OUT"/*.deb 2>/dev/null | sed 's|.*/|  |' >&2 || die "no .deb was produced"
