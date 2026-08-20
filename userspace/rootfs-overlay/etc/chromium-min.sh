#!/bin/sh
# chromium-min.sh — the smallest browser case, on its own, in seconds.
#
# The window run costs ten minutes: fetch packages, start a compositor, wait for
# a surface. None of that is needed to reproduce the stall. One process, a blank
# page, no window system and no GPU is enough — and the same binary with the
# same flags prints the document on a Linux host in about a second, so a hang
# here is this kernel's, not the browser's.
#
# Gated on b1nix.chromium-min so an ordinary boot pays a fork and nothing else.
if ! grep -q "b1nix.chromium-min" /proc/cmdline 2>/dev/null; then
	exit 0
fi

up() { cut -d" " -f1 /proc/uptime 2>/dev/null || echo "?"; }
export HOME=/root
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
mkdir -p /run /tmp /root /var /dev/shm

# No D-Bus, and say so up front.
#
# Something in the image answers on the system bus socket but never replies to
# a method call, so every NameHasOwner sits out its timeout — minutes of the
# browser's start-up spent waiting for a bus that is not there. The Linux host
# this is measured against has no bus either; it just fails immediately, which
# is what these two make the guest do.
# A short budget turns a forty-minute experiment into a one-minute one, for the
# questions that do not need the browser to finish. b1nix.chromium-budget=<s>.
CMIN_BUDGET=$(sed -n "s/.*b1nix.chromium-budget=\([0-9]*\).*/\1/p" /proc/cmdline 2>/dev/null)
[ -n "$CMIN_BUDGET" ] || CMIN_BUDGET=600

export DBUS_SYSTEM_BUS_ADDRESS=disabled:
export DBUS_SESSION_BUS_ADDRESS=disabled:

# Take the D-Bus socket out of the picture entirely.
#
# The two "disabled:" addresses above tell libdbus not to connect, but the
# Bluetooth extension asks BlueZ through its own path and reaches the socket
# anyway. Something in this image answers on that socket and never replies, and
# the browser's UI thread waits for the reply with no timeout — measured at
# three minutes of a two-minute budget, ending only when a signal arrived. With
# no socket at all the connect fails at once, which is what a machine without a
# bus is supposed to do.
# /dev/shm has to BE shared memory.
#
# Without a tmpfs mounted there it is an ordinary directory on the ext2 root,
# so the "in memory" profile — its preference files, its SQLite databases and
# the Simple Cache indexes — is written to the disk, one fdatasync at a time.
# The stalled snapshots showed six browser threads inside open(2) and four
# inside fdatasync(2) at once. Mounting a real tmpfs is what the flag was
# always assuming.
mount -t tmpfs tmpfs /dev/shm 2>/dev/null || echo "CMIN: warn no tmpfs on /dev/shm"

pkill -f dbus-daemon 2>/dev/null
rm -f /run/dbus/system_bus_socket /run/dbus/session_bus_socket 2>/dev/null

echo "CMIN: start t=$(up)"
if [ ! -x /usr/bin/chromium ]; then
	echo "CMIN: FAIL no-browser (/usr/bin/chromium missing)"
	exit 0
fi

# Four ways of asking the same question.
#
# The identical binary with the identical flags prints the document on a Linux
# host in about a second, so the fault is this kernel's — but "the browser
# stops" names no subsystem. Varying one decision at a time does: whether the
# work happens in one process or several, whether a zygote is forked at all,
# and which headless implementation runs. A variant that finishes tells us more
# than another repetition of the one that does not.
# The profile goes in memory, not on the disk.
#
# Ten minutes of the browser's start-up sat between two adjacent log lines while
# it built a fresh profile: a few dozen small files and a sqlite database. On
# the disk-backed root that is thousands of tiny writes through the block layer.
# /dev/shm is the same choice a Linux user makes for a scratch profile, and it
# tells slow storage apart from a slow kernel in one run.
#
# --disable-dev-shm-usage is deliberately NOT passed: it exists for containers
# with a tiny /dev/shm and pushes Chromium's shared memory onto the filesystem,
# which is the opposite of what is wanted here.
# Background networking is off.
#
# The run reached the component updater fetching from edgedl.me.gvt1.com and
# spent the rest of its budget there. None of that is what "print the document"
# needs, and the same three switches are what anyone driving headless Chromium
# in a test harness passes.
# Every request goes to a port nothing listens on, so it fails at once instead
# of holding the start-up open. A resolver rule would have been the natural way
# to say this, but its value contains spaces and this list is expanded
# unquoted — the shell split it and chromium read the pieces as extra page
# targets, which it refuses ("Multiple targets are not supported").
#
# Nothing in this list may contain a space or a glob character for the same
# reason.
# The list above is a workaround list, and every entry on it is a bug somewhere
# else. `b1nix.chromium-bare` asks for the browser the way anyone would start
# it: headless because there is no display in this run, no sandbox because the
# sandbox is not supported yet, and nothing else. What fails without a flag is
# what to fix next, so this mode exists to keep that list honest.
BARE="--no-sandbox --headless=new --dump-dom --enable-logging=stderr --v=1"

COMMON="--no-sandbox --disable-gpu --enable-logging=stderr --v=1 --dump-dom \
        --disable-component-update --disable-background-networking --no-first-run \
        --disable-extensions --disable-sync --disable-domain-reliability \
        --disable-client-side-phishing-detection --safebrowsing-disable-auto-update \
        --metrics-recording-only --no-pings --proxy-server=127.0.0.1:1 \
        --disable-features=OptimizationHints,OptimizationTargetPrediction,Translate,MediaRouter"

# What the network costs, separately from what the browser does with it.
#
# The flag matrix answered its question: of the whole workaround list, only
# --proxy-server=127.0.0.1:1 decides whether the document appears, and with it
# the browser finishes in forty seconds instead of four minutes. That flag does
# one thing — it stops real outbound connections — so the cost is in the
# network path itself. This mode measures the two halves of it separately, a
# name resolution and a connection, so the next fix has an address.
# b1nix.chromium-netprobe.
if grep -q b1nix.chromium-netprobe /proc/cmdline 2>/dev/null; then
	for host in update.googleapis.com clients2.google.com example.com; do
		t0=$(up)
		nslookup $host > /tmp/ns-$host.out 2>&1
		rc=$?
		echo "CMIN-NET: resolve $host rc=$rc t=$t0 -> $(up)"
	done
	# One real fetch each way. The browser's start-up is dozens of these, so
	# if a single one is slow the sum is the whole run: with a dead proxy the
	# same browser prints the document in forty seconds, and with real network
	# it does not finish in seven minutes.
	for url in http://clients3.google.com/generate_204 \
	           https://clients3.google.com/generate_204; do
		t0=$(up)
		wget -q -T 20 -O /dev/null "$url" 2>/dev/null
		echo "CMIN-NET: fetch $url rc=$? t=$t0 -> $(up)"
	done
	for target in 10.0.2.2:80 93.184.216.34:80 10.0.2.3:53; do
		t0=$(up)
		nc -w 5 -z ${target%:*} ${target#*:} > /dev/null 2>&1
		rc=$?
		echo "CMIN-NET: connect $target rc=$rc t=$t0 -> $(up)"
	done
	echo "CMIN-NET: done"
fi


# What /proc/<pid>/stat actually looks like from inside.
#
# Chromium reads a field of this line by index and traps when the line is not
# what it expects — an illegal instruction its own crash handler catches and
# re-enters, four hundred times in one run. Printing the line and its field
# count says whether the kernel or the parser is wrong.
if grep -q b1nix.chromium-statprobe /proc/cmdline 2>/dev/null; then
	line=$(cat /proc/self/stat)
	echo "CMIN-STAT: fields=$(echo "$line" | wc -w)"
	echo "CMIN-STAT: line=$line"
	echo "CMIN-STAT: bytes=$(cat /proc/self/stat | wc -c)"
	echo "CMIN-STAT: done"
fi

# Run it and wait for it, rather than polling from a second process.
#
# The polling loop cost a background subshell, a sleep and two greps every ten
# seconds, and when the browser had every core busy the shell stopped being
# scheduled often enough to report anything at all — a run that had finished
# looked exactly like one that had hung. Waiting for the process is one line and
# cannot be starved out of its own verdict.
run_variant() {
	name=$1
	shift
	log=/var/min-$name.log
	mkdir -p /dev/shm
	# Every previous profile, not just this run's. /dev/shm is memory: the
	# first run leaves a few hundred megabytes of profile behind it, and the
	# second then starts on a machine with that much less to work with — which
	# is what made the second run in a boot stall where the first had been
	# fine. Measuring runs against each other requires them to start alike.
	rm -rf /dev/shm/cmin-*
	echo "CMIN: variant $name start t=$(up)"
	timeout -k 5 ${CMIN_BUDGET:-600} /usr/bin/chromium $COMMON --user-data-dir=/dev/shm/cmin-$name \
		"$@" about:blank > $log 2>&1
	rc=$?
	echo "CMIN: variant $name returned from wait rc=$rc t=$(up)"
	dom=$(grep -ac "<html" $log 2>/dev/null)
	echo "CMIN: variant $name rc=$rc dom=$dom t=$(up) bytes=$(wc -c < $log 2>/dev/null)"
	# One profile sample per run, taken here rather than by the periodic task
	# dump: the dump costs more than what it measures.
	[ -r /proc/b1nix-prof ] && cat /proc/b1nix-prof > /dev/null 2>&1
	if [ "${dom:-0}" -gt 0 ]; then
		echo "CMIN: variant $name OK — the document was printed"
		grep -a "<html" $log | head -1
		return 0
	fi
	echo "CMIN: variant $name FAILED"
	grep -aiE "FATAL|CHECK failed|NOTREACHED|Aborted|out of memory" $log 2>/dev/null | head -5
	grep -av "bus\.cc" $log 2>/dev/null | tail -120 | sed "s/^/    /"
	return 1
}

# One variant by default. All four behave identically (measured), and a run that
# only asks the question once leaves the watchdog's snapshot easy to read and
# the loop short enough to iterate in.
VARIANTS="single"
grep -q "b1nix.chromium-bare" /proc/cmdline 2>/dev/null && VARIANTS="bare"
# One flag at a time on top of the bare set, to find which workaround is
# carrying the run rather than guessing. b1nix.chromium-flagmatrix.
grep -q "b1nix.chromium-flagmatrix" /proc/cmdline 2>/dev/null &&
	VARIANTS="bare-bgnet bare-net"
# A quiet run, for the case where the logging is part of the problem.
#
# The stall was found inside a log call — device_event_log allocating while the
# UI thread built its message — and --enable-logging=stderr --v=1 makes every
# subsystem log, which is not how anyone runs a browser. `b1nix.chromium-quiet`
# drops the logging switches so the same page is asked for without them.
grep -q "b1nix.chromium-quiet" /proc/cmdline 2>/dev/null &&
	COMMON="--no-sandbox --disable-gpu --dump-dom --disable-component-update \
	        --disable-background-networking --no-first-run --disable-extensions"
grep -q "b1nix.chromium-variants" /proc/cmdline 2>/dev/null &&
	VARIANTS="single multi nozygote oldheadless"

# Repeat a variant inside one boot, for measurement.
#
# A single run is not a measurement: the same build printed the document in 38
# seconds and in 224 on consecutive boots, because how much the browser fetches
# and how warm the host's cache is vary more than anything the kernel does.
# Repeating inside one boot holds the machine, the page cache and the network
# steady, so the numbers can be compared to each other. b1nix.chromium-repeat=N.
CMIN_REPEAT=$(sed -n "s/.*b1nix.chromium-repeat=\([0-9]*\).*/\1/p" /proc/cmdline 2>/dev/null)
[ -n "$CMIN_REPEAT" ] || CMIN_REPEAT=1

r=1
while [ "$r" -le "$CMIN_REPEAT" ]; do
for v in $VARIANTS; do
	case "$v" in
	bare)        COMMON="$BARE" run_variant bare-$r ;;
	bare-ext)    COMMON="$BARE --disable-extensions" run_variant bare-ext ;;
	bare-net)    COMMON="$BARE --proxy-server=127.0.0.1:1" run_variant bare-net-$r ;;
	# Not a workaround for anything of ours: with a live network the browser
	# fetches its variations seed, component updates and model lists, and that
	# work — not the kernel's network path, which resolves a name in 30 ms and
	# finishes a TLS fetch in 90 ms — is what the run spends its minutes on.
	# This is the flag anyone would use for it.
	bare-bgnet)  COMMON="$BARE --disable-background-networking" run_variant bare-bgnet ;;
	single)      run_variant single --headless=new --single-process ;;
	multi)       run_variant multi --headless=new ;;
	nozygote)    run_variant nozygote --headless=new --single-process --no-zygote ;;
	oldheadless) run_variant oldheadless --headless --single-process ;;
	esac
done
	r=$((r + 1))
done

echo "CMIN: summary"
for v in $VARIANTS; do
	echo "  $v: dom=$(grep -ac "<html" /var/min-$v.log 2>/dev/null) lines=$(wc -l < /var/min-$v.log 2>/dev/null)"
done
pkill -f "cmin-" 2>/dev/null
echo "CMIN: done t=$(up)"
