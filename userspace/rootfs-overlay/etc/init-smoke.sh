#!/bin/sh
# M108: init smoke hook, run by /etc/inittab as a `::wait:` action — i.e. by
# PID 1 itself, after `openrc default` has finished. On any boot that is not the
# init smoke instance this exits immediately and changes nothing.
grep -q 'b1nix.test=1' /proc/cmdline 2>/dev/null || exit 0
grep -q 'b1nix.smoke=init' /proc/cmdline 2>/dev/null || exit 0

# All checks (PID 1 identity, orphan reaping, getty respawn, a usable shell, and
# whether the OpenRC default runlevel really ran under BusyBox init) live in the
# C test, so each marker is emitted only after its result has been verified.
# This script does no verification of its own and prints no marker of its own.
[ -x /bin/m108_smoke ] || exit 0
/bin/m108_smoke initcheck

# The getty check has to outlive this hook: BusyBox init finishes every `wait`
# action before it starts the `respawn` entries, so the getty does not exist yet
# while this script is running. Backgrounding it lets init get on with the rest
# of /etc/inittab; the check then kills the getty it finds and requires PID 1 to
# put a new one there. It prints the run's done marker when it is finished.
/bin/m108_smoke gettycheck &
