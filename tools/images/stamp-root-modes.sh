#!/bin/sh
# tools/images/stamp-root-modes.sh - the setuid inodes a b1nix root image needs.
#
#   DEBUGFS=... stamp-root-modes.sh IMAGE
#
# Two things build a bootable root: tools/images/mk-root-image.sh (build/<arch>/
# root.ext4) and _mkimg in tests/smoke.sh (the per-lane disks the aarch64
# instances boot). They drifted -- the second copied ownership and stopped
# there, so unix_chkpwd was not setuid and /etc/shadow was world-readable on
# every aarch64 lane, while the same tree packed by the first was correct. One
# list, called from both, is the only way that stays fixed.
#
# M108: /bin/{su,passwd,login} are symlinks onto the BusyBox multicall ELF, so
# the setuid bit belongs on the inode they resolve to -- the dedicated
# busybox-suid copy -- and NOT on the symlinks (stamping a mode on a symlink
# inode would only corrupt it). The plain /opt/busybox/bin/busybox that every
# other applet resolves to is deliberately left non-setuid.
set -eu

IMAGE="$1"
DEBUGFS="${DEBUGFS:-debugfs}"

for cmd in \
	"sif /bin/m31_setuid uid 0" \
	"sif /bin/m31_setuid mode 0104755" \
	"sif /opt/busybox/bin/busybox-suid uid 0" \
	"sif /opt/busybox/bin/busybox-suid gid 0" \
	"sif /opt/busybox/bin/busybox-suid mode 0104755" \
	"sif /sbin/unix_chkpwd uid 0" \
	"sif /sbin/unix_chkpwd gid 0" \
	"sif /sbin/unix_chkpwd mode 0104755" \
	"sif /etc/shadow uid 0" \
	"sif /etc/shadow mode 0100400" \
; do
	"$DEBUGFS" -w -R "$cmd" "$IMAGE" 2>/dev/null || true
done
