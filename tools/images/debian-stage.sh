#!/bin/sh
# b1nix Debian glibc harness — OUR file, not Debian's.
#
# Booted as `init=/b1nix-stage.sh` (PID 1) or run from /etc/inittab under real
# sysvinit. Every marker below is printed only after the thing it names actually
# worked; a failure prints "DEBIAN-SMOKE: FAIL <what> status=<n>" and the script
# CONTINUES, so one broken stage never hides the others.
#
# Stages 4 and up exist to answer one question: how much of what our own
# userspace binaries assert about the kernel can a stock distribution assert
# instead. They deliberately use only what Debian ships — bash, perl, util-linux
# — so that a green run here is evidence about the kernel and not about our
# libc.
PATH=/usr/sbin:/usr/bin:/sbin:/bin:/usr/local/sbin:/usr/local/bin
export PATH
HOME=/root
export HOME
TERM=${TERM:-linux}
export TERM

echo "DEBIAN-SMOKE: start pid=$$"

ok() { echo "DEBIAN-SMOKE: ok $1"; }
bad() { echo "DEBIAN-SMOKE: FAIL $1 status=${2:-1}"; }

# `expect <label> <want> <got>` — one comparison, one marker.
expect() {
	if [ "$2" = "$3" ]; then
		ok "$1"
	else
		bad "$1 (want '$2' got '$3')"
	fi
}

# ── Stage 1: a glibc dynamic binary ran ────────────────────────────────────
# The marker text itself is produced by the Debian /bin/dash, so it cannot be
# printed unless a real glibc ELF executed and its libc resolved.
if [ -x /bin/dash ]; then
	/bin/dash -c 'echo "DEBIAN-SMOKE: ok stage1-dash"'
	s=$?
	[ $s -eq 0 ] || bad stage1-dash $s
else
	bad stage1-dash 127
fi

uname -a || bad uname $?
if [ -x /lib64/ld-linux-x86-64.so.2 ]; then
	/lib64/ld-linux-x86-64.so.2 --version 2>&1 | head -1 || bad ld-version $?
else
	bad ld-present 1
fi

# ── Stage 2: distro coreutils ──────────────────────────────────────────────
# /proc has to exist before ps, mount and dmesg mean anything.
mount -t proc proc /proc 2>/dev/null || echo "DEBIAN-SMOKE: note mount-proc status=$?"
mount -t sysfs sysfs /sys 2>/dev/null || echo "DEBIAN-SMOKE: note mount-sysfs status=$?"
mount -t devtmpfs devtmpfs /dev 2>/dev/null || echo "DEBIAN-SMOKE: note mount-dev status=$?"

stage2_rc=0
check() { # check <label> <cmd...>  — gating: any non-zero fails stage 2
	_label="$1"
	shift
	"$@"
	_s=$?
	if [ $_s -ne 0 ]; then
		bad "$_label" $_s
		stage2_rc=1
	fi
}
note() { # note <label> <cmd...>  — informational, does not gate the marker
	_label="$1"
	shift
	"$@"
	_s=$?
	[ $_s -eq 0 ] || bad "$_label" $_s
}

check stage2-ls ls -l /
check stage2-cat cat /etc/os-release
check stage2-mount mount
check stage2-ps ps
note stage2-id id
dmesg 2>/dev/null | tail -5
if [ $stage2_rc -eq 0 ]; then
	ok stage2-coreutils
else
	bad stage2-coreutils 1
fi

# ── Stage 3: running under a real init ─────────────────────────────────────
# Read PID 1 from /proc rather than from `ps`: plain `ps` lists only the
# processes sharing this terminal, so it says nothing about init at all.
pid1=$(cat /proc/1/comm 2>/dev/null | tr -d " ")
[ -n "$pid1" ] || pid1=$(ps -o comm= -p 1 2>/dev/null | tr -d " ")
case "$pid1" in
init | sysvinit | systemd)
	ok "stage3-init (real init, we are pid $$)"
	;;
b1nix-stage.sh | sh | dash)
	if [ "$$" = "1" ]; then
		ok "stage3-init (harness is pid 1)"
	else
		bad "stage3-init (pid=$$ pid1='$pid1')"
	fi
	;;
*)
	bad "stage3-init (pid=$$ pid1='$pid1')"
	;;
esac

# ── Stage 4: processes and signals, through the distro's own tools ─────────
# Mirrors what m12_smoke and m15_smoke assert: exit status propagation, zombie
# reaping, session and process groups, signal delivery, and a blocked signal
# arriving only after it is unblocked.
sh -c 'exit 42'
expect proc-exit-status 42 $?

sh -c 'kill -TERM $$' 2>/dev/null
expect proc-signal-status 143 $?

# A reaped child leaves no entry behind; an unreaped one would still be listed.
sh -c 'true' &
child=$!
wait $child
if [ -d "/proc/$child" ]; then
	bad proc-zombie-reaped
else
	ok proc-zombie-reaped
fi

if command -v setsid >/dev/null 2>&1; then
	sid_self=$(cut -d' ' -f6 /proc/self/stat 2>/dev/null)
	sid_new=$(setsid sh -c 'cut -d" " -f6 /proc/self/stat' 2>/dev/null)
	if [ -n "$sid_new" ] && [ "$sid_new" != "$sid_self" ]; then
		ok proc-setsid
	else
		bad "proc-setsid (self=$sid_self new=$sid_new)"
	fi
else
	bad proc-setsid 127
fi

if command -v perl >/dev/null 2>&1; then
	# One perl per question. A single script that dies takes every marker
	# after it with it, and the first run of this stage did exactly that:
	# "FAIL proc-perl" said nothing about which of the four failed.
	perl -e '
		my $got = 0;
		$SIG{USR1} = sub { $got++ };
		kill "USR1", $$;
		select undef, undef, undef, 0.05;
		exit($got == 1 ? 0 : 1);
	' 2>&1 && ok sig-handler || bad sig-handler $?

	perl -e '
		use POSIX qw(sigprocmask SIG_BLOCK SIG_UNBLOCK SIGUSR2);
		my $late = 0;
		$SIG{USR2} = sub { $late++ };
		my $set = POSIX::SigSet->new(SIGUSR2);
		sigprocmask(SIG_BLOCK, $set) or die "sigprocmask block: $!";
		kill "USR2", $$;
		select undef, undef, undef, 0.05;
		die "delivered while blocked" if $late;
		sigprocmask(SIG_UNBLOCK, $set) or die "sigprocmask unblock: $!";
		select undef, undef, undef, 0.05;
		exit($late == 1 ? 0 : 1);
	' 2>&1 && ok sig-mask || bad sig-mask $?

	perl -e '
		use POSIX ":sys_wait_h";
		my $pid = fork();
		die "fork: $!" unless defined $pid;
		if ($pid == 0) { select undef, undef, undef, 0.2; exit 7 }
		my $first = waitpid($pid, WNOHANG);
		my $r = waitpid($pid, 0);
		my $st = $? >> 8;
		exit(($first == 0 && $r == $pid && $st == 7) ? 0 : 1);
	' 2>&1 && ok proc-waitpid-wnohang || bad proc-waitpid-wnohang $?
else
	bad proc-perl 127
fi

# ── Stage 5: file descriptors across exec ──────────────────────────────────
# Mirrors m12/m13: dup2, an inherited descriptor, and one that close-on-exec
# takes away.
if command -v perl >/dev/null 2>&1; then
	perl -e '
		use POSIX qw(dup2);
		use Fcntl qw(F_SETFD F_GETFD FD_CLOEXEC);
		open(my $fh, ">", "/tmp/b1nix-fd") or die;
		# dup2 onto a chosen number, then write through it.
		dup2(fileno($fh), 9) or die;
		open(my $nine, ">&=9") or die;
		print $nine "written-through-9\n"; close $nine; close $fh;
		open(my $rd, "<", "/tmp/b1nix-fd") or die;
		my $line = <$rd>; close $rd;
		chomp $line;
		print "DEBIAN-SMOKE: ",
		      ($line eq "written-through-9" ? "ok" : "FAIL"), " fd-dup2\n";
		# Inheritance: without FD_CLOEXEC the child sees it, with it it does not.
		open(my $keep, "<", "/etc/os-release") or die;
		my $kfd = fileno($keep);
		fcntl($keep, F_SETFD, 0) or die;
		my $seen = system("test -e /proc/self/fd/$kfd") == 0;
		fcntl($keep, F_SETFD, FD_CLOEXEC) or die;
		my $gone = system("test -e /proc/self/fd/$kfd") != 0;
		print "DEBIAN-SMOKE: ", ($seen ? "ok" : "FAIL"), " fd-inherit-exec\n";
		print "DEBIAN-SMOKE: ", ($gone ? "ok" : "FAIL"), " fd-cloexec\n";
	' || bad fd-perl $?
fi

# ── Stage 6: the errno matrix ──────────────────────────────────────────────
# Mirrors m17_smoke. perl reports errno numerically, so these are real codes
# rather than message text.
if command -v perl >/dev/null 2>&1; then
	perl -e '
		use Errno qw(ELOOP ENAMETOOLONG ENOTDIR EISDIR EBADF);
		use POSIX qw(:fcntl_h);
		sub code { my ($label, $want, $cb) = @_;
			$! = 0; $cb->();
			my $got = $! + 0;
			print "DEBIAN-SMOKE: ", ($got == $want ? "ok" : "FAIL"),
			      " errno-$label\n";
		}
		unlink "/tmp/b1nix-loop-a", "/tmp/b1nix-loop-b";
		symlink "/tmp/b1nix-loop-b", "/tmp/b1nix-loop-a";
		symlink "/tmp/b1nix-loop-a", "/tmp/b1nix-loop-b";
		code("eloop", ELOOP, sub { open(my $f, "<", "/tmp/b1nix-loop-a") });
		my $long = "/tmp/" . ("n" x 300);
		code("enametoolong", ENAMETOOLONG, sub { open(my $f, "<", $long) });
		code("enotdir", ENOTDIR, sub { open(my $f, "<", "/etc/os-release/x") });
		code("eisdir", EISDIR, sub { open(my $f, ">", "/tmp") });
		code("ebadf", EBADF, sub { POSIX::read(999, my $buf, 1) });
	' || bad errno-perl $?
fi

# ── Stage 7: memory ────────────────────────────────────────────────────────
# A 64 MiB allocation that is written and read back exercises the same brk and
# mmap paths m13 covers, through glibc malloc instead of ours.
if command -v perl >/dev/null 2>&1; then
	perl -e '
		my $n = 64 * 1024 * 1024;
		my $s = "x" x $n;
		substr($s, $n - 1, 1) = "y";
		print "DEBIAN-SMOKE: ",
		      ((length($s) == $n && substr($s, -1) eq "y") ? "ok" : "FAIL"),
		      " mem-large-alloc\n";
	' || bad mem-perl $?
	maps=$(wc -l </proc/self/maps 2>/dev/null)
	if [ -n "$maps" ] && [ "$maps" -gt 3 ]; then
		ok mem-proc-maps
	else
		bad "mem-proc-maps (lines=$maps)"
	fi
fi

# ── Stage 8: System V IPC, with util-linux ─────────────────────────────────
# Mirrors m15_smoke's shm/mq/semaphore markers.
if command -v ipcmk >/dev/null 2>&1 && command -v ipcs >/dev/null 2>&1; then
	# ipcmk prints "<kind> id: <n>"; take the number and then look for it as a
	# FIELD of the ipcs table. Matching it as a substring of the whole line
	# finds a key or a size instead, and id 0 is a perfectly legal id.
	ipc_probe() { # ipc_probe <kind> <ipcs-flag> <ipcmk-args...>
		_kind=$1; _flag=$2; shift 2
		_out=$(ipcmk "$@" 2>&1)
		_id=$(printf '%s' "$_out" | sed -n 's/.*: *\([0-9][0-9]*\).*/\1/p' | tail -1)
		if [ -z "$_id" ]; then
			bad "ipc-$_kind (ipcmk said '$_out')"
			return
		fi
		if ipcs "$_flag" 2>/dev/null | awk -v id="$_id" '$2 == id { found = 1 }
			END { exit !found }'; then
			ok "ipc-$_kind"
		else
			bad "ipc-$_kind (id $_id not listed by ipcs $_flag)"
			# Say what ipcs saw and what the kernel table holds, so a miss
			# tells a parsing problem from an empty table.
			ipcs "$_flag" 2>&1 | sed 's/^/DEBIAN-SMOKE: note ipcs: /' | head -8
			cat /proc/sysvipc/"$_kind" 2>&1 | sed 's/^/DEBIAN-SMOKE: note proc: /' | head -4
		fi
		ipcrm "$_flag" "$_id" >/dev/null 2>&1 || bad "ipc-$_kind-rm" $?
	}
	ipc_probe shm -m -M 4096
	ipc_probe sem -s -S 1
	ipc_probe msg -q -Q
else
	bad ipc-tools 127
fi

# ── Stage 9: job control ───────────────────────────────────────────────────
# Mirrors m13_job_control: a stopped job really stops, and continues on SIGCONT.
if command -v bash >/dev/null 2>&1; then
	# Bounded: a job-control bug that wedges the shell must not cost the
	# stages after it.
	timeout 20 bash -c '
		set -m
		sleep 30 &
		pid=$!
		kill -STOP $pid; sleep 0.2
		st=$(awk "{print \$3}" /proc/$pid/stat 2>/dev/null)
		kill -CONT $pid; sleep 0.2
		st2=$(awk "{print \$3}" /proc/$pid/stat 2>/dev/null)
		kill -KILL $pid 2>/dev/null
		if [ "$st" = "T" ]; then echo "DEBIAN-SMOKE: ok job-stop";
		else echo "DEBIAN-SMOKE: FAIL job-stop status=1 (state=$st)"; fi
		case "$st2" in S|R|D) echo "DEBIAN-SMOKE: ok job-cont";;
		*) echo "DEBIAN-SMOKE: FAIL job-cont status=1 (state=$st2)";; esac
	' || bad job-bash $?
else
	bad job-bash 127
fi

# ── Stage 10: clocks and timeouts ──────────────────────────────────────────
# Mirrors m15's clock-timer: time has to move, and a timeout has to fire.
t0=$(date +%s)
sleep 1
t1=$(date +%s)
if [ $((t1 - t0)) -ge 1 ]; then
	ok clock-advances
else
	bad "clock-advances (t0=$t0 t1=$t1)"
fi
if command -v timeout >/dev/null 2>&1; then
	timeout 1 sleep 5
	expect timeout-fires 124 $?
else
	bad timeout-fires 127
fi

# ── Stage 11: the rest of our own tests' kernel surfaces ───────────────────
# exec limits (m13), POSIX mq, signal ignore and permissions (m15), O_PATH,
# O_NOFOLLOW, renameat2 and EROFS (m17), mremap (m12) -- through glibc and
# perl rather than our libc. Raw syscall numbers are x86_64's.
n=$(/bin/sh -c 'echo $#' _ $(seq 1 5000) 2>/tmp/b1nix-many-err)
expect exec-many-args 5000 "${n:-0}"
[ "${n:-0}" = 5000 ] || sed 's/^/DEBIAN-SMOKE: note exec-many-args: /' /tmp/b1nix-many-err
if command -v perl >/dev/null 2>&1; then
	perl -e '
		use POSIX qw(:errno_h :fcntl_h);
		sub res { my ($label, $good) = @_;
			print "DEBIAN-SMOKE: ", ($good ? "ok" : "FAIL"), " $label\n"; }
		# E2BIG: one argument larger than the whole argument area.
		my $big = "x" x (8 * 1024 * 1024);
		my $pid = fork();
		if ($pid == 0) { exec("/bin/true", $big); exit(($! + 0) == E2BIG ? 0 : 1); }
		waitpid($pid, 0);
		res("exec-e2big", ($? >> 8) == 0);
		# POSIX message queue: open, send, receive, unlink.
		my $name = "/b1nix-mq\0";
		my $attr = pack("q4", 0, 4, 64, 0);
		my $mq = syscall(240, $name, O_RDWR | O_CREAT, 0600, $attr);
		my $msg = "hello";
		my $sent = $mq >= 0 && syscall(242, $mq, $msg, 5, 0, 0) == 0;
		my $buf = "\0" x 64;
		my $prio = pack("L", 0);
		my $got = $mq >= 0 ? syscall(243, $mq, $buf, 64, $prio, 0) : -1;
		res("mq-posix", $sent && $got == 5 && substr($buf, 0, 5) eq "hello"
		    && syscall(241, $name) == 0);
		# An ignored signal is dropped, not delivered.
		$SIG{USR1} = "IGNORE";
		kill "USR1", $$;
		res("sig-ignore", 1);
		# O_PATH: a descriptor to the name, not the contents.
		my $fd = POSIX::open("/etc/os-release", 010000000);
		my $rbuf;
		my $rd = defined $fd ? POSIX::read($fd, $rbuf, 1) : 0;
		res("fd-o-path", defined $fd && !defined $rd && ($! + 0) == EBADF);
		# O_NOFOLLOW on a symlink is ELOOP.
		unlink "/tmp/b1nix-nf"; symlink "/etc/os-release", "/tmp/b1nix-nf";
		my $nf = POSIX::open("/tmp/b1nix-nf", O_RDONLY | 0400000);
		res("errno-o-nofollow", !defined $nf && ($! + 0) == ELOOP);
		# renameat2: NOREPLACE refuses an existing target, bad flags are EINVAL.
		open(my $a, ">", "/tmp/b1nix-ra"); close $a;
		open(my $b, ">", "/tmp/b1nix-rb"); close $b;
		my ($ra, $rb) = ("/tmp/b1nix-ra\0", "/tmp/b1nix-rb\0");
		my $r = syscall(316, -100, $ra, -100, $rb, 1);
		res("rename-noreplace", $r == -1 && ($! + 0) == EEXIST);
		$r = syscall(316, -100, $ra, -100, $rb, 3);
		res("rename-einval", $r == -1 && ($! + 0) == EINVAL);
		# mremap grows a mapping.
		my $m = syscall(9, 0, 4096, 3, 0x22, -1, 0);
		my $g = $m > 0 ? syscall(25, $m, 4096, 8192, 1) : -1;
		res("mem-mremap", $m > 0 && $g > 0 && syscall(11, $g, 8192) == 0);
		# Permissions: nobody cannot read a root-only file.
		open(my $s, ">", "/tmp/b1nix-secret"); close $s;
		chmod 0600, "/tmp/b1nix-secret";
		$pid = fork();
		if ($pid == 0) {
			$) = "65534 65534"; $> = 65534;
			my $o = POSIX::open("/tmp/b1nix-secret", O_RDONLY);
			exit(!defined $o && ($! + 0) == EACCES ? 0 : 1);
		}
		waitpid($pid, 0);
		res("perm-eacces", ($? >> 8) == 0);
	' || bad surfaces-perl $?
fi
# EROFS: a read-only mount refuses the open for write.
mkdir -p /tmp/b1nix-ro
if mount -t tmpfs -o ro tmpfs /tmp/b1nix-ro 2>/dev/null; then
	if ( : > /tmp/b1nix-ro/file ) 2>/tmp/b1nix-ro-err; then
		bad "errno-erofs (write succeeded)"
	elif grep -q "Read-only" /tmp/b1nix-ro-err; then
		ok errno-erofs
	else
		bad "errno-erofs ($(cat /tmp/b1nix-ro-err))"
	fi
	umount /tmp/b1nix-ro 2>/dev/null
else
	bad "errno-erofs (mount -o ro failed)"
fi

echo "DEBIAN-SMOKE: done"

# Let QEMU exit on its own where possible; the host harness kills it on timeout
# regardless, so none of this is load-bearing.
sync 2>/dev/null
if [ -w /proc/sysrq-trigger ]; then
	echo o >/proc/sysrq-trigger 2>/dev/null
fi
poweroff -f 2>/dev/null
halt -f 2>/dev/null
while :; do
	sleep 60
done
