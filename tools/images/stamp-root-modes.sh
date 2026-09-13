#!/bin/sh
# tools/images/stamp-root-modes.sh - the setuid and private modes a root needs.
#
#   stamp-root-modes.sh ROOTFS
#
# Applied to the staging tree right before it is packed, by every script that
# packs one (mk-root-image.sh, _mkimg in tests/smoke.sh), so the lists cannot
# drift. Ownership is not set here: the tree is packed inside `unshare -r`,
# where the building user's files read as uid 0.
#
# /bin/{su,passwd,login} are symlinks onto /bin/busybox-suid, so the setuid bit
# belongs on that copy; the plain /bin/busybox stays non-setuid.
set -eu

ROOTFS="$1"
# The image root takes the staging directory's own mode.
chmod 0755 "$ROOTFS"
for spec in \
	"4755 bin/m31_setuid" \
	"4755 bin/busybox-suid" \
	"4755 sbin/unix_chkpwd" \
	"0400 etc/shadow" \
	"1777 tmp" \
	"1777 var/tmp" \
; do
	f="$ROOTFS/${spec#* }"
	[ -e "$f" ] && [ ! -L "$f" ] && chmod "${spec%% *}" "$f"
done
exit 0
