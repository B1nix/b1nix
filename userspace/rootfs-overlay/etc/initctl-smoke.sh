#!/bin/sh
# M94 — the pieces an init system needs at runtime, exercised the way an init
# system uses them: a tmpfs for volatile state, and a control FIFO that a
# separate process writes while PID 1 reads.

echo "M94-CTL: start"

# tmpfs: mount, create state under it, and see it from a fresh path lookup.
mkdir -p /mnt/rundir
if mount -t tmpfs -o mode=0755,nosuid,nodev tmpfs /mnt/rundir 2>/dev/null; then
	echo "M94-CTL: ok tmpfs-mount"
else
	echo "M94-CTL: fail tmpfs-mount"
fi
# checkpath is OpenRC's own static binary; it creates $RC_SVCDIR the way
# init.sh does, which is the case that caught the mount-seam path bug (a
# directory created through a dirfd inside a mount landed at the root).
/sbin/checkpath -d /mnt/rundir/openrc >/dev/null 2>&1
if : > /mnt/rundir/openrc/softlevel 2>/dev/null && [ -f /mnt/rundir/openrc/softlevel ]; then
	echo "M94-CTL: ok tmpfs-state"
else
	echo "M94-CTL: fail tmpfs-state"
fi

# The control channel itself: mkfifo on the tmpfs, a background writer, and a
# blocking reader — the shape of openrc-shutdown talking to openrc-init.
rm -f /mnt/rundir/openrc/init.ctl
mkfifo /mnt/rundir/openrc/init.ctl 2>/dev/null
if [ -p /mnt/rundir/openrc/init.ctl ]; then
	echo "M94-CTL: ok fifo-on-tmpfs"
else
	echo "M94-CTL: fail fifo-on-tmpfs"
fi
(echo halt > /mnt/rundir/openrc/init.ctl) &
cmd=$(head -n 1 < /mnt/rundir/openrc/init.ctl)
wait
if [ -p /mnt/rundir/openrc/init.ctl ] && [ "$cmd" = "halt" ]; then
	echo "M94-CTL: ok fifo-command"
else
	echo "M94-CTL: fail fifo-command got='$cmd'"
fi
rm -f /mnt/rundir/openrc/init.ctl

echo "M94-CTL: done"
