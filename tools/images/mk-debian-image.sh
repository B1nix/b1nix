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
	PACKAGES="${PACKAGES:-systemd systemd-sysv dbus procps libproc2-0 libncursesw6}"
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
	# the device. This image has no udev (systemd's own device monitor also
	# needs SO_ATTACH_FILTER, which b1nix does not implement), so dev-ttyS0
	# never appears and the getty waits out its 90-second job timeout and then
	# fails on the dependency. console-getty.service is systemd's own unit for
	# exactly this situation — it is what a container gets — and it depends on
	# nothing but /dev/console existing.
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

say() { echo "$@" >/dev/console 2>/dev/null || echo "$@"; }
run() { "$@" 2>&1 | sed 's/^/    /' >/dev/console 2>/dev/null; }

say "SYSTEMD-SMOKE: start pid=$$"

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
	sysinit.target basic.target multi-user.target; do
	st=$(systemctl is-active "$u" 2>&1)
	if [ "$st" = "active" ]; then
		say "SYSTEMD-SMOKE: ok unit-active $u"
	else
		say "SYSTEMD-SMOKE: FAIL unit-active $u state=$st"
	fi
done

# ── 5. the journal ─────────────────────────────────────────────────────────
# journalctl reading back its own records is the end-to-end test of
# /dev/kmsg, the journald sockets and the on-disk (well, /run) journal files.
if journalctl -b --no-pager -n 5 >/tmp/jout 2>/tmp/jerr; then
	lines=$(wc -l </tmp/jout | tr -d ' ')
	if [ "$lines" -gt 0 ]; then
		say "SYSTEMD-SMOKE: ok journalctl ($lines lines)"
		sed 's/^/    /' /tmp/jout >/dev/console 2>/dev/null
	else
		say "SYSTEMD-SMOKE: FAIL journalctl (empty)"
	fi
else
	say "SYSTEMD-SMOKE: FAIL journalctl status=$?"
	sed 's/^/    /' /tmp/jerr >/dev/console 2>/dev/null
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


say "SYSTEMD-SMOKE: done"
SSTAGE_EOF
	chmod 0755 "$ROOTFS/b1nix-systemd-stage.sh"

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
		TimeoutStartSec=90s

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
