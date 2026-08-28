#!/bin/sh
# Build an Arch Linux root filesystem as an ext4 image that b1nix can boot.
#
#   sh tools/images/mk-arch-image.sh                    # build/x86_64/arch-systemd.ext4
#   PROFILE=graphics sh tools/images/mk-arch-image.sh   # build/x86_64/arch-graphics.ext4
#
# Why Arch beside the Debian image: Debian bookworm ships systemd 252 against
# glibc 2.36. Arch is rolling, so the same boot is asked of systemd 261 against
# glibc 2.44 -- a manager that reaches for kernel interfaces the older one never
# touched. What it asks for that this kernel does not answer is the point of
# having it.
#
# Runs as an ORDINARY USER: no root, no pacstrap, no loop mounts, no sudo. The
# tree is the official archlinux-bootstrap tarball, extra .pkg.tar.zst packages
# unpacked over it with `tar --zstd`, and the whole thing handed to `mke2fs -d`
# inside one fakeroot session so the recorded ownership survives.
#
# Everything downloaded is cached under $BUILD_DIR/arch, so a second run with a
# warm cache never touches the network. This is NEVER part of `make iso`.
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
ARCH="${ARCH:-x86_64}"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build/$ARCH}"
case "$BUILD_DIR" in /*) ;; *) BUILD_DIR="$ROOT_DIR/$BUILD_DIR" ;; esac

CACHE="$BUILD_DIR/arch"
ROOTFS="$CACHE/rootfs"
PKGS_DIR="$CACHE/pkgs"

# Which shape the image is built in.
#   systemd  -- Arch's systemd as PID 1, which is what a stock Arch install is
#   graphics -- the same image plus a Wayland compositor that modesets a DRM
#               card, so the boot ends at a desktop on the scanout rather than
#               at a target with nothing behind it
PROFILE="${PROFILE:-systemd}"
case "$PROFILE" in
systemd)
	IMG="${IMG:-$BUILD_DIR/arch-systemd.ext4}"
	IMG_SIZE_MB="${IMG_SIZE_MB:-2048}"
	IMG_LABEL="${IMG_LABEL:-b1nix-arch}"
	;;
graphics)
	IMG="${IMG:-$BUILD_DIR/arch-graphics.ext4}"
	# The bootstrap tree alone is ~1.5 GiB (Arch ships the whole gcc runtime
	# set and every locale); Weston drags in mesa, pango and gstreamer on top.
	IMG_SIZE_MB="${IMG_SIZE_MB:-4096}"
	IMG_LABEL="${IMG_LABEL:-b1nix-archgfx}"
	;;
*) echo "mk-arch-image: unknown PROFILE '$PROFILE'" >&2; exit 1 ;;
esac

# ── where the tree and the packages come from ───────────────────────────────
# The bootstrap tarball is the official installation medium: the `base` group
# plus pacman, produced by Arch's own release process. It is preferred over the
# `archlinux` image on the Docker registry, which is the same set with
# NoExtract rules applied -- no locales, no man pages, and container-specific
# unit masking. The point of this image is to ask a STOCK distribution's
# systemd what it wants from the kernel, and a container-trimmed tree is not
# stock.
MIRROR="${ARCH_MIRROR:-https://geo.mirror.pkgbuild.com}"
BOOTSTRAP_URL="${BOOTSTRAP_URL:-$MIRROR/iso/latest/archlinux-bootstrap-x86_64.tar.zst}"
BOOTSTRAP_SUMS_URL="${BOOTSTRAP_SUMS_URL:-$MIRROR/iso/latest/sha256sums.txt}"
REPOS="${ARCH_REPOS:-core extra}"
PKG_ARCH="${PKG_ARCH:-x86_64}"

case "$PROFILE" in
systemd)
	# Nothing. The bootstrap tarball already carries systemd 261 and
	# systemd-sysvcompat, which is what provides /sbin/init. Naming packages
	# that are already there would only add things that can fail for reasons
	# having nothing to do with whether Arch boots.
	PACKAGES="${PACKAGES:-}"
	;;
graphics)
	#   weston      Arch's Wayland compositor, 15.x. Its DRM backend takes the
	#               route a desktop takes: find the card through libudev, open
	#               it through libseat, modeset it, scan out of it.
	#   ttf-dejavu  the panel and the terminal draw text through pango, and the
	#               bootstrap tree ships no font at all.
	#   seatd       Weston 15 has no launcher of its own any more: every device
	#               open goes through libseat. seatd is staged so both of
	#               libseat's routes -- its daemon and its builtin backend --
	#               are available to the run.
	PACKAGES="${PACKAGES:-weston ttf-dejavu seatd}"
	;;
esac

die() { echo "mk-arch-image: $*" >&2; exit 1; }
log() { echo "[arch-image] $*"; }

# ── fakeroot ────────────────────────────────────────────────────────────────
# Same reason as the Debian image: we are not root, but an Arch tree owned by
# uid 1000 with no setuid bits is not the thing we set out to boot. One exec
# keeps the whole run inside a single fakeroot session so the recorded
# ownership survives from `tar x` through to `mke2fs -d`.
if [ -z "${B1NIX_ARCH_FAKEROOT:-}" ]; then
	if command -v fakeroot >/dev/null 2>&1; then
		B1NIX_ARCH_FAKEROOT=1
		export B1NIX_ARCH_FAKEROOT
		exec fakeroot -- "$0" "$@"
	fi
	echo "[arch-image] WARNING: fakeroot not found -- the image will be owned by" >&2
	echo "[arch-image]          uid $(id -u), not root, and setuid bits are lost." >&2
	TAR_OWNER_FLAGS="--no-same-owner"
else
	TAR_OWNER_FLAGS="-p --same-owner"
fi

for t in curl tar zstd mke2fs debugfs python3 sha256sum; do
	command -v "$t" >/dev/null 2>&1 || die "missing host tool: $t"
done

mkdir -p "$CACHE" "$PKGS_DIR"

# ── 1. the bootstrap tarball ────────────────────────────────────────────────
TARBALL="$CACHE/bootstrap.tar.zst"
SUMFILE="$CACHE/bootstrap.sha256"

verify_tarball() {
	[ -f "$TARBALL" ] || return 1
	[ -s "$SUMFILE" ] || return 1
	_want="$(cat "$SUMFILE")"
	_have="$(sha256sum "$TARBALL" | cut -d' ' -f1)"
	[ "$_want" = "$_have" ]
}

fetch_tarball() {
	log "fetching $BOOTSTRAP_URL"
	# The checksum comes from the release's own sha256sums.txt, so a truncated
	# or mirror-corrupted download is a hard error rather than a tree missing
	# half of /usr that boots into something inexplicable.
	_sum=$(curl -sfL "$BOOTSTRAP_SUMS_URL" |
		awk '/archlinux-bootstrap-x86_64\.tar\.zst$/ { print $1; exit }')
	curl -fL --retry 3 "$BOOTSTRAP_URL" -o "$TARBALL.part" ||
		die "bootstrap tarball fetch failed"
	mv "$TARBALL.part" "$TARBALL"
	if [ -n "$_sum" ]; then
		echo "$_sum" >"$SUMFILE"
		verify_tarball || die "bootstrap tarball sha256 mismatch -- the download is corrupt"
	else
		log "WARNING: no sha256 published for the tarball -- using it unverified"
		: >"$SUMFILE"
	fi
}

if verify_tarball; then
	log "bootstrap tarball cached and verified ($(wc -c <"$TARBALL") bytes)"
elif [ -f "$TARBALL" ] && [ ! -s "$SUMFILE" ]; then
	log "bootstrap tarball present but unverified -- using as-is"
else
	fetch_tarball
fi

# ── 2. repository indexes ───────────────────────────────────────────────────
fetch_dbs() {
	for r in $REPOS; do
		[ -s "$CACHE/$r.db" ] && continue
		log "fetching $r.db"
		curl -sfL "$MIRROR/$r/os/$PKG_ARCH/$r.db" -o "$CACHE/$r.db.part" ||
			die "$r.db fetch failed"
		mv "$CACHE/$r.db.part" "$CACHE/$r.db"
	done
}

# ── 3. dependency closure ───────────────────────────────────────────────────
# What the seed packages need that the bootstrap tree does not already have.
# "Already have" is read from the tarball's OWN pacman database, so a package
# `base` already installed is never downloaded again, and a virtual name one of
# them provides satisfies a dependency exactly as pacman would.
CLOSURE="$CACHE/closure-$PROFILE.txt"
: >"$CLOSURE.empty"
if [ -n "$PACKAGES" ]; then
	fetch_dbs
	if [ ! -s "$CLOSURE" ]; then
		log "resolving the dependency closure of: $PACKAGES"
		if [ ! -s "$CACHE/local-db.txt" ]; then
			# Every %NAME% and %PROVIDES% the tree already has. Extracted once
			# and cached: walking a 126 MiB zstd stream is the most expensive
			# thing in this script.
			zstd -dc "$TARBALL" |
				tar -x --strip-components=1 -O --wildcards \
					'root.x86_64/var/lib/pacman/local/*/desc' \
				>"$CACHE/local-desc.txt" ||
				die "cannot read the tarball's pacman database"
			[ -s "$CACHE/local-desc.txt" ] || die "the tarball has no pacman database"
			python3 - "$CACHE/local-desc.txt" >"$CACHE/local-db.txt" <<-'PY'
				import sys
				key, out = None, []
				for line in open(sys.argv[1], encoding="utf-8", errors="replace"):
				    line = line.rstrip("\n")
				    if line.startswith("%") and line.endswith("%"):
				        key = line
				        continue
				    if not line.strip():
				        key = None
				        continue
				    if key in ("%NAME%", "%PROVIDES%"):
				        name = line.split("=")[0].split("<")[0].split(">")[0].strip()
				        if name:
				            out.append(name)
				print("\n".join(sorted(set(out))))
			PY
		fi
		_dbs=""
		for r in $REPOS; do _dbs="$_dbs $CACHE/$r.db"; done
		# shellcheck disable=SC2086
		PACKAGES="$PACKAGES" python3 "$ROOT_DIR/tools/images/arch-closure.py" \
			--installed "$CACHE/local-db.txt" $_dbs >"$CLOSURE.new" ||
			die "dependency resolution failed"
		mv "$CLOSURE.new" "$CLOSURE"
	fi
	log "closure ($(wc -l <"$CLOSURE" | tr -d ' ') packages): $(cut -d' ' -f1 <"$CLOSURE" | tr '\n' ' ')"
else
	CLOSURE="$CLOSURE.empty"
fi

# ── 4. download the packages ────────────────────────────────────────────────
while read -r _name _repo _file _sum _ver; do
	[ -n "$_name" ] || continue
	_dst="$PKGS_DIR/$_file"
	if [ -f "$_dst" ]; then
		[ -z "$_sum" ] && continue
		[ "$(sha256sum "$_dst" | cut -d' ' -f1)" = "$_sum" ] && continue
		log "$_name: the cached copy is corrupt, downloading again"
		rm -f "$_dst"
	fi
	log "downloading $_name $_ver"
	curl -sfL --retry 3 "$MIRROR/$_repo/os/$PKG_ARCH/$_file" -o "$_dst.part" ||
		die "download failed: $MIRROR/$_repo/os/$PKG_ARCH/$_file"
	if [ -n "$_sum" ]; then
		[ "$(sha256sum "$_dst.part" | cut -d' ' -f1)" = "$_sum" ] ||
			die "$_name: sha256 mismatch"
	fi
	mv "$_dst.part" "$_dst"
done <"$CLOSURE"

# ── 5. unpack the tree ──────────────────────────────────────────────────────
# Rebuilt from the cached tarballs on every run, so the result is deterministic
# and re-running never doubles up a half-applied package.
log "unpacking the bootstrap tree into $ROOTFS"
rm -rf "$ROOTFS"
mkdir -p "$ROOTFS"
# ./dev/* is excluded: the tarball ships device nodes an ordinary user cannot
# create, and b1nix mounts its own devtmpfs there anyway.
# --keep-directory-symlink is what makes usr-merge survive: Arch's /bin, /sbin,
# /lib and /lib64 are symlinks into /usr, and without it tar replaces them with
# real directories the first time a package ships one, splitting the tree in
# half.
zstd -dc "$TARBALL" |
	tar -x --strip-components=1 -C "$ROOTFS" \
		--exclude='root.x86_64/dev/*' --keep-directory-symlink $TAR_OWNER_FLAGS ||
	die "bootstrap extraction failed"

while read -r _name _repo _file _sum _ver; do
	[ -n "$_name" ] || continue
	log "unpacking $_name $_ver"
	# .PKGINFO/.BUILDINFO/.MTREE/.INSTALL are pacman's own metadata and have no
	# place in the filesystem. .INSTALL in particular is the maintainer script,
	# and unpacking a package never runs one -- what a hook would have made is
	# staged by hand below.
	tar --zstd -x -f "$PKGS_DIR/$_file" -C "$ROOTFS" \
		--exclude='.PKGINFO' --exclude='.BUILDINFO' --exclude='.MTREE' \
		--exclude='.INSTALL' --exclude='.CHANGELOG' \
		--keep-directory-symlink $TAR_OWNER_FLAGS ||
		die "$_name: unpack failed"
done <"$CLOSURE"

# ── 6. the configuration a pacman hook would have made ──────────────────────
log "staging the systemd configuration"

[ -x "$ROOTFS/usr/lib/systemd/systemd" ] || die "systemd binary missing from the tree"
[ -e "$ROOTFS/usr/bin/init" ] || die "/usr/bin/init missing (no systemd-sysvcompat?)"

# A machine-id must exist and be non-empty: an EMPTY one means "first boot" to
# systemd, which then wants to run systemd-firstboot on the console and waits
# there. 32 hex digits exactly -- anything else is not a machine ID.
printf 'b100b100b100b100b100b100b100b100\n' >"$ROOTFS/etc/machine-id"
chmod 0444 "$ROOTFS/etc/machine-id"

# The kernel mounted the root already, and systemd mounts the API filesystems
# itself. passno 0: nothing should fsck a mounted rw root.
cat >"$ROOTFS/etc/fstab" <<-FSTAB_EOF
	# b1nix Arch test image
	LABEL=$IMG_LABEL / ext4 defaults 0 0
FSTAB_EOF
echo "b1nix-arch" >"$ROOTFS/etc/hostname"

# Keep the boot inside the test's timeout: a unit that never comes up should
# fail the run, not spend 90 s per attempt doing it.
mkdir -p "$ROOTFS/etc/systemd/system.conf.d"
# SD_LOG_LEVEL/SD_LOG_TARGET are knobs rather than constants because a manager
# that has stopped saying anything is the hardest state to work in, and a
# kernel command line is not always enough to change its mind: the manager
# re-reads this file after it has parsed the command line whenever it reloads.
SD_LOG_LEVEL="${SD_LOG_LEVEL:-info}"
SD_LOG_TARGET="${SD_LOG_TARGET:-console}"
cat >"$ROOTFS/etc/systemd/system.conf.d/b1nix.conf" <<-SDCONF_EOF
	[Manager]
	DefaultTimeoutStartSec=25s
	DefaultTimeoutStopSec=15s
	ShowStatus=yes
	LogLevel=$SD_LOG_LEVEL
	LogTarget=$SD_LOG_TARGET
	# A service that fails before it can reach the journal otherwise says
	# nothing at all; on a test machine the console is where the evidence has
	# to land.
	DefaultStandardError=journal+console
SDCONF_EOF

# journald: the image has no persistent journal directory and the test reads
# the journal back, so keep it in /run.
mkdir -p "$ROOTFS/etc/systemd/journald.conf.d"
cat >"$ROOTFS/etc/systemd/journald.conf.d/b1nix.conf" <<-'JCONF_EOF'
	[Journal]
	Storage=volatile
	RuntimeMaxUse=16M
JCONF_EOF

# The login prompt: console-getty.service runs agetty on /dev/console, which on
# this machine is the serial line. NOT serial-getty@ttyS0.service, which is
# BoundTo= a .device unit and only becomes active once udev has told systemd
# about the device.
mkdir -p "$ROOTFS/etc/systemd/system/getty.target.wants"
ln -sf /usr/lib/systemd/system/console-getty.service \
	"$ROOTFS/etc/systemd/system/getty.target.wants/console-getty.service"

# An empty root password, so the getty prompt can be used from the serial
# console. This is a test image with no network listener.
if [ -f "$ROOTFS/etc/shadow" ]; then
	sed -i 's/^root:[^:]*:/root::/' "$ROOTFS/etc/shadow"
fi

# ── name resolution: files, and only files ──────────────────────────────────
# Arch ships `group: files [SUCCESS=merge] systemd`, and `[SUCCESS=merge]`
# means the `systemd` module is consulted even when `files` has already
# answered. That module is a varlink client: it connects to
# /run/systemd/userdb/io.systemd.Multiplexer, which PID 1 binds and listens on
# before it runs the system generators -- and then blocks, waiting for those
# generators to finish. The first generator that looks up a group therefore
# waits on a manager that is waiting on it, and only its own timeout ends the
# standoff, tens of seconds per lookup.
#
# This image runs no systemd-userdbd, so there is nothing for the module to
# add. Trimming the lines to `files` is the configuration that matches what is
# installed, not a way around a kernel fault: the connect, the wait and the
# timeout are all doing exactly what they are supposed to.
if [ -f "$ROOTFS/etc/nsswitch.conf" ]; then
	cp "$ROOTFS/etc/nsswitch.conf" "$ROOTFS/etc/nsswitch.conf.arch"
	sed -i -E 's/^(passwd|group|shadow|gshadow):.*/\1: files/' \
		"$ROOTFS/etc/nsswitch.conf"
fi

mkdir -p "$ROOTFS/dev" "$ROOTFS/proc" "$ROOTFS/sys" "$ROOTFS/run" "$ROOTFS/tmp"
chmod 1777 "$ROOTFS/tmp"
mkdir -p "$ROOTFS/var/log/journal" "$ROOTFS/run/systemd"

# ── 7. the harness ──────────────────────────────────────────────────────────
# Written by its own script so that a change to what the guest measures is a
# change to one readable file rather than to a heredoc nested three levels deep
# inside this one.
sh "$ROOT_DIR/tools/images/arch-stage-harness.sh" "$ROOTFS" "$PROFILE" ||
	die "staging the harness failed"

# ── 8. graphics profile: a compositor that modesets a card ──────────────────
if [ "$PROFILE" = "graphics" ]; then
	log "staging the Weston graphical session"
	[ -x "$ROOTFS/usr/bin/weston" ] || die "weston missing from the unpacked tree"

	mkdir -p "$ROOTFS/etc/xdg/weston"
	cat >"$ROOTFS/etc/xdg/weston/weston.ini" <<-'WINI_EOF'
		[core]
		# No GL: the pixman renderer composites in software into a DRM dumb
		# buffer, which is the smallest honest path to a picture and needs no
		# Mesa driver at run time at all.
		renderer=pixman
		require-input=false
		idle-time=0

		[shell]
		background-color=0xff1a3d5c
		panel-position=top
		locking=false
		animation=none
	WINI_EOF
	mkdir -p "$ROOTFS/run/user/0"
	chmod 0700 "$ROOTFS/run/user/0"
fi

# ── 9. the ext4 image ───────────────────────────────────────────────────────
# b1nix's ext4 driver does NOT implement metadata_csum, 64bit, flex_bg or
# huge_file -- those flags are mandatory, not a preference.
log "building $IMG (${IMG_SIZE_MB} MiB, label $IMG_LABEL)"
rm -f "$IMG"
mke2fs -t ext4 -O ^metadata_csum,^64bit,^flex_bg,^huge_file,^orphan_file -q \
	-L "$IMG_LABEL" -E root_owner=0:0 -d "$ROOTFS" \
	"$IMG" "${IMG_SIZE_MB}m" || die "mke2fs failed"

# ── 10. verify ──────────────────────────────────────────────────────────────
log "verifying image"
debugfs -R "ls -l /" "$IMG" 2>/dev/null || die "debugfs: cannot list /"
VERIFY_FILES="/usr/bin/bash /usr/lib/systemd/systemd /usr/bin/systemctl \
	/usr/bin/journalctl /usr/lib/systemd/systemd-journald /usr/bin/init \
	/etc/machine-id /etc/arch-release /b1nix-arch-stage.sh \
	/etc/systemd/system/b1nix-arch.service"
if [ "$PROFILE" = "graphics" ]; then
	VERIFY_FILES="$VERIFY_FILES /usr/bin/weston /usr/bin/weston-terminal \
		/etc/xdg/weston/weston.ini /b1nix-arch-graphics.sh"
fi
for f in $VERIFY_FILES; do
	debugfs -R "stat $f" "$IMG" >/dev/null 2>&1 || die "missing from image: $f"
done

log "done: $IMG ($(wc -c <"$IMG") bytes)"
log "boot it with: root=LABEL=$IMG_LABEL init=/sbin/init"
