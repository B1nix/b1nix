#!/bin/sh
# bpkg-smoke.sh - deterministic, offline smoke test for the bpkg package manager.
# Drives the REAL pipeline (curl file:// -> sha256sum -> tar -xzf -> metadata)
# against fixtures pre-staged in the initramfs (/pkgs). Emits BPKG-SMOKE markers
# the host smoke harness greps for. No fake passes: each marker is gated on the
# actual operation having succeeded (or, for checksum-reject, on it FAILING).
echo "BPKG-SMOKE: start"

# bpkg reads INDEX_URL from /etc/bpkg.conf; the test config points it at the
# staged file:// index. Install into a scratch root so we don't touch the live
# rootfs and can assert presence/absence cleanly.
ROOT=/tmp/bpkgroot
rm -rf "$ROOT"
mkdir -p "$ROOT"
rm -rf /var/lib/bpkg
mkdir -p /var/lib
ln -s "$ROOT/var/lib/bpkg" /var/lib/bpkg
export ROOT

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

rm -f /var/lib/bpkg
rm -rf "$ROOT"
echo "BPKG-SMOKE: done"
