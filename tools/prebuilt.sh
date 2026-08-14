#!/bin/sh
# Prebuilt port artifacts: fetch what someone already compiled, or say so.
#
# Some ports cost more than the rest of the build put together — Skia is half a
# gigabyte of installed output, Mesa another two hundred megabytes, and the LLVM
# runtimes are an hour of a machine's life. None of it changes between checkouts
# unless the recipe does, so it does not need to be built twice, let alone once
# per developer.
#
# The design is the one alpine-fetch.sh already uses for packages, applied to
# what we compile ourselves:
#
#   key      a hash of everything that decides the output — the port script, the
#            wrappers it calls, the compiler's version, the architecture. A
#            recipe change changes the key, so a stale artifact can never be
#            mistaken for a current one.
#   lock     tools/packages/prebuilt.lock records key -> sha256. A download is
#            trusted only when its hash is the recorded one; anything else is a
#            different tarball, whatever the server says it is.
#   fallback missing artifact is not an error. `fetch` exits non-zero and the
#            caller builds from source, which is exactly what it did before.
#
# Usage:
#   tools/prebuilt.sh key   <port>      print the cache key
#   tools/prebuilt.sh fetch <port>      unpack a matching artifact, or exit 1
#   tools/prebuilt.sh pack  <port>      package what is built, for uploading
#   tools/prebuilt.sh list              show the ports this knows about
#
# Environment:
#   B1NIX_PREBUILT_DIR   a directory of artifacts, tried first (default:
#                        build/<arch>/prebuilt-cache)
#   B1NIX_PREBUILT_URL   where to download from; empty disables the network
#   B1NIX_PREBUILT_OFF   set to 1 to ignore artifacts entirely and always build
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
ARCH="${B1NIX_ARCH:-${ARCH:-x86_64}}"
BUILD_DIR="$ROOT_DIR/build/$ARCH"
LOCK="$ROOT_DIR/tools/packages/prebuilt.lock"
CACHE_DIR="${B1NIX_PREBUILT_DIR:-$BUILD_DIR/prebuilt-cache}"
PREBUILT_URL="${B1NIX_PREBUILT_URL-https://github.com/B1nix/b1nix-prebuilt/releases/download/latest}"

#
# What each port is made of, and what it leaves behind.
#
# Three fields per line: the port's name, the files whose contents decide the
# build, and the directories the artifact carries. The inputs are listed rather
# than guessed because "the script that builds it" is not the whole story — a
# port compiled through tools/b1nix-musl-cc changes when that wrapper does, and
# an artifact keyed only on the port script would survive a change it should not
# have survived.
#
port_inputs() {
	case "$1" in
	musl)       echo "tools/ports/build-musl.sh tools/toolchain/build-toolchain.sh" ;;
	skia)       echo "tools/ports/build-skia.sh tools/b1nix-clang++ tools/b1nix-musl-cc" ;;
	mesa)       echo "tools/ports/build-mesa.sh tools/b1nix-mesa-cc tools/b1nix-musl-cc" ;;
	netsurf-fb) echo "tools/ports/build-netsurf-fb.sh tools/b1nix-musl-cc" ;;
	libcxx-musl) echo "tools/ports/build-libcxx-musl.sh tools/toolchain/build-toolchain.sh" ;;
	*)          echo "" ;;
	esac
}

port_outputs() {
	case "$1" in
	musl)       echo "ports/musl/install" ;;
	skia)       echo "ports/skia/install" ;;
	mesa)       echo "ports/mesa/install" ;;
	netsurf-fb) echo "ports/netsurf-fb/install" ;;
	libcxx-musl) echo "toolchain/llvm-runtimes-build-musl/install" ;;
	*)          echo "" ;;
	esac
}

known_ports() { echo "musl skia mesa netsurf-fb libcxx-musl"; }

usage() {
	echo "usage: $0 {key|fetch|pack|list} [port]" >&2
	exit 2
}

[ $# -ge 1 ] || usage
CMD="$1"

if [ "$CMD" = list ]; then
	known_ports | tr ' ' '\n'
	exit 0
fi

[ $# -eq 2 ] || usage
PORT="$2"
INPUTS="$(port_inputs "$PORT")"
OUTPUTS="$(port_outputs "$PORT")"
[ -n "$INPUTS" ] || { echo "prebuilt: $PORT is not a known port" >&2; exit 2; }

#
# The key.
#
# The compiler's own version is in here because the same source built by a
# different clang is a different artifact — and that is not hypothetical on a
# rolling-release host, where a toolchain update lands without anything in this
# tree changing.
#
compute_key() {
	# Checked here rather than inside the pipeline below: an `exit` in a
	# subshell ends the subshell, and the key was computed from what remained —
	# a hash that looks fine and means nothing.
	for f in $INPUTS; do
		[ -f "$ROOT_DIR/$f" ] || {
			echo "prebuilt: $PORT names an input that does not exist: $f" >&2
			exit 2
		}
	done
	{
		echo "arch=$ARCH"
		echo "stdlib=${B1NIX_CXX_STDLIB:-default}"
		"${CC:-clang}" --version 2>/dev/null | head -1
		for f in $INPUTS; do
			printf '%s ' "$f"
			sha256sum "$ROOT_DIR/$f" | cut -d' ' -f1
		done
	} | sha256sum | cut -c1-16
}

KEY="$(compute_key)"
ARTIFACT="$PORT-$ARCH-$KEY.tar.zst"

if [ "$CMD" = key ]; then
	echo "$KEY"
	exit 0
fi

locked_sha() {
	[ -f "$LOCK" ] || return 0
	awk -v a="$1" '$1 == a { print $2; exit }' "$LOCK"
}

if [ "$CMD" = fetch ]; then
	[ "${B1NIX_PREBUILT_OFF:-0}" = 1 ] && exit 1

	want="$(locked_sha "$ARTIFACT")"
	[ -n "$want" ] || exit 1   # nothing recorded for this recipe: build it

	src="$CACHE_DIR/$ARTIFACT"
	if [ ! -f "$src" ] && [ -n "$PREBUILT_URL" ]; then
		mkdir -p "$CACHE_DIR"
		# --fail so a 404 page never lands in the cache as if it were a tarball.
		curl -fsSL --retry 2 -o "$src.part" "$PREBUILT_URL/$ARTIFACT" 2>/dev/null &&
			mv "$src.part" "$src" || rm -f "$src.part"
	fi
	[ -f "$src" ] || exit 1

	got="$(sha256sum "$src" | cut -d' ' -f1)"
	if [ "$got" != "$want" ]; then
		echo "prebuilt: $ARTIFACT does not match prebuilt.lock; ignoring it" >&2
		echo "  expected $want" >&2
		echo "  got      $got" >&2
		rm -f "$src"
		exit 1
	fi

	# Unpacked into place only after the hash matched, and over a clean
	# directory: a half-replaced install tree is worse than no artifact.
	for out in $OUTPUTS; do
		rm -rf "${BUILD_DIR:?}/$out"
		mkdir -p "$(dirname "$BUILD_DIR/$out")"
	done
	tar -I zstd -xf "$src" -C "$BUILD_DIR"
	echo "PREBUILT $PORT ($KEY) unpacked into build/$ARCH"
	exit 0
fi

if [ "$CMD" = pack ]; then
	for out in $OUTPUTS; do
		[ -d "$BUILD_DIR/$out" ] || {
			echo "prebuilt: $PORT is not built ($out missing)" >&2
			exit 1
		}
	done
	OUT_DIR="$BUILD_DIR/prebuilt-out"
	mkdir -p "$OUT_DIR"
	# Reproducible enough to be worth re-uploading only when it changed: the
	# same tree packs to the same bytes, so an unchanged port keeps its hash.
	tar --sort=name --owner=0 --group=0 --numeric-owner \
	    --mtime='@0' -I 'zstd -19 -T0' \
	    -cf "$OUT_DIR/$ARTIFACT" -C "$BUILD_DIR" $OUTPUTS
	sha="$(sha256sum "$OUT_DIR/$ARTIFACT" | cut -d' ' -f1)"
	mkdir -p "$(dirname "$LOCK")"
	# One line per artifact, replacing any previous entry for the same name.
	if [ -f "$LOCK" ]; then
		grep -v "^$ARTIFACT " "$LOCK" > "$LOCK.tmp" || true
		mv "$LOCK.tmp" "$LOCK"
	fi
	printf '%s %s\n' "$ARTIFACT" "$sha" >> "$LOCK"
	sort -o "$LOCK" "$LOCK"
	echo "PACKED $OUT_DIR/$ARTIFACT"
	echo "  sha256 $sha (recorded in tools/packages/prebuilt.lock)"
	exit 0
fi

usage
