#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Turn the built .debs into a signed, static apt repository -- a directory tree
# that can be served by GitHub Pages, a web server or file:// with no software
# behind it.
#
#   sh tools/deb/publish-repo.sh                  # build the tree
#   SIGN_KEY=<key id> sh tools/deb/publish-repo.sh   # and sign Release
#   SIGN_KEY="<old> <new>" ...                      # two keys, mid-rotation
#
# The key and its rotation: docs/distro/archive-key.md. GNUPGHOME selects the
# keyring the key lives in.
#
# The layout is the ordinary Debian one, so `apt` needs no special handling:
#
#   <repo>/dists/<suite>/Release[.gpg,InRelease]
#   <repo>/dists/<suite>/main/binary-<arch>/Packages[.gz]
#   <repo>/pool/main/<name>_<version>_<arch>.deb
#
# apt-ftparchive runs inside the Debian chroot; the signature is made on the
# host, because the key belongs to the person doing the release and not to a
# build environment that anything can write to.
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
# Every architecture there is a package for gets its own index; arch-all
# packages appear in each. ARCHES overrides what the pool says.
ARCHES="${ARCHES:-}"
SUITE="${SUITE:-trixie}"
COMPONENT="${COMPONENT:-main}"
ORIGIN="${ORIGIN:-b1nix}"
LABEL="${LABEL:-b1nix}"
IN="${IN:-$ROOT_DIR/build/packages/out}"
REPO="${REPO:-$ROOT_DIR/build/packages/repo}"
SIGN_KEY="${SIGN_KEY:-}"

CHROOT="$ROOT_DIR/tools/deb/debian-chroot.sh"
# The chroot apt-ftparchive runs in; it indexes every architecture.
CHROOT_ARCH="${CHROOT_ARCH:-amd64}"

log() { printf '\033[1;34m[publish-repo]\033[0m %s\n' "$*" >&2; }
die() { printf '\033[1;31m[publish-repo] %s\033[0m\n' "$*" >&2; exit 1; }

ls "$IN"/*.deb >/dev/null 2>&1 || die "no .deb files in $IN -- run tools/deb/build-deb.sh first"

POOL="$REPO/pool/$COMPONENT"
DIST="$REPO/dists/$SUITE"

# ── the pool ────────────────────────────────────────────────────────────────
# Copying rather than moving: $IN is the build output and stays as it is, so a
# failed publish can simply be run again.
mkdir -p "$POOL"
for d in "$IN"/*.deb; do
	cp -f "$d" "$POOL/"
done

[ -n "$ARCHES" ] ||
	ARCHES=$(ls "$POOL"/*.deb | sed -n 's/.*_\([a-z0-9]*\)\.deb$/\1/p' |
		grep -v '^all$' | sort -u | tr '\n' ' ')
ARCHES=$(printf '%s' "$ARCHES" | sed 's/  */ /g; s/^ //; s/ $//')
[ -n "$ARCHES" ] || die "no architecture-specific package in $POOL"

# A version that sorts below what the suite already carries would be invisible
# to every machine that already upgraded, which is the one failure mode of a
# repository that nobody notices until a user reports "there is no update".
for _arch in $ARCHES; do
	BIN="$DIST/$COMPONENT/binary-$_arch"
	[ -f "$BIN/Packages" ] || continue
	for d in "$IN"/*.deb; do
		case "$d" in *_"$_arch".deb | *_all.deb) ;; *) continue ;; esac
		_pkg=$(basename "$d" | sed 's/_.*//')
		_new=$(basename "$d" | sed 's/^[^_]*_//; s/_[^_]*$//')
		_old=$(awk -v p="$_pkg" '
			$1 == "Package:" { cur = $2 }
			$1 == "Version:" && cur == p { print $2 }
		' "$BIN/Packages" | sort -V | tail -1)
		[ -n "$_old" ] || continue
		[ "$_old" = "$_new" ] && continue
		_top=$(printf '%s\n%s\n' "$_old" "$_new" | sort -V | tail -1)
		[ "$_top" = "$_new" ] ||
			die "$_pkg $_new sorts below $_old, which is already published -- refusing"
	done
done

# ── the indices ─────────────────────────────────────────────────────────────
_repo_rel="${REPO#"$ROOT_DIR"/}"
for _arch in $ARCHES; do
	log "generating Packages for $SUITE/$COMPONENT/$_arch"
	mkdir -p "$DIST/$COMPONENT/binary-$_arch"
	DEB_ARCH="$CHROOT_ARCH" sh "$CHROOT" run "cd '/src/$_repo_rel' && apt-ftparchive --arch $_arch packages pool/$COMPONENT >dists/$SUITE/$COMPONENT/binary-$_arch/Packages" ||
		die "apt-ftparchive failed for $_arch"
	DEB_ARCH="$CHROOT_ARCH" sh "$CHROOT" run "cd '/src/$_repo_rel' && gzip -9kf dists/$SUITE/$COMPONENT/binary-$_arch/Packages" ||
		die "compressing Packages failed for $_arch"
done

# by-hash lets a client fetch an index by its checksum, which is what stops a
# mirror that updates mid-fetch from handing out an inconsistent pair.
DEB_ARCH="$CHROOT_ARCH" sh "$CHROOT" run "cd '/src/$_repo_rel' && apt-ftparchive \
	-o APT::FTPArchive::Release::Origin='$ORIGIN' \
	-o APT::FTPArchive::Release::Label='$LABEL' \
	-o APT::FTPArchive::Release::Suite='$SUITE' \
	-o APT::FTPArchive::Release::Codename='$SUITE' \
	-o APT::FTPArchive::Release::Architectures='$ARCHES' \
	-o APT::FTPArchive::Release::Components='$COMPONENT' \
	-o APT::FTPArchive::Release::Description='b1nix overlay for Debian $SUITE' \
	-o APT::FTPArchive::DoByHash=true \
	release dists/$SUITE >dists/$SUITE/Release.tmp && mv dists/$SUITE/Release.tmp dists/$SUITE/Release" ||
	die "generating Release failed"

# ── the signature ───────────────────────────────────────────────────────────
rm -f "$DIST/Release.gpg" "$DIST/InRelease"
if [ -n "$SIGN_KEY" ]; then
	command -v gpg >/dev/null 2>&1 || die "gpg is not installed on the host"
	# One signature per key. During a rotation Release carries two, and a
	# machine that trusts either key accepts it (docs/distro/archive-key.md).
	_users=""
	for k in $SIGN_KEY; do
		_users="$_users --local-user $k"
	done
	log "signing Release with $SIGN_KEY"
	# shellcheck disable=SC2086
	gpg --batch --yes $_users --armor --detach-sign \
		--output "$DIST/Release.gpg" "$DIST/Release" || die "detached signature failed"
	# shellcheck disable=SC2086
	gpg --batch --yes $_users --clearsign \
		--output "$DIST/InRelease" "$DIST/Release" || die "inline signature failed"
	# shellcheck disable=SC2086
	gpg --batch --yes --armor --export $SIGN_KEY >"$REPO/b1nix-archive.asc"
	log "public key(s) written to $REPO/b1nix-archive.asc"
else
	# Saying this once, loudly, beats a release that quietly ships unsigned:
	# apt will refuse the repository, and the person publishing should find
	# that out here rather than from a user.
	log "SIGN_KEY is not set -- the repository is UNSIGNED and apt will refuse it"
	log "this is for local testing only; a published repository is always signed"
fi

log "repository at $REPO"
log "  $(find "$POOL" -name '*.deb' | wc -l | tr -d ' ') package(s), suite $SUITE, architectures $ARCHES"
