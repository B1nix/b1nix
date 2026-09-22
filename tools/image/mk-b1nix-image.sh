#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Build a bootable b1nix disk image: a Debian trixie root with the b1nix
# overlay installed, an ESP carrying Limine and the kernel, and a GPT around
# the two.
#
#   sh tools/image/mk-b1nix-image.sh                 # build/x86_64/b1nix-disk.img
#   PROFILE=broken sh tools/image/mk-b1nix-image.sh  # same, with a kernel that fails
#
# Runs as an ORDINARY USER, like everything else in this tree: the root tree is
# assembled in the user-namespace chroot from tools/deb/debian-chroot.sh,
# the ESP is written with mtools, the root filesystem with `mke2fs -d`, and the
# partition table by tests/mkgpt4k.py. No sudo, no loop mounts.
#
# PROFILE=broken installs a second kernel whose userspace cannot come up, which
# is what DISTRO-SMOKE uses to prove the boot-counting fallback. It is the only
# way to test that path honestly: a kernel that always works proves nothing
# about what happens when one does not.
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
ARCH="${ARCH:-x86_64}"
DEB_ARCH="${DEB_ARCH:-amd64}"
SUITE="${SUITE:-trixie}"
PROFILE="${PROFILE:-plain}"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build/$ARCH}"
IMG="${IMG:-$BUILD_DIR/b1nix-disk.img}"
[ "$PROFILE" = "plain" ] || IMG="${IMG_OVERRIDE:-$BUILD_DIR/b1nix-disk-$PROFILE.img}"

DISK_MIB="${DISK_MIB:-3072}"
ESP_MIB="${ESP_MIB:-256}"
# Limine's BIOS stage 2 does not fit in the gap before the first partition on a
# GPT disk, so it gets a partition of its own.
BIOS_MIB="${BIOS_MIB:-1}"
ROOT_LABEL="${ROOT_LABEL:-b1nix-root}"
# What every boot entry carries beyond root=. console=ttyS0 is not optional for
# a lane: it is the only console the harness can read. Add kernel diagnostics
# here when chasing something, for instance b1nix.trace-mount.
CMDLINE_EXTRA="${CMDLINE_EXTRA:-console=ttyS0}"

REPO="${REPO:-$ROOT_DIR/build/packages/repo}"
WORK="${WORK:-$ROOT_DIR/build/images}"
ROOTFS_BASE="$WORK/rootfs-$PROFILE"
ROOTFS="$ROOTFS_BASE/chroot-$SUITE-$DEB_ARCH"
CHROOT="$ROOT_DIR/tools/deb/debian-chroot.sh"
LIMINE_DATADIR="${LIMINE_DATADIR:-$(limine --print-datadir 2>/dev/null || echo /usr/share/limine)}"

# What an installed b1nix needs to reach a login prompt. Deliberately short:
# the desktop is a separate package and a separate phase.
SYSTEM_PKGS="${SYSTEM_PKGS:-systemd-sysv udev dbus-broker kmod initramfs-tools \
	util-linux e2fsprogs btrfs-progs iproute2 iputils-ping ca-certificates \
	less nano procps strace}"

log() { printf '\033[1;34m[mk-image]\033[0m %s\n' "$*" >&2; }
die() { printf '\033[1;31m[mk-image] %s\033[0m\n' "$*" >&2; exit 1; }

for t in mformat mcopy mmd mke2fs python3; do
	command -v "$t" >/dev/null 2>&1 || die "missing host tool: $t (mtools and e2fsprogs)"
done
[ -f "$LIMINE_DATADIR/BOOTX64.EFI" ] || die "no BOOTX64.EFI in $LIMINE_DATADIR"
ls "$REPO"/dists/"$SUITE"/Release >/dev/null 2>&1 ||
	die "no repository at $REPO -- run tools/deb/build-deb.sh and tools/deb/publish-repo.sh"

in_rootfs() { CHROOT_BASE="$ROOTFS_BASE" BUILD_DEPS="$SYSTEM_PKGS" sh "$CHROOT" run "$@"; }

# The kernel release, composed exactly as the kernel composes it for uname(2).
# The image has to name the versioned package: b1nix-kernel is a metapackage
# whose version does not move when the kernel binary does, so reinstalling it
# alone leaves the old kernel in place -- silently, with every timestamp around
# it looking fresh.
_vh="$ROOT_DIR/kernel/include/b1nix/version.h"
RELEASE="$(sed -n 's/^#define B1NIX_LINUX_ABI_RELEASE[ \t]*"\([^"]*\)".*/\1/p' "$_vh" | head -1)-b1nix-$(sed -n 's/^#define B1NIX_VERSION_STR[ \t]*"\([^"]*\)".*/\1/p' "$_vh" | head -1)"

mkdir -p "$WORK" "$BUILD_DIR"

# ── 1. the root tree ────────────────────────────────────────────────────────
# Reuse what is already there. Assembling the root tree is the slow half of
# this script -- an apt run against a local repository still unpacks and
# configures a few hundred megabytes -- and it only has to happen when the
# packages or the files staged by hand have actually changed. The stamp records
# both, so a rebuild that changes nothing costs a few seconds instead of four
# minutes, and REUSE=0 forces the long way when in doubt.
STAMP="$ROOTFS/.b1nix-image-stamp"
stamp_now() {
	printf 'profile=%s cmdline=%s\n' "$PROFILE" "$CMDLINE_EXTRA"
	for d in "$REPO"/pool/main/*.deb; do
		[ -f "$d" ] || continue
		printf '%s %s\n' "$(basename "$d")" "$(stat -c %Y "$d")"
	done
	# The files this script stages itself: change one and the image must be
	# rebuilt even though no package moved.
	printf 'self %s\n' "$(stat -c %Y "$0")"
}
NEW_STAMP="$(stamp_now)"

if [ "${REUSE:-1}" = "1" ] && [ -f "$STAMP" ] &&
   [ "$NEW_STAMP" = "$(cat "$STAMP" 2>/dev/null)" ]; then
	log "root tree is current -- reusing it (REUSE=0 to rebuild it anyway)"
	SKIP_ROOTFS=1
else
	SKIP_ROOTFS=0
fi

log "assembling the $SUITE root tree ($PROFILE profile)"
if [ "$SKIP_ROOTFS" = "0" ]; then
CHROOT_BASE="$ROOTFS_BASE" BUILD_DEPS="$SYSTEM_PKGS" sh "$CHROOT" create ||
	die "could not build the root tree"

# b1nix-base-files ships the source for the published overlay, which does not
# exist yet and will not answer. On a rebuild over a root tree that already has
# the package installed, that unreachable source makes every apt run fail, so
# it is set aside for the duration of the build and put back at the end -- the
# image must ship it, the build must not trip over it.
SHIPPED_SOURCE="$ROOTFS/etc/apt/sources.list.d/b1nix.sources"
[ ! -f "$SHIPPED_SOURCE" ] || mv "$SHIPPED_SOURCE" "$SHIPPED_SOURCE.build-disabled"

repo_rel="${REPO#"$ROOT_DIR"/}"
# trusted=yes: the tree's repository is signed only when a key exists, and an
# image built from an unsigned local repository is a development artifact. The
# release image is built from the signed one, and INSTALL-SMOKE checks that.
in_rootfs "printf 'deb [trusted=yes] file:/src/$repo_rel $SUITE main\n' >/etc/apt/sources.list.d/b1nix.list" ||
	die "could not add the overlay repository"
in_rootfs "apt-get update -qq" || die "apt-get update failed in the root tree"
# apt-get clean first: between tags the package version does not change while
# the kernel binary does, and apt reinstalls from its own cache by version. The
# image then carries yesterday kernel while every timestamp says otherwise --
# which cost an afternoon of chasing a fix that was already in the tree.
in_rootfs "apt-get clean" || die "could not clear the apt cache in the root tree"
# A build interrupted mid-install leaves dpkg half-configured, and every later
# run then refuses with "dpkg was interrupted". Finishing the previous run is
# the repair dpkg itself asks for, and it costs nothing when there is nothing
# to finish.
in_rootfs "dpkg --configure -a" >/dev/null 2>&1 || true
# And a run that failed in the UNPACK leaves a package neither installed nor
# absent, which `dpkg --configure -a` cannot finish: apt then refuses every
# later install with "unmet dependencies ... you might want to run apt
# --fix-broken install", naming a package that is in fact right there in the
# repository. This is that repair, and it too costs nothing when the tree is
# clean. The root tree is cached between builds, so one failed build otherwise
# poisons every build after it.
in_rootfs "DEBIAN_FRONTEND=noninteractive apt-get -y --fix-broken install" >/dev/null 2>&1 || true
in_rootfs "DEBIAN_FRONTEND=noninteractive apt-get install -y --reinstall b1nix-kernel b1nix-kernel-$RELEASE b1nix-base-files b1nix-tools" ||
	die "installing the overlay failed"

# The command line every entry carries. console=ttyS0 is what makes systemd,
# the getty and the kernel talk to the serial port, which is the only console a
# smoke lane can read.
cat >"$ROOTFS/etc/default/b1nix-boot" <<-EOF
	# Read by b1nix-update-bootloader. Edit here, not in /tools/image/limine.conf.
	CMDLINE_EXTRA="$CMDLINE_EXTRA"
	TIMEOUT=1
EOF

# The kernel package's postinst runs in a chroot with no idea what the root
# device will be, so the boot configuration is generated here, where we know.
in_rootfs "ESP=/boot ROOT_SPEC=LABEL=$ROOT_LABEL /usr/sbin/b1nix-update-bootloader" ||
	die "generating the boot configuration failed"

# A machine with no root password and no user is not a system anyone can log
# into; a machine with a password in an image is worse. An empty root password
# on a development image, stated here, is the honest middle.
in_rootfs "sed -i 's|^root:[^:]*:|root::|' /etc/shadow" || true
in_rootfs "systemctl enable b1nix-boot-good.service" ||
	log "could not enable b1nix-boot-good.service -- the fallback will never be cleared"
# No serial getty. systemd will not start one until `dev-ttyS0.device` exists,
# and that unit is created from a udev event this kernel does not send for the
# serial port -- so the job waits its 90 seconds, fails, and takes the rest of
# the boot with it often enough to make the lane useless. The console is read
# through -serial anyway. The missing uevent is a kernel gap with its own entry
# in docs/kernel/abi-gaps.md, not something to paper over here.
in_rootfs "systemctl disable serial-getty@ttyS0.service" >/dev/null 2>&1 || true
# Put the shipped source back, and take the local one away: the installed
# system points at the published repository, not at a path on the build host.
[ ! -f "$SHIPPED_SOURCE.build-disabled" ] || mv "$SHIPPED_SOURCE.build-disabled" "$SHIPPED_SOURCE"
rm -f "$ROOTFS/etc/apt/sources.list.d/b1nix.list"

# A diagnostic init, for booting with init=/usr/local/sbin/b1nix-diag. It runs
# the kernel interfaces an init system needs, one per line with its result, and
# then stops. When systemd fails with "Invalid argument" and no syscall trace
# says why, this is what separates a kernel refusal from a systemd decision.
mkdir -p "$ROOTFS/usr/local/sbin"
cat >"$ROOTFS/usr/local/sbin/b1nix-diag" <<'DIAG'
#!/bin/sh
say() { printf 'DIAG %s=%s\n' "$1" "$2" >/dev/console; }
mount -t proc proc /proc 2>/dev/null; say proc $?
mount -t tmpfs tmpfs /mnt 2>/dev/null; say tmpfs-plain $?
umount /mnt 2>/dev/null
mount -t tmpfs -o mode=1777,strictatime,nosuid,nodev,size=50%,nr_inodes=1M tmpfs /mnt 2>/dev/null
say tmpfs-systemd-opts $?
umount /mnt 2>/dev/null
mount -t tmpfs -o strictatime tmpfs /mnt 2>/tmp/e; say tmpfs-strictatime $?
while read -r l; do say "strictatime-err=$l"; done </tmp/e
# Which call refuses it, and with what. The result code alone cannot tell the
# old mount(2) apart from the new fsopen/fsconfig sequence, and the two are
# fixed in different places.
strace -o /tmp/st -e trace=mount,fsopen,fsconfig,fsmount,move_mount,open_tree \
	mount -t tmpfs -o strictatime tmpfs /mnt >/dev/null 2>&1
while read -r l; do say "strace=$l"; done </tmp/st
umount /mnt 2>/dev/null
mount -t tmpfs -o size=50% tmpfs /mnt 2>/dev/null; say tmpfs-size-percent $?
umount /mnt 2>/dev/null
mount -t tmpfs -o nr_inodes=1M tmpfs /mnt 2>/dev/null; say tmpfs-nr-inodes $?
umount /mnt 2>/dev/null
unshare -m true 2>/dev/null; say unshare-mount-ns $?
mount --make-rslave / 2>/dev/null; say make-rslave $?
unshare -m sh -c 'mount --bind /etc /mnt' 2>/dev/null; say bind-in-ns $?
say done 0
exec /bin/sh
DIAG
chmod 0755 "$ROOTFS/usr/local/sbin/b1nix-diag"

# The in-guest half of DISTRO-SMOKE. It is installed in every image and runs
# only when the command line asks for it, so the image a lane boots is the same
# image a person would install -- a lane that tests a special build tests the
# special build.
cat >"$ROOTFS/usr/local/sbin/b1nix-smoke" <<'SMOKE'
#!/bin/sh
say() { printf 'DISTRO-SMOKE: %s\n' "$*" >/dev/console; }

# Sample the state only once startup has settled: a unit ordered after
# multi-user.target still runs while jobs are queued, and "starting" says
# nothing about whether the machine came up.
# Bounded, and NOT silent: on a machine where a job never finishes -- which is
# exactly the machine this lane exists to report on -- an unbounded wait hangs
# the checks and the lane learns nothing at all. Worse, a quiet wait looks like
# a wedge: the host kills a guest whose console has said nothing for 45 s, so a
# deliberate wait has to say that it is waiting. It also names what it is
# waiting for, which is the whole question when the wait does not end.
__w=0
while [ $__w -lt 60 ]; do
	__st=$(timeout 10 systemctl is-system-running 2>&1)
	case "$__st" in
	running | degraded) break ;;
	esac
	# "starting" while the ONLY job left is this unit is as far as a boot can
	# get: systemd counts our own job, so waiting for "running" from inside a
	# unit waits for something that cannot happen. Linux behaves the same way.
	__jobs=$(timeout 10 systemctl list-jobs --no-legend --no-pager 2>/dev/null)
	__njobs=$(printf '%s\n' "$__jobs" | grep -c '[a-z]')
	if [ "$__njobs" = "1" ] &&
		printf '%s\n' "$__jobs" | grep -q 'b1nix-smoke\.service'; then
		say "startup=$__st with only this unit left after ${__w}s"
		break
	fi
	say "startup=$__st jobs=$__njobs after ${__w}s"
	__w=$((__w + 5))
	sleep 5
done

say "uname=$(uname -r)"
say "os-release=$(sed -n 's/^PRETTY_NAME=//p' /etc/os-release | tr -d \")"
say "state=$(systemctl is-system-running 2>/dev/null)"

# Every failed unit, one per line, so the host side can compare the set against
# the list of the ones we know about. A unit that starts failing must be seen;
# a unit that stops failing must be noticed too.
systemctl --failed --no-legend --plain 2>/dev/null | awk '{print $1}' | while read -r u; do
	[ -n "$u" ] || continue
	say "failed-unit=$u"
	# Why, not just which. A list of unit names sends the reader back into the
	# guest; the status lines usually name the syscall or the path that failed.
	systemctl status --no-pager --lines=0 "$u" 2>/dev/null |
		grep -E "Process|Failed|error|Invalid|Operation|status=|Result" | head -3 |
		while read -r why; do say "why[$u]=$why"; done
	# And what the unit itself said before it died. journald works now, so the
	# message that names the failing path or call is usually right here.
	journalctl -u "$u" --no-pager -n 3 -o cat 2>/dev/null |
		while read -r jl; do say "log[$u]=$jl"; done
done

# The boot state, as the machine sees it: which entry booted, how many tries it
# has left, and whether the unit that marks a boot good has run.
# Why /boot is not mounted, when it is not: the ESP carries the boot state, and
# the unit that marks a boot good writes it there. systemd generates boot.mount
# from fstab and orders it after the device unit for /dev/vda2, so the answer is
# either "no unit" (the generator skipped the line) or "no device" (udev never
# announced the partition to systemd).
say "boot-unit=$(timeout 15 systemctl show -p LoadState -p ActiveState -p Result boot.mount 2>&1 | tr '\n' ' ')"
say "boot-device=$(timeout 15 systemctl show -p LoadState -p ActiveState dev-vda2.device 2>&1 | tr '\n' ' ')"
say "boot-fstab=$(grep -a boot /etc/fstab 2>/dev/null | tr '\n' ' ')"
timeout 15 journalctl -u boot.mount -n 5 --no-pager 2>&1 |
	while read -r bl; do say "boot-log=$bl"; done

if [ -r /boot/b1nix/boot-state ]; then
	while read -r l; do say "boot-state=$l"; done </boot/b1nix/boot-state
else
	say "boot-state=MISSING"
fi

# apt against a repository shared from the host over 9p: no network needed, and
# it proves the whole path -- mount, read, index parse, install.
# No options: this kernel's 9p takes the tag and nothing else, which is how
# the tree's own lanes mount hostshare. trans=/version=/ro made mount(8) answer
# "bad option" before the kernel ever saw the call.
if mount -t 9p b1nixrepo /mnt 2>/tmp/9p.err; then
	say "repo-mounted=yes"
	printf 'deb [trusted=yes] file:/mnt trixie main\n' >/etc/apt/sources.list.d/b1nix-smoke.list
	if apt-get update -qq -o Dir::Etc::sourcelist=/etc/apt/sources.list.d/b1nix-smoke.list \
		-o Dir::Etc::sourceparts=/dev/null -o APT::Get::List-Cleanup=0 >/dev/null 2>&1; then
		say "apt-update=ok"
		if apt-get install -y --reinstall -o Dir::Etc::sourcelist=/etc/apt/sources.list.d/b1nix-smoke.list \
			-o Dir::Etc::sourceparts=/dev/null b1nix-tools >/dev/null 2>&1; then
			say "apt-install=ok"
		else
			say "apt-install=FAIL"
		fi
	else
		say "apt-update=FAIL"
		apt-get update -o Dir::Etc::sourcelist=/etc/apt/sources.list.d/b1nix-smoke.list \
			-o Dir::Etc::sourceparts=/dev/null -o APT::Get::List-Cleanup=0 2>&1 |
			tail -3 | while read -r l; do say "apt-err=$l"; done
		# Which call failed, not which message apt printed. apt reports "read
		# (22: Invalid argument)" without saying what it read, and the answer
		# decides whether this is a kernel gap or apt's own fallback working as
		# designed. strace is in the image for exactly this.
		if command -v strace >/dev/null 2>&1; then
			# Not silent, and short: the host kills a guest whose console has
			# said nothing for 45 s, and a quiet trace looks exactly like the
			# wedge this lane exists to catch.
			say "apt-strace=starting"
			timeout 30 strace -f -qq -e trace=openat,open,read,pread64,readv,preadv,preadv2,symlink,symlinkat,lseek,ioctl \
				apt-get update -o Dir::Etc::sourcelist=/etc/apt/sources.list.d/b1nix-smoke.list \
				-o Dir::Etc::sourceparts=/dev/null -o APT::Get::List-Cleanup=0 \
				>/tmp/apt-strace.txt 2>&1
			say "apt-strace=done"
			grep -a -- "-1 E" /tmp/apt-strace.txt | tail -12 |
				while IFS= read -r l; do say "apt-syscall=$l"; done
		fi
	fi
else
	say "repo-mounted=no"
	while read -r l; do say "9p-err=$l"; done </tmp/9p.err
fi

# What the kernel says about /tmp, next to what systemd thinks of it. A mount
# unit that reports "Result: protocol" means systemd could not see its mount
# appear, so the interesting question is whether the mount is there at all and
# under which name.
say "tmp-mounted=$(awk '$5 == "/tmp" {print $5, $9; found=1} END {if (!found) print "no"}' /proc/self/mountinfo | head -1)"
say "runlock-mounted=$(awk '$5 == "/run/lock" {print $5, $9; found=1} END {if (!found) print "no"}' /proc/self/mountinfo | head -1)"
say "mountinfo-lines=$(wc -l </proc/self/mountinfo)"
say "dev-vda=$(ls /dev/vda* 2>&1 | tr '\n' ' ')"
say "boot-mount-try=$(mount /boot 2>&1 | head -1; echo rc=$?)"
say "boot-mounted=$(awk '$5 == "/boot" {print $9; found=1} END {if (!found) print "no"}' /proc/self/mountinfo | head -1)"
# The policy calls Debian units use, answered by the kernel rather than guessed.
say "sched-idle=$(chrt --idle 0 true 2>&1 | head -1; echo rc=$?)"

say "done"
systemctl poweroff --no-block 2>/dev/null || { sync; echo o >/proc/sysrq-trigger; }
SMOKE
chmod 0755 "$ROOTFS/usr/local/sbin/b1nix-smoke"

cat >"$ROOTFS/etc/systemd/system/b1nix-smoke.service" <<'UNIT'
[Unit]
Description=b1nix distribution smoke checks
# Only when asked for: an installed system must never run this.
ConditionKernelCommandLine=b1nix.smoke
After=multi-user.target b1nix-boot-good.service
Wants=b1nix-boot-good.service

[Service]
Type=oneshot
ExecStart=/usr/local/sbin/b1nix-smoke
StandardOutput=journal

[Install]
WantedBy=multi-user.target
UNIT
# Why a boot stopped, when it stops before the checks above can run.
#
# A wedged boot prints nothing: the units that would have printed are the ones
# waiting. This runs from sysinit, before anything can block, and says every few
# seconds what the manager is still waiting for and which units have failed.
# Gated on b1nix.smoke, like the harness itself, and it stops on its own.
cat >"$ROOTFS/usr/local/sbin/b1nix-boot-probe" <<'PROBE'
#!/bin/sh
# Bounded: 24 reports, five seconds apart, then gone.
# To the console, not /dev/kmsg: a kmsg write lands at the info level, which
# the console drops unless the command line raised loglevel -- and this has to
# be readable on the lane's ordinary command line.
say() { printf 'BOOT-PROBE: %s\n' "$*" >/dev/console 2>/dev/null ||
	echo "BOOT-PROBE: $*"; }
# The two mount units Debian's local-fs.target waits for, asked about once:
# their own journal carries systemd's reason, and a mount done by hand next to
# them says whether the kernel or the manager is at fault.
for u in run-lock.mount tmp.mount; do
	timeout 15 systemctl show -p Result -p ActiveState "$u" 2>&1 |
		while IFS= read -r l; do say "  $u $l"; done
	timeout 15 journalctl -u "$u" -n 6 --no-pager 2>&1 |
		while IFS= read -r l; do say "  $u| $l"; done
done
say "systemd $(timeout 10 systemctl --version 2>&1 | head -1)"
# The table itself, line for line. Everything else about this is inference; the
# bytes systemd's parser is handed are not.
timeout 10 cat /proc/self/mountinfo | while IFS= read -r l; do say "  MI| $l"; done
# And what the distribution's own libmount makes of it, which is the same
# parser systemd uses.
run findmnt --target /tmp --output TARGET,SOURCE,FSTYPE
run findmnt --target /run/lock --output TARGET,SOURCE,FSTYPE
# Which descriptors PID 1 holds, and what they name: the mount monitor is one of
# them, and knowing whether it watches /proc/self/mountinfo directly or through
# libmount's own epoll says which path has to carry the notification.
for f in /proc/1/fd/*; do
	[ -e "$f" ] || continue
	say "  pid1 fd $(basename "$f") -> $(readlink "$f" 2>/dev/null)"
done

# Started again, now that the boot is quiet: a unit that fails at 0.9 s and
# works at 7 s failed to a race, and one that fails both times does not.
for u in tmp.mount run-lock.mount; do
	timeout 20 systemctl reset-failed "$u" 2>/dev/null
	if timeout 25 systemctl start "$u" >/tmp/probe-$u.err 2>&1 &&
		[ "$(timeout 10 systemctl is-active "$u" 2>&1)" = "active" ]; then
		say "retry $u: started"
	else
		say "retry $u: $(timeout 10 systemctl show -p Result -p ActiveState "$u" 2>&1 | tr '\n' ' ')$(head -1 /tmp/probe-$u.err 2>/dev/null)"
	fi
done

# What PID 1 actually does when the mount table changes. strace is in the
# image; eight seconds of it around a mount says whether the manager is told at
# all -- whether its epoll returns the mountinfo descriptor and whether it then
# opens the file again. Everything else about this bug is inference.
if command -v strace >/dev/null 2>&1; then
	( timeout 8 strace -qq -f -p 1 -e trace=epoll_wait,epoll_pwait,openat,ppoll \
		>/tmp/probe-strace.txt 2>&1 ) &
	sleep 2
	mkdir -p /run/b1nix-strace-mnt
	timeout 15 mount -t tmpfs tmpfs /run/b1nix-strace-mnt -o mode=1777 2>/dev/null
	sleep 4
	timeout 10 umount /run/b1nix-strace-mnt 2>/dev/null
	wait
	say "strace of pid 1 around a mount:"
	grep -a "mountinfo" /tmp/probe-strace.txt | tail -6 |
		while IFS= read -r l; do say "  st $l"; done
	tail -8 /tmp/probe-strace.txt | while IFS= read -r l; do say "  st $l"; done
fi

mkdir -p /run/b1nix-probe-mnt
if timeout 20 mount -t tmpfs tmpfs /run/b1nix-probe-mnt \
	-o mode=1777,strictatime,nosuid,nodev,size=5242880 >/tmp/probe-mnt.err 2>&1; then
	if awk '{print $5}' /proc/self/mountinfo | grep -qx /run/b1nix-probe-mnt; then
		say "by-hand mount: mounted AND named in mountinfo"
	else
		say "by-hand mount: mounted but NOT in mountinfo"
		timeout 10 awk '{print $1, $2, $5, $9}' /proc/self/mountinfo |
			tail -10 | while IFS= read -r l; do say "  mi $l"; done
	fi
	timeout 10 umount /run/b1nix-probe-mnt 2>/dev/null
else
	say "by-hand mount FAILED: $(head -2 /tmp/probe-mnt.err | tr '\n' ' ')"
fi

i=0
while [ $i -lt 24 ]; do
	i=$((i + 1))
	_state=$(timeout 10 systemctl is-system-running 2>&1)
	_jobs=$(timeout 10 systemctl list-jobs --no-legend --no-pager 2>/dev/null | wc -l)
	say "t=$(cut -d\  -f1 /proc/uptime) state=$_state jobs=$_jobs"
	timeout 10 systemctl list-jobs --no-legend --no-pager 2>/dev/null |
		head -30 | while IFS= read -r l; do say "  job $l"; done
	timeout 10 systemctl list-units --state=failed --no-legend --no-pager 2>/dev/null |
		head -8 | while IFS= read -r l; do say "  failed $l"; done
	# A unit stuck in "activating" is the one holding the boot: the jobs above
	# are all merely waiting for it.
	timeout 10 systemctl list-units --state=activating --no-legend --no-pager 2>/dev/null |
		head -8 | while IFS= read -r l; do say "  activating $l"; done
	case "$_state" in running | degraded) say "system is up"; exit 0 ;; esac
	sleep 5
done
say "gave up watching"
PROBE
chmod 0755 "$ROOTFS/usr/local/sbin/b1nix-boot-probe"

cat >"$ROOTFS/etc/systemd/system/b1nix-boot-probe.service" <<'UNIT'
[Unit]
Description=b1nix boot progress probe
ConditionKernelCommandLine=b1nix.smoke
DefaultDependencies=no
After=systemd-journald.service
Before=basic.target

[Service]
Type=simple
ExecStart=/usr/local/sbin/b1nix-boot-probe
StandardOutput=journal

[Install]
WantedBy=sysinit.target
UNIT
in_rootfs "systemctl enable b1nix-boot-probe.service" ||
	die "could not enable the boot probe"

in_rootfs "systemctl enable b1nix-smoke.service" || die "could not enable the smoke unit"

printf 'b1nix\n' >"$ROOTFS/etc/hostname"
# By device, not by label. A label is resolved through /dev/disk/by-label,
# which udev creates -- and systemd-udevd does not come up on this kernel yet,
# so a label in fstab means /boot is never mounted and the boot-counting state
# is invisible to the running system. The image knows its own layout: the ESP
# is the second partition of the only virtio disk.
printf 'LABEL=%s / ext4 defaults 0 1\n' "$ROOT_LABEL" >"$ROOTFS/etc/fstab"
printf '/dev/vda2 /boot vfat defaults,nofail 0 2\n' >>"$ROOTFS/etc/fstab"

if [ "$PROFILE" = "broken" ]; then
	# A second kernel that boots and then cannot bring userspace up: the
	# initramfs hook still runs, so it spends a try on every attempt, which
	# is exactly the case the fallback is for. Its release sorts above the
	# real one, so it becomes the default.
	_real=$(ls "$ROOTFS"/boot/b1nix-* 2>/dev/null | grep -v debug | head -1)
	[ -n "$_real" ] || die "no kernel in the root tree to derive a broken one from"
	_rel="${_real##*/b1nix-}"
	_broken="${_rel%.*}.$(( ${_rel##*.} + 1 ))"
	log "adding a deliberately failing kernel $_broken"
	cp "$_real" "$ROOTFS/boot/b1nix-$_broken"
	cp "$ROOTFS/boot/initrd-$_rel" "$ROOTFS/boot/initrd-$_broken" 2>/dev/null ||
		die "no initramfs to copy for the broken kernel"
	in_rootfs "ESP=/boot ROOT_SPEC=LABEL=$ROOT_LABEL /usr/sbin/b1nix-update-bootloader" ||
		die "regenerating the boot configuration failed"
	# init=/bin/false on the broken entry only: the kernel boots, the
	# initramfs hook counts the try, and userspace never comes up, so
	# b1nix-boot-good never marks it good.
	# The file b1nix-update-bootloader just wrote, which is on the ESP under
	# the root tree -- not the template in the source tree, which is what this
	# named and which does not exist inside the chroot at all: the stage died
	# with FileNotFoundError before the fallback was ever exercised.
	python3 - "$ROOTFS/boot/limine.conf" "$_broken" <<-'PY'
		import sys
		path, broken = sys.argv[1], sys.argv[2]
		out = []
		for line in open(path):
		    if line.lstrip().startswith("cmdline:") and f"b1nix.entry={broken}" in line:
		        line = line.rstrip("\n") + " init=/bin/false\n"
		    out.append(line)
		open(path, "w").writelines(out)
	PY
fi

printf '%s' "$NEW_STAMP" >"$STAMP"
fi  # SKIP_ROOTFS

# ── 2. the ESP ──────────────────────────────────────────────────────────────
log "writing the ESP ($ESP_MIB MiB)"
ESP_IMG="$WORK/esp-$PROFILE.img"
rm -f "$ESP_IMG"
# mformat makes a FAT filesystem in a plain file; no loop device, no root.
mformat -i "$ESP_IMG" -C -T $((ESP_MIB * 1024 * 1024 / 512)) -v B1NIX-ESP -F ::
mmd -i "$ESP_IMG" ::/EFI ::/EFI/BOOT ::/b1nix
mcopy -i "$ESP_IMG" "$LIMINE_DATADIR/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
# The BIOS stages installed into the boot partition load this from a
# filesystem; without it the disk gets as far as "boot will fail".
mcopy -i "$ESP_IMG" "$LIMINE_DATADIR/limine-bios.sys" ::/limine-bios.sys
for f in "$ROOTFS"/boot/*; do
	[ -f "$f" ] || continue
	mcopy -i "$ESP_IMG" "$f" "::/$(basename "$f")"
done
[ ! -f "$ROOTFS/boot/b1nix/boot-state" ] ||
	mcopy -i "$ESP_IMG" "$ROOTFS/boot/b1nix/boot-state" ::/b1nix/boot-state

# ── 3. the root filesystem ──────────────────────────────────────────────────
ROOT_MIB=$((DISK_MIB - ESP_MIB - BIOS_MIB - 8))
log "writing the root filesystem ($ROOT_MIB MiB)"
ROOT_IMG="$WORK/root-$PROFILE.img"
rm -f "$ROOT_IMG"
# /boot lives on the ESP; leaving its content in the root filesystem too would
# double the kernel and hide which copy is actually booted.
rm -rf "$WORK/stage-$PROFILE"
mkdir -p "$WORK/stage-$PROFILE"
tar -C "$ROOTFS" --exclude=./boot/* --exclude=./proc/* --exclude=./sys/* \
	--exclude=./src --exclude=./.b1nix-chroot-ready -cf - . |
	tar -C "$WORK/stage-$PROFILE" -xf -
mke2fs -q -t ext4 -L "$ROOT_LABEL" -d "$WORK/stage-$PROFILE" "$ROOT_IMG" "${ROOT_MIB}m" ||
	die "mke2fs failed"

# ── 4. the disk ─────────────────────────────────────────────────────────────
# Three partitions: a BIOS boot partition for Limine's stage 2, the ESP, and
# the root filesystem.
#
# The ESP is there because that is where this is going, and because Limine
# reads limine.conf and the kernels from it under BIOS just as it would under
# UEFI. b1nix does not boot under UEFI today: its multiboot2 header asks for a
# fixed load address at 1 MiB, the firmware is already using that memory, and
# Limine says so ("Could not find viable load address for executable"). The
# tree's own ISOs fail the same way under OVMF, so this is a property of the
# kernel and not of this image; making the kernel relocatable is kernel work
# and has its own entry in docs/kernel/abi-gaps.md.
log "writing the GPT disk image"
rm -f "$IMG"
python3 "$ROOT_DIR/tests/mkgpt4k.py" --block-size 512 "$IMG" "$DISK_MIB" \
	"BIOS:$BIOS_MIB::biosboot" \
	"ESP:$ESP_MIB:$ESP_IMG:esp" \
	"$ROOT_LABEL:$ROOT_MIB:$ROOT_IMG:linux" ||
	die "writing the partition table failed"

command -v limine >/dev/null 2>&1 || die "limine is not installed on the host"
limine bios-install "$IMG" >/dev/null || die "limine bios-install failed"

log "image at $IMG ($(du -h "$IMG" | cut -f1))"
