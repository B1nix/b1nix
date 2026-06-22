#!/bin/sh
# Populate a rootfs from published packages (default) or locally built ports.
set -eu

ROOTFS="$1"
ARCH="$2"
MODE="${3:-download}"
INDEX_URL="${4:-https://cdn.jsdelivr.net/gh/B1nix/b1nix-pkgs@main/pkgs/index}"
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"

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

	while read -r name version arch sha url extra; do
		case "$name" in ''|'#'*) continue ;; esac
		[ "$arch" = "$PKG_ARCH" ] || continue
		[ -z "${extra:-}" ] || { echo "install-ports: malformed index entry for $name" >&2; exit 1; }
		case "$name:$version:$sha:$url" in
			*[!A-Za-z0-9._:+~@%/=,-]*) echo "install-ports: unsafe index entry for $name" >&2; exit 1 ;;
		esac
		case "$sha:$url" in
			????????????????????????????????????????????????????????????????:https://*) ;;
			*) echo "install-ports: invalid checksum or URL for $name" >&2; exit 1 ;;
		esac

		archive="$tmp/$name.tar.gz"
		echo "FETCH $name $version [$arch]"
		fetch "$url" "$archive"
		[ "$(sha256_of "$archive")" = "$sha" ] || { echo "install-ports: checksum failed for $name" >&2; exit 1; }
		tar -tzf "$archive" | awk '/^\// || /(^|\/)\.\.($|\/)/ { bad=1 } END { exit bad }' || {
			echo "install-ports: unsafe archive paths in $name" >&2; exit 1;
		}
		tar -xzf "$archive" -C "$ROOTFS"
	done < "$tmp/index"
}

local_ports() {
	export B1NIX_ARCH="$ARCH"
	echo "BUILD bash curl wget dropbear netsurf [$PKG_ARCH]"
	bash_src="$("$ROOT_DIR/tools/build-bash.sh")"
	B1NIX_TLS="${B1NIX_TLS:-mbedtls}" "$ROOT_DIR/tools/build-curl.sh"
	B1NIX_TLS="${B1NIX_TLS:-mbedtls}" "$ROOT_DIR/tools/build-wget.sh"
	dropbear_src="$("$ROOT_DIR/tools/build-dropbear.sh" all)"
	netsurf_bin="$("$ROOT_DIR/tools/build-netsurf-fb.sh")"

	mkdir -p "$ROOTFS/bin"
	cp "$bash_src/bash" "$ROOTFS/bin/bash"
	cp "$ROOT_DIR/build/curl-b1nix/$TRIPLET/src/curl" "$ROOTFS/bin/curl"
	cp "$ROOT_DIR/build/wget-b1nix/$TRIPLET/src/wget" "$ROOTFS/bin/wget"
	cp "$dropbear_src/dropbearmulti" "$ROOTFS/bin/dropbearmulti"
	for name in dropbear dbclient dropbearkey; do ln -sfn dropbearmulti "$ROOTFS/bin/$name"; done
	cp "$netsurf_bin" "$ROOTFS/bin/netsurf-fb"
}

mkdir -p "$ROOTFS"
case "$MODE" in
	download) download_ports ;;
	local) local_ports ;;
	*) echo "install-ports: mode must be 'download' or 'local'" >&2; exit 2 ;;
esac
