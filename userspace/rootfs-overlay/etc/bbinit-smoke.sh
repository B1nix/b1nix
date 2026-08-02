#!/bin/sh
# M108: BusyBox-init smoke hook, run by /etc/inittab as a `::wait:` action —
# i.e. by PID 1 itself, after `openrc default` has finished. On any boot that is
# not the bbinit smoke instance this exits immediately and changes nothing.
grep -q 'b1nix.test=1' /proc/cmdline 2>/dev/null || exit 0
grep -q 'b1nix.smoke=bbinit' /proc/cmdline 2>/dev/null || exit 0

# All checks (PID 1 identity, orphan reaping, a usable shell, and whether the
# OpenRC default runlevel really ran under BusyBox init) live in the C test, so
# each marker is emitted only after its result has been verified. This script
# does no verification of its own and prints no marker of its own.
[ -x /bin/m108_smoke ] && /bin/m108_smoke initcheck
