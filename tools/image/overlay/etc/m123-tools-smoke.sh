#!/bin/sh
# M123: namespaces as distribution tools use them — util-linux's unshare and
# nsenter, and bubblewrap, all from Alpine and all run by an unprivileged user.
# Each marker is printed only after every property it names was read back from
# inside the sandbox the tool built.

UNPRIV="/usr/bin/setpriv --reuid=1000 --regid=1000 --clear-groups"

# The M109 uevent test runs mdev without a configuration, which leaves the
# memory devices at mdev's default 0660. Inside a user namespace root is not
# the owner of host devices (their owner is unmapped there), so a sandbox gets
# only the "other" bits: give them back their usual 0666.
chmod 666 /dev/null /dev/zero /dev/full /dev/random /dev/urandom 2>/dev/null

# unshare -Urpf --mount-proc: a user namespace mapping the caller to root, a PID
# namespace whose first process is 1, and a proc mount that shows only it.
out="$($UNPRIV /usr/bin/unshare -Urpf --mount-proc /bin/sh -c \
	'id -u; echo $$; ls /proc | grep -c "^[0-9]"; cat /proc/self/uid_map' 2>&1)"
uid="$(echo "$out" | sed -n 1p)"
pid="$(echo "$out" | sed -n 2p)"
npids="$(echo "$out" | sed -n 3p)"
map="$(echo "$out" | sed -n 4p | tr -s ' ' | sed 's/^ //')"
if [ "$uid" = "0" ] && [ "$pid" = "1" ] && [ -n "$npids" ] &&
   [ "$npids" -ge 1 ] && [ "$npids" -le 4 ] && [ "$map" = "0 1000 1" ]; then
	echo "M123-TOOLS: ok unshare-Urpf"
else
	echo "M123-TOOLS: FAIL unshare-Urpf ($(echo "$out" | tr '\n' '|'))"
fi

# nsenter into another process's user and UTS namespaces by pid: it becomes
# root there and sees the hostname that process set.
$UNPRIV /usr/bin/unshare -Uru /bin/sh -c \
	'hostname m123-nsenter; exec sleep 30' </dev/null >/dev/null 2>&1 &
target=$!
seen=""
for i in 1 2 3 4 5 6 7 8 9 10; do
	# The sleeping shell is the unshare process itself after exec.
	seen="$(/usr/bin/nsenter -t "$target" -U -u /bin/sh -c 'id -u; hostname' 2>/dev/null | tr '\n' ' ')"
	[ "$seen" = "0 m123-nsenter " ] && break
	/bin/usleep 100000
done
kill "$target" 2>/dev/null
wait "$target" 2>/dev/null
if [ "$seen" = "0 m123-nsenter " ]; then
	echo "M123-TOOLS: ok nsenter-userns"
else
	echo "M123-TOOLS: FAIL nsenter-userns (seen=$seen)"
fi

# bubblewrap, unprivileged: every namespace, a read-only view of the root, its
# own /proc, /dev and /tmp, a hostname, and no network but loopback.
out="$($UNPRIV /usr/bin/bwrap --unshare-all --uid 0 --gid 0 \
	--ro-bind / / --proc /proc --dev /dev --tmpfs /tmp --hostname m123box \
	-- /bin/sh -c '
		id -u
		hostname
		ls /proc | grep -c "^[0-9]"
		ip -o link 2>/dev/null | grep -vc " lo:"
		if touch /etc/m123-bwrap 2>/dev/null; then echo rw; else echo ro; fi
		echo sandboxed > /tmp/m123 && cat /tmp/m123
		[ -c /dev/null ] && [ -e /dev/pts/ptmx ] && echo dev-ok
	' 2>&1)"
set -- $(echo "$out" | tr '\n' ' ')
if [ "$1" = "0" ] && [ "$2" = "m123box" ] && [ "$3" -ge 1 ] 2>/dev/null &&
   [ "$3" -le 4 ] && [ "$4" = "0" ] && [ "$5" = "ro" ] &&
   [ "$6" = "sandboxed" ] && [ "$7" = "dev-ok" ]; then
	echo "M123-TOOLS: ok bwrap-sandbox"
else
	echo "M123-TOOLS: FAIL bwrap-sandbox ($(echo "$out" | tr '\n' '|'))"
fi
