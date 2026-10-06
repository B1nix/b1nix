#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Build the b1nix overlay packages.
#
#   sh tools/deb/build-deb.sh              # every source package
#   sh tools/deb/build-deb.sh b1nix-meta   # one of them
#   ARCH=aarch64 DEB_ARCH=arm64 sh tools/deb/build-deb.sh   # the arm64 kernel
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
# The architecture of the chroot the builds run in. A package for another
# architecture is cross-built there: the kernel is compiled by the tree's own
# Makefile, so all its package needs is binutils that read the target's ELF.
CHROOT_ARCH="${CHROOT_ARCH:-amd64}"
B1CC_DIR="${B1CC_DIR:-$ROOT_DIR/third_party/b1cc}"
# Upstream release tarballs (Limine), fetched once and checked every time.
UPSTREAM_CACHE="${UPSTREAM_CACHE:-$ROOT_DIR/build/packages/upstream}"

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

# b1cc has its own upstream version, not the kernel's (packaging.md): its
# repository's commit date, commit count and hash, under a 0~ that any real
# b1cc release number sorts above.
B1CC_COMMIT=$(git -C "$B1CC_DIR" rev-parse --short=8 HEAD 2>/dev/null || echo unknown)
# Limine is not in Debian, so its revision says whose package it is; 0b1nix1
# sorts below a Debian upload of the same upstream version, should one appear.
LIMINE_VERSION=$(sed -n 's/^VERSION=//p' "$ROOT_DIR/packaging/limine/release.conf")
LIMINE_DEB_VERSION="$LIMINE_VERSION-0b1nix1"
B1CC_DEB_VERSION="0~git$(git -C "$B1CC_DIR" log -1 --format=%cd --date=format:%Y%m%d 2>/dev/null || echo 0).$(git -C "$B1CC_DIR" rev-list --count HEAD 2>/dev/null || echo 0).$B1CC_COMMIT-1"

# The release packages published before this one.
#
# b1nix-kernel-common took over the files every release used to ship its own
# copy of -- the boot-counting hook, the boot-good unit, the bootloader writer.
# Taking a path over from another package is exactly what Replaces is for, and
# without it dpkg refuses the unpack ("trying to overwrite ... which is also in
# package b1nix-kernel-6.6.0-b1nix-0.123.0") on every machine that has an older
# release installed -- which is every machine that has ever upgraded. Replaces
# WITHOUT Conflicts or Breaks, deliberately: the old release keeps its kernel
# and stays bootable, which is the whole point of installing them side by side.
#
# The list is what the repository has published, since that is what a machine
# can have installed. No older release, no field at all.
_repo_pool="${REPO_POOL:-$ROOT_DIR/build/packages/repo/pool/main}"
KERNEL_COMMON_REPLACES=""
if [ -d "$_repo_pool" ]; then
	_old=$(ls "$_repo_pool" 2>/dev/null |
		sed -n 's/^\(b1nix-kernel-[0-9][^_]*\)_.*\.deb$/\1/p' |
		grep -v -- '-dbg$' | grep -v '^b1nix-kernel-headers' |
		grep -v "^b1nix-kernel-$RELEASE\$" | sort -u | paste -sd, - |
		sed 's/,/, /g')
	[ -n "$_old" ] && KERNEL_COMMON_REPLACES="Replaces: $_old
"
fi

log "kernel release $RELEASE, package version $DEB_VERSION ($GIT_DESCRIBE)"

# ── rendering a source package ──────────────────────────────────────────────
# A template is a file whose name ends in .in. Its content is substituted, and
# so is its name: pkg.postinst.in becomes b1nix-kernel-<release>.postinst,
# which is how debhelper finds a maintainer script for a package whose name
# contains the release.
render() { # src-file dst-file
	# The Replaces list can be empty, and an empty value must leave no line
	# behind -- a bare "Replaces:" is a parse error in a control file. It is
	# substituted with awk rather than sed because it carries a newline.
	awk -v repl="$KERNEL_COMMON_REPLACES" \
	    '{ if (index($0, "@KERNEL_COMMON_REPLACES@")) {
	           sub(/@KERNEL_COMMON_REPLACES@/, repl);
	           if ($0 == "") next;
	       }
	       print }' "$1" >"$1.awk" && mv "$1.awk" "$1"
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
	    -e "s|@B1CC_DEB_VERSION@|$B1CC_DEB_VERSION|g" \
	    -e "s|@B1CC_COMMIT@|$B1CC_COMMIT|g" \
	    -e "s|@LIMINE_VERSION@|$LIMINE_VERSION|g" \
	    -e "s|@LIMINE_DEB_VERSION@|$LIMINE_DEB_VERSION|g" \
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
	# The loadable modules and their index. The kernel loads isofs, ntfs,
	# hda, ipv6 and their dependencies from /lib/modules/<release> once the
	# root is up; a package without them leaves an installed system with no
	# IPv6 and no way to mount a CD.
	mkdir -p "$_dst/artifacts/modules"
	ls "$KERNEL_BUILD_DIR"/modules/*.ko >/dev/null 2>&1 ||
		die "no modules in $KERNEL_BUILD_DIR/modules -- build the kernel first"
	cp "$KERNEL_BUILD_DIR"/modules/*.ko "$_dst/artifacts/modules/"
	for f in modules.dep modules.alias modules.builtin; do
		cp "$KERNEL_BUILD_DIR/inc/.modules-stage/$f" "$_dst/artifacts/modules/" ||
			die "no $f for the kernel modules"
	done

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

build_b1cc() {
	[ -f "$B1CC_DIR/Makefile" ] ||
		die "no b1cc source at $B1CC_DIR -- git submodule update --init third_party/b1cc"
	_dst=$(stage_source b1cc)
	# The upstream tree as the build needs it, without its build output or the
	# submodule's git metadata.
	for f in Makefile README.md LICENSE LICENSING.md src include runtime; do
		[ -e "$B1CC_DIR/$f" ] || continue
		cp -a "$B1CC_DIR/$f" "$_dst/"
	done
	run_build b1cc "$_dst"
}

build_limine() {
	_conf="$ROOT_DIR/packaging/limine/release.conf"
	_url=$(sed -n 's/^URL=//p' "$_conf")
	_sum=$(sed -n 's/^SHA256=//p' "$_conf")
	_tar="$UPSTREAM_CACHE/limine-$LIMINE_VERSION-binary.tar.xz"
	mkdir -p "$UPSTREAM_CACHE"
	if [ ! -f "$_tar" ]; then
		log "fetching Limine $LIMINE_VERSION"
		curl -sSfL -o "$_tar.part" "$_url" || die "could not fetch $_url"
		mv "$_tar.part" "$_tar"
	fi
	[ "$(sha256sum "$_tar" | cut -d' ' -f1)" = "$_sum" ] ||
		die "$_tar does not match the checksum in $_conf -- refusing it"
	_dst=$(stage_source limine)
	mkdir -p "$_dst/upstream"
	tar -C "$_dst/upstream" --strip-components=1 -xJf "$_tar" ||
		die "could not unpack $_tar"
	rm -f "$_dst/release.conf"
	run_build limine "$_dst"
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
	_bp="dpkg-buildpackage -b -us -uc"
	if [ "$DEB_ARCH" != "$CHROOT_ARCH" ]; then
		# A cross build. -d: the Build-Depends are tools for the build
		# machine, and dpkg-checkbuilddeps would look for them as $DEB_ARCH
		# packages. What the cross build really needs is installed here.
		_gnu=$(DEB_ARCH="$CHROOT_ARCH" sh "$CHROOT" run "dpkg-architecture -a$DEB_ARCH -qDEB_HOST_GNU_TYPE 2>/dev/null") ||
			die "dpkg-architecture does not know $DEB_ARCH"
		DEB_ARCH="$CHROOT_ARCH" sh "$CHROOT" run "command -v $_gnu-objcopy >/dev/null || DEBIAN_FRONTEND=noninteractive apt-get install -y binutils-$_gnu" ||
			die "could not install binutils for $_gnu in the chroot"
		# -B: only the architecture-dependent packages. The arch-all ones
		# come from the native build, and a second build of the same version
		# would put two different files under one name in the pool.
		_bp="dpkg-buildpackage -B -us -uc -a$DEB_ARCH -d"
	fi
	log "building $_name for $DEB_ARCH in the $SUITE chroot"
	DEB_ARCH="$CHROOT_ARCH" sh "$CHROOT" run "cd '/src/$_rel' && $_bp" ||
		die "$_name failed to build -- the rendered source is at $_dst"
	log "lintian on $_name"
	# Errors fail the build; tags below error level are printed and do not.
	# A derivative that ships packages with broken dependencies teaches its
	# users to distrust apt.
	# Only this package's .changes: a glob also picks up the ones left by
	# earlier builds, whose .debs have already been moved to $OUT, and
	# lintian exits non-zero over the missing file rather than over a tag.
	DEB_ARCH="$CHROOT_ARCH" sh "$CHROOT" run "cd '/src/$(dirname "$_rel")' && lintian --fail-on error ${_name}_*_$DEB_ARCH.changes" ||
		die "$_name has lintian errors"
}

mkdir -p "$OUT/src"

# ── nothing changed since the last build ────────────────────────────────────
# Every distribution lane builds the packages first, and a full run of them
# built the same tree four times over. A build is skipped when everything it
# is made from is what the last successful build of these targets was made
# from: the versions, the committed tree, every uncommitted change and every
# untracked file under the directories a package is built out of, and b1cc's
# tree the same way. DEB_NOCACHE=1 builds regardless -- the packages lane does,
# since checking the build is its job.
deb_inputs() {
	printf '%s\n' "$DEB_VERSION $B1CC_DEB_VERSION $LIMINE_DEB_VERSION $DEB_ARCH $CHROOT_ARCH"
	printf '%s\n' "$TARGETS" "$KERNEL_COMMON_REPLACES"
	for _tree in "$ROOT_DIR" "$B1CC_DIR"; do
		git -C "$_tree" rev-parse HEAD 2>/dev/null
		git -C "$_tree" diff HEAD 2>/dev/null
		git -C "$_tree" ls-files --others --exclude-standard -z 2>/dev/null |
			xargs -0 -r sha256sum 2>/dev/null
	done
	sha256sum "$0"
}
_stamp="$OUT/.inputs-$DEB_ARCH-$(printf '%s' "$TARGETS" | sha256sum | cut -c1-12)"

if [ $# -gt 0 ]; then
	TARGETS="$*"
elif [ "$DEB_ARCH" = "$CHROOT_ARCH" ]; then
	TARGETS="b1nix-kernel b1nix-meta b1cc limine b1nix-installer-config"
else
	# The architecture-independent packages are built once, natively; another
	# architecture adds its kernel.
	TARGETS="b1nix-kernel"
fi

_fp=$(deb_inputs | sha256sum | cut -c1-64)
if [ "${DEB_NOCACHE:-0}" != 1 ] && [ -f "$_stamp" ] &&
	[ "$(head -1 "$_stamp")" = "$_fp" ] &&
	tail -n +2 "$_stamp" | while read -r _deb; do
		[ -f "$OUT/$_deb" ] || exit 1
	done; then
	log "inputs unchanged since the last build of $TARGETS for $DEB_ARCH -- reusing it"
	tail -n +2 "$_stamp" | sed 's/^/  /' >&2
	exit 0
fi
rm -f "$_stamp"

for t in $TARGETS; do
	case "$t" in
	b1nix-kernel) build_b1nix_kernel ;;
	b1nix-meta) build_b1nix_meta ;;
	b1cc) build_b1cc ;;
	limine) build_limine ;;
	b1nix-installer-config) run_build b1nix-installer-config "$(stage_source b1nix-installer-config)" ;;
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
	_want="$DEB_VERSION"
	case "${_pkg%-dbgsym}" in
	b1cc) _want="$B1CC_DEB_VERSION" ;;
	limine) _want="$LIMINE_DEB_VERSION" ;;
	esac
	[ "$_ver" != "$_want" ] || continue
	log "removing superseded $(basename "$d")"
	rm -f "$d"
done
log "packages in $OUT:"
ls -1 "$OUT"/*.deb 2>/dev/null | sed 's|.*/|  |' >&2 || die "no .deb was produced"
# What this build produced, for the next one to reuse: the input fingerprint,
# then the packages of these targets at the versions just built.
{
	echo "$_fp"
	for d in "$OUT"/*.deb; do
		_v=$(basename "$d" | sed 's/^[^_]*_//; s/_[^_]*$//')
		case "$_v" in
		"$DEB_VERSION" | "$B1CC_DEB_VERSION" | "$LIMINE_DEB_VERSION") basename "$d" ;;
		esac
	done
} >"$_stamp"
