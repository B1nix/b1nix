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
	rm -rf "/dev/shm/cmin-$name"
	echo "CMIN: variant $name start t=$(up)"
	# Time the DOM, not the exit.
	#
	# --dump-dom prints the document and the browser then does NOT leave: the
	# run ends when the budget kills it, so its exit time says how long the
	# budget was, not how long the page took. The only honest moment is the one
	# the first "<html" is written, so the output goes through an awk that
	# stamps it from /proc/uptime as it passes and otherwise copies its input
	# unchanged. awk only wakes when the browser writes, which costs nothing in
	# between — unlike the polling loop this file already replaced once, which
	# could not get scheduled when the browser had every core busy.
	# The exit status has to survive the pipe (ash has no PIPESTATUS), so the
	# browser's own status is carried out in a file.
	# The stamp is written to a file rather than to the console: awk's stderr
	# is not reliably the console here, and the moment is in the value, not in
	# when it gets echoed. It is printed below, once the run is over.
	rcf=/tmp/cmin-rc-$name
	domf=/tmp/cmin-dom-$name
	rm -f $rcf $domf
	# In the background, and watched.
	#
	# --dump-dom prints the document and the browser then stays up, so a
	# foreground pipeline sits in silence until the budget kills it — and every
	# run that was cut short before that moment reported nothing at all, which
	# read as "the browser never finished". It had, in fifteen seconds. Run it
	# beside us and watch for the stamp instead: the answer is printed when it
	# is true, not when the budget says so.
	( { timeout -k 5 ${CMIN_BUDGET:-600} /usr/bin/chromium $COMMON \
		--user-data-dir=${CMIN_PROFILE:-/dev/shm/cmin-$name} "$@" about:blank 2>&1
	  echo $? > $rcf; } | awk -v d="$domf" '
		/<html/ && !seen {
			seen = 1
			if ((getline u < "/proc/uptime") > 0) {
				close("/proc/uptime")
				split(u, f, " ")
				print f[1] > d
				close(d)
			}
		}
		{ print }' > $log ) &
	pipe_pid=$!
	waited=0
	while [ "$waited" -lt "${CMIN_BUDGET:-600}" ]; do
		[ -s "$domf" ] && break
		[ -s "$rcf" ] && break
		sleep 2
		waited=$((waited + 2))
	done
	if [ -s "$domf" ]; then
		echo "CMIN: variant $name DOM at t=$(cat $domf) (waited ${waited}s)"
		# Do not try to kill it here.
		#
		# `pkill -f cmin-$name` matches on the whole command line, and this
		# script's own shell carries that string too — the run killed itself
		# and the console went quiet one line after printing the answer. The
		# browser is already under `timeout`, which will end it; the document
		# is what this variant exists to report, and it has been reported.
		echo "CMIN: variant $name ok — the document was printed"
		return 0
	fi
	wait $pipe_pid 2>/dev/null
	rc=$(cat $rcf 2>/dev/null)
	rc=${rc:-1}
	domt=$(cat $domf 2>/dev/null)
	[ -n "$domt" ] && echo "CMIN: variant $name DOM at t=$domt"
	rm -f $rcf $domf
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
grep -q "b1nix.chromium-swaylike" /proc/cmdline 2>/dev/null && VARIANTS="swaylike"
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

# The trace runs instead of the variants, not after them: the variant it would
# follow is the one that hangs, so a block placed behind it never runs at all.
grep -q "b1nix.chromium-trace" /proc/cmdline 2>/dev/null && VARIANTS=""

# Somebody talking while the variant runs.
#
# Every ingredient of a variant — the flags, the budget wrapper, the pipe, the
# awk, the profile on the tmpfs — finishes in fifteen seconds when the browser
# is started in the background and the script keeps printing. Run in the
# foreground, in silence, the same command does not finish at all. This starts
# a printer beside it so the two runs differ in nothing else.
# b1nix.chromium-watch.
if grep -q "b1nix.chromium-watch" /proc/cmdline 2>/dev/null; then
	( n=0
	  while [ $n -lt 20 ]; do
		sleep 15
		n=$((n + 1))
		echo "CMIN-WATCH: t=$(up) log=$(wc -l < /var/min-single.log 2>/dev/null) dom=$(grep -ac '<html' /var/min-single.log 2>/dev/null)"
	  done ) &
fi

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
	# A name per repeat, not one for all of them.
	#
	# Every repeat wiped /dev/shm/cmin-* before starting, and the previous
	# browser was still running out of exactly that directory — the second run
	# of three died with rc=137 for no reason of its own. Distinct profiles let
	# the repeats overlap harmlessly, which is what makes them comparable.
	single)      run_variant single-$r --headless=new --single-process ;;
	# The one that was seen to work.
	#
	# The same binary printed the document under the compositor image with a
	# profile on the ordinary filesystem and shared memory switched off, while
	# this image's run — profile on a tmpfs, /dev/shm in use — stops seven
	# seconds in. The two differ in exactly those two decisions, so this
	# variant makes them the same and the comparison says which one matters.
	swaylike)    COMMON="--no-sandbox --disable-gpu --dump-dom --disable-dev-shm-usage"
	             CMIN_PROFILE=/tmp/cmin-swaylike
	             run_variant swaylike --headless=new --single-process ;;
	multi)       run_variant multi --headless=new ;;
	nozygote)    run_variant nozygote --headless=new --single-process --no-zygote ;;
	oldheadless) run_variant oldheadless --headless --single-process ;;
	esac
done
	r=$((r + 1))
done

# What the browser says while it is stopping, not after.
#
# Every variant above reports only once its budget has run out, and a run whose
# shell never gets that far reports nothing at all — which is how a stall came
# to be described as "no output". This mode starts the browser in the
# background and prints the tail of its own log every fifteen seconds, so the
# last thing it managed to do is on the console whatever happens afterwards.
# b1nix.chromium-trace.
if grep -q "b1nix.chromium-trace" /proc/cmdline 2>/dev/null; then
	log=/var/trace.log
	rm -f $log
	echo "CMIN-TRACE: start t=$(up)"

	# A pipe, on its own, before the browser.
	#
	# The same browser with the same flags prints its document in fifteen
	# seconds when its output goes to a file and never finishes when it goes
	# through a pipe into awk. That is a claim about pipes, not about browsers,
	# and it deserves a test that takes ten seconds and involves neither.
	echo "CMIN-TRACE: pipe test (dd | wc) t=$(up)"
	dd if=/dev/zero bs=4096 count=4000 2>/dev/null | wc -c
	echo "CMIN-TRACE: pipe test done t=$(up)"
	echo "CMIN-TRACE: pipe test (dd | awk) t=$(up)"
	dd if=/dev/zero bs=1024 count=2000 2>/dev/null |
		awk '{ n++ } END { print "awk saw", n, "records" }'
	echo "CMIN-TRACE: awk pipe test done t=$(up)"
	# The same flags the variants use, so the only thing this mode changes is
	# where the output goes: a file instead of a pipe into awk. Two runs that
	# differ in one decision are a comparison; two that differ in two are not.
	trace_flags="--no-sandbox --headless=new --single-process --disable-gpu \
		--enable-logging=stderr --v=1 --dump-dom"
	grep -q "b1nix.chromium-trace-common" /proc/cmdline 2>/dev/null &&
		trace_flags="$COMMON --headless=new --single-process"
	# Where the profile lives, as the one thing that changes. The variant that
	# stalls keeps it on the tmpfs; every trace that finishes keeps it on the
	# root filesystem. b1nix.chromium-trace-shmprofile.
	trace_profile=/tmp/cmin-trace
	grep -q "b1nix.chromium-trace-shmprofile" /proc/cmdline 2>/dev/null &&
		trace_profile=/dev/shm/cmin-trace
	echo "CMIN-TRACE: flags $trace_flags"
	echo "CMIN-TRACE: profile $trace_profile"
	# With the budget wrapper, when asked. The variants run the browser under
	# `timeout` and never finish; this mode runs it bare and finishes in
	# fifteen seconds. The wrapper is the only remaining difference, so make it
	# the one thing that changes. b1nix.chromium-trace-timeout.
	if grep -q "b1nix.chromium-trace-timeout" /proc/cmdline 2>/dev/null; then
		timeout -k 5 ${CMIN_BUDGET:-600} /usr/bin/chromium $trace_flags \
			--user-data-dir=$trace_profile about:blank > $log 2>&1 &
	elif grep -q "b1nix.chromium-trace-pipe" /proc/cmdline 2>/dev/null; then
		# Through awk, exactly as the variants do it. Everything else is the
		# same as the run that finishes in fifteen seconds, so if this one does
		# not, the pipeline is the answer.
		# The variants' awk, verbatim — it stamps the moment the first
		# "<html" passes by reading /proc/uptime, and that read is the last
		# thing left that the finishing run does not do.
		( /usr/bin/chromium $trace_flags \
			--user-data-dir=$trace_profile about:blank 2>&1 |
			awk -v d=/tmp/trace-dom '
			/<html/ && !seen {
				seen = 1
				if ((getline u < "/proc/uptime") > 0) {
					close("/proc/uptime")
					split(u, f, " ")
					print f[1] > d
					close(d)
				}
			}
			{ print }' > $log ) &
	else
		/usr/bin/chromium $trace_flags \
			--user-data-dir=$trace_profile about:blank > $log 2>&1 &
	fi
	bpid=$!
	n=0
	while [ $n -lt 14 ]; do
		sleep 15
		n=$((n + 1))
		echo "CMIN-TRACE: t=$(up) lines=$(wc -l < $log 2>/dev/null) dom=$(grep -ac '<html' $log 2>/dev/null)"
		tail -6 $log 2>/dev/null | sed 's/^/    /'
		if grep -aq "<html" $log 2>/dev/null; then
			echo "CMIN-TRACE: DOM at t=$(up)"
			break
		fi
		kill -0 $bpid 2>/dev/null || { echo "CMIN-TRACE: browser exited"; break; }
	done
	kill $bpid 2>/dev/null
	echo "CMIN-TRACE: done t=$(up)"
fi

echo "CMIN: summary"
for v in $VARIANTS; do
	echo "  $v: dom=$(grep -ac "<html" /var/min-$v.log 2>/dev/null) lines=$(wc -l < /var/min-$v.log 2>/dev/null)"
done
pkill -f "cmin-" 2>/dev/null
echo "CMIN: done t=$(up)"
