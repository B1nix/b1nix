#!/bin/sh
# arch-stage-harness.sh — write the in-guest harness into an Arch rootfs tree.
#
#   sh tools/images/arch-stage-harness.sh <rootfs> <profile>
#
# Called by tools/images/mk-arch-image.sh. It is a separate file so that a
# change to what the guest measures is a change to one readable script rather
# than to a heredoc nested three deep inside the image builder.
#
# Everything staged here is OURS and prefixed b1nix-; nothing below patches or
# replaces a file Arch ships.
set -eu

ROOTFS="${1:?usage: arch-stage-harness.sh <rootfs> <profile>}"
PROFILE="${2:?usage: arch-stage-harness.sh <rootfs> <profile>}"

[ -d "$ROOTFS" ] || { echo "arch-stage-harness: no such tree: $ROOTFS" >&2; exit 1; }

# ── a wrapper that can be booted in front of systemd ────────────────────────
# Not part of any run that passes: it exists for the case where PID 1 dies
# saying nothing, which is indistinguishable from a PID 1 whose every write
# went nowhere. Booted as init=/b1nix-probe-init.sh, it prints one marker down
# each of the three routes a manager has to the console and then execs the real
# one, so the log says which of those routes carries anything at all.
cat >"$ROOTFS/b1nix-probe-init.sh" <<'PROBE_EOF'
#!/usr/bin/bash
echo "PROBE-INIT: stdout pid=$$"
echo "PROBE-INIT: stderr pid=$$" >&2
echo "PROBE-INIT: console" >/dev/console 2>/dev/null ||
	echo "PROBE-INIT: console FAILED rc=$?"
echo "PROBE-INIT: kmsg" >/dev/kmsg 2>/dev/null ||
	echo "PROBE-INIT: kmsg FAILED rc=$?"
# b1nix.probe-watch=<dir>: list a directory every three seconds, from a process
# that outlives the hand-over. It exists because the state that explains a stall
# is inside the guest at the moment of the stall, and PID 1 is in no position to
# report it -- being stalled is the whole problem. The listing goes to /dev/kmsg
# so it reaches the serial line whoever owns the console.
__watch=$(sed -n 's/.*b1nix\.probe-watch=\([^ ]*\).*/\1/p' /proc/cmdline 2>/dev/null)
if [ -n "$__watch" ]; then
	echo "PROBE-INIT: watching $__watch"
	(
		# Each line is written on its own, and the sink is chosen per line.
		# A single redirect over the whole block is one open, and once
		# systemd has mounted a fresh devtmpfs over /dev that open fails --
		# taking the entire listing with it and leaving a watcher that
		# reports nothing at exactly the moment it is needed.
		__say() {
			echo "$*" >/dev/kmsg 2>/dev/null ||
				echo "$*" >/dev/console 2>/dev/null || true
		}
		__i=0
		while [ $__i -lt 60 ]; do
			__say "PROBE-WATCH t=${__i}x3s $__watch"
			ls -la "$__watch" 2>&1 | while IFS= read -r __l; do
				__say "PROBE-WATCH   $__l"
			done
			__i=$((__i + 1))
			sleep 3
		done
	) &
fi

echo "PROBE-INIT: handing over to systemd"
exec /usr/lib/systemd/systemd "$@"
PROBE_EOF
chmod 0755 "$ROOTFS/b1nix-probe-init.sh"
sh -n "$ROOTFS/b1nix-probe-init.sh" ||
	{ echo "arch-stage-harness: b1nix-probe-init.sh does not parse" >&2; exit 1; }

# ── the systemd harness ─────────────────────────────────────────────────────
cat >"$ROOTFS/b1nix-arch-stage.sh" <<'STAGE_EOF'
#!/bin/sh
# b1nix Arch systemd harness — OUR file, not Arch's. Run by b1nix-arch.service
# once multi-user.target is up. Every marker is printed only after the thing it
# names actually worked; a failure prints ARCH-SMOKE: FAIL and does not stop
# the rest, so one broken stage never hides the others.
PATH=/usr/local/sbin:/usr/local/bin:/usr/bin:/usr/sbin
export PATH
export SYSTEMD_COLORS=0
export SYSTEMD_PAGER=cat
export SYSTEMD_LESS=

# /dev/kmsg is the fallback, and it is not a nicety: console-getty is Type=idle,
# so agetty claims /dev/console (and vhangup()s it) as soon as the boot
# transaction settles -- which is while this script is still running. Writes
# from here then go nowhere and every marker after that point silently vanishes
# from the serial log. /dev/kmsg reaches the same serial line and nobody can
# take it away.
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

say "ARCH-SMOKE: start pid=$$"

# ── 0. which distribution, which systemd, which libc ───────────────────────
# The whole reason this image exists beside the Debian one is that the versions
# are different, so the versions are evidence and get printed on every run
# rather than being assumed from the name of the script.
say "ARCH-SMOKE: os-release=$(sed -n 's/^PRETTY_NAME=//p' /etc/os-release 2>/dev/null | tr -d '"')"
say "ARCH-SMOKE: systemd=$(systemctl --version 2>/dev/null | head -1)"
say "ARCH-SMOKE: libc=$(ldd --version 2>/dev/null | head -1)"
if [ -f /etc/arch-release ]; then
	say "ARCH-SMOKE: ok arch-release"
else
	say "ARCH-SMOKE: FAIL arch-release (/etc/arch-release absent: this is not an Arch tree)"
fi

# ── 1. systemd is PID 1 ────────────────────────────────────────────────────
pid1=$(tr -d '\000' </proc/1/comm 2>/dev/null | tr -d ' \n')
if [ "$pid1" = "systemd" ]; then
	say "ARCH-SMOKE: ok pid1-systemd"
else
	say "ARCH-SMOKE: FAIL pid1-systemd got='$pid1'"
fi

# ── 2. cgroup v2 ───────────────────────────────────────────────────────────
# systemd refuses to run without a unified hierarchy; prove it is really there
# and that this process is inside the unit's own cgroup.
if [ -f /sys/fs/cgroup/cgroup.controllers ]; then
	say "ARCH-SMOKE: controllers=$(cat /sys/fs/cgroup/cgroup.controllers)"
	mycg=$(sed -n 's/^0:://p' /proc/self/cgroup)
	case "$mycg" in
	*b1nix-arch.service*) say "ARCH-SMOKE: ok cgroup2 ($mycg)" ;;
	*) say "ARCH-SMOKE: FAIL cgroup2 cgroup='$mycg'" ;;
	esac
else
	say "ARCH-SMOKE: FAIL cgroup2 (no /sys/fs/cgroup/cgroup.controllers)"
fi

# ── 3. the manager answers ─────────────────────────────────────────────────
state=$(systemctl is-system-running 2>&1)
say "ARCH-SMOKE: is-system-running=$state"
case "$state" in
running | degraded | starting) say "ARCH-SMOKE: ok systemctl-state" ;;
*) say "ARCH-SMOKE: FAIL systemctl-state got='$state'" ;;
esac

say "ARCH-SMOKE: --- systemctl list-units --failed ---"
run systemctl --no-pager --no-legend --plain list-units --state=failed
say "ARCH-SMOKE: --- systemctl list-units (active) ---"
run systemctl --no-pager --no-legend --plain list-units --state=active

# What each failed unit actually said. Without this a failure is a name and a
# status number, and the reason is sitting in the journal unread.
for u in $(systemctl --no-pager --no-legend --plain list-units --state=failed |
	awk '{print $1}'); do
	say "ARCH-SMOKE: --- journal: $u ---"
	run journalctl -u "$u" -b --no-pager -n 12
done

# ── 4. specific units really reached active ────────────────────────────────
for u in systemd-journald.service systemd-tmpfiles-setup.service \
	systemd-udevd.service systemd-logind.service \
	sysinit.target basic.target multi-user.target; do
	st=$(systemctl is-active "$u" 2>&1)
	if [ "$st" = "active" ]; then
		say "ARCH-SMOKE: ok unit-active $u"
	else
		say "ARCH-SMOKE: FAIL unit-active $u state=$st"
	fi
done

# ── 4b. past multi-user: graphical.target ──────────────────────────────────
# graphical.target is what a desktop machine's default target is, and reaching
# it is the honest measure of "further than multi-user". It Requires
# multi-user.target and Wants display-manager.service; a machine with no
# display manager installed still reaches it, on Linux and here.
gstate=$(timeout 30 systemctl start graphical.target 2>&1)
grc=$?
gst=$(timeout 15 systemctl is-active graphical.target 2>&1)
if [ "$gst" = "active" ]; then
	say "ARCH-SMOKE: ok unit-active graphical.target"
else
	say "ARCH-SMOKE: FAIL unit-active graphical.target state=$gst rc=$grc out='$gstate'"
fi

# ── 5. the journal ─────────────────────────────────────────────────────────
if journalctl -b --no-pager -n 5 >/tmp/jout 2>/tmp/jerr; then
	lines=$(wc -l </tmp/jout | tr -d ' ')
	if [ "$lines" -gt 0 ]; then
		say "ARCH-SMOKE: ok journalctl ($lines lines)"
		run cat /tmp/jout
	else
		say "ARCH-SMOKE: FAIL journalctl (empty)"
	fi
else
	say "ARCH-SMOKE: FAIL journalctl status=$?"
	run cat /tmp/jerr
fi

# ── 6. the manager can start something on demand ───────────────────────────
# systemd-run exercises the whole path: bus call to PID 1, a transient unit, a
# new cgroup, fork+exec, and the exit status coming back.
out=$(systemd-run --quiet --wait --pipe --collect \
	/bin/sh -c 'echo b1nix-transient-ok' 2>&1)
case "$out" in
*b1nix-transient-ok*) say "ARCH-SMOKE: ok systemd-run" ;;
*) say "ARCH-SMOKE: FAIL systemd-run out='$out'" ;;
esac

# ── 7. a getty is running on the console ───────────────────────────────────
gst=$(systemctl is-active console-getty.service 2>&1)
if [ "$gst" = "active" ]; then
	say "ARCH-SMOKE: ok console-getty"
else
	say "ARCH-SMOKE: FAIL console-getty state=$gst"
fi

# ── 8. udev is really running, not merely "active" ─────────────────────────
# udevadm --ping is a round trip over /run/udev/control: the daemon answers it
# itself, so a reply is the daemon and not systemd's opinion of it.
if timeout 15 udevadm control --ping >/dev/null 2>&1; then
	say "ARCH-SMOKE: ok udevadm-ping"
else
	say "ARCH-SMOKE: FAIL udevadm-ping rc=$?"
	say "ARCH-SMOKE: --- udev diagnosis ---"
	run systemctl status --no-pager -l systemd-udevd.service
	run ls -la /run/udev
	cat /proc/b1nix-tasks >/dev/null 2>&1
fi
say "ARCH-SMOKE: --- journal: systemd-udevd.service ---"
run journalctl -u systemd-udevd.service -b --no-pager -n 15

timeout 20 udevadm trigger --action=add >/dev/null 2>&1 ||
	say "ARCH-SMOKE: udevadm trigger rc=$?"
sleep 2
timeout 20 udevadm settle >/dev/null 2>&1 || true

# ── 9. a .device unit really activated ─────────────────────────────────────
# A .device unit exists only because udev told systemd about the device; the
# manager cannot invent one.
say "ARCH-SMOKE: --- systemctl list-units --type=device ---"
run systemctl --no-pager --no-legend --plain list-units --type=device --state=active
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
*.device) say "ARCH-SMOKE: ok device-unit $dev_unit" ;;
*)
	say "ARCH-SMOKE: FAIL device-unit none active"
	say "ARCH-SMOKE: --- udev enumeration diagnosis ---"
	run sh -c 'timeout 15 udevadm info /sys/class/block/vda 2>&1 | head -24'
	run sh -c 'ls /run/udev/data 2>&1 | tr "\n" " "'
	run sh -c 'grep -a " /dev " /proc/self/mountinfo 2>&1'
	cat /proc/b1nix-tasks >/dev/null 2>&1
	;;
esac

# ── 10. logind answers on the bus ──────────────────────────────────────────
# is-active is systemd's bookkeeping; loginctl is a method call logind serves.
if timeout 15 loginctl --no-pager --no-legend list-seats >/tmp/seats 2>/tmp/seaterr; then
	say "ARCH-SMOKE: ok logind-answers seats=$(wc -l </tmp/seats | tr -d ' ')"
else
	say "ARCH-SMOKE: FAIL logind-answers"
	run cat /tmp/seaterr
	run systemctl status --no-pager -l systemd-logind.service
	run sh -c 'journalctl -u systemd-logind.service -b --no-pager -n 40 2>&1'
fi

# ── 11. which bus implementation this is ───────────────────────────────────
# Arch's default message bus is dbus-broker, not the reference dbus-daemon that
# Debian runs. It is a different program making different kernel calls for the
# same job, so which one actually served the calls above is worth recording.
say "ARCH-SMOKE: bus=$(systemctl show -p Id --value dbus.service 2>/dev/null) state=$(systemctl is-active dbus.service 2>&1)"

# ── 12. the unit types nothing above exercised ─────────────────────────────
#
# Reaching multi-user.target proves only that the manager can start services in
# order. What follows drives the parts systemd is built out of -- socket
# activation, timers, sandboxing, the readiness protocol -- each of which
# exercises a different kernel surface underneath.
mkdir -p /run/systemd/system
say "ARCH-SMOKE:   unitdir=[$(ls -d /run/systemd/system 2>&1)] write=[$(: >/run/systemd/system/.probe 2>&1 && echo ok || echo failed)]"

sd_load() {
	__lr=$(timeout 60 systemctl daemon-reload 2>&1)
	__lrc=$?
	[ "$__lrc" = 0 ] ||
		say "ARCH-SMOKE:   daemon-reload for $1 rc=$__lrc: ${__lr:-no output}"
}
# Lines in a file that may not exist. `wc -l < missing` is the SHELL failing to
# open the redirect before wc runs at all, so `2>/dev/null` on wc silences
# nothing and `|| echo 0` never fires.
count_lines() {
	[ -f "$1" ] || { echo 0; return; }
	wc -l <"$1" 2>/dev/null || echo 0
}
sd_start() {
	__u="$1"
	__t0=$(cut -d' ' -f1 /proc/uptime 2>/dev/null)
	__err=$(timeout 25 systemctl start "$__u" 2>&1)
	__rc=$?
	__st=$(timeout 15 systemctl is-active "$__u" 2>&1)
	__t1=$(cut -d' ' -f1 /proc/uptime 2>/dev/null)
	if [ "$__st" = "active" ] || [ "$__st" = "activating" ]; then
		return 0
	fi
	say "ARCH-SMOKE:   start $__u ${__t0}->${__t1} rc=$__rc: ${__err:-no error text} state=$__st"
	return 1
}

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
# listen, and a SYN this machine has to deliver to itself.
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

say "ARCH-SMOKE:   unit files on disk: $(ls /usr/lib/systemd/system 2>/dev/null | wc -l)"
sd_load initial

wait_for_file() {
	__f="$1"; __i=0
	while [ $__i -lt 12 ] && [ ! -f "$__f" ]; do
		__i=$((__i + 1)); sleep 1
	done
	[ -f "$__f" ]
}

# The connectors. This image carries no perl and no python, which is what the
# Debian harness used; it does carry curl, whose --unix-socket speaks AF_UNIX,
# and bash, whose /dev/tcp speaks AF_INET. Neither needs anything installed.
unix_connect() { # unix_connect <path>
	timeout 15 curl -s --max-time 10 --unix-socket "$1" http://localhost/ \
		>/dev/null 2>&1
	return 0
}
tcp_connect() { # tcp_connect <host> <port>
	timeout 15 bash -c "exec 3<>/dev/tcp/$1/$2 && printf 'hi\n' >&3" >/dev/null 2>&1
}

if sd_start b1nix-echo.socket; then
	say "ARCH-SMOKE: ok socket-listening"
	unix_connect /run/b1nix-echo.sock
	if wait_for_file /run/b1nix-echo.done; then
		say "ARCH-SMOKE: ok socket-activation"
	else
		say "ARCH-SMOKE: FAIL socket-activation (socket up, connection did not start the service)"
	fi
else
	say "ARCH-SMOKE: FAIL socket-listening"
fi

if sd_start b1nix-echo-tcp.socket; then
	say "ARCH-SMOKE: ok socket-listening-tcp"
	tcp_connect 127.0.0.1 17999
	__prc=$?
	say "ARCH-SMOKE:   tcp-probe connect rc=$__prc"
	if [ "$__prc" != "0" ]; then
		run sh -c 'cat /proc/net/tcp 2>&1 | head -6'
		run sh -c 'systemctl show -p Listen b1nix-echo-tcp.socket 2>&1'
	fi
	if wait_for_file /run/b1nix-echo-tcp.done; then
		say "ARCH-SMOKE: ok socket-activation-tcp"
	else
		say "ARCH-SMOKE: FAIL socket-activation-tcp (socket up, connection did not start the service)"
	fi
else
	say "ARCH-SMOKE: FAIL socket-listening-tcp"
fi

# The readiness protocol: the manager waits for READY=1 over the datagram
# socket named in NOTIFY_SOCKET.
cat >/run/systemd/system/b1nix-notify.service <<'EOF'
[Service]
Type=notify
NotifyAccess=all
ExecStart=/bin/sh -c 'systemd-notify --ready; sleep 30'
EOF
sd_load b1nix-notify.service
if sd_start b1nix-notify.service; then
	say "ARCH-SMOKE: ok notify-ready"
else
	say "ARCH-SMOKE: FAIL notify-ready"
fi
systemctl stop b1nix-notify.service 2>/dev/null

# PrivateTmp: the unit gets its own /tmp through a mount namespace. The test is
# that its file is NOT visible outside -- and that the unit ran at all, because
# "no file outside" is also what a unit that never started looks like.
#
# On systemd 256 and later PrivateTmp= is implemented with the new mount API
# (fsopen/fsconfig/fsmount/move_mount) rather than mount(2), so this check asks
# a different set of syscalls of the kernel than the same check does on Debian.
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
	say "ARCH-SMOKE: FAIL private-tmp (the unit never ran: $(systemctl is-failed b1nix-private.service 2>&1))"
	run sh -c 'journalctl -u b1nix-private.service -b --no-pager -n 20 2>&1'
elif [ -f /tmp/b1nix-private-probe ]; then
	say "ARCH-SMOKE: FAIL private-tmp (the unit's /tmp was the host's)"
else
	say "ARCH-SMOKE: ok private-tmp"
fi

# ProtectSystem=strict: the filesystem is read-only for the unit, so the write
# must FAIL. Same care as above -- the unit has to have run for the absence of
# the file to mean anything.
rm -f /etc/b1nix-should-not-exist /run/b1nix-ro.ran /run/b1nix-ro.err
cat >/run/systemd/system/b1nix-ro.service <<'EOF'
[Service]
Type=oneshot
ProtectSystem=strict
ExecStart=/bin/sh -c 'echo x > /etc/b1nix-should-not-exist 2>/run/b1nix-ro.err; echo "wrc=$?" >> /run/b1nix-ro.err; echo ran > /run/b1nix-ro.ran'
ReadWritePaths=/run
EOF
sd_load b1nix-ro.service
timeout 25 systemctl start b1nix-ro.service >/dev/null 2>&1
if [ ! -f /run/b1nix-ro.ran ]; then
	say "ARCH-SMOKE: FAIL protect-system (the unit never ran: $(systemctl is-failed b1nix-ro.service 2>&1))"
	run sh -c 'journalctl -u b1nix-ro.service -b --no-pager -n 20 2>&1'
elif [ -f /etc/b1nix-should-not-exist ]; then
	say "ARCH-SMOKE: FAIL protect-system (the write went through)"
	run sh -c 'cat /run/b1nix-ro.err 2>&1 | head -12'
	rm -f /etc/b1nix-should-not-exist
else
	say "ARCH-SMOKE: ok protect-system"
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
		say "ARCH-SMOKE: ok timer-fires"
	else
		say "ARCH-SMOKE: FAIL timer-fires (started, never fired)"
	fi
else
	say "ARCH-SMOKE: FAIL timer-fires"
fi

# Restart=on-failure: the manager notices the exit status and starts it again,
# which is SIGCHLD to PID 1 and waitpid semantics.
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
	say "ARCH-SMOKE: ok restart-on-failure"
else
	say "ARCH-SMOKE: FAIL restart-on-failure (ran $(count_lines /run/b1nix-restart.count) times)"
fi

# The journal, asked for ONE unit rather than the whole boot: that is its index,
# not its ability to append.
#
# Written to STDOUT on purpose. Every other marker goes to /dev/kmsg, which
# reaches journald as a KERNEL message belonging to no unit; this line is the
# unit's own output, through StandardOutput=journal.
echo "ARCH-SMOKE journal-probe $$"
sync 2>/dev/null || true
sleep 1
if timeout 20 journalctl -u b1nix-arch.service -n 20 --no-pager 2>/dev/null |
	grep -q 'ARCH-SMOKE journal-probe'; then
	say "ARCH-SMOKE: ok journal-filter-unit"
else
	say "ARCH-SMOKE: FAIL journal-filter-unit ($(timeout 20 journalctl -u b1nix-arch.service -n 1 --no-pager 2>&1 | head -1))"
fi

# enable/disable moves a unit between states. It needs an [Install] section to
# be enabled at all -- without one the answer is "static", which is a fault of
# the unit and not of the manager.
__ed_a=$(cut -d' ' -f1 /proc/uptime)
timeout 60 systemctl enable b1nix-tick.timer >/dev/null 2>&1
__ed_rc=$?
__ed_b=$(cut -d' ' -f1 /proc/uptime)
say "ARCH-SMOKE:   enable rc=$__ed_rc ${__ed_a}->${__ed_b}"
if [ "$__ed_rc" = "0" ] &&
   [ "$(timeout 20 systemctl is-enabled b1nix-tick.timer 2>&1)" = "enabled" ] &&
   timeout 60 systemctl disable b1nix-tick.timer >/dev/null 2>&1 &&
   [ "$(timeout 20 systemctl is-enabled b1nix-tick.timer 2>&1)" = "disabled" ]; then
	say "ARCH-SMOKE: ok unit-enable-disable"
else
	say "ARCH-SMOKE: FAIL unit-enable-disable: $(timeout 20 systemctl is-enabled b1nix-tick.timer 2>&1)"
	cat /proc/b1nix-tasks >/dev/null 2>&1
fi

# Masking: the strongest "no" -- a masked unit refuses even a direct start.
if timeout 60 systemctl mask b1nix-tick.service >/dev/null 2>&1 &&
   ! timeout 10 systemctl start b1nix-tick.service >/dev/null 2>&1; then
	say "ARCH-SMOKE: ok unit-mask-refuses"
else
	say "ARCH-SMOKE: FAIL unit-mask-refuses (a masked unit started)"
fi
systemctl unmask b1nix-tick.service >/dev/null 2>&1

# How far the boot got, by target rather than by impression, with the chain of
# units it waited on in order.
#
# systemd-analyze refuses to answer while the boot transaction is still running,
# and this harness IS part of that transaction -- asking here only ever prints
# "Bootup is not yet finished". So ask from a process that outlives it.
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
		echo "ARCH-SMOKE: --- systemd-analyze (after the transaction) ---"
		timeout 30 systemd-analyze time 2>&1 | head -4
		timeout 30 systemd-analyze critical-chain 2>&1 | head -24
		timeout 30 systemd-analyze blame 2>&1 | head -10
		echo "ARCH-SMOKE: --- systemd-analyze end ---"
	} >/dev/kmsg 2>&1
) &

say "ARCH-SMOKE: units-loaded=$(systemctl list-units --no-legend --no-pager 2>/dev/null | wc -l) failed=$(systemctl list-units --state=failed --no-legend --no-pager 2>/dev/null | wc -l)"

say "ARCH-SMOKE: done"
STAGE_EOF
chmod 0755 "$ROOTFS/b1nix-arch-stage.sh"
# A harness with a syntax error dies at the first line the shell cannot parse
# and prints nothing after it, which reads exactly like a kernel that stopped
# answering. Parse it here, where the failure is a build error.
sh -n "$ROOTFS/b1nix-arch-stage.sh" ||
	{ echo "arch-stage-harness: b1nix-arch-stage.sh does not parse" >&2; exit 1; }

# ── the graphical harness ───────────────────────────────────────────────────
if [ "$PROFILE" = "graphics" ]; then
	cat >"$ROOTFS/b1nix-arch-graphics.sh" <<'GSTAGE_EOF'
#!/bin/sh
# b1nix Arch graphical harness — OUR file, not Arch's. Started by
# b1nix-arch.service once multi-user.target is up.
#
# Every "ok" below is printed only after the thing it names was observed to
# have happened, and the thing the run is finally judged on is not printed here
# at all: it is the frame the host takes off the virtual GPU with the QEMU
# monitor's screendump, which nothing in this guest takes part in producing.
PATH=/usr/local/sbin:/usr/local/bin:/usr/bin:/usr/sbin
export PATH
export SYSTEMD_COLORS=0 SYSTEMD_PAGER=cat SYSTEMD_LESS=

say() {
	echo "$@" >/dev/kmsg 2>/dev/null ||
		echo "$@" >/dev/console 2>/dev/null ||
		echo "$@"
}
run() {
	timeout 25 "$@" 2>&1 | sed 's/^/    /' |
		while IFS= read -r __l; do say "$__l"; done
}
ok()  { say "GFX-SMOKE: ok $1"; }
bad() { say "GFX-SMOKE: FAIL $1"; }

RUN_SECONDS=${GFX_RUN_SECONDS:-90}
__rs=$(grep -o 'b1nix.gfx-seconds=[0-9]*' /proc/cmdline 2>/dev/null | head -1)
[ -n "$__rs" ] && RUN_SECONDS=${__rs#b1nix.gfx-seconds=}

say "GFX-SMOKE: start pid=$$ run_seconds=$RUN_SECONDS"
say "GFX-SMOKE: os-release=$(sed -n 's/^PRETTY_NAME=//p' /etc/os-release 2>/dev/null | tr -d '"')"
say "GFX-SMOKE: systemd=$(systemctl --version 2>/dev/null | head -1)"
say "GFX-SMOKE: weston=$(weston --version 2>&1 | head -1)"

# ── 0. whose init this is ──────────────────────────────────────────────────
# Read PID 1 from /proc rather than from the boot messages: what a distribution
# prints on the console is a banner, and a banner is not evidence.
pid1=$(tr -d '\000' </proc/1/comm 2>/dev/null | tr -d ' \n')
if [ "$pid1" = "systemd" ]; then
	ok "pid1-systemd"
else
	bad "pid1-systemd got='$pid1'"
fi
if [ -f /etc/arch-release ]; then
	ok "arch-release"
else
	bad "arch-release (this is not an Arch tree)"
fi

# ── 1. the card is there, and sysfs describes it ───────────────────────────
say "GFX-SMOKE: --- /dev/dri ---"
run sh -c 'ls -l /dev/dri 2>&1'
say "GFX-SMOKE: --- /sys/class/drm ---"
run sh -c 'ls -l /sys/class/drm 2>&1'

CARD=
for c in card1 card0; do
	[ -e "/dev/dri/$c" ] || continue
	# A node in /dev that sysfs does not describe is invisible to libudev, and
	# weston finds its card through libudev and nothing else.
	if [ -d "/sys/class/drm/$c" ]; then CARD=$c; break; fi
done
if [ -n "$CARD" ]; then
	ok "drm-card $CARD"
else
	bad "drm-card (no /dev/dri/card* that /sys/class/drm also knows)"
fi

# ── 2. udev catalogued it, which is what gives it a seat ───────────────────
run sh -c 'timeout 20 udevadm trigger --action=add --subsystem-match=drm 2>&1'
timeout 20 udevadm settle --timeout=15 >/dev/null 2>&1 || true
say "GFX-SMOKE: --- /run/udev/data ---"
run sh -c 'ls /run/udev/data 2>&1 | tr "\n" " "'
if ls /run/udev/data/c226:* >/dev/null 2>&1; then
	ok "udev-db-drm"
	run sh -c 'head -20 /run/udev/data/c226:* 2>&1'
else
	bad "udev-db-drm (no /run/udev/data entry for a DRM card)"
fi
if [ -n "$CARD" ]; then
	say "GFX-SMOKE: --- udevadm info on the card ---"
	run sh -c "timeout 15 udevadm info /dev/dri/$CARD 2>&1 | head -25"
fi

# ── 3. logind's view of the seat ───────────────────────────────────────────
say "GFX-SMOKE: --- loginctl seat-status seat0 ---"
run sh -c 'timeout 15 loginctl --no-pager seat-status seat0 2>&1 | head -20'
if timeout 15 loginctl --no-pager show-seat seat0 2>/dev/null | grep -q '^CanGraphical=yes'; then
	ok "seat-can-graphical"
else
	bad "seat-can-graphical (logind does not consider seat0 graphical)"
	run sh -c 'timeout 15 loginctl --no-pager show-seat seat0 2>&1'
fi

# ── 4. the compositor ──────────────────────────────────────────────────────
XDG_RUNTIME_DIR=/run/user/0
export XDG_RUNTIME_DIR
mkdir -p "$XDG_RUNTIME_DIR"
chmod 0700 "$XDG_RUNTIME_DIR"
export XDG_SEAT=seat0 XDG_VTNR=1 XDG_SESSION_TYPE=wayland
export HOME=/root
# Weston 15 has no launcher of its own: every device open goes through libseat.
# Its logind backend hands devices only to a real session, and a system unit is
# not one -- which is correct Linux behaviour and not a defect here. The
# builtin backend is what weston-launch used to be: it opens the card itself as
# root. Named explicitly so the run does not depend on which backend libseat
# happens to try first.
export LIBSEAT_BACKEND=${LIBSEAT_BACKEND:-builtin}
WLOG=/run/weston.log
: >"$WLOG"

if [ -z "$CARD" ]; then
	bad "weston-start (no card to give it)"
	say "GFX: SCANOUT-END"
	say "GFX-SMOKE: done"
	exit 0
fi

say "GFX-SMOKE: starting weston on $CARD (LIBSEAT_BACKEND=$LIBSEAT_BACKEND)"
weston --backend=drm --drm-device="$CARD" --renderer=pixman \
	--continue-without-input --idle-time=0 --tty=1 \
	--log="$WLOG" >/run/weston.stdout 2>&1 &
WPID=$!

# Two independent things have to be true before the compositor is up, and
# waiting for only one of them is how a harness ends up reporting a compositor
# that has already exited: the Wayland socket exists, so clients can connect,
# AND weston's own log says it created an output on the card.
i=0
sock=
while [ $i -lt 60 ]; do
	kill -0 "$WPID" 2>/dev/null || break
	for s in "$XDG_RUNTIME_DIR"/wayland-*; do
		case "$s" in
		*'wayland-*') ;;
		*.lock) ;;
		*) [ -S "$s" ] && sock=$s ;;
		esac
	done
	if [ -n "$sock" ] && grep -qa "utput" "$WLOG" 2>/dev/null; then break; fi
	i=$((i + 1))
	sleep 1
done

if kill -0 "$WPID" 2>/dev/null && [ -n "$sock" ]; then
	ok "weston-socket $(basename "$sock")"
	WAYLAND_DISPLAY=$(basename "$sock")
	export WAYLAND_DISPLAY
else
	bad "weston-socket (no wayland socket after ${i}s)"
fi
say "GFX-SMOKE: --- weston log ---"
run sh -c "head -80 $WLOG 2>&1"
run sh -c 'head -20 /run/weston.stdout 2>&1'

# The DRM backend, not some other one. Weston names the backend it loaded and
# the connector it lit; a run that fell back to a headless backend says so in
# the same file.
if grep -qa "drm-backend\|DRM backend\|Output DRM\|onnector" "$WLOG" 2>/dev/null; then
	ok "weston-drm-backend"
else
	bad "weston-drm-backend (weston's log does not name the DRM backend)"
fi

if ! kill -0 "$WPID" 2>/dev/null; then
	bad "weston-alive (weston exited during start-up)"
	say "GFX: SCANOUT-END"
	say "GFX-SMOKE: done"
	exit 0
fi

# Clients, so the frame carries more than the shell's background.
weston-simple-shm >/run/wshm.log 2>&1 &
SHM_PID=$!
weston-terminal >/run/wterm.log 2>&1 &
TERM_PID=$!
weston-flower >/run/wflower.log 2>&1 &
FLOWER_PID=$!
sleep 5
say "GFX: SCANOUT-READY"
n=0
alive=1
clients_reported=0
while [ $n -lt "$RUN_SECONDS" ]; do
	if ! kill -0 "$WPID" 2>/dev/null; then
		bad "weston-alive (it exited at t=${n}s)"
		alive=0
		break
	fi
	[ $((n % 15)) -eq 0 ] && say "GFX-SMOKE: weston alive t=${n}s"
	if [ "$clients_reported" = 0 ] && [ $n -ge 8 ]; then
		clients_reported=1
		say "GFX-SMOKE: --- weston clients ---"
		for __c in shm term flower; do
			say "GFX-SMOKE: client $__c:"
			run sh -c "head -25 /run/w$__c.log 2>&1"
		done
		# A client that is still running is one that connected, allocated a
		# buffer and got it accepted. kill -0 on the pid the shell recorded,
		# not a name in ps(1): comm is truncated to fifteen characters and
		# weston-simple-shm is seventeen.
		if kill -0 "$SHM_PID" 2>/dev/null; then
			ok "client-drawing"
		else
			bad "client-drawing (no wayland client survived its first frame)"
			run sh -c 'timeout 6 weston-simple-shm; echo "simple-shm rc=$?"'
			run sh -c 'echo "env: XDG_RUNTIME_DIR=$XDG_RUNTIME_DIR WAYLAND_DISPLAY=$WAYLAND_DISPLAY"'
			run sh -c 'ls -la "$XDG_RUNTIME_DIR" 2>&1'
		fi
	fi
	n=$((n + 1))
	sleep 1
done
[ "$alive" = 1 ] && ok "weston-alive"
say "GFX: SCANOUT-END"
say "GFX-SMOKE: --- weston log (tail) ---"
run sh -c "tail -40 $WLOG 2>&1"
kill "$WPID" 2>/dev/null || true

say "GFX-SMOKE: done"
GSTAGE_EOF
	chmod 0755 "$ROOTFS/b1nix-arch-graphics.sh"
	sh -n "$ROOTFS/b1nix-arch-graphics.sh" ||
		{ echo "arch-stage-harness: b1nix-arch-graphics.sh does not parse" >&2; exit 1; }
fi

# ── the unit that runs whichever harness this profile has ───────────────────
if [ "$PROFILE" = "graphics" ]; then
	EXEC=/b1nix-arch-graphics.sh
	DESC="b1nix Arch graphical session harness"
else
	EXEC=/b1nix-arch-stage.sh
	DESC="b1nix Arch systemd boot harness"
fi
mkdir -p "$ROOTFS/etc/systemd/system/multi-user.target.wants"
cat >"$ROOTFS/etc/systemd/system/b1nix-arch.service" <<UNIT_EOF
[Unit]
Description=$DESC
After=multi-user.target systemd-user-sessions.service systemd-logind.service
Wants=multi-user.target

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=$EXEC
StandardOutput=journal+console
StandardError=journal+console
# The harness bounds every command it runs and the run itself is bounded by the
# host script, so this must be looser than both.
TimeoutStartSec=600s

[Install]
WantedBy=multi-user.target
UNIT_EOF
ln -sf /etc/systemd/system/b1nix-arch.service \
	"$ROOTFS/etc/systemd/system/multi-user.target.wants/b1nix-arch.service"
