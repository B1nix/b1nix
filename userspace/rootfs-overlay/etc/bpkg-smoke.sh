#!/bin/sh
# bpkg-smoke.sh - deterministic, offline smoke test for /bin/bpkg, b1nix's
# native (C, statically linked against musl like every other userspace ELF)
# package manager. Drives the REAL pipeline -- its own hand-rolled gzip/
# deflate + tar decoder + sha256, no shelling out to curl/tar/sha256sum --
# against fixtures pre-staged in the rootfs (/pkgs and /apkrepo). Emits
# BPKG-SMOKE markers the host smoke harness greps for. No fake passes: each
# marker is gated on the actual operation having succeeded (or, for
# checksum-reject, on it FAILING).
echo "BPKG-SMOKE: start"

# bpkg reads INDEX_URL from /etc/bpkg.conf; the test config points it at the
# staged file:// index. Install into a scratch root (via BPKG_ROOT, which
# bpkg honors for file extraction/removal) so we don't touch the live rootfs
# and can assert presence/absence cleanly.
ROOT=/tmp/bpkgroot
rm -rf "$ROOT"
mkdir -p "$ROOT"
# mkdir("/var/lib/bpkg") in ensure_state_dirs() only ever sees a symlink
# already there (EEXIST, checked nowhere) -- it never dereferences the link
# to create the TARGET directory, and bpkg.c's own mkdir calls are single-
# level (no -p). Pre-create the real target tree here so the symlink below
# actually resolves to something bpkg can write into.
mkdir -p "$ROOT/var/lib/bpkg"
rm -rf /var/lib/bpkg
mkdir -p /var/lib
ln -s "$ROOT/var/lib/bpkg" /var/lib/bpkg
export BPKG_ROOT="$ROOT"

# update: fetch the index from file:///pkgs/index via curl.
if bpkg update >/dev/null 2>&1 && [ -f "$ROOT/var/lib/bpkg/index" ]; then
	echo "BPKG-SMOKE: ok update"
else
	echo "BPKG-SMOKE: fail update"
fi

# install: fetch tarball, verify sha256, extract, record metadata.
if bpkg install hello >/dev/null 2>&1 \
	&& [ -f "$ROOT/bin/hello" ] \
	&& [ "$(cat "$ROOT/bin/hello")" = "hello from bpkg" ] \
	&& [ -f /var/lib/bpkg/installed/hello.list ] \
	&& [ "$(cat "$ROOT/var/lib/bpkg/installed/hello.ver")" = "1.0" ]; then
	echo "BPKG-SMOKE: ok install"
else
	echo "BPKG-SMOKE: fail install"
fi

# list: installed package shows up with its version.
if bpkg list 2>/dev/null | grep -q '^hello 1.0$'; then
	echo "BPKG-SMOKE: ok list"
else
	echo "BPKG-SMOKE: fail list"
fi

# checksum-reject: 'badpkg' shares the same tarball but the index lists a wrong
# sha256. install MUST fail (non-zero) and MUST NOT extract anything. This proves
# the integrity check is real, not decorative.
if bpkg install badpkg >/dev/null 2>&1; then
	echo "BPKG-SMOKE: fail checksum-reject"
else
	if [ ! -f "$ROOT/bin/badfile" ] && [ ! -f "$ROOT/var/lib/bpkg/installed/badpkg.ver" ]; then
		echo "BPKG-SMOKE: ok checksum-reject"
	else
		echo "BPKG-SMOKE: fail checksum-reject"
	fi
fi

# remove: delete recorded files + metadata; the installed file must be gone.
if bpkg remove hello >/dev/null 2>&1 \
	&& [ ! -f "$ROOT/bin/hello" ] \
	&& [ ! -f "$ROOT/var/lib/bpkg/installed/hello.ver" ]; then
	echo "BPKG-SMOKE: ok remove"
else
	echo "BPKG-SMOKE: fail remove"
fi

# dep-resolution: 'needsdep' carries deps=dep1 in the index. Installing it MUST
# transitively install 'dep1' first (its file + metadata) AND 'needsdep' itself.
# This proves real dependency resolution, not a parsed-but-ignored field.
if bpkg install needsdep >/dev/null 2>&1 \
	&& [ -f "$ROOT/lib/dep1.flag" ] \
	&& [ -f "$ROOT/var/lib/bpkg/installed/dep1.ver" ] \
	&& [ -f "$ROOT/var/lib/bpkg/installed/needsdep.ver" ] \
	&& [ -f "$ROOT/bin/hello" ]; then
	echo "BPKG-SMOKE: ok dep-resolution"
else
	echo "BPKG-SMOKE: fail dep-resolution"
fi

# apk-format: a real Alpine-shaped repo staged at /apkrepo/<arch> --
# APKINDEX.tar.gz (gzipped tar of one text index) + a genuine .apk (three
# concatenated gzip members: signature, .PKGINFO control, data). Proves the
# apk-container splitter and the P:/V:/A:/D: index parser both work against
# byte-for-byte real Alpine tooling output, not just the house flat format.
export BPKG_INDEX_URL="file:///apkrepo/x86_64/APKINDEX.tar.gz"
rm -rf /var/lib/bpkg
mkdir -p /var/lib
ln -s "$ROOT/var/lib/bpkg" /var/lib/bpkg
if bpkg update >/dev/null 2>&1 \
	&& bpkg install apkhello >/dev/null 2>&1 \
	&& [ -f "$ROOT/usr/bin/apkhello" ] \
	&& grep -q "hello from real apk format" "$ROOT/usr/bin/apkhello" \
	&& [ "$(cat "$ROOT/var/lib/bpkg/installed/apkhello.ver")" = "3.0-r0" ] \
	&& [ ! -f "$ROOT/.PKGINFO" ]; then
	echo "BPKG-SMOKE: ok apk-format"
else
	echo "BPKG-SMOKE: fail apk-format"
fi
unset BPKG_INDEX_URL

rm -f /var/lib/bpkg
rm -rf "$ROOT"
echo "BPKG-SMOKE: done"
