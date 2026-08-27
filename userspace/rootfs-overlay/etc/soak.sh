#!/bin/sh
# /etc/soak.sh — the guest half of the overnight soak.
#
# One boot runs one set of workloads and says what happened in markers the host
# runner greps for. Nothing here starts unless the kernel cmdline asks for it,
# so an ordinary boot pays a fork and an exit.
#
#   b1nix.soak=<list>       comma-separated: mem,vm,cpu,disk,net,fd,spawn,shm,gfx
#                           or `all`. Order is preserved.
#   b1nix.soak-seconds=N    per-workload wall-clock budget (default 20)
#   b1nix.soak-scale=N      work multiplier in percent (default 100)
#   b1nix.soak-threads=N    threads per workload (default: one per CPU)
#
# Every workload prints `SOAK-<NAME>: ok ...` or `SOAK-<NAME>: fail ...` and the
# run ends with `SOAK: done`, which is what the host waits for. A workload that
# neither passes nor fails — because the kernel died under it — leaves its
# `SOAK-<NAME>: begin` line as the last thing in the log, which is how a hang is
# attributed to a workload rather than to the boot.

# Whole-word cmdline matching. `grep -q b1nix.soak` also matches
# b1nix.soak-seconds, and the dot is a wildcard besides.
cmdline_value() {
	for tok in $(cat /proc/cmdline 2>/dev/null); do
		case "$tok" in
		"$1"=*) echo "${tok#*=}"; return 0 ;;
		esac
	done
	return 1
}

SPEC=$(cmdline_value b1nix.soak) || exit 0
[ -n "$SPEC" ] || exit 0

SECONDS_BUDGET=$(cmdline_value b1nix.soak-seconds || echo 20)
SCALE=$(cmdline_value b1nix.soak-scale || echo 100)
THREADS=$(cmdline_value b1nix.soak-threads || echo "")

export SOAK_SECONDS="$SECONDS_BUDGET"
export SOAK_SCALE="$SCALE"
[ -n "$THREADS" ] && export SOAK_THREADS="$THREADS"
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
export HOME=/root

up() { cut -d" " -f1 /proc/uptime 2>/dev/null || echo "?"; }

echo "SOAK: start spec=$SPEC seconds=$SECONDS_BUDGET scale=$SCALE threads=${THREADS:-auto} boot_t=$(up)"
echo "SOAK: cpus=$(grep -c ^processor /proc/cpuinfo 2>/dev/null) mem=$(awk '/MemTotal/{print $2}' /proc/meminfo 2>/dev/null)kB"

FAILED=""
RAN=""

# Run one stressor as a child and grade it by exit status, not by its output:
# a program killed by a signal prints nothing at all, and that must not read as
# a pass. The `begin` line goes out before the fork so a kernel that dies under
# the workload still names it.
run_one() {
	name=$1
	shift
	echo "SOAK-$(echo "$name" | tr a-z A-Z): begin t=$(up)"
	"$@"
	rc=$?
	if [ "$rc" = 0 ]; then
		echo "SOAK-$(echo "$name" | tr a-z A-Z): ok t=$(up)"
		RAN="$RAN $name"
	else
		echo "SOAK-$(echo "$name" | tr a-z A-Z): fail rc=$rc t=$(up)"
		FAILED="$FAILED $name"
	fi
}

# What it costs to start a program, three ways.
#
#   builtin  — the shell doing nothing at all, so the loop itself is priced.
#   static   — a program with no dynamic linking to do.
#   dynamic  — an ordinary PIE against the shared libc, which is what
#              everything in the image is.
#
# The difference between the last two is the loader's bill, and the difference
# between the first two is the kernel's.
soak_exec() {
	n=${SOAK_EXEC_N:-200}
	t0=$(cut -d. -f1-2 /proc/uptime)

	i=0
	while [ $i -lt $n ]; do : ; i=$((i + 1)); done
	t1=$(cut -d. -f1-2 /proc/uptime)

	i=0
	while [ $i -lt $n ]; do /bin/true; i=$((i + 1)); done
	t2=$(cut -d. -f1-2 /proc/uptime)

	i=0
	while [ $i -lt $n ]; do /bin/hello >/dev/null 2>&1; i=$((i + 1)); done
	t3=$(cut -d. -f1-2 /proc/uptime)

	echo "SOAK-EXEC: n=$n builtin_end=$t1 true_end=$t2 hello_end=$t3 start=$t0"
	return 0
}

# The graphics workload: a compositor with no GPU, a client that draws, and a
# screenshot that has to contain more than one colour.
#
# Headless rather than DRM because this runs under plain QEMU with no card
# assigned — the question here is whether the compositor and its clients
# survive the same kernel the other workloads are hammering, not whether the
# display pipeline works. That has its own instance.
soak_gfx() {
	command -v sway >/dev/null 2>&1 || { echo "SOAK-GFX: skip no-sway"; return 0; }
	mkdir -p /run/user/0
	export XDG_RUNTIME_DIR=/run/user/0
	# Whichever path this machine can actually run, chosen the same way the
	# other compositor launchers choose it: accelerated when a driver is there
	# and answers, software otherwise. The soak is about surviving the kernel
	# underneath, and both renderers put different pressure on it.
	. /etc/render-select.sh
	render_select
	export LIBSEAT_BACKEND=noop
	# A real output when the machine has one.
	#
	# The headless backend has no mode and no vertical blank to pace against,
	# so wlroots repaints continuously — the compositor then never gets round
	# to its clients or its IPC, which reads as a hang and is really a spin. A
	# DRM device gives it a mode and a flip to wait for, and it is also the
	# shape a windowed program actually runs in.
	if [ -e /dev/dri/card0 ] || [ -e /dev/dri/card1 ]; then
		echo "SOAK-GFX: using the DRM backend ($(ls /dev/dri 2>/dev/null | tr '\n' ' '))"
		export WLR_BACKENDS=drm
	else
		echo "SOAK-GFX: no card, falling back to the headless backend"
		export WLR_BACKENDS=headless
		export WLR_HEADLESS_OUTPUTS=1
	fi

	# A watchdog on every child that talks to the compositor. A compositor that
	# stops answering is one of the failures being hunted, and without a bound
	# the client simply waits forever and the run reports a hang with no idea
	# which side stopped. `timeout` is a BusyBox applet here; if it is missing
	# the calls run bare and the host's own limit is the only bound.
	if command -v timeout >/dev/null 2>&1; then
		TMO="timeout"
	else
		TMO=""
	fi

	sway -c /etc/sway/config > /tmp/soak-sway.log 2>&1 &
	swaypid=$!

	i=0
	sock=""
	while [ $i -lt 30 ]; do
		sock=$(ls -1 "$XDG_RUNTIME_DIR" 2>/dev/null | grep '^wayland-[0-9]*$' | head -1)
		[ -n "$sock" ] && break
		i=$((i + 1))
		sleep 1
	done
	if [ -z "$sock" ]; then
		echo "SOAK-GFX: no wayland socket after ${i}s"
		tail -40 /tmp/soak-sway.log
		kill $swaypid 2>/dev/null
		return 1
	fi
	export WAYLAND_DISPLAY="$sock"
	# The IPC socket, which swaymsg needs and which sway does not advertise
	# anywhere but its own environment. Without it every swaymsg fails to
	# connect, which reads as "the compositor stopped answering" and is
	# nothing of the sort.
	SWAYSOCK=$(ls -1 "$XDG_RUNTIME_DIR"/sway-ipc.*.sock 2>/dev/null | head -1)
	export SWAYSOCK
	echo "SOAK-GFX: socket $sock, ipc ${SWAYSOCK:-none}"

	# A client, and then a client again: the second start is the one that finds
	# a compositor whose state is no longer fresh.
	rounds=0
	failures=0
	end=$(( $(cut -d. -f1 /proc/uptime) + SOAK_SECONDS ))
	while [ "$(cut -d. -f1 /proc/uptime)" -lt "$end" ]; do
		if command -v foot >/dev/null 2>&1; then
			echo "SOAK-GFX: round $rounds starting client t=$(up)"
			${TMO:+$TMO 20} foot -- /bin/sh -c 'i=0; while [ $i -lt 200 ]; do echo "soak $i"; i=$((i+1)); done; sleep 2' \
				> /tmp/soak-foot.log 2>&1 &
			fpid=$!
			sleep 3
			echo "SOAK-GFX: round $rounds killing client t=$(up)"
			kill $fpid 2>/dev/null
			# Bounded: a client that will not die must not take the run with
			# it, and what the compositor was doing at that moment is the
			# evidence that says why.
			w=0
			while kill -0 $fpid 2>/dev/null && [ $w -lt 8 ]; do
				sleep 1
				w=$((w + 1))
			done
			if kill -0 $fpid 2>/dev/null; then
				echo "SOAK-GFX: client $fpid ignored SIGTERM for ${w}s t=$(up)"
				echo "--- compositor log tail ---"
				tail -30 /tmp/soak-sway.log 2>/dev/null
				echo "--- client log tail ---"
				tail -10 /tmp/soak-foot.log 2>/dev/null
				kill -9 $fpid 2>/dev/null
				sleep 1
			fi
			echo "SOAK-GFX: round $rounds client reaped t=$(up)"
			echo "  client said: $(tail -3 /tmp/soak-foot.log 2>/dev/null | tr '\n' ' ')"
		else
			sleep 1
		fi
		# The compositor must still answer after every client.
		echo "SOAK-GFX: round $rounds asking the compositor t=$(up)"
		if ! ${TMO:+$TMO 15} swaymsg -t get_outputs > /tmp/soak-outputs.json 2>&1; then
			echo "SOAK-GFX: compositor stopped answering after $rounds client rounds"
			echo "  SWAYSOCK=$SWAYSOCK WAYLAND_DISPLAY=$WAYLAND_DISPLAY"
			echo "  runtime dir: $(ls -1 $XDG_RUNTIME_DIR 2>&1 | tr '\n' ' ')"
			echo "  swaymsg said: $(cat /tmp/soak-outputs.json 2>/dev/null | head -3)"
			failures=$((failures + 1))
			break
		fi
		echo "SOAK-GFX: outputs: $(head -c 200 /tmp/soak-outputs.json 2>/dev/null)"
		rounds=$((rounds + 1))
	done

	# A window, and a picture of it.
	#
	# "The compositor did not crash" is a weak claim: it can be alive with no
	# client, no surface and nothing on screen. The tree says whether a window
	# exists, and a screenshot whose pixels are not all one colour says
	# something was actually drawn into it.
	if command -v foot >/dev/null 2>&1; then
		${TMO:+$TMO 20} foot -- /bin/sh -c 'echo b1nix-window-proof; sleep 6' \
			> /tmp/soak-window.log 2>&1 &
		wpid=$!
		sleep 4
		echo "SOAK-GFX: tree: $(${TMO:+$TMO 10} swaymsg -t get_tree 2>&1 | tr -d '\n' | head -c 220)"
		if command -v grim >/dev/null 2>&1; then
			if ${TMO:+$TMO 20} grim /tmp/soak-shot.png 2>/tmp/soak-grim.log; then
				echo "SOAK-GFX: screenshot $(wc -c < /tmp/soak-shot.png 2>/dev/null) bytes"
			else
				echo "SOAK-GFX: grim failed: $(head -2 /tmp/soak-grim.log 2>/dev/null)"
			fi
		else
			echo "SOAK-GFX: no grim in the image"
		fi
		kill -9 $wpid 2>/dev/null
		wait $wpid 2>/dev/null
	fi

	# Still alive, and its log free of the allocator's own alarm.
	alive=0
	kill -0 $swaypid 2>/dev/null && alive=1
	if grep -qiE 'heap corruption|invalid free|double free|Aborted|assertion' /tmp/soak-sway.log 2>/dev/null; then
		echo "SOAK-GFX: compositor log carries an allocator complaint:"
		grep -iE 'heap corruption|invalid free|double free|Aborted|assertion' /tmp/soak-sway.log | head -5
		failures=$((failures + 1))
	fi
	kill $swaypid 2>/dev/null

	echo "SOAK-GFX: $rounds client rounds, compositor alive=$alive"
	[ "$alive" = 1 ] && [ "$failures" = 0 ]
}

for w in $(echo "$SPEC" | tr ',' ' '); do
	case "$w" in
	all)
		set -- mem vm cpu fd shm spawn disk net gfx
		for sub in "$@"; do
			case "$sub" in
			mem)   run_one mem   /bin/memstress ;;
			vm)    run_one vm    /bin/vmstress ;;
			cpu)   run_one cpu   /bin/cpustress ;;
			fd)    run_one fd    /bin/fdstress ;;
			shm)   run_one shm   /bin/shmstress ;;
			spawn) run_one spawn /bin/spawnstress ;;
			disk)  run_one disk  /bin/diskstress ;;
			net)   run_one net   /bin/netstress ;;
			gfx)   run_one gfx   soak_gfx ;;
			esac
		done
		;;
	mem)   run_one mem   /bin/memstress ;;
	vm)    run_one vm    /bin/vmstress ;;
	cpu)   run_one cpu   /bin/cpustress ;;
	fd)    run_one fd    /bin/fdstress ;;
	shm)   run_one shm   /bin/shmstress ;;
	spawn) run_one spawn /bin/spawnstress ;;
	disk)  run_one disk  /bin/diskstress ;;
	net)   run_one net   /bin/netstress ;;
	# The futex round-trip test, as a soak workload: a handoff that costs
	# milliseconds instead of microseconds is invisible to a pass/fail smoke,
	# and it is what turns a threaded program's runtime into minutes.
	futex) run_one futex /bin/futex_stress ;;
	# What a timer actually waits. A compositor's frame loop is built on this.
	timer) run_one timer /bin/timerstress ;;
	# What one process costs, measured rather than inferred. The suite launches
	# hundreds of small programs, so a startup that is milliseconds too slow is
	# minutes across a run.
	exec)  run_one exec soak_exec ;;
	gfx)   run_one gfx   soak_gfx ;;
	# vm-<ops>: the same address-space workload with only some of its
	# operations, for bisecting which one a fault needs. The list is passed
	# straight through, e.g. b1nix.soak=vm-map or b1nix.soak=vm-map,protect.
	vm-*)  # `+` separates the operations, since `,` already separates
	       # workloads. vm-none runs the bare mmap/munmap loop.
	       VMSTRESS_OPS=$(echo "${w#vm-}" | tr '+' ',')
	       export VMSTRESS_OPS
	       run_one "$w" /bin/vmstress
	       unset VMSTRESS_OPS ;;
	*)     echo "SOAK: unknown workload '$w'" ;;
	esac
done

# What the kernel thinks of itself afterwards. A heap whose canaries are intact
# and a free-page count that has not collapsed are the two cheap answers to
# "did this run leak or corrupt anything", and they cost one read each.
echo "--- after the run ---"
[ -r /proc/meminfo ] && grep -E 'MemFree|MemTotal|Slab' /proc/meminfo 2>/dev/null
[ -r /proc/b1nix-prof ] && cat /proc/b1nix-prof 2>/dev/null
echo "--- end after the run ---"

if [ -n "$FAILED" ]; then
	echo "SOAK: FAILED$FAILED"
else
	echo "SOAK: PASSED$RAN"
fi
echo "SOAK: done t=$(up)"
