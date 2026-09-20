#!/bin/sh
# M123: a rootless container with Alpine's podman and crun, started by an
# ordinary user. podman takes the user's subordinate ids through newuidmap, and
# crun builds the namespaces, the mounts and the pivot inside them; the marker
# is printed only after the container itself reported what it saw.

# The subordinate ranges an administrator hands a user for rootless containers
# (usermod --add-subuids does the same); Alpine's shadow-subids ships the files
# empty.
for f in /etc/subuid /etc/subgid; do
	grep -q '^user:' "$f" 2>/dev/null || echo "user:100000:65536" >> "$f"
done

WORK=/tmp/m123-podman
rm -rf "$WORK"
mkdir -p "$WORK/rootfs/bin" "$WORK/rootfs/lib" "$WORK/rootfs/proc" \
	"$WORK/rootfs/dev" "$WORK/rootfs/sys" "$WORK/rootfs/etc" \
	"$WORK/rootfs/tmp" "$WORK/home" "$WORK/run"
# The container's root: BusyBox and the musl loader it runs under, nothing else.
cp /bin/busybox "$WORK/rootfs/bin/busybox"
for applet in sh id cat hostname ls grep; do
	ln -s busybox "$WORK/rootfs/bin/$applet"
done
cp /lib/ld-musl-*.so.1 "$WORK/rootfs/lib/"
# A --rootfs container's root belongs to the user running it: podman writes
# /etc/mtab and the resolver files into it.
chown -R 1000:1000 "$WORK/rootfs" "$WORK/home" "$WORK/run"
chmod 0700 "$WORK/run"

# The cgroup v2 hierarchy, where OpenRC's cgroups service would have mounted
# it; podman reads the cgroup version from it even with --cgroups disabled.
mountpoint -q /sys/fs/cgroup || mount -t cgroup2 cgroup2 /sys/fs/cgroup

# Files, not a command substitution: a runtime process left behind by a failed
# run would hold a pipe open and the substitution would never return.
( cd "$WORK/home" && timeout 240 /usr/bin/setpriv --reuid=1000 --regid=1000 \
	--init-groups env HOME="$WORK/home" XDG_RUNTIME_DIR="$WORK/run" \
	/usr/bin/podman --log-level=debug --storage-driver vfs --cgroup-manager cgroupfs \
	run --rm --network none --cgroups disabled \
	--security-opt seccomp=unconfined --hostname m123pod \
	--rootfs "$WORK/rootfs" /bin/sh -c \
	'id -u; echo $$; hostname; ls /proc | grep -c "^[0-9]"; cat /proc/self/uid_map' \
	>"$WORK/out" 2>"$WORK/err" </dev/null )
set -- $(tr '\n' ' ' <"$WORK/out")
# uid_map inside: container root is the user, 1..65536 the subordinate range.
if [ "$1" = "0" ] && [ "$2" = "1" ] && [ "$3" = "m123pod" ] &&
   [ "$4" -ge 1 ] 2>/dev/null && [ "$4" -le 3 ] &&
   [ "$5 $6 $7" = "0 1000 1" ] && [ "$8 $9 ${10}" = "1 100000 65536" ]; then
	echo "M123-PODMAN: ok rootless-run"
else
	echo "M123-PODMAN: FAIL rootless-run ($(tr '\n' '|' <"$WORK/out") $(grep -v "OCI runtime" "$WORK/err" | tail -25 | tr '\n' '|'))"
fi
