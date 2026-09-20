#!/bin/sh
# A Debian build chroot for the b1nix overlay packages.
#
#   sh tools/packages/debian-chroot.sh create        # fetch and prepare it
#   sh tools/packages/debian-chroot.sh run <cmd...>  # run a command inside it
#   sh tools/packages/debian-chroot.sh shell         # interactive shell
#   sh tools/packages/debian-chroot.sh clean         # delete it
#
# Runs as an ORDINARY USER: no sudo, no debootstrap, no container runtime. The
# rootfs is the official debian:<suite>-slim image layer pulled from the Docker
# Hub registry with curl -- the same flow tools/images/mk-debian-image.sh uses
# for the Debian lane -- and the chroot is entered through a user namespace
# (`unshare -Urm`), where the caller is root and the host is untouched.
#
# Everything is cached under $CHROOT_BASE, so a second run with a warm cache
# never touches the network.
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
DEB_ARCH="${DEB_ARCH:-amd64}"
SUITE="${SUITE:-trixie}"
CHROOT_BASE="${CHROOT_BASE:-$ROOT_DIR/build/packages}"
case "$CHROOT_BASE" in /*) ;; *) CHROOT_BASE="$ROOT_DIR/$CHROOT_BASE" ;; esac

CACHE="$CHROOT_BASE/cache-$SUITE-$DEB_ARCH"
ROOTFS="$CHROOT_BASE/chroot-$SUITE-$DEB_ARCH"
STAMP="$ROOTFS/.b1nix-chroot-ready"

DOCKER_REPO="${DOCKER_REPO:-library/debian}"
DOCKER_TAG="${DOCKER_TAG:-$SUITE-slim}"
MIRROR="${MIRROR:-http://deb.debian.org/debian}"

# What a b1nix overlay package needs to build. Kept short on purpose: a build
# dependency here is one more thing that has to exist on a release day.
BUILD_DEPS="${BUILD_DEPS:-build-essential debhelper dpkg-dev fakeroot lintian gnupg apt-utils xz-utils}"

log() { printf '\033[1;34m[chroot]\033[0m %s\n' "$*" >&2; }
die() { printf '\033[1;31m[chroot] %s\033[0m\n' "$*" >&2; exit 1; }

for t in curl tar python3 sha256sum unshare; do
	command -v "$t" >/dev/null 2>&1 || die "missing host tool: $t"
done
unshare -Urm true 2>/dev/null ||
	die "unprivileged user namespaces are not available; this script needs them instead of sudo"
# dpkg installs files owned by uids other than root (man:man, messagebus, ...).
# A namespace that maps only our own uid to root cannot chown to them -- the
# call fails with EINVAL and the package's unpack fails with it. --map-auto
# maps the subuid block from /etc/subuid as well, which needs an allocation for
# this user and the setuid helpers from the shadow package.
unshare -r --map-auto -m true 2>/dev/null ||
	die "no subuid range for $(id -un): add one to /etc/subuid and /etc/subgid (e.g. '$(id -un):100000:65536') and install newuidmap/newgidmap"

# ── the base layer ──────────────────────────────────────────────────────────
LAYER_TGZ="$CACHE/rootfs.tar.gz"
LAYER_DIGEST_FILE="$LAYER_TGZ.sha256"

verify_layer() {
	[ -f "$LAYER_TGZ" ] || return 1
	[ -f "$LAYER_DIGEST_FILE" ] || return 1
	[ "$(cat "$LAYER_DIGEST_FILE")" = "$(sha256sum "$LAYER_TGZ" | cut -d' ' -f1)" ]
}

fetch_layer() {
	log "fetching $DOCKER_REPO:$DOCKER_TAG ($DEB_ARCH)"
	_tok=$(curl -sfL "https://auth.docker.io/token?service=registry.docker.io&scope=repository:$DOCKER_REPO:pull" |
		sed -n 's/.*"token":"\([^"]*\)".*/\1/p')
	[ -n "$_tok" ] || die "could not get a Docker Hub pull token"

	_a1='application/vnd.oci.image.index.v1+json'
	_a2='application/vnd.docker.distribution.manifest.list.v2+json'
	_a3='application/vnd.oci.image.manifest.v1+json'
	_a4='application/vnd.docker.distribution.manifest.v2+json'

	curl -sfL -H "Authorization: Bearer $_tok" \
		-H "Accept: $_a1" -H "Accept: $_a2" -H "Accept: $_a3" -H "Accept: $_a4" \
		"https://registry-1.docker.io/v2/$DOCKER_REPO/manifests/$DOCKER_TAG" \
		-o "$CACHE/index.json" || die "manifest fetch failed"

	_digest=$(DEB_ARCH="$DEB_ARCH" python3 - "$CACHE/index.json" <<-'PY'
		import json, os, sys
		d = json.load(open(sys.argv[1]))
		want = {"amd64": "amd64", "x86_64": "amd64"}.get(os.environ["DEB_ARCH"], os.environ["DEB_ARCH"])
		for m in d.get("manifests", []):
		    p = m.get("platform", {})
		    if p.get("architecture") == want and p.get("os") == "linux":
		        print(m["digest"])
		        break
	PY
	)
	if [ -n "$_digest" ]; then
		curl -sfL -H "Authorization: Bearer $_tok" -H "Accept: $_a3" -H "Accept: $_a4" \
			"https://registry-1.docker.io/v2/$DOCKER_REPO/manifests/$_digest" \
			-o "$CACHE/manifest.json" || die "$DEB_ARCH manifest fetch failed"
	else
		cp "$CACHE/index.json" "$CACHE/manifest.json"
	fi

	_layer=$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["layers"][0]["digest"])' "$CACHE/manifest.json")
	[ -n "$_layer" ] || die "no layer digest in manifest"
	curl -sfL -H "Authorization: Bearer $_tok" \
		"https://registry-1.docker.io/v2/$DOCKER_REPO/blobs/$_layer" -o "$LAYER_TGZ.part" ||
		die "layer blob fetch failed"
	mv "$LAYER_TGZ.part" "$LAYER_TGZ"
	echo "${_layer#sha256:}" >"$LAYER_DIGEST_FILE"
	verify_layer || die "layer digest mismatch -- the download is corrupt"
}

# Removing a chroot is not a plain rm: apt and dpkg create files owned by the
# subuid range, and outside the namespace that maps it those are owned by uids
# this user does not have, so rm gets EPERM. The deletion has to happen inside
# a namespace with the same mapping.
nsrm() { # path
	[ -e "$1" ] || return 0
	NSRM_PATH="$1" unshare -r --map-auto -m sh -c 'rm -rf -- "$NSRM_PATH"' ||
		die "could not remove $1 -- it holds files owned by the subuid range"
}

# ── entering it ─────────────────────────────────────────────────────────────
# One helper, used by `create` (before the stamp exists) and by `run`. The
# mounts live in the new namespace and disappear with the process; the bind of
# the source tree is what lets a build see the packaging sources.
enter() {
	[ -d "$ROOTFS" ] || die "no chroot at $ROOTFS -- run 'create' first"
	_cmd="$1"
	# -p -f as well as the namespace flags: mounting a fresh procfs is only
	# permitted to a namespace that owns a PID namespace, and unshare has to
	# fork for the new PID namespace to take effect. -r maps us to root;
	# --map-auto adds the subuid block above it, which is the range dpkg needs
	# to chown files to man:man and friends.
	# build/ is a symlink to a volume with room for it, in this tree and in
	# the main checkout alike. A bind of the source directory alone leaves
	# that symlink dangling inside the chroot, so the target is bound at its
	# own absolute path as well and the link resolves.
	_build_real=""
	[ ! -L "$ROOT_DIR/build" ] || _build_real=$(readlink -f "$ROOT_DIR/build")
	CHROOT_ROOTFS="$ROOTFS" CHROOT_SRC="$ROOT_DIR" CHROOT_CMD="$_cmd" \
	CHROOT_BUILD="$_build_real" \
		unshare -r --map-auto -mpf --propagation private sh -c '
		set -eu
		r="$CHROOT_ROOTFS"
		mkdir -p "$r/proc" "$r/sys" "$r/dev" "$r/tmp" "$r/src"
		mount -t proc proc "$r/proc"
		mount --rbind /sys "$r/sys"
		mount --rbind /dev "$r/dev"
		mount --bind "$CHROOT_SRC" "$r/src"
		if [ -n "${CHROOT_BUILD:-}" ] && [ -d "$CHROOT_BUILD" ]; then
			mkdir -p "$r$CHROOT_BUILD"
			mount --bind "$CHROOT_BUILD" "$r$CHROOT_BUILD"
		fi
		# LC_ALL=C keeps the locale of the host out of the chroot: every perl
		# tool in there otherwise warns about locales it does not have, which
		# buries the output that matters.
		LC_ALL=C LANG=C chroot "$r" /bin/sh -c "$CHROOT_CMD"
	'
}

cmd_create() {
	mkdir -p "$CACHE" "$CHROOT_BASE"
	if verify_layer; then
		log "base layer cached -- not downloading"
	else
		fetch_layer
	fi

	if [ -f "$STAMP" ]; then
		log "chroot ready at $ROOTFS"
		return 0
	fi

	nsrm "$ROOTFS"
	mkdir -p "$ROOTFS"
	log "unpacking the base layer"
	# --no-same-owner: we are an ordinary user on the host side. Ownership
	# inside the chroot is irrelevant to a build that runs as its fake root,
	# and dpkg records the ownership it wants in the package itself.
	tar -C "$ROOTFS" --no-same-owner -xzf "$LAYER_TGZ"

	cp /etc/resolv.conf "$ROOTFS/etc/resolv.conf" 2>/dev/null || true
	# apt drops privileges to _apt for downloads, which cannot work when the
	# whole namespace is a fake root with no real uid mapping behind it.
	mkdir -p "$ROOTFS/etc/apt/apt.conf.d"
	cat >"$ROOTFS/etc/apt/apt.conf.d/00b1nix" <<-EOF
		APT::Sandbox::User "root";
		APT::Install-Recommends "false";
		Acquire::Retries "3";
	EOF
	# The slim image already ships /etc/apt/sources.list.d/debian.sources;
	# adding a second entry for the same suite only produces warnings.
	if [ ! -s "$ROOTFS/etc/apt/sources.list.d/debian.sources" ]; then
		printf 'deb %s %s main\n' "$MIRROR" "$SUITE" >"$ROOTFS/etc/apt/sources.list"
	fi

	log "installing build dependencies: $BUILD_DEPS"
	enter "export DEBIAN_FRONTEND=noninteractive; apt-get update -qq && apt-get install -y --no-install-recommends $BUILD_DEPS" ||
		die "installing build dependencies failed"

	date -u +%Y-%m-%dT%H:%M:%SZ >"$STAMP"
	log "chroot ready at $ROOTFS"
}

cmd_run() {
	[ -f "$STAMP" ] || cmd_create
	[ $# -gt 0 ] || die "run: nothing to run"
	enter "cd /src && $*"
}

cmd_shell() {
	[ -f "$STAMP" ] || cmd_create
	enter "cd /src && exec /bin/bash"
}

cmd_clean() {
	log "removing $ROOTFS"
	nsrm "$ROOTFS"
}

case "${1:-create}" in
create) cmd_create ;;
run) shift; cmd_run "$@" ;;
shell) cmd_shell ;;
clean) cmd_clean ;;
*) die "usage: $0 [create|run <cmd...>|shell|clean]" ;;
esac
