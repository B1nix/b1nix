#!/bin/busybox sh
# SPDX-License-Identifier: GPL-2.0-only
# /init of the guests QEMU boots inside b1nix (M131): say what kernel this is
# and on how many CPUs, read the first line of a virtio disk when the command
# line names one (the modules for it listed in /lib/l2-modules), power off.
/bin/busybox mount -t proc proc /proc
echo "B1NIX-L2-INIT $(/bin/busybox uname -r) cpus=$(/bin/busybox grep -c ^processor /proc/cpuinfo) up=$(/bin/busybox cut -d' ' -f1 /proc/uptime)"
if /bin/busybox grep -q b1nix.l2disk=vda /proc/cmdline; then
	for m in $(/bin/busybox cat /lib/l2-modules 2>/dev/null); do
		/bin/busybox insmod "/lib/$m.ko" || echo "B1NIX-L2-INSMOD-FAILED $m"
	done
	/bin/busybox mount -t devtmpfs devtmpfs /dev
	n=0
	while [ ! -b /dev/vda ] && [ $n -lt 50 ]; do
		/bin/busybox usleep 100000
		n=$((n + 1))
	done
	echo "B1NIX-L2-VIRTIO $(/bin/busybox head -n 1 /dev/vda)"
fi
/bin/busybox poweroff -f
