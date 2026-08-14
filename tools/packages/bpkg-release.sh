#!/bin/sh
# bpkg-release.sh — build a signed b1nix package repository in Alpine's format.
#
# What this replaces, and why.
#
# The old publisher put one gzipped tar per package into a git repository and
# served it through a CDN that fronts GitHub. That caps a package at what the
# CDN will serve and pays for every byte with git history: the kernel's debug
# symbols alone are 28 MB, Mesa is 55 MB, and a repository that carries a few
# revisions of those is a repository nobody wants to clone.
#
# Release assets have neither limit — two gigabytes per file, as many files as
# you like, and they are not in the history at all. Their URLs are flat:
#
#   https://github.com/<owner>/<repo>/releases/download/<tag>/<file>
#
# which is exactly the shape apk expects, because a package's URL is the
# index's own directory plus "<name>-<version>.apk". So the index and the
# packages become assets of one release and the client needs no special case.
#
# The format is Alpine's, not one of ours: APKINDEX.tar.gz plus .apk files, each
# a concatenation of three gzip members — signature, control (.PKGINFO carrying
# the payload's sha256 as `datahash`), and the payload. bpkg verifies that chain
# against a public key in /etc/apk/keys, so a package that was not signed by us
# does not install, whatever it is named and wherever it was downloaded from.
#
# Usage:
#   tools/packages/bpkg-release.sh [options]
#
#     --manifest <file>   what to package (default: tools/packages/b1nix-packages.list)
#     --key <file.rsa>    private key to sign with (default: build/<arch>/repo-key/b1nix.rsa)
#     --keyname <name>    the name the public key has in /etc/apk/keys
#     --out <dir>         where the repository is written (default: build/<arch>/repo)
#     --repo <owner/name> GitHub repository the assets will live in
#     --tag <tag>         release tag the assets will live under
#     --max-mb <n>        complain above this size per package (default: 75)
#
# It prints the commands that upload the result. It does not upload: publishing
# is a decision, and one that needs credentials this script has no business
# holding.
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
ARCH="${B1NIX_ARCH:-${ARCH:-x86_64}}"
BUILD_DIR="$ROOT_DIR/build/$ARCH"
ROOTFS="$BUILD_DIR/rootfs"

MANIFEST="$ROOT_DIR/tools/packages/b1nix-packages.list"
KEY="$BUILD_DIR/repo-key/b1nix.rsa"
KEYNAME="b1nix-packages.rsa.pub"
OUT="$BUILD_DIR/repo"
GH_REPO="B1nix/b1nix-pkgs"
TAG="pkgs-$ARCH"
MAX_MB=75

while [ $# -gt 0 ]; do
	case "$1" in
	--manifest) MANIFEST="$2"; shift 2 ;;
	--key)      KEY="$2"; shift 2 ;;
	--keyname)  KEYNAME="$2"; shift 2 ;;
	--out)      OUT="$2"; shift 2 ;;
	--repo)     GH_REPO="$2"; shift 2 ;;
	--tag)      TAG="$2"; shift 2 ;;
	--max-mb)   MAX_MB="$2"; shift 2 ;;
	-h|--help)  sed -n '2,40p' "$0"; exit 0 ;;
	*) echo "bpkg-release: unknown option $1" >&2; exit 2 ;;
	esac
done

[ -f "$MANIFEST" ] || { echo "bpkg-release: no manifest at $MANIFEST" >&2; exit 2; }
[ -d "$ROOTFS" ] || { echo "bpkg-release: no rootfs at $ROOTFS — build first" >&2; exit 2; }
command -v openssl >/dev/null || { echo "bpkg-release: openssl is required" >&2; exit 2; }

VERSION="$(sed -n 's/.*B1NIX_VERSION_STR "\(.*\)".*/\1/p' \
	"$ROOT_DIR/kernel/include/b1nix/version.h")"
[ -n "$VERSION" ] || { echo "bpkg-release: cannot read the version" >&2; exit 2; }

#
# The signing key.
#
# Generated on first use and kept out of the tree: a key that lives in the
# repository signs nothing meaningful, since anyone holding the repository can
# sign with it. The public half belongs in the image, and is written here so it
# can be committed once and trusted thereafter.
#
if [ ! -f "$KEY" ]; then
	mkdir -p "$(dirname "$KEY")"
	openssl genrsa -out "$KEY" 4096 2>/dev/null
	echo "bpkg-release: generated a new signing key at $KEY"
	echo "  keep it; packages signed by a different key will not install"
fi
PUBKEY="$BUILD_DIR/repo-key/$KEYNAME"
openssl rsa -in "$KEY" -pubout -out "$PUBKEY" 2>/dev/null

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
rm -rf "$OUT"
mkdir -p "$OUT/$ARCH"

INDEX="$WORK/APKINDEX"
: > "$INDEX"
total=0
count=0

#
# One package.
#
# $1 name, $2 space-separated paths (relative to the rootfs, or absolute for
# things that are not in it, like the kernel), $3 comma-separated dependencies,
# $4 one-line description.
#
build_package() {
	pkg_name="$1"; pkg_paths="$2"; pkg_deps="$3"; pkg_desc="$4"
	stage="$WORK/stage-$pkg_name"
	rm -rf "$stage"; mkdir -p "$stage"

	found=0
	for path in $pkg_paths; do
		case "$path" in
		/*) src="$path"; dest="${path#/}" ;;
		*)  src="$ROOTFS/$path"; dest="$path" ;;
		esac
		[ -e "$src" ] || continue
		mkdir -p "$stage/$(dirname "$dest")"
		cp -a "$src" "$stage/$dest"
		found=1
	done
	if [ "$found" = 0 ]; then
		echo "bpkg-release: $pkg_name has nothing to package; skipped" >&2
		return 0
	fi

	# The payload, and its hash as stored — datahash covers these exact bytes,
	# so it is computed on the member, not on the directory it came from.
	data="$WORK/$pkg_name.data.tar.gz"
	tar --sort=name --owner=0 --group=0 --numeric-owner --mtime='@0' \
	    -czf "$data" -C "$stage" .
	datahash="$(sha256sum "$data" | cut -d' ' -f1)"
	installed="$(du -sb "$stage" | cut -f1)"
	size="$(stat -c %s "$data")"

	# The control member: Alpine's .PKGINFO, with the fields bpkg reads and the
	# ones a human reads.
	ctl="$WORK/ctl-$pkg_name"
	rm -rf "$ctl"; mkdir -p "$ctl"
	{
		echo "# Generated by tools/packages/bpkg-release.sh"
		echo "pkgname = $pkg_name"
		echo "pkgver = $VERSION"
		echo "pkgdesc = $pkg_desc"
		echo "url = https://github.com/$GH_REPO"
		echo "builddate = 0"
		echo "packager = b1nix"
		echo "size = $installed"
		echo "arch = $ARCH"
		echo "origin = $pkg_name"
		echo "datahash = $datahash"
		[ -n "$pkg_deps" ] && printf 'depend = %s\n' "$(echo "$pkg_deps" | tr ',' ' ')"
	} > "$ctl/.PKGINFO"
	control="$WORK/$pkg_name.control.tar.gz"
	tar --sort=name --owner=0 --group=0 --numeric-owner --mtime='@0' \
	    -czf "$control" -C "$ctl" .PKGINFO

	# The signature member signs the control member's stored bytes with
	# RSA-SHA1, which is what apk does and what bpkg checks.
	sig="$WORK/sig-$pkg_name"
	rm -rf "$sig"; mkdir -p "$sig"
	openssl dgst -sha1 -sign "$KEY" -out "$sig/.SIGN.RSA.$KEYNAME" "$control"
	signature="$WORK/$pkg_name.sig.tar.gz"
	tar --sort=name --owner=0 --group=0 --numeric-owner --mtime='@0' \
	    -czf "$signature" -C "$sig" ".SIGN.RSA.$KEYNAME"

	apk="$OUT/$ARCH/$pkg_name-$VERSION.apk"
	cat "$signature" "$control" "$data" > "$apk"
	apk_size="$(stat -c %s "$apk")"
	apk_mb=$((apk_size / 1048576))
	if [ "$apk_mb" -gt "$MAX_MB" ]; then
		echo "bpkg-release: $pkg_name is ${apk_mb} MB, above the ${MAX_MB} MB you asked for" >&2
		echo "  split it in the manifest rather than raising the limit: a package" >&2
		echo "  nobody can install by parts is one everybody downloads whole" >&2
	fi

	{
		echo "C:Q1$(sha1sum "$apk" | cut -d' ' -f1)"
		echo "P:$pkg_name"
		echo "V:$VERSION"
		echo "A:$ARCH"
		echo "S:$apk_size"
		echo "I:$installed"
		echo "T:$pkg_desc"
		echo "U:https://github.com/$GH_REPO"
		echo "L:MIT"
		echo "o:$pkg_name"
		echo "m:b1nix"
		echo "t:0"
		[ -n "$pkg_deps" ] && echo "D:$(echo "$pkg_deps" | tr ',' ' ')"
		echo "p:$pkg_name=$VERSION"
		echo
	} >> "$INDEX"

	total=$((total + apk_size))
	count=$((count + 1))
	printf '  %-24s %6.1f MB  %s\n' "$pkg_name" \
		"$(echo "$apk_size" | awk '{printf "%.1f", $1/1048576}')" "$pkg_desc"
}

echo "b1nix package repository $VERSION ($ARCH)"
# Manifest lines: name | paths | deps | description. Blank lines and # comments
# are skipped, so the file can explain itself.
while IFS='|' read -r m_name m_paths m_deps m_desc; do
	case "$m_name" in ''|\#*) continue ;; esac
	# Trimmed with sed, not xargs: a description that says "b1nix's own
	# utilities" is not a quoting error, and xargs treats it as one.
	m_name="$(printf '%s' "$m_name" | sed 's/^ *//; s/ *$//')"
	m_deps="$(printf '%s' "${m_deps:-}" | sed 's/^ *//; s/ *$//')"
	m_desc="$(printf '%s' "${m_desc:-$m_name}" | sed 's/^ *//; s/ *$//')"
	build_package "$m_name" "$m_paths" "$m_deps" "$m_desc"
done < "$MANIFEST"

tar --sort=name --owner=0 --group=0 --numeric-owner --mtime='@0' \
    -czf "$OUT/$ARCH/APKINDEX.tar.gz" -C "$WORK" APKINDEX
cp "$PUBKEY" "$OUT/$ARCH/$KEYNAME"

echo
printf '%d packages, %.1f MB total\n' "$count" \
	"$(echo "$total" | awk '{printf "%.1f", $1/1048576}')"
echo "written to $OUT/$ARCH"
echo
echo "to publish:"
echo "  gh release create $TAG --repo $GH_REPO --title 'b1nix packages $VERSION' --notes '' || true"
echo "  gh release upload $TAG --repo $GH_REPO --clobber $OUT/$ARCH/*.apk $OUT/$ARCH/APKINDEX.tar.gz"
echo
echo "clients then use:"
echo "  INDEX_URL=https://github.com/$GH_REPO/releases/download/$TAG/APKINDEX.tar.gz"
