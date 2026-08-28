#!/bin/sh
# Build a Debian (glibc) root filesystem as an ext4 image that b1nix can boot,
# so the Linux-ABI layer is exercised against a real glibc distribution instead
# of against our own musl userspace.
#
#   sh tools/images/mk-debian-image.sh              # build/x86_64/debian.ext4
#   BUILD_DIR=smoke_run/debian-build sh tools/images/mk-debian-image.sh
#
# Runs as an ORDINARY USER: no debootstrap, no dpkg, no loop mounts, no sudo.
# The rootfs is the official debian:bookworm-slim amd64 image layer pulled
# straight from the Docker Hub registry with curl, a handful of .deb packages
# unpacked over it with `ar` + `tar`, and the whole tree handed to `mke2fs -d`.
#
# Everything we download is cached under $BUILD_DIR/debian, so a second run with
# a warm cache never touches the network. This is NEVER part of `make iso`.
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
ARCH="${ARCH:-x86_64}"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build/$ARCH}"
case "$BUILD_DIR" in /*) ;; *) BUILD_DIR="$ROOT_DIR/$BUILD_DIR" ;; esac

CACHE="$BUILD_DIR/debian"
ROOTFS="$CACHE/rootfs"
DEBS="$CACHE/debs"
# Which init the image is built around.
#   sysvinit — Debian's sysvinit-core as PID 1 (the original image)
#   systemd  — Debian's systemd as PID 1, with its full dependency closure
PROFILE="${PROFILE:-sysvinit}"
case "$PROFILE" in
sysvinit)
	IMG="${IMG:-$BUILD_DIR/debian.ext4}"
	IMG_SIZE_MB="${IMG_SIZE_MB:-512}"
	IMG_LABEL="${IMG_LABEL:-b1nix-debian}"
	;;
systemd)
	IMG="${IMG:-$BUILD_DIR/debian-systemd.ext4}"
	IMG_SIZE_MB="${IMG_SIZE_MB:-768}"
	IMG_LABEL="${IMG_LABEL:-b1nix-systemd}"
	;;
*) echo "mk-debian-image: unknown PROFILE '$PROFILE'" >&2; exit 1 ;;
esac

# Docker Hub source of the base rootfs.
DOCKER_REPO="${DOCKER_REPO:-library/debian}"
DOCKER_TAG="${DOCKER_TAG:-bookworm-slim}"

# Debian archive the extra packages come from. Versions are NOT hardcoded: the
# exact .deb filename is resolved from the suite's Packages index, so this keeps
# working across point releases. A package that is not in the index, or a URL
# that 404s, is a hard error.
MIRROR="${MIRROR:-http://deb.debian.org/debian}"
SUITE="${SUITE:-bookworm}"
COMPONENT="${COMPONENT:-main}"
DEB_ARCH="${DEB_ARCH:-amd64}"

# Packages unpacked on top of the slim base:
#   procps        /bin/ps, /usr/bin/top
#   libproc2-0    procps' library
#   libncursesw6  top's curses dependency (slim ships libtinfo6 only)
#   sysvinit-core real /sbin/init + /etc/inittab
#   sysvinit-utils /sbin/killall5 etc, and the shutdown/poweroff helpers
#
# The systemd profile names only what it WANTS; everything those packages
# depend on is resolved from the same index (see resolve_closure below),
# because systemd's dependency tree is 20-odd libraries deep and hand-listing
# it goes stale on the first point release.
case "$PROFILE" in
sysvinit)
	PACKAGES="${PACKAGES:-procps libproc2-0 libncursesw6 sysvinit-core sysvinit-utils}"
	RESOLVE_DEPS="${RESOLVE_DEPS:-0}"
	;;
systemd)
	# systemd-sysv provides /sbin/init -> /lib/systemd/systemd.
	# dbus is what systemctl and systemd's own bus API talk over.
	# udev is systemd-udevd and its rules: without it nothing ever writes
	# /run/udev/data, so no device carries the "systemd" tag and no .device
	# unit can ever activate.
	PACKAGES="${PACKAGES:-systemd systemd-sysv udev dbus procps libproc2-0 libncursesw6}"
	RESOLVE_DEPS="${RESOLVE_DEPS:-1}"
	;;
esac

die() { echo "mk-debian-image: $*" >&2; exit 1; }
log() { echo "[debian-image] $*"; }

# ── fakeroot ────────────────────────────────────────────────────────────────
# We are not root, but the image has to be owned root:root with the distro's
# setuid bits intact — a Debian tree owned by uid 1000 is not the thing we set
# out to test. fakeroot fakes exactly the chown/stat the extraction and mke2fs
# need, and one exec keeps the whole run inside a single fakeroot session so the
# recorded ownership survives from `tar x` through to `mke2fs -d`.
if [ -z "${B1NIX_DEBIAN_FAKEROOT:-}" ]; then
	if command -v fakeroot >/dev/null 2>&1; then
		B1NIX_DEBIAN_FAKEROOT=1
		export B1NIX_DEBIAN_FAKEROOT
		exec fakeroot -- "$0" "$@"
	fi
	echo "[debian-image] WARNING: fakeroot not found — the image will be owned by" >&2
	echo "[debian-image]          uid $(id -u), not root, and setuid bits are lost." >&2
	echo "[debian-image]          Install fakeroot for a faithful Debian tree." >&2
	TAR_OWNER_FLAGS="--no-same-owner"
else
	TAR_OWNER_FLAGS="-p --same-owner"
fi

for t in curl ar tar mke2fs debugfs python3 sha256sum; do
	command -v "$t" >/dev/null 2>&1 || die "missing host tool: $t"
done

mkdir -p "$CACHE" "$DEBS"

# ── 1. Base rootfs layer from the Docker Hub registry ───────────────────────
# Identical flow to smoke_run/debian/fetch.sh: anonymous pull token, image
# index, amd64 manifest, then the single layer blob. The blob is verified
# against the digest the manifest names.
LAYER_TGZ="$CACHE/rootfs.tar.gz"
LAYER_DIGEST_FILE="$CACHE/rootfs.tar.gz.sha256"

verify_layer() {
	[ -f "$LAYER_TGZ" ] || return 1
	[ -f "$LAYER_DIGEST_FILE" ] || return 1
	_want="$(cat "$LAYER_DIGEST_FILE")"
	_have="$(sha256sum "$LAYER_TGZ" | cut -d' ' -f1)"
	[ "$_want" = "$_have" ]
}

fetch_layer() {
	log "fetching $DOCKER_REPO:$DOCKER_TAG ($DEB_ARCH) from the Docker Hub registry"
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

	_amd=$(DEB_ARCH="$DEB_ARCH" python3 - "$CACHE/index.json" <<-'PY'
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
	if [ -n "$_amd" ]; then
		curl -sfL -H "Authorization: Bearer $_tok" -H "Accept: $_a3" -H "Accept: $_a4" \
			"https://registry-1.docker.io/v2/$DOCKER_REPO/manifests/$_amd" \
			-o "$CACHE/manifest.json" || die "$DEB_ARCH manifest fetch failed"
	else
		cp "$CACHE/index.json" "$CACHE/manifest.json"
	fi

	_layer=$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["layers"][0]["digest"])' "$CACHE/manifest.json")
	[ -n "$_layer" ] || die "no layer digest in manifest"
	log "layer $_layer"
	curl -sfL -H "Authorization: Bearer $_tok" \
		"https://registry-1.docker.io/v2/$DOCKER_REPO/blobs/$_layer" -o "$LAYER_TGZ.part" ||
		die "layer blob fetch failed"
	mv "$LAYER_TGZ.part" "$LAYER_TGZ"
	echo "${_layer#sha256:}" >"$LAYER_DIGEST_FILE"
	verify_layer || die "layer digest mismatch — the download is corrupt"
}

if verify_layer; then
	log "base layer cached ($(wc -c <"$LAYER_TGZ") bytes) — not downloading"
elif [ -f "$LAYER_TGZ" ] && [ ! -f "$LAYER_DIGEST_FILE" ]; then
	# Pre-existing tarball with no recorded digest (e.g. copied in by hand).
	# Trust it rather than forcing a download; record nothing.
	log "base layer present but unverified (no recorded digest) — using as-is"
else
	fetch_layer
fi

# ── 2. Extra .deb packages ──────────────────────────────────────────────────
PKG_INDEX="$CACHE/Packages"

fetch_index() {
	[ -s "$PKG_INDEX" ] && return 0
	log "fetching $SUITE/$COMPONENT/$DEB_ARCH Packages index"
	_url="$MIRROR/dists/$SUITE/$COMPONENT/binary-$DEB_ARCH/Packages.xz"
	curl -sfL "$_url" -o "$CACHE/Packages.xz" || die "Packages index fetch failed: $_url"
	if command -v xz >/dev/null 2>&1; then
		xz -dc "$CACHE/Packages.xz" >"$PKG_INDEX" || die "cannot decompress Packages.xz"
	else
		python3 -c 'import lzma,sys;sys.stdout.buffer.write(lzma.open(sys.argv[1]).read())' \
			"$CACHE/Packages.xz" >"$PKG_INDEX" || die "cannot decompress Packages.xz"
	fi
	[ -s "$PKG_INDEX" ] || die "empty Packages index"
}

# resolve <package> -> "<Filename> <SHA256> <Version>" from the index.
resolve_pkg() {
	# The closure already carries Filename/SHA256 for every package it named.
	if [ -s "${CLOSURE_CACHE:-/nonexistent}" ]; then
		_hit=$(awk -v w="$1" '$1 == w { print $2, $3, $4; exit }' "$CLOSURE_CACHE")
		if [ -n "$_hit" ]; then
			echo "$_hit"
			return 0
		fi
	fi
	awk -v want="$1" '
		/^Package: /   { pkg = $2 }
		/^Version: /   { if (pkg == want) ver = $2 }
		/^Filename: /  { if (pkg == want) fn  = $2 }
		/^SHA256: /    { if (pkg == want) sum = $2 }
		/^$/           { if (pkg == want && fn != "") { print fn, sum, ver; exit } }
		END            { if (pkg == want && fn != "") print fn, sum, ver }
	' "$PKG_INDEX"
}

# ── 2a. Dependency closure ──────────────────────────────────────────────────
# Everything the seed packages need that the base layer does not already have.
# "Already have" is read from the layer's own dpkg status file, so a package
# debian:bookworm-slim ships is never downloaded a second time, and a virtual
# package one of them Provides satisfies the dependency exactly as apt would.
CLOSURE_CACHE="$CACHE/closure-$PROFILE.txt"
if [ "$RESOLVE_DEPS" = "1" ]; then
	fetch_index
	if [ ! -s "$CLOSURE_CACHE" ]; then
		log "resolving the dependency closure of: $PACKAGES"
		# The member name is './var/...' in some layers and 'var/...' in
		# others; ask for the one this tarball actually has.
		_st=$(tar tzf "$LAYER_TGZ" | grep -m1 -E '^(\./)?var/lib/dpkg/status$') ||
			die "the base layer has no dpkg status file"
		tar xzf "$LAYER_TGZ" -O "$_st" >"$CACHE/base-status" ||
			die "cannot read the base layer's dpkg status"
		PACKAGES="$PACKAGES" python3 - "$PKG_INDEX" "$CACHE/base-status" \
			>"$CLOSURE_CACHE.new" <<-'PY' || die "dependency resolution failed"
			import os, sys

			def paragraphs(path):
			    para, cur = [], {}
			    key = None
			    for line in open(path, encoding="utf-8", errors="replace"):
			        line = line.rstrip("\n")
			        if not line.strip():
			            if cur:
			                para.append(cur); cur = {}; key = None
			            continue
			        if line[0] in " \t":
			            if key:
			                cur[key] += " " + line.strip()
			            continue
			        if ":" in line:
			            key, _, val = line.partition(":")
			            key = key.strip(); cur[key] = val.strip()
			    if cur:
			        para.append(cur)
			    return para

			def dep_names(field):
			    """['a (>= 1) | b', 'c'] -> [['a','b'], ['c']] (alternatives kept)"""
			    out = []
			    for group in field.split(","):
			        alts = []
			        for alt in group.split("|"):
			            name = alt.strip().split()[0] if alt.strip() else ""
			            name = name.split(":")[0]          # drop :any / :amd64
			            if name:
			                alts.append(name)
			        if alts:
			            out.append(alts)
			    return out

			index = {}
			provides = {}
			for p in paragraphs(sys.argv[1]):
			    name = p.get("Package")
			    if not name or name in index:
			        continue                                # first (highest) wins
			    index[name] = p
			    for v in p.get("Provides", "").split(","):
			        v = v.strip().split()[0].split(":")[0] if v.strip() else ""
			        if v:
			            provides.setdefault(v, name)

			installed = set()
			for p in paragraphs(sys.argv[2]):
			    st = p.get("Status", "")
			    if "installed" not in st:
			        continue
			    name = p.get("Package")
			    if name:
			        installed.add(name)
			    for v in p.get("Provides", "").split(","):
			        v = v.strip().split()[0].split(":")[0] if v.strip() else ""
			        if v:
			            installed.add(v)

			seeds = os.environ["PACKAGES"].split()
			selected, order, queue = set(), [], list(seeds)
			seen = set(seeds)
			while queue:
			    name = queue.pop(0)
			    if name not in index:
			        real = provides.get(name)
			        if not real:
			            if name in installed:
			                continue
			            print("unresolvable dependency: " + name, file=sys.stderr)
			            sys.exit(1)
			        name = real
			    if name in selected:
			        continue
			    selected.add(name); order.append(name)
			    p = index[name]
			    for field in ("Pre-Depends", "Depends"):
			        for alts in dep_names(p.get(field, "")):
			            # apt takes the first alternative it can satisfy; anything
			            # already in the base layer satisfies the whole group.
			            if any(a in installed or a in selected for a in alts):
			                continue
			            pick = next((a for a in alts if a in index or a in provides), None)
			            if pick is None:
			                print("no candidate for: " + " | ".join(alts), file=sys.stderr)
			                sys.exit(1)
			            if pick not in seen:
			                seen.add(pick); queue.append(pick)

			for name in order:
			    if name in installed and name not in seeds:
			        continue
			    p = index[name]
			    print(name, p.get("Filename", ""), p.get("SHA256", ""), p.get("Version", "?"))
		PY
		mv "$CLOSURE_CACHE.new" "$CLOSURE_CACHE"
	fi
	PACKAGES="$(cut -d' ' -f1 <"$CLOSURE_CACHE" | tr '\n' ' ')"
	log "closure ($(wc -l <"$CLOSURE_CACHE" | tr -d ' ') packages): $PACKAGES"
fi

need_download=0
for p in $PACKAGES; do
	[ -f "$DEBS/$p.deb" ] || need_download=1
done

if [ "$need_download" = "1" ]; then
	fetch_index
	for p in $PACKAGES; do
		[ -f "$DEBS/$p.deb" ] && continue
		set -- $(resolve_pkg "$p")
		[ $# -ge 1 ] || die "package '$p' not found in $SUITE/$COMPONENT/$DEB_ARCH Packages"
		_fn="$1"; _sum="${2:-}"; _ver="${3:-?}"
		log "downloading $p $_ver -> $_fn"
		curl -sfL "$MIRROR/$_fn" -o "$DEBS/$p.deb.part" ||
			die "download failed (404?): $MIRROR/$_fn"
		if [ -n "$_sum" ]; then
			_have="$(sha256sum "$DEBS/$p.deb.part" | cut -d' ' -f1)"
			[ "$_have" = "$_sum" ] || die "$p: sha256 mismatch"
		fi
		mv "$DEBS/$p.deb.part" "$DEBS/$p.deb"
		echo "$p $_ver $_fn" >>"$DEBS/versions.txt.new"
	done
	if [ -f "$DEBS/versions.txt.new" ]; then
		cat "$DEBS/versions.txt.new" >>"$DEBS/versions.txt"
		rm -f "$DEBS/versions.txt.new"
	fi
else
	log "all .deb packages cached — not downloading"
fi

# ── 3. Unpack the tree ──────────────────────────────────────────────────────
# Rebuilt from the cached tarballs on every run so the result is deterministic
# and re-running never doubles up half-applied packages.
log "unpacking base layer into $ROOTFS"
rm -rf "$ROOTFS"
mkdir -p "$ROOTFS"
# ./dev/* is excluded: the layer ships device nodes we cannot create as a normal
# user, and b1nix mounts its own devtmpfs there anyway.
tar xzf "$LAYER_TGZ" -C "$ROOTFS" --exclude='./dev/*' --keep-directory-symlink \
	$TAR_OWNER_FLAGS ||
	die "base layer extraction failed"

for p in $PACKAGES; do
	_deb="$DEBS/$p.deb"
	_member=$(ar t "$_deb" | grep '^data\.tar' | head -1)
	[ -n "$_member" ] || die "$p: no data.tar member in the .deb"
	# GNU tar does not sniff the compression of a non-seekable stream, so the
	# decompressor is chosen from the member name instead of guessed.
	case "$_member" in
	data.tar.xz) _dec="xz -dc" ;;
	data.tar.gz) _dec="gzip -dc" ;;
	data.tar.bz2) _dec="bzip2 -dc" ;;
	data.tar.zst) _dec="zstd -dc" ;;
	data.tar) _dec="cat" ;;
	*) die "$p: unknown data member '$_member'" ;;
	esac
	log "unpacking $p ($_member)"
	# --keep-directory-symlink is what makes usr-merge survive: several bookworm
	# packages still ship ./bin, ./lib and ./sbin members, and without it tar
	# REPLACES the base layer's bin -> usr/bin symlinks with real directories,
	# splitting the tree in half (/bin/dash disappears, ps lands in a second
	# /bin). Follow the symlink instead, exactly as dpkg does.
	ar p "$_deb" "$_member" | $_dec |
		tar -x -f - -C "$ROOTFS" --keep-directory-symlink $TAR_OWNER_FLAGS ||
		die "$p: unpack failed"
done

# ── 4. Our test harness (everything we add is prefixed b1nix-) ──────────────
log "staging /b1nix-stage.sh"
cat >"$ROOTFS/b1nix-stage.sh" <<'STAGE_EOF'
#!/bin/sh
# b1nix Debian glibc harness — OUR file, not Debian's.
#
# Booted as `init=/b1nix-stage.sh` (PID 1) or run from /etc/inittab under real
# sysvinit. Every marker below is printed only after the thing it names actually
# worked; a failure prints "DEBIAN-SMOKE: FAIL <what> status=<n>" and the script
# CONTINUES, so one broken stage never hides the others.
PATH=/usr/sbin:/usr/bin:/sbin:/bin:/usr/local/sbin:/usr/local/bin
export PATH
HOME=/root
export HOME
TERM=${TERM:-linux}
export TERM

echo "DEBIAN-SMOKE: start pid=$$"

# ── Stage 1: a glibc dynamic binary ran ────────────────────────────────────
# The marker text itself is produced by the Debian /bin/dash, so it cannot be
# printed unless a real glibc ELF executed and its libc resolved.
if [ -x /bin/dash ]; then
	/bin/dash -c 'echo "DEBIAN-SMOKE: ok stage1-dash"'
	s=$?
	[ $s -eq 0 ] || echo "DEBIAN-SMOKE: FAIL stage1-dash status=$s"
else
	echo "DEBIAN-SMOKE: FAIL stage1-dash status=127 (no /bin/dash)"
fi

uname -a || echo "DEBIAN-SMOKE: FAIL uname status=$?"
if [ -x /lib64/ld-linux-x86-64.so.2 ]; then
	/lib64/ld-linux-x86-64.so.2 --version 2>&1 | head -1 ||
		echo "DEBIAN-SMOKE: FAIL ld-version status=$?"
else
	echo "DEBIAN-SMOKE: FAIL ld-present status=1 (/lib64/ld-linux-x86-64.so.2 missing)"
fi

# ── Stage 2: distro coreutils ──────────────────────────────────────────────
# /proc has to exist before ps, mount and dmesg mean anything.
mount -t proc proc /proc 2>/dev/null || echo "DEBIAN-SMOKE: note mount-proc status=$?"
mount -t sysfs sysfs /sys 2>/dev/null || echo "DEBIAN-SMOKE: note mount-sysfs status=$?"
mount -t devtmpfs devtmpfs /dev 2>/dev/null || echo "DEBIAN-SMOKE: note mount-dev status=$?"

stage2_rc=0
check() { # check <label> <cmd...>  — gating: any non-zero fails stage 2
	_label="$1"
	shift
	"$@"
	_s=$?
	if [ $_s -ne 0 ]; then
		echo "DEBIAN-SMOKE: FAIL $_label status=$_s"
		stage2_rc=1
	fi
}
note() { # note <label> <cmd...>  — informational, does not gate the marker
	_label="$1"
	shift
	"$@"
	_s=$?
	[ $_s -eq 0 ] || echo "DEBIAN-SMOKE: FAIL $_label status=$_s"
}

check stage2-ls ls -l /
check stage2-cat cat /etc/os-release
check stage2-mount mount
check stage2-ps ps
note stage2-id id
dmesg 2>/dev/null | tail -5
if [ $stage2_rc -eq 0 ]; then
	echo "DEBIAN-SMOKE: ok stage2-coreutils"
else
	echo "DEBIAN-SMOKE: FAIL stage2-coreutils status=1"
fi

# ── Stage 3: running under a real init ─────────────────────────────────────
# Read PID 1 from /proc rather than from `ps`: plain `ps` lists only the
# processes sharing this terminal, so it says nothing about init at all.
pid1=$(cat /proc/1/comm 2>/dev/null | tr -d " ")
[ -n "$pid1" ] || pid1=$(ps -o comm= -p 1 2>/dev/null | tr -d " ")
case "$pid1" in
init | sysvinit)
	echo "DEBIAN-SMOKE: ok stage3-init (real init, we are pid $$)"
	;;
b1nix-stage.sh | sh | dash)
	if [ "$$" = "1" ]; then
		echo "DEBIAN-SMOKE: ok stage3-init (harness is pid 1)"
	else
		echo "DEBIAN-SMOKE: FAIL stage3-init status=1 (pid=$$ pid1='$pid1')"
	fi
	;;
*)
	echo "DEBIAN-SMOKE: FAIL stage3-init status=1 (pid=$$ pid1='$pid1')"
	;;
esac

echo "DEBIAN-SMOKE: done"

# Let QEMU exit on its own where possible; the host harness kills it on timeout
# regardless, so none of this is load-bearing.
sync 2>/dev/null
if [ -w /proc/sysrq-trigger ]; then
	echo o >/proc/sysrq-trigger 2>/dev/null
fi
poweroff -f 2>/dev/null
halt -f 2>/dev/null
while :; do
	sleep 60
done
STAGE_EOF
chmod 0755 "$ROOTFS/b1nix-stage.sh"

# /etc/inittab is OUR harness configuration for the case where the kernel boots
# the distro's /sbin/init instead of the stage script directly. sysvinit-core
# ships one; keep it beside ours for reference rather than losing it.
if [ -f "$ROOTFS/etc/inittab" ] && [ ! -f "$ROOTFS/etc/inittab.debian" ]; then
	cp "$ROOTFS/etc/inittab" "$ROOTFS/etc/inittab.debian"
fi
cat >"$ROOTFS/etc/inittab" <<'INITTAB_EOF'
# b1nix test harness inittab (ours, not Debian's; the stock file is
# /etc/inittab.debian). Deliberately has no getty: the harness owns the console.
id:2:initdefault:
si::sysinit:/b1nix-stage.sh
b1:2345:once:/b1nix-stage.sh
INITTAB_EOF
chmod 0644 "$ROOTFS/etc/inittab"

# A couple of files a bare boot expects to exist.
mkdir -p "$ROOTFS/dev" "$ROOTFS/proc" "$ROOTFS/sys" "$ROOTFS/run" "$ROOTFS/tmp"
chmod 1777 "$ROOTFS/tmp"
[ -f "$ROOTFS/etc/fstab" ] || cat >"$ROOTFS/etc/fstab" <<'FSTAB_EOF'
# b1nix test harness fstab
LABEL=b1nix-debian /     ext4  defaults  0 1
proc               /proc proc  defaults  0 0
sysfs              /sys  sysfs defaults  0 0
FSTAB_EOF
[ -f "$ROOTFS/etc/hostname" ] || echo "b1nix-debian" >"$ROOTFS/etc/hostname"

# ── 4b. systemd profile: the configuration dpkg's postinst would have made ──
# Unpacking a .deb runs no maintainer script, so the things `systemctl preset`
# and the sysusers/tmpfiles postinst hooks would have created have to be made
# here. Everything below is ordinary image preparation — enablement symlinks,
# a machine-id, an fstab — not a workaround for anything the kernel gets wrong.
if [ "$PROFILE" = "systemd" ]; then
	log "staging systemd configuration"

	[ -x "$ROOTFS/lib/systemd/systemd" ] || [ -x "$ROOTFS/usr/lib/systemd/systemd" ] ||
		die "systemd binary missing from the unpacked tree"

	# systemd-sysv ships /sbin/init as a symlink; if the tree came without it,
	# say so rather than inventing one.
	[ -e "$ROOTFS/sbin/init" ] || die "/sbin/init missing (systemd-sysv not unpacked?)"

	# A machine-id must exist and be non-empty: an EMPTY one means "first boot"
	# to systemd, which then wants to run systemd-firstboot on the console.
	# 32 hex digits, exactly: anything else is not a machine ID and systemd
	# replaces it from the random pool on every boot.
	printf 'b100b100b100b100b100b100b100b100\n' >"$ROOTFS/etc/machine-id"
	chmod 0444 "$ROOTFS/etc/machine-id"

	# The kernel mounted the root already, and systemd mounts the API
	# filesystems itself. passno 0: nothing should fsck a mounted rw root.
	cat >"$ROOTFS/etc/fstab" <<-FSTAB_EOF
		# b1nix systemd test image
		LABEL=$IMG_LABEL / ext4 defaults 0 0
	FSTAB_EOF

	echo "b1nix" >"$ROOTFS/etc/hostname"

	# Keep the boot inside the test's timeout: a unit that never comes up
	# should fail the run, not spend 90 s per attempt doing it.
	mkdir -p "$ROOTFS/etc/systemd/system.conf.d"
	cat >"$ROOTFS/etc/systemd/system.conf.d/b1nix.conf" <<-'SDCONF_EOF'
		[Manager]
		DefaultTimeoutStartSec=25s
		DefaultTimeoutStopSec=15s
		ShowStatus=yes
		LogLevel=info
		LogTarget=console
		# A service that fails before it can reach the journal otherwise says
		# nothing at all; on a test machine the console is where the evidence
		# has to land.
		DefaultStandardError=journal+console
	SDCONF_EOF

	# journald: no persistent journal directory exists in the image, and the
	# test wants to read the journal back, so keep it in /run.
	mkdir -p "$ROOTFS/etc/systemd/journald.conf.d"
	cat >"$ROOTFS/etc/systemd/journald.conf.d/b1nix.conf" <<-'JCONF_EOF'
		[Journal]
		Storage=volatile
		RuntimeMaxUse=16M
	JCONF_EOF

	# The login prompt: console-getty.service, which runs agetty on
	# /dev/console — on this machine, the serial line.
	#
	# NOT serial-getty@ttyS0.service. That unit is BoundTo=dev-ttyS0.device,
	# and a .device unit only ever becomes active when udev tells systemd about
	# the device. The image now ships udev and the kernel has the whole
	# transport it needs — NETLINK_KOBJECT_UEVENT with SO_ATTACH_FILTER, a
	# writable sysfs `uevent` file for coldplug, and the subsystem links — but
	# systemd-udevd does not yet come up (see docs/debian-systemd-boot.md), so
	# dev-ttyS0 still never appears and the getty would wait out its job
	# timeout. console-getty.service is systemd's own unit for exactly this
	# situation — it is what a container gets — and it depends on nothing but
	# /dev/console existing.
	mkdir -p "$ROOTFS/etc/systemd/system/getty.target.wants"
	ln -sf /lib/systemd/system/console-getty.service \
		"$ROOTFS/etc/systemd/system/getty.target.wants/console-getty.service"

	# An empty root password, so the getty prompt can actually be used from the
	# serial console. This is a test image with no network listener.
	if [ -f "$ROOTFS/etc/shadow" ]; then
		sed -i 's/^root:[^:]*:/root::/' "$ROOTFS/etc/shadow"
	fi

	# ── the harness, as a unit ──────────────────────────────────────────────
	cat >"$ROOTFS/b1nix-systemd-stage.sh" <<'SSTAGE_EOF'
#!/bin/sh
# b1nix systemd harness — OUR file, not Debian's. Run by b1nix-smoke.service
# once multi-user.target is up. Every marker is printed only after the thing it
# names actually worked; failures print SYSTEMD-SMOKE: FAIL and do not stop the
# rest, so one broken stage never hides the others.
PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH
export SYSTEMD_COLORS=0
export SYSTEMD_PAGER=cat
export SYSTEMD_LESS=

# /dev/kmsg is the fallback, and it is not a nicety: console-getty is Type=idle,
# so agetty claims /dev/console (and vhangup()s it) as soon as the boot
# transaction settles -- which is while this script is still running. Writes
# from here then go nowhere, and every marker after that point silently
# vanishes from the serial log. /dev/kmsg reaches the same serial line and
# nobody can take it away.
say() {
	echo "$@" >/dev/kmsg 2>/dev/null ||
		echo "$@" >/dev/console 2>/dev/null ||
		echo "$@"
}
# Bounded, like every other call here: a diagnostic that hangs takes the rest
# of the harness with it, and the markers after it are the point.
run() {
	timeout 25 "$@" 2>&1 | sed 's/^/    /' |
		while IFS= read -r __l; do say "$__l"; done
}

say "SYSTEMD-SMOKE: start pid=$$"

# Deep diagnostics, off unless b1nix.sysd-debug is on the kernel command line:
# they cost seconds and produce hundreds of lines, so a normal run does not pay
# for them. /proc/b1nix-prof prints the kernel's own syscall profile to the
# console and starts a fresh interval, so two reads bracket one operation.
SYSD_DEBUG=0
grep -q 'b1nix.sysd-debug' /proc/cmdline 2>/dev/null && SYSD_DEBUG=1
prof() {
	[ "$SYSD_DEBUG" = 1 ] || return 0
	say "SYSTEMD-SMOKE: --- prof $1 ---"
	cat /proc/b1nix-prof >/dev/null 2>&1
}

# ── 1. systemd is PID 1 ────────────────────────────────────────────────────
pid1=$(tr -d '\000' </proc/1/comm 2>/dev/null | tr -d ' \n')
if [ "$pid1" = "systemd" ]; then
	say "SYSTEMD-SMOKE: ok pid1-systemd"
else
	say "SYSTEMD-SMOKE: FAIL pid1-systemd got='$pid1'"
fi

# ── 2. cgroup v2 ───────────────────────────────────────────────────────────
# systemd refuses to run without a unified hierarchy; prove it is really there
# and that this process is inside the unit's own cgroup.
if [ -f /sys/fs/cgroup/cgroup.controllers ]; then
	say "SYSTEMD-SMOKE: controllers=$(cat /sys/fs/cgroup/cgroup.controllers)"
	mycg=$(sed -n 's/^0:://p' /proc/self/cgroup)
	case "$mycg" in
	*b1nix-smoke.service*)
		say "SYSTEMD-SMOKE: ok cgroup2 ($mycg)"
		;;
	*)
		say "SYSTEMD-SMOKE: FAIL cgroup2 cgroup='$mycg'"
		;;
	esac
else
	say "SYSTEMD-SMOKE: FAIL cgroup2 (no /sys/fs/cgroup/cgroup.controllers)"
fi

# ── 3. the manager answers ─────────────────────────────────────────────────
state=$(systemctl is-system-running 2>&1)
say "SYSTEMD-SMOKE: is-system-running=$state"
case "$state" in
running | degraded | starting)
	say "SYSTEMD-SMOKE: ok systemctl-state"
	;;
*)
	say "SYSTEMD-SMOKE: FAIL systemctl-state got='$state'"
	;;
esac

say "SYSTEMD-SMOKE: --- systemctl list-units --failed ---"
run systemctl --no-pager --no-legend --plain list-units --state=failed
say "SYSTEMD-SMOKE: --- systemctl list-units (active) ---"
run systemctl --no-pager --no-legend --plain list-units --state=active

# What each failed unit actually said. Without this a failure is a name and a
# status number, and the reason is sitting in the journal unread.
for u in $(systemctl --no-pager --no-legend --plain list-units --state=failed |
	awk '{print $1}'); do
	say "SYSTEMD-SMOKE: --- journal: $u ---"
	run journalctl -u "$u" -b --no-pager -n 12
done

# ── 4. specific units really reached active ────────────────────────────────
for u in systemd-journald.service systemd-tmpfiles-setup.service \
	systemd-udevd.service systemd-logind.service \
	sysinit.target basic.target multi-user.target; do
	st=$(systemctl is-active "$u" 2>&1)
	if [ "$st" = "active" ]; then
		say "SYSTEMD-SMOKE: ok unit-active $u"
	else
		say "SYSTEMD-SMOKE: FAIL unit-active $u state=$st"
	fi
done

# ── 4b. past multi-user: graphical.target ──────────────────────────────────
# graphical.target is what a desktop machine's default target is, and reaching
# it is the honest measure of "further than multi-user". It Requires
# multi-user.target and Wants display-manager.service; a machine with no
# display manager installed still reaches it, on Linux and here. The boot was
# asked for multi-user.target, so this starts it explicitly and reports the
# target's own state rather than an impression of it.
gstate=$(timeout 30 systemctl start graphical.target 2>&1)
grc=$?
gst=$(timeout 15 systemctl is-active graphical.target 2>&1)
if [ "$gst" = "active" ]; then
	say "SYSTEMD-SMOKE: ok unit-active graphical.target"
else
	say "SYSTEMD-SMOKE: FAIL unit-active graphical.target state=$gst rc=$grc out='$gstate'"
fi
# Where the boot's time went, by unit, and the chain that decided it. This is
# the report the milestone is measured by, so it is printed on every run and
# not only under b1nix.sysd-debug.
say "SYSTEMD-SMOKE: --- systemd-analyze ---"
run sh -c 'timeout 25 systemd-analyze 2>&1'
say "SYSTEMD-SMOKE: --- systemd-analyze critical-chain ---"
run sh -c 'timeout 30 systemd-analyze critical-chain 2>&1 | head -30'
say "SYSTEMD-SMOKE: --- systemd-analyze blame (top 12) ---"
run sh -c 'timeout 30 systemd-analyze blame 2>&1 | head -12'

# ── 5. the journal ─────────────────────────────────────────────────────────
# journalctl reading back its own records is the end-to-end test of
# /dev/kmsg, the journald sockets and the on-disk (well, /run) journal files.
if journalctl -b --no-pager -n 5 >/tmp/jout 2>/tmp/jerr; then
	lines=$(wc -l </tmp/jout | tr -d ' ')
	if [ "$lines" -gt 0 ]; then
		say "SYSTEMD-SMOKE: ok journalctl ($lines lines)"
		run cat /tmp/jout
	else
		say "SYSTEMD-SMOKE: FAIL journalctl (empty)"
	fi
else
	say "SYSTEMD-SMOKE: FAIL journalctl status=$?"
	run cat /tmp/jerr
fi

# ── 6. the manager can start something on demand ───────────────────────────
# systemd-run exercises the whole path: bus call to PID 1, a transient unit,
# a new cgroup, fork+exec, and the exit status coming back.
out=$(systemd-run --quiet --wait --pipe --collect \
	/bin/sh -c 'echo b1nix-transient-ok' 2>&1)
case "$out" in
*b1nix-transient-ok*)
	say "SYSTEMD-SMOKE: ok systemd-run"
	;;
*)
	say "SYSTEMD-SMOKE: FAIL systemd-run out='$out'"
	;;
esac

# ── 7. a getty is running on the console ───────────────────────────────────
gst=$(systemctl is-active console-getty.service 2>&1)
if [ "$gst" = "active" ]; then
	say "SYSTEMD-SMOKE: ok console-getty"
else
	say "SYSTEMD-SMOKE: FAIL console-getty state=$gst"
fi

# ── 8. udev is really running, not merely "active" ─────────────────────────
# udevadm --ping is a round trip over /run/udev/control: the daemon answers it
# itself, so a reply is the daemon, not systemd's opinion of it.
if timeout 15 udevadm control --ping >/dev/null 2>&1; then
	say "SYSTEMD-SMOKE: ok udevadm-ping"
else
	say "SYSTEMD-SMOKE: FAIL udevadm-ping rc=$?"
	say "SYSTEMD-SMOKE: --- udev diagnosis ---"
	run systemctl status --no-pager -l systemd-udevd.service
	run ls -la /run/udev
	# Which syscall every task in the machine is sitting in, sampled at the
	# moment udevd stopped answering. Printed by the kernel to the console.
	cat /proc/b1nix-tasks >/dev/null 2>&1
fi
# What udevd itself said. It is the only account of what it did with the
# events it was sent, and it exists now that journald runs.
say "SYSTEMD-SMOKE: --- journal: systemd-udevd.service ---"
run journalctl -u systemd-udevd.service -b --no-pager -n 15

# Coldplug, now. systemd-udev-trigger.service ran during sysinit, when udevd
# was still failing to start, so those events had no listener; replaying them
# is what a working udevd is for and is the only way a .device unit can appear
# on this boot.
timeout 20 udevadm trigger --action=add >/dev/null 2>&1 ||
	say "SYSTEMD-SMOKE: udevadm trigger rc=$?"
sleep 2
timeout 20 udevadm settle >/dev/null 2>&1 || true

# ── 9. a .device unit really activated ─────────────────────────────────────
# A .device unit exists only because udev told systemd about the device; the
# manager cannot invent one. Print the list, then name the one we found.
say "SYSTEMD-SMOKE: --- systemctl list-units --type=device ---"
run systemctl --no-pager --no-legend --plain list-units --type=device --state=active
# Polled, not sampled once. A .device unit appears when the manager's device
# monitor receives the event udevd re-broadcasts after processing, and those
# are two asynchronous programs: asking once, at whatever moment this line is
# reached, measures the scheduler as much as the kernel. Twenty seconds is far
# longer than the path takes when it works.
dev_unit=""
__i=0
while [ $__i -lt 12 ]; do
	dev_unit=$(timeout 15 systemctl --no-pager --no-legend --plain list-units \
		--type=device --state=active 2>/dev/null | awk 'NF {print $1; exit}')
	case "$dev_unit" in *.device) break ;; esac
	__i=$((__i + 1))
	sleep 1
done
case "$dev_unit" in
*.device)
	say "SYSTEMD-SMOKE: ok device-unit $dev_unit"
	;;
*)
	say "SYSTEMD-SMOKE: FAIL device-unit none active"
	# A .device unit exists only if udev enumerated a device, processed it and
	# told systemd. Print each link of that chain rather than the verdict.
	say "SYSTEMD-SMOKE: --- udev enumeration diagnosis ---"
	run sh -c 'timeout 15 udevadm info /sys/class/block/vda 2>&1 | head -24'
	# The manager's own account of what it did with the event, rather than the
	# absence of a unit. PID 1 only creates a .device unit from a message its
	# device monitor accepted, and every reason it can refuse one is logged at
	# debug level and nowhere else.
	# Debug for ONE device and then straight back to info: at debug level this
	# manager writes faster than the serial console drains, and a machine that
	# slow times out its own udev workers -- which then looks like the defect
	# being investigated.
	run sh -c 'timeout 15 systemctl log-level debug 2>&1'
	run sh -c 'timeout 20 udevadm trigger --action=change --subsystem-match=block --sysname-match=vda 2>&1'
	sleep 2
	run sh -c 'timeout 15 systemctl log-level info 2>&1'
	run sh -c 'journalctl -b --no-pager -n 150 2>/dev/null | grep -ai "device\|monitor" | tail -25'
	# Which syscall every task is sitting in, at the moment the units are
	# missing. A udev worker that stopped answering is a task parked
	# somewhere, and this names the place -- the alternative is guessing from
	# the outside about a program that has stopped saying anything.
	cat /proc/b1nix-tasks >/dev/null 2>&1
	run sh -c 'timeout 15 udevadm settle --timeout=5 2>&1; echo "settle rc=$?"'
	run sh -c 'ls /run/udev/data 2>&1 | tr "\n" " "'
	# Whether /dev looks like a devtmpfs to the test systemd applies: the mount
	# id of /dev, and the mountinfo row that id must name.
	run sh -c 'grep -a " /dev " /proc/self/mountinfo 2>&1'
	run sh -c 'timeout 15 systemctl --no-pager --no-legend --plain list-units --type=device --all 2>&1 | head -20'
	;;
esac

# Deep udev diagnosis: what the daemon says with udev.log_level=debug, and
# what the device itself looks like in sysfs -- the subsystem link, the uevent
# file and the properties udev derived from them.
if [ "$SYSD_DEBUG" = 1 ]; then
	say "SYSTEMD-SMOKE: --- udev deep: journal ---"
	run sh -c 'journalctl -u systemd-udevd.service -b --no-pager -n 120 2>&1'
	say "SYSTEMD-SMOKE: --- udev deep: sysfs vda ---"
	run sh -c 'ls -la /sys/class/block/ 2>&1 | head -10'
	run sh -c 'ls -la /sys/block/vda/ 2>&1 | head -30'
	run sh -c 'readlink -f /sys/class/block/vda 2>&1'
	# The path block_get_whole_disk() actually builds is
	# /sys/dev/block/<major>:<minor>/queue -- not the /sys/block one -- so the
	# view that decides the answer is listed rather than assumed from another.
	run sh -c 'ls -la /sys/dev/block/ 2>&1 | head -6; ls -la /sys/dev/block/8:0/ 2>&1'
	run sh -c 'ls -la /sys/block/loop0/ 2>&1 | head -10'
	# /sys/dev/char: the view libudev uses to find a character device by its
	# number, and libdrm to find the bus a DRM node sits on.
	run sh -c 'echo "char dir: [$(ls /sys/dev/char/ 2>&1 | tr "\n" " ")]"'
	run sh -c 'ls -la /sys/dev/char/226:0/ 2>&1; cat /sys/dev/char/226:0/uevent 2>&1'
	run sh -c 'ls -l /dev/dri 2>&1 | head -6'
	# The PCI bus tree, and the cheap proof it works: a device with a parent.
	# udevadm walks /sys/devices upward, so "looking at parent device" appears
	# only when the node really has one.
	run sh -c 'ls /sys/devices/pci0000:00/ 2>&1 | head -12'
	run sh -c 'ls /sys/bus/pci/devices/ 2>&1 | head -12'
	run sh -c 'for d in /sys/bus/pci/devices/*; do echo "$d vendor=$(cat $d/vendor 2>&1) device=$(cat $d/device 2>&1) class=$(cat $d/class 2>&1)"; done 2>&1 | head -12'
	run sh -c 'timeout 15 udevadm info /dev/dri/card0 2>&1 | head -20; echo "---"; timeout 15 udevadm info /dev/vda 2>&1 | grep -i "parent\|P:\|E: SUBSYSTEM" | head -10'
	run sh -c 'ls -la /sys/devices 2>&1 | head -20'
	run sh -c 'cat /sys/class/block/vda/uevent 2>&1'
	run sh -c 'ls -la /sys/class/block/vda/subsystem 2>&1; readlink /sys/class/block/vda/subsystem 2>&1'
	run sh -c 'ls -la /run/udev 2>&1; ls -la /run/udev/data 2>&1 | head -20'
	# systemd asks "is this a whole disk?" by looking for
	# /sys/dev/block/<major>:<minor>/queue -- NOT /sys/block/<name>/queue.
	# Every block device in this machine answers "Failed to get whole disk
	# device: No such file or directory", so this prints the exact path it
	# looks at and whether it is there.
	say "SYSTEMD-SMOKE: --- udev deep: /sys/dev/block ---"
	run sh -c '[ -e /sys/dev/block/8:0/queue ] && echo "direct-first: 8:0/queue exists" || echo "direct-first: 8:0/queue MISSING"'
	# And from inside the kind of mount namespace systemd-udevd.service runs
	# in. The harness's own shell is in the initial namespace; the worker that
	# answers "No such file or directory" is not, and "it is there" from the
	# wrong namespace is not an answer.
	run sh -c 'timeout 30 systemd-run --quiet --wait --pipe -p MountFlags=slave -p PrivateMounts=yes /bin/sh -c "[ -e /sys/dev/block/8:0/queue ] && echo ns: queue exists || echo ns: queue MISSING; ls /sys/dev/block 2>&1 | head -3" 2>&1'
	run sh -c 'ls -la /sys/dev/block/ 2>&1 | head -24'
	run sh -c 'for d in /sys/dev/block/*; do printf "%s queue=%s partition=%s\n" "$d" "$([ -e "$d/queue" ] && echo yes || echo NO)" "$([ -e "$d/partition" ] && echo yes || echo no)"; done 2>&1 | head -24'
	run sh -c 'cat /sys/class/block/vda/uevent 2>&1; echo "--- /sys/block/vda:"; ls /sys/block/vda 2>&1'
	run sh -c 'ls -la /dev/vda /dev/disk 2>&1 | head -12'
	run sh -c 'ls /dev 2>&1 | tr "\n" " " | cut -c1-300'
	say "SYSTEMD-SMOKE: --- udev deep: udevadm info ---"
	run sh -c 'timeout 15 udevadm info --path=/sys/class/block/vda 2>&1 | head -30'
	run sh -c 'timeout 15 udevadm test /sys/class/block/vda 2>&1 | tail -40'
	say "SYSTEMD-SMOKE: --- udev deep: ping again ---"
	run sh -c 'timeout 15 udevadm control --ping 2>&1; echo "ping rc=$?"'
	run sh -c 'timeout 10 udevadm control --log-priority=debug 2>&1; echo "logprio rc=$?"'
fi

# ── 10. logind answers on the bus ──────────────────────────────────────────
# is-active is systemd's bookkeeping; loginctl is a method call logind serves.
# Bounded: a bus call to a logind that is not there blocks for the client's
# whole 25 s default, and this harness must always reach its last marker.
if timeout 15 loginctl --no-pager --no-legend list-seats >/tmp/seats 2>/tmp/seaterr; then
	say "SYSTEMD-SMOKE: ok logind-answers seats=$(wc -l </tmp/seats | tr -d ' ')"
else
	say "SYSTEMD-SMOKE: FAIL logind-answers"
	run cat /tmp/seaterr
fi

if [ "$(systemctl is-active systemd-logind.service 2>&1)" != "active" ] ||
   [ "$SYSD_DEBUG" = 1 ]; then
	say "SYSTEMD-SMOKE: --- logind diagnosis ---"
	run systemctl status --no-pager -l systemd-logind.service
	run ls -la /lib/systemd/systemd-logind
	run sh -c 'journalctl -u systemd-logind.service -b --no-pager -n 60 2>&1'
	run sh -c 'journalctl -u dbus.service -b --no-pager -n 40 2>&1'
	run sh -c 'systemctl show systemd-logind.service -p Result -p ExecMainStatus -p ExecMainCode -p ActiveState -p SubState 2>&1'
	run sh -c 'timeout 15 /lib/systemd/systemd-logind --help >/dev/null 2>&1; echo "logind --help rc=$?"'
	run sh -c 'ls -la /run/systemd/sessions /run/systemd/seats /run/systemd/users 2>&1'
	# Whether what is missing is one directory or a whole shape of the tree:
	# the same question /sys/block/vda/queue turned out to be. A unit's start
	# task creates its cgroup and its RuntimeDirectory, so both trees get
	# listed rather than the one that is currently suspected.
	run sh -c 'ls -la /run/systemd 2>&1 | head -30'
	run sh -c 'ls -la /sys/fs/cgroup 2>&1 | head -30'
	run sh -c 'ls -la /sys/fs/cgroup/system.slice 2>&1 | head -12'
	run sh -c 'cat /sys/fs/cgroup/cgroup.controllers /sys/fs/cgroup/cgroup.subtree_control 2>&1'
fi

# ── diagnosis: what a down journald leaves behind ──────────────────────────
# journalctl reads nothing when journald never opened its runtime journal, and
# the reason is in the directory it failed to create, not in the journal.
if [ "$(systemctl is-active systemd-journald.service 2>&1)" != "active" ]; then
	say "SYSTEMD-SMOKE: --- journald diagnosis ---"
	run ls -la /run/log
	run ls -la /run/log/journal
	run cat /etc/machine-id
	run systemctl status --no-pager -l systemd-journald.service
fi

# ── 12. the unit types nothing exercised ────────────────────────────────
#
# 16 checks stood against an image carrying 225 unit files and 45 helper
# daemons. What follows drives the parts systemd is actually built out of --
# socket activation, timers, sandboxing, the readiness protocol -- because a
# manager that reaches multi-user.target proves only that it can start
# services in order, and every one of these exercises a different kernel
# surface underneath.

# Units go in /run/systemd/system, which has to exist before anything is
# written into it -- a unit file that never landed makes every check below it
# report "inactive", which reads exactly like a broken manager. And systemctl's
# errors are kept: swallowing them is how a harness ends up measuring itself.
mkdir -p /run/systemd/system
# Two things that must be true before any unit below can start, printed rather
# than assumed: the directory exists and takes a file, and the manager accepts
# a reload. "Unit not found" is what both failures look like from the outside.
say "SYSTEMD-SMOKE:   unitdir=[$(ls -d /run/systemd/system 2>&1)] write=[$(: >/run/systemd/system/.probe 2>&1 && echo ok || echo failed)]"
# A unit file the manager has not read is "not found" -- which is the right
# answer, and not a kernel failure. Every unit below is written into
# /run/systemd/system at runtime, and the six that were written AFTER the single
# daemon-reload could never start: the harness was measuring its own ordering.
# The reload no longer costs 90 s (it is under a second), so each new unit file
# is followed by one.
sd_load() {
	__lr=$(timeout 60 systemctl daemon-reload 2>&1)
	__lrc=$?
	[ "$__lrc" = 0 ] ||
		say "SYSTEMD-SMOKE:   daemon-reload for $1 rc=$__lrc: ${__lr:-no output}"
}
# Lines in a file that may not exist. `wc -l < missing` is the SHELL failing to
# open the redirect, before wc runs at all, so `2>/dev/null` on wc silences
# nothing and `|| echo 0` never fires -- the error went to stderr, into the
# journal, and became the newest entry for this unit. The journal-index check
# below then read it back instead of a marker and called the index broken.
count_lines() {
	[ -f "$1" ] || { echo 0; return; }
	wc -l <"$1" 2>/dev/null || echo 0
}

sd_reload() {
	prof "before daemon-reload"
	__t0=$(cut -d' ' -f1 /proc/uptime 2>/dev/null)
	__r=$(timeout 120 systemctl daemon-reload 2>&1)
	__rc=$?
	__t1=$(cut -d' ' -f1 /proc/uptime 2>/dev/null)
	prof "after daemon-reload"
	# rc as well as the text. A `timeout`-killed systemctl prints nothing, so
	# reporting "${__r:-ok}" alone would call a reload that was cut off at two
	# minutes a success -- a number that reads as a measurement and is not one.
	# rc=124 is the timeout; anything else is what systemctl itself returned.
	say "SYSTEMD-SMOKE:   daemon-reload ${__t0}->${__t1} rc=${__rc}: ${__r:-no output}"
}
sd_start() {
	__u="$1"
	__t0=$(cut -d' ' -f1 /proc/uptime 2>/dev/null)
	__err=$(timeout 25 systemctl start "$__u" 2>&1)
	__rc=$?
	# Every query bounded too. An unbounded `systemctl is-active` against a
	# manager that has stopped answering is what turned a 25 s failure into a
	# five-minute one, and it hid which call was actually stuck.
	__st=$(timeout 15 systemctl is-active "$__u" 2>&1)
	__t1=$(cut -d' ' -f1 /proc/uptime 2>/dev/null)
	if [ "$__st" = "active" ] || [ "$__st" = "activating" ]; then
		return 0
	fi
	say "SYSTEMD-SMOKE:   start $__u ${__t0}->${__t1} rc=$__rc: ${__err:-no error text} state=$__st"
	# The manager not answering is a fact about PID 1, and /proc still works
	# when the bus does not: this says whether it is running, blocked or gone.
	if [ "$SYSD_DEBUG" = 1 ]; then
		run sh -c 'cat /proc/1/stat 2>&1 | cut -c1-200'
		run sh -c 'cat /proc/1/status 2>&1 | head -12'
		run sh -c 'ls -l /proc/1/fd 2>&1 | wc -l'
		# Which syscall every task is sitting in, sampled at the moment the
		# manager stopped answering.
		cat /proc/b1nix-tasks >/dev/null 2>&1
	fi
	return 1
}

# Socket activation. This is systemd's central idea: the manager holds the
# listening socket and starts the service on the first connection, handing the
# descriptor over.
#
# Over TCP on loopback rather than AF_UNIX, for one practical reason: this
# image carries no socat, nc, curl or python, and bash's /dev/tcp is the only
# connector present. It costs nothing -- the manager's side of socket
# activation is the same either way -- and it exercises the loopback path too.
# Two probes with no systemd in them at all, so a failure here is the kernel's
# and a failure above it is the manager's.
if [ "$SYSD_DEBUG" = 1 ]; then
	# 1. Does `timeout` measure time correctly? Every bounded call in this
	#    harness rests on it, and an rc=124 that arrives early would make a
	#    working daemon look hung -- so the instrument is checked before its
	#    readings are believed. Written to a file rather than passed with -e:
	#    a multi-line perl program inside a shell single-quoted string is one
	#    stray quote away from a syntax error that looks like a kernel fault.
	# Does a plain kill reach a sleeping child at all? `timeout` returns the
	# right answer and does not bound anything, and the two candidate causes --
	# "the alarm is wrong" and "the signal never lands" -- are separated by
	# doing the same thing without timeout(1) in the way.
	say "SYSTEMD-SMOKE: --- kill-sleeper probe ---"
	run sh -c 'sleep 30 & __p=$!; sleep 1; kill -TERM $__p; wait $__p; echo "kill-sleeper rc=$? (137/143 = signalled)"'
	run sh -c '__a=$(cut -d" " -f1 /proc/uptime); sleep 30 & __p=$!; sleep 1; kill -KILL $__p; wait $__p >/dev/null 2>&1; __b=$(cut -d" " -f1 /proc/uptime); echo "kill-sleeper KILL elapsed ${__a}->${__b}"'
	say "SYSTEMD-SMOKE: --- timer probe ---"
	for __spec in "3 60 124" "10 60 124" "10 2 0"; do
		set -- $__spec
		__lim=$1; __slp=$2; __want=$3
		__a=$(cut -d' ' -f1 /proc/uptime)
		timeout "$__lim" sleep "$__slp" >/dev/null 2>&1
		__rc=$?
		__b=$(cut -d' ' -f1 /proc/uptime)
		say "  timerprobe: timeout $__lim sleep $__slp -> rc=$__rc (want $__want) elapsed ${__a}->${__b}"
	done

	# 1b. systemd's PrivateTmp sequence, reproduced with three commands.
	#     setup_one_tmp_dir() does mkdtemp("/tmp/systemd-private-<id>-<unit>-
	#     XXXXXX") and then mkdir(that + "/tmp"), and it is the SECOND call
	#     that answers ENOENT -- which says the directory mkdtemp reported
	#     creating is not there. Every unit with PrivateTmp= fails the same
	#     way, systemd-logind among them, so this is one defect wearing
	#     several names rather than a logind problem.
	say "SYSTEMD-SMOKE: --- mkdtemp probe ---"
	__d=$(mktemp -d /tmp/b1nix-probe-XXXXXX 2>&1)
	say "  mkdtemp: mktemp -d -> [$__d]"
	say "  mkdtemp: stat parent -> [$(ls -ld "$__d" 2>&1)]"
	if mkdir "$__d/tmp" 2>/dev/null; then
		say "  mkdtemp: inner mkdir ok"
	else
		say "  mkdtemp: inner mkdir FAILED rc=$? err=[$(mkdir "$__d/tmp" 2>&1)]"
	fi
	say "  mkdtemp: listing -> [$(ls -la "$__d" 2>&1 | head -4 | tr '\n' '|')]"
	# And the same shape by hand, to separate "mkdtemp is wrong" from
	# "mkdir into a just-created directory is wrong".
	rm -rf /tmp/b1nix-plain
	if mkdir /tmp/b1nix-plain && mkdir /tmp/b1nix-plain/tmp; then
		say "  mkdtemp: plain nested mkdir ok"
	else
		say "  mkdtemp: plain nested mkdir FAILED"
	fi
	say "  mkdtemp: /tmp is [$(ls -ld /tmp 2>&1)] on [$(stat -f -c %T /tmp 2>&1)]"
	# The decisive case: a component of the length systemd actually uses.
	# systemd's private-tmp names are "systemd-private-<32 hex>-<unit>-XXXXXX",
	# which is 78 characters -- and the path resolver split components into a
	# 64-byte buffer, so anything from 64 up was looked up as two directories
	# that do not exist. Short names never showed it.
	for __n in 60 63 64 70 78 100; do
		__long=$(printf 'n%.0s' $(seq 1 $__n))
		rm -rf "/tmp/$__long" 2>/dev/null
		if mkdir "/tmp/$__long" 2>/dev/null && [ -d "/tmp/$__long" ] &&
		   mkdir "/tmp/$__long/tmp" 2>/dev/null; then
			say "  namelen: $__n ok"
		else
			say "  namelen: $__n FAILED (mkdir=$(mkdir -p \"/tmp/$__long/tmp\" 2>&1))"
		fi
		rm -rf "/tmp/$__long" 2>/dev/null
	done

	# 2. A TCP loopback exchange: listen, connect, write, accept, read, in one
	#    process. If this hangs the loopback datapath is at fault; if it works
	#    while the .socket unit does not, the manager is.
	cat >/tmp/tcpprobe.pl <<'PROBE_EOF'
use Socket;
$| = 1;
sub now {
        open(my $f, "<", "/proc/uptime") or return 0;
        my $l = <$f>; close $f;
        my ($u) = split / /, $l;
        return $u;
}
my $t0 = now();
socket(L, PF_INET, SOCK_STREAM, 0) or die "socket: $!";
setsockopt(L, SOL_SOCKET, SO_REUSEADDR, pack("l", 1));
bind(L, sockaddr_in(17998, inet_aton("127.0.0.1"))) or die "bind: $!";
listen(L, 5) or die "listen: $!";
printf "listen ok after %.2fs\n", now() - $t0;
my $pid = fork();
die "fork: $!" if !defined $pid;
if ($pid == 0) {
        socket(C, PF_INET, SOCK_STREAM, 0) or exit 11;
        my $c0 = now();
        if (!connect(C, sockaddr_in(17998, inet_aton("127.0.0.1")))) {
                printf "connect FAILED after %.2fs: %s\n", now() - $c0, $!;
                exit 12;
        }
        printf "connect ok after %.2fs\n", now() - $c0;
        syswrite(C, "ping\n");
        exit 0;
}
my $a0 = now();
my $paddr = accept(A, L);
printf "accept %s after %.2fs\n", ($paddr ? "ok" : "FAILED: $!"), now() - $a0;
if ($paddr) {
        my $buf = "";
        sysread(A, $buf, 16);
        $buf =~ s/[\r\n]+$//;
        print "recv=[$buf]\n";
}
waitpid($pid, 0);
printf "child rc=%d, total %.2fs\n", $? >> 8, now() - $t0;
PROBE_EOF
	say "SYSTEMD-SMOKE: --- tcp loopback probe ---"
	timeout 90 perl /tmp/tcpprobe.pl 2>&1 |
		while IFS= read -r __l; do say "  tcpprobe: $__l"; done
	say "SYSTEMD-SMOKE: --- tcp loopback probe end ---"
fi

cat >/run/systemd/system/b1nix-echo.socket <<'EOF'
[Socket]
ListenStream=/run/b1nix-echo.sock
[Install]
WantedBy=sockets.target
EOF
cat >/run/systemd/system/b1nix-echo.service <<'EOF'
[Service]
ExecStart=/bin/sh -c 'echo activated > /run/b1nix-echo.done'
EOF
# The same thing over loopback TCP. The manager's half of socket activation is
# identical either way; what differs underneath is the whole IPv4 path -- bind,
# listen, and a SYN that has to be delivered to this machine by this machine --
# so both families are checked rather than one standing in for the other.
cat >/run/systemd/system/b1nix-echo-tcp.socket <<'EOF'
[Socket]
ListenStream=127.0.0.1:17999
[Install]
WantedBy=sockets.target
EOF
cat >/run/systemd/system/b1nix-echo-tcp.service <<'EOF'
[Service]
ExecStart=/bin/sh -c 'echo activated > /run/b1nix-echo-tcp.done'
EOF
rm -f /run/b1nix-echo.done /run/b1nix-echo-tcp.done /run/b1nix-echo.sock
# Does the reload's cost scale with the number of units?
#
# b1nix.sysd-fillers=<n> writes n trivial units into /run/systemd/system before
# the reload below. Compared against a run with none, that separates "per-unit
# work" from "one stall that happens to take the same time whatever is loaded" --
# and the reload can only be measured once per boot, because it ends by dropping
# the manager's bus connection and nothing after it can ask a question.
__fillers=$(sed -n 's/.*b1nix\.sysd-fillers=\([0-9]*\).*/\1/p' /proc/cmdline 2>/dev/null)
[ -n "$__fillers" ] || __fillers=0
if [ "$__fillers" -gt 0 ] 2>/dev/null; then
	__i=0
	while [ "$__i" -lt "$__fillers" ]; do
		printf '[Unit]\nDescription=b1nix filler %s\n[Service]\nType=oneshot\nExecStart=/bin/true\n' \
			"$__i" >"/run/systemd/system/b1nix-filler-$__i.service"
		__i=$((__i + 1))
	done
fi
# Which generator is the reload waiting on?
#
# manager_reload() re-runs every system generator, and execute_directories()
# bounds them with systemd's DEFAULT_TIMEOUT_USEC -- which is 90 seconds, the
# exact figure the reload takes. Running each one by hand, bounded and timed,
# names the one that hangs instead of inferring it from the total.
if [ "$SYSD_DEBUG" = 1 ]; then
	say "SYSTEMD-SMOKE: --- generator probe ---"
	rm -rf /tmp/gen-n /tmp/gen-e /tmp/gen-l
	mkdir -p /tmp/gen-n /tmp/gen-e /tmp/gen-l
	for __g in /usr/lib/systemd/system-generators/*; do
		[ -x "$__g" ] || continue
		__a=$(cut -d' ' -f1 /proc/uptime)
		timeout 30 "$__g" /tmp/gen-n /tmp/gen-e /tmp/gen-l >/dev/null 2>&1
		__rc=$?
		__b=$(cut -d' ' -f1 /proc/uptime)
		say "  generator: $(basename "$__g") rc=$__rc ${__a}->${__b}"
	done
	say "SYSTEMD-SMOKE: --- generator probe end ---"
fi

say "SYSTEMD-SMOKE:   unit files on disk: $(ls /lib/systemd/system 2>/dev/null | wc -l) + $__fillers fillers in /run"

# Every unit written, then ONE reload. daemon-reload re-reads the whole unit
# search path, so doing it once per unit multiplied a cost the harness does not
# need to pay more than a single time.
sd_reload

# Wait for the service behind a socket to have run, bounded.
wait_for_file() {
	__f="$1"; __i=0
	while [ $__i -lt 12 ] && [ ! -f "$__f" ]; do
		__i=$((__i + 1)); sleep 1
	done
	[ -f "$__f" ]
}

if sd_start b1nix-echo.socket; then
	say "SYSTEMD-SMOKE: ok socket-listening"
	timeout 15 perl -e 'use Socket; socket(S, PF_UNIX, SOCK_STREAM, 0) or exit 1; connect(S, sockaddr_un("/run/b1nix-echo.sock")) or exit 2; print S "hi\n"; close S' >/dev/null 2>&1
	if wait_for_file /run/b1nix-echo.done; then
		say "SYSTEMD-SMOKE: ok socket-activation"
	else
		say "SYSTEMD-SMOKE: FAIL socket-activation (socket up, connection did not start the service)"
	fi
else
	say "SYSTEMD-SMOKE: FAIL socket-listening"
fi

if sd_start b1nix-echo-tcp.socket; then
	say "SYSTEMD-SMOKE: ok socket-listening-tcp"
	timeout 15 perl -e 'use Socket; socket(S, PF_INET, SOCK_STREAM, 0) or exit 1; connect(S, sockaddr_in(17999, inet_aton("127.0.0.1"))) or exit 2; print S "hi\n"; close S' >/dev/null 2>&1
	__prc=$?
	# rc 1 = no socket, 2 = connect refused/timed out, 0 = the connection was
	# really made. "The service did not start" means something different in
	# each case, and the check used to discard it.
	say "SYSTEMD-SMOKE:   tcp-probe connect rc=$__prc"
	if [ "$__prc" != "0" ]; then
		run sh -c 'cat /proc/net/tcp 2>&1 | head -6'
		run sh -c 'systemctl show -p Listen b1nix-echo-tcp.socket 2>&1'
	fi
	if wait_for_file /run/b1nix-echo-tcp.done; then
		say "SYSTEMD-SMOKE: ok socket-activation-tcp"
	else
		say "SYSTEMD-SMOKE: FAIL socket-activation-tcp (socket up, connection did not start the service)"
	fi
else
	say "SYSTEMD-SMOKE: FAIL socket-listening-tcp"
fi

# The readiness protocol: the manager waits for READY=1 over the datagram
# socket named in NOTIFY_SOCKET, so this checks AF_UNIX datagrams with a
# filesystem name AND that the manager is listening on one.
cat >/run/systemd/system/b1nix-notify.service <<'EOF'
[Service]
Type=notify
NotifyAccess=all
ExecStart=/bin/sh -c 'systemd-notify --ready; sleep 30'
EOF
sd_load b1nix-notify.service
if sd_start b1nix-notify.service; then
	say "SYSTEMD-SMOKE: ok notify-ready"
else
	say "SYSTEMD-SMOKE: FAIL notify-ready"
fi
systemctl stop b1nix-notify.service 2>/dev/null

# PrivateTmp: the unit gets its own /tmp through a mount namespace. The test is
# that its file is NOT visible outside -- and that the unit ran at all, because
# "no file outside" is also what a unit that never started looks like.
rm -f /tmp/b1nix-private-probe /run/b1nix-private.ran
cat >/run/systemd/system/b1nix-private.service <<'EOF'
[Service]
Type=oneshot
PrivateTmp=yes
ExecStart=/bin/sh -c 'echo inside > /tmp/b1nix-private-probe; echo ran > /run/b1nix-private.ran'
EOF
sd_load b1nix-private.service
timeout 25 systemctl start b1nix-private.service >/dev/null 2>&1
if [ ! -f /run/b1nix-private.ran ]; then
	say "SYSTEMD-SMOKE: FAIL private-tmp (the unit never ran: $(systemctl is-failed b1nix-private.service 2>&1))"
elif [ -f /tmp/b1nix-private-probe ]; then
	say "SYSTEMD-SMOKE: FAIL private-tmp (the unit's /tmp was the host's)"
else
	say "SYSTEMD-SMOKE: ok private-tmp"
fi

# ProtectSystem=strict: the filesystem is read-only for the unit, so the write
# must FAIL. Same care as above -- the unit has to have run for the absence of
# the file to mean anything.
rm -f /etc/b1nix-should-not-exist /run/b1nix-ro.ran /run/b1nix-ro.err
cat >/run/systemd/system/b1nix-ro.service <<'EOF'
[Service]
Type=oneshot
ProtectSystem=strict
ExecStart=/bin/sh -c 'echo x > /etc/b1nix-should-not-exist 2>/run/b1nix-ro.err; echo "wrc=$?" >> /run/b1nix-ro.err; grep -c " ro," /proc/self/mountinfo >> /run/b1nix-ro.err 2>&1; grep " / " /proc/self/mountinfo >> /run/b1nix-ro.err 2>&1; grep " /etc " /proc/self/mountinfo >> /run/b1nix-ro.err 2>&1; echo ran > /run/b1nix-ro.ran'
ReadWritePaths=/run
EOF
sd_load b1nix-ro.service
timeout 25 systemctl start b1nix-ro.service >/dev/null 2>&1
if [ ! -f /run/b1nix-ro.ran ]; then
	say "SYSTEMD-SMOKE: FAIL protect-system (the unit never ran: $(systemctl is-failed b1nix-ro.service 2>&1))"
elif [ -f /etc/b1nix-should-not-exist ]; then
	say "SYSTEMD-SMOKE: FAIL protect-system (the write went through)"
	run sh -c 'cat /run/b1nix-ro.err 2>&1 | head -12'
	rm -f /etc/b1nix-should-not-exist
else
	say "SYSTEMD-SMOKE: ok protect-system"
fi

# A timer: a clock the manager owns, rather than a sleep in a script.
cat >/run/systemd/system/b1nix-tick.service <<'EOF'
[Service]
Type=oneshot
ExecStart=/bin/sh -c 'echo tick >> /run/b1nix-tick.count'
EOF
cat >/run/systemd/system/b1nix-tick.timer <<'EOF'
[Timer]
OnActiveSec=1s
AccuracySec=1s
[Install]
WantedBy=timers.target
EOF
sd_load b1nix-tick.timer
rm -f /run/b1nix-tick.count
if sd_start b1nix-tick.timer; then
	i=0
	while [ $i -lt 15 ] && [ ! -f /run/b1nix-tick.count ]; do
		i=$((i + 1)); sleep 1
	done
	if [ -f /run/b1nix-tick.count ]; then
		say "SYSTEMD-SMOKE: ok timer-fires"
	else
		say "SYSTEMD-SMOKE: FAIL timer-fires (started, never fired)"
	fi
else
	say "SYSTEMD-SMOKE: FAIL timer-fires"
fi

# Restart=on-failure: the manager notices the exit status and starts it again,
# which is SIGCHLD to PID 1 and waitpid semantics -- both wrong here before.
rm -f /run/b1nix-restart.count
cat >/run/systemd/system/b1nix-restart.service <<'EOF'
[Service]
ExecStart=/bin/sh -c 'echo run >> /run/b1nix-restart.count; exit 1'
Restart=on-failure
RestartSec=1
EOF
sd_load b1nix-restart.service
timeout 20 systemctl start b1nix-restart.service >/dev/null 2>&1
i=0
while [ $i -lt 12 ]; do
	runs=$(count_lines /run/b1nix-restart.count)
	[ "$runs" -ge 2 ] && break
	i=$((i + 1)); sleep 1
done
systemctl stop b1nix-restart.service 2>/dev/null
if [ "$(count_lines /run/b1nix-restart.count)" -ge 2 ]; then
	say "SYSTEMD-SMOKE: ok restart-on-failure"
else
	say "SYSTEMD-SMOKE: FAIL restart-on-failure (ran $(count_lines /run/b1nix-restart.count) times)"
fi

# The journal, asked for ONE unit rather than the whole boot: that is its
# index, not its ability to append.
#
# Written to STDOUT on purpose: every other marker goes to /dev/kmsg (agetty
# takes the console away mid-run), and /dev/kmsg reaches journald as a KERNEL
# message, which belongs to no unit. So the harness had nothing of its own in
# its unit's journal and this check was reading whatever noise landed there
# last. This line is the unit's own output, through StandardOutput=journal.
echo "SYSTEMD-SMOKE journal-probe $$"
sync 2>/dev/null || true
sleep 1
if timeout 20 journalctl -u b1nix-smoke.service -n 20 --no-pager 2>/dev/null |
	grep -q 'SYSTEMD-SMOKE journal-probe'; then
	say "SYSTEMD-SMOKE: ok journal-filter-unit"
else
	say "SYSTEMD-SMOKE: FAIL journal-filter-unit ($(timeout 20 journalctl -u b1nix-smoke.service -n 1 --no-pager 2>&1 | head -1))"
fi

# enable/disable moves a unit between states. It needs an [Install] section to
# be enabled at all -- without one the answer is "static", which is not a
# failure of the manager but of the unit, and asking the wrong question was
# this harness's own bug the first time round.
# Bounded, like every other call in this harness. These four were not, and
# `systemctl enable` does an implicit daemon-reload: when that stopped
# answering the harness stopped with it, and every check after this point was
# reported missing rather than failed. A bound turns "the run ended" into "this
# call did not return", which is the thing worth knowing.
__ed_a=$(cut -d' ' -f1 /proc/uptime)
timeout 60 systemctl enable b1nix-tick.timer >/dev/null 2>&1
__ed_rc=$?
__ed_b=$(cut -d' ' -f1 /proc/uptime)
say "SYSTEMD-SMOKE:   enable rc=$__ed_rc ${__ed_a}->${__ed_b}"
if [ "$__ed_rc" = "0" ] &&
   [ "$(timeout 20 systemctl is-enabled b1nix-tick.timer 2>&1)" = "enabled" ] &&
   timeout 60 systemctl disable b1nix-tick.timer >/dev/null 2>&1 &&
   [ "$(timeout 20 systemctl is-enabled b1nix-tick.timer 2>&1)" = "disabled" ]; then
	say "SYSTEMD-SMOKE: ok unit-enable-disable"
else
	say "SYSTEMD-SMOKE: FAIL unit-enable-disable: $(timeout 20 systemctl is-enabled b1nix-tick.timer 2>&1)"
	# Which syscall every task is in, at the moment the manager stopped
	# answering. Printed by the kernel, so a wedged PID 1 cannot suppress it.
	cat /proc/b1nix-tasks >/dev/null 2>&1
fi

# Masking: the strongest "no" -- a masked unit refuses even a direct start.
if timeout 60 systemctl mask b1nix-tick.service >/dev/null 2>&1 &&
   ! timeout 10 systemctl start b1nix-tick.service >/dev/null 2>&1; then
	say "SYSTEMD-SMOKE: ok unit-mask-refuses"
else
	say "SYSTEMD-SMOKE: FAIL unit-mask-refuses (a masked unit started)"
fi
systemctl unmask b1nix-tick.service >/dev/null 2>&1

# How far the boot got, by target rather than by impression. systemd-analyze
# answers only once the manager has finished starting up, so a machine that
# reaches this point has genuinely completed its boot transaction -- and the
# chain names the units it waited on, in order, which no marker of ours can.
# systemd-analyze refuses to answer while the boot transaction is still
# running, and this harness IS part of that transaction -- so asking here only
# ever prints "Bootup is not yet finished". Ask from a process that outlives
# it: this waits for the manager to stop reporting "starting" and then prints
# the chain, which lands on the console after the harness's last marker.
(
	__i=0
	while [ $__i -lt 30 ]; do
		case "$(systemctl is-system-running 2>&1)" in
		starting) ;;
		*) break ;;
		esac
		__i=$((__i + 1))
		sleep 1
	done
	{
		echo "SYSTEMD-SMOKE: --- systemd-analyze (after the transaction) ---"
		timeout 30 systemd-analyze time 2>&1 | head -4
		timeout 30 systemd-analyze critical-chain 2>&1 | head -24
		timeout 30 systemd-analyze blame 2>&1 | head -10
		echo "SYSTEMD-SMOKE: --- systemd-analyze end ---"
	} >/dev/kmsg 2>&1
) &

say "SYSTEMD-SMOKE: units-loaded=$(systemctl list-units --no-legend --no-pager 2>/dev/null | wc -l) failed=$(systemctl list-units --state=failed --no-legend --no-pager 2>/dev/null | wc -l)"

say "SYSTEMD-SMOKE: done"
SSTAGE_EOF
	chmod 0755 "$ROOTFS/b1nix-systemd-stage.sh"
	# A harness with a syntax error dies at the first line the shell cannot
	# parse and prints nothing after it -- which reads exactly like a kernel
	# that stopped answering. One unbalanced quote in a diagnostic cost a whole
	# run that way. Parse it here, where the failure is a build error.
	sh -n "$ROOTFS/b1nix-systemd-stage.sh" ||
		die "b1nix-systemd-stage.sh does not parse"

	mkdir -p "$ROOTFS/etc/systemd/system/multi-user.target.wants"
	cat >"$ROOTFS/etc/systemd/system/b1nix-smoke.service" <<-'UNIT_EOF'
		[Unit]
		Description=b1nix systemd boot harness
		After=multi-user.target systemd-user-sessions.service
		Wants=multi-user.target

		[Service]
		Type=oneshot
		RemainAfterExit=yes
		ExecStart=/b1nix-systemd-stage.sh
		StandardOutput=journal+console
		StandardError=journal+console
		# The harness bounds every command it runs, and the run itself is
		# bounded by tests/systemd-smoke.sh. 90 s was tighter than either, so
		# with b1nix.sysd-debug on -- where a single probe deliberately waits
		# out a 30 s sleep -- systemd killed the harness partway through and
		# every check after that point was reported as a missing marker.
		TimeoutStartSec=480s

		[Install]
		WantedBy=multi-user.target
	UNIT_EOF
	ln -sf /etc/systemd/system/b1nix-smoke.service \
		"$ROOTFS/etc/systemd/system/multi-user.target.wants/b1nix-smoke.service"

	# Users and groups the packages' postinst would have made. systemd-sysusers
	# creates the rest at boot from /usr/lib/sysusers.d.
	if ! grep -q '^messagebus:' "$ROOTFS/etc/passwd" 2>/dev/null; then
		echo 'messagebus:x:100:101::/nonexistent:/usr/sbin/nologin' >>"$ROOTFS/etc/passwd"
		echo 'messagebus:x:101:' >>"$ROOTFS/etc/group"
	fi

	mkdir -p "$ROOTFS/var/log/journal" "$ROOTFS/run/systemd"
fi

# ── 5. ext4 image ───────────────────────────────────────────────────────────
# b1nix's ext4 driver does NOT implement metadata_csum, 64bit, flex_bg or
# huge_file — those flags are mandatory, not a preference.
log "building $IMG (${IMG_SIZE_MB} MiB, label $IMG_LABEL)"
rm -f "$IMG"
mke2fs -t ext4 -O ^metadata_csum,^64bit,^flex_bg,^huge_file,^orphan_file -q \
	-L "$IMG_LABEL" -E root_owner=0:0 -d "$ROOTFS" \
	"$IMG" "${IMG_SIZE_MB}m" || die "mke2fs failed"

# ── 6. Verify ───────────────────────────────────────────────────────────────
log "verifying image"
debugfs -R "ls -l /" "$IMG" 2>/dev/null || die "debugfs: cannot list /"
VERIFY_FILES="/b1nix-stage.sh /usr/bin/dash /usr/lib64/ld-linux-x86-64.so.2 /usr/bin/ps /sbin/init"
if [ "$PROFILE" = "systemd" ]; then
	VERIFY_FILES="$VERIFY_FILES /usr/lib/systemd/systemd /usr/bin/systemctl \
		/usr/bin/journalctl /usr/lib/systemd/systemd-journald \
		/b1nix-systemd-stage.sh /etc/systemd/system/b1nix-smoke.service"
fi
for f in $VERIFY_FILES; do
	debugfs -R "stat $f" "$IMG" >/dev/null 2>&1 || die "missing from image: $f"
done

log "done: $IMG ($(wc -c <"$IMG") bytes)"
log "boot it with: root=LABEL=$IMG_LABEL init=/sbin/init"
