#!/bin/sh
# Populate a rootfs from published packages (default) or locally built ports.
#
# Download mode installs EVERY arch-matching entry from the index, which now
# includes the 'kernel' base package (static libs + crt0 + headers), so the
# resulting rootfs is build-capable (b1cc/make can compile + link on-target).
set -eu

ROOTFS="$1"
ARCH="$2"
MODE="${3:-download}"
INDEX_URL="${4:-https://cdn.jsdelivr.net/gh/B1nix/b1nix-pkgs@main/pkgs/index}"
ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"

case "$ARCH" in
	x86) PKG_ARCH=i686; TRIPLET=i686-b1nix ;;
	x86_64) PKG_ARCH=x86_64; TRIPLET=x86_64-b1nix ;;
	*) echo "install-ports: unsupported architecture '$ARCH'" >&2; exit 2 ;;
esac

fetch() {
	if command -v curl >/dev/null 2>&1; then
		curl -fL --retry 3 -o "$2" "$1"
	elif command -v wget >/dev/null 2>&1; then
		wget -O "$2" "$1"
	else
		echo "install-ports: need curl or wget" >&2
		exit 1
	fi
}

sha256_of() {
	if command -v sha256sum >/dev/null 2>&1; then
		sha256sum "$1" | cut -d' ' -f1
	else
		shasum -a 256 "$1" | cut -d' ' -f1
	fi
}

download_ports() {
	tmp="$(mktemp -d)"
	trap 'rm -rf "$tmp"' EXIT HUP INT TERM
	fetch "$INDEX_URL" "$tmp/index"
	state="$ROOTFS/var/lib/bpkg"
	installed="$state/installed"
	mkdir -p "$installed" "$ROOTFS/etc"
	cmp -s "$tmp/index" "$state/index" 2>/dev/null || cp "$tmp/index" "$state/index"
	# Only when the image does not already carry one. The rootfs overlay ships a
	# commented bpkg.conf and is applied after this, so this line's one-liner
	# never survived a build anyway -- it just rewrote the file, which repacked
	# the root image behind it.
	[ -f "$ROOTFS/etc/bpkg.conf" ] ||
		printf "INDEX_URL='%s'\n" "$INDEX_URL" > "$ROOTFS/etc/bpkg.conf"

	# Fields: name version arch sha url [deps]. The 6th 'deps' field is optional
	# (comma-separated package names); we don't resolve deps here (download mode
	# installs EVERY arch-matching entry anyway, incl. 'kernel'), but we must accept
	# it. 'extra' would catch a stray 7th+ field => malformed.
	while read -r name version arch sha url deps extra; do
		case "$name" in ''|'#'*) continue ;; esac
		[ "$arch" = "$PKG_ARCH" ] || continue
		[ -z "${extra:-}" ] || { echo "install-ports: malformed index entry for $name" >&2; exit 1; }
		case "${deps:-}" in *[!A-Za-z0-9._+,-]*) echo "install-ports: bad deps field for $name" >&2; exit 1 ;; esac
		case "$name:$version:$sha:$url" in
			*[!A-Za-z0-9._:+~@%/=,-]*) echo "install-ports: unsafe index entry for $name" >&2; exit 1 ;;
		esac
		case "$sha:$url" in
			????????????????????????????????????????????????????????????????:https://*) ;;
			*) echo "install-ports: invalid checksum or URL for $name" >&2; exit 1 ;;
		esac

		# Already installed, at this exact version and checksum: nothing to
		# download and nothing to unpack.
		#
		# This ran unconditionally, so every build re-downloaded the whole
		# published set and re-extracted it over the staging root. That put the
		# archives' timestamps back on hundreds of files, which made the next
		# step's `cp -u` copy them all again, which made the root image look
		# stale, which cost a full repack -- on a build that had changed
		# nothing. The index is still fetched every time, so a package that
		# moves is still picked up.
		if [ "$(cat "$installed/$name.ver" 2>/dev/null)" = "$version" ] &&
		   [ "$(cat "$installed/$name.sha" 2>/dev/null)" = "$sha" ] &&
		   [ -f "$installed/$name.list" ]; then
			continue
		fi

		archive="$tmp/$name.tar.gz"
		echo "FETCH $name $version [$arch]"
		fetch "$url" "$archive"
		[ "$(sha256_of "$archive")" = "$sha" ] || { echo "install-ports: checksum failed for $name" >&2; exit 1; }
		list="$tmp/$name.list"
		tar -tzf "$archive" > "$list"
		awk '/^\// || /(^|\/)\.\.($|\/)/ { bad=1 } END { exit bad }' "$list" || {
			echo "install-ports: unsafe archive paths in $name" >&2; exit 1;
		}
		tar -xzf "$archive" -C "$ROOTFS"
		cp "$list" "$installed/$name.list"
		printf '%s\n' "$version" > "$installed/$name.ver"
		printf '%s\n' "$sha" > "$installed/$name.sha"
	done < "$tmp/index"
}

local_ports() {
	export B1NIX_ARCH="$ARCH"
	# zsh, curl and dropbear are Alpine packages, installed by the root-image
	# rule from the staging root; NetSurf is not packaged anywhere and is still
	# built here. See docs/ports-migration-plan.md.
	echo "BUILD netsurf [$PKG_ARCH]"
	netsurf_bin="$("$ROOT_DIR/tools/ports/build-netsurf-fb.sh")"

	mkdir -p "$ROOTFS/bin"
	cp "$netsurf_bin" "$ROOTFS/bin/netsurf-fb"
}

# NetSurf runtime resources + the M53 test page. The browser resolves its
# stylesheets/Messages through RESPATH (/usr/local/share/netsurf), and the M53
# smoke loads file:///netsurf/test.html — both used to ride in the kernel
# initramfs and now have to land in the ext4 rootfs alongside the binary, or
# every render test starts with an unstyled/missing document. Runs for both
# install modes: the published netsurf package ships only the ELF.
stage_netsurf_assets() {
	[ -x "$ROOTFS/bin/netsurf-fb" ] || return 0
	res_dir="$(ls -d "$ROOT_DIR/build/$ARCH/ports/netsurf-fb/build/frontends/framebuffer/res" \
		"$ROOT_DIR"/build/src/netsurf/netsurf-*/frontends/framebuffer/res 2>/dev/null | head -1)"
	if [ -n "${res_dir:-}" ]; then
		mkdir -p "$ROOTFS/usr/local/share/netsurf"
		for f in default.css quirks.css internal.css adblock.css Messages; do
			[ -f "$res_dir/$f" ] && cp "$res_dir/$f" "$ROOTFS/usr/local/share/netsurf/$f"
		done
	fi
	mkdir -p "$ROOTFS/netsurf"
	for f in test.html test.png test.svg test.jxl; do
		[ -f "$ROOT_DIR/tools/netsurf-assets/$f" ] &&
			cp "$ROOT_DIR/tools/netsurf-assets/$f" "$ROOTFS/netsurf/$f"
	done
	return 0
}

# The published dropbear package predates shadow-password support: it was built
# without HAVE_SHADOW_H, so it never calls getspnam(3) and falls back to the
# /etc/passwd "x" placeholder. crypt("x") then returns NULL and the daemon
# rejects every password login with "User account 'root' is locked" — the
# M32B-SSH handshake/pty failures. The Makefile builds dropbearmulti locally as
# a dependency of install-ports either way, so prefer that fresher binary over
# the packaged one instead of shipping a knowingly broken SSH server.

# The rest of the published set predates the musl migration: those packages are
# ET_EXEC binaries linked against the retired b1nix libc, while everything else
# in the image is musl PIE. NetSurf is the visible casualty — the stale ELF
# renders file:// pages but every network fetch comes back empty (M53-WEB /
# M53-HTTPS has-content=0) — and the packaged curl/wget were built without a
# working TLS backend. The Makefile builds all of them locally as install-ports
# prerequisites, so prefer the fresh binary whenever one exists; a machine
# without local build output still gets the packaged one.
overlay_local_ports() {
	nsfb="$ROOT_DIR/build/$ARCH/ports/netsurf-fb/install/bin/nsfb"
	[ -f "$nsfb" ] && { cp "$nsfb" "$ROOTFS/bin/netsurf-fb"; echo "OVERLAY netsurf (locally built) [$PKG_ARCH]"; }

	# zsh comes from Alpine's package, installed by the root-image rule.
	return 0
}

# The published package index still carries retired components, and download
# mode installs every arch-matching entry it finds: GNU bash (replaced by zsh)
# and TinyCC (replaced by b1cc as the native compiler). The index lives in a
# separate repo and cannot be fixed from here, so drop them after extraction
# rather than shipping software nothing references. This also clears the rootfs
# staging directory, which is populated incrementally and therefore keeps
# whatever an earlier build left behind.
purge_retired_components() {
	if [ -e "$ROOTFS/bin/bash" ]; then
		rm -f "$ROOTFS/bin/bash"
		echo "PURGE bash (retired in M98; /bin/zsh is the shell) [$PKG_ARCH]"
	fi
	rm -f "$ROOTFS/etc/bash-smoke.sh"
	if [ -e "$ROOTFS/bin/tcc" ]; then
		rm -f "$ROOTFS/bin/tcc"
		echo "PURGE tcc (retired; /bin/b1cc is the native compiler) [$PKG_ARCH]"
	fi
	rm -rf "$ROOTFS/lib/tcc"
	rm -f "$ROOTFS/lib/libtcc1.a"
	return 0
}

mkdir -p "$ROOTFS"
case "$MODE" in
	download) download_ports; overlay_local_ports ;;
	local) local_ports ;;
	*) echo "install-ports: mode must be 'download' or 'local'" >&2; exit 2 ;;
esac

purge_retired_components
stage_netsurf_assets
