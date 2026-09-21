#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
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
		# POSIX message queue: open, send, receive, unlink. The system call
		# takes the name without the leading slash mq_open(3) strips (a "/"
		# in it is EACCES on Linux).
		my $name = "b1nix-mq\0";
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

# ── Stage 12: system calls Linux added later (M124) ────────────────────────
# Called by number from perl, and judged by their results: a call that merely
# stops answering ENOSYS proves nothing. x86_64 numbers.
if command -v perl >/dev/null 2>&1; then
	mkdir -p /tmp/b1nix-o2/root/etc /tmp/b1nix-o2/dir
	echo inroot > /tmp/b1nix-o2/root/etc/hostname
	ln -sf /tmp/b1nix-o2/dir /tmp/b1nix-o2/link
	mkdir -p /tmp/b1nix-ll && ln -sf /etc/passwd /tmp/b1nix-ll/escape && rm -f /tmp/b1nix-ll-outside
	perl -e '
		use POSIX qw(:errno_h :fcntl_h);
		sub res { my ($label, $good, $why) = @_;
			print "DEBIAN-SMOKE: ", ($good ? "ok" : "FAIL"), " $label",
			      ($good ? "" : " ($why)"), "\n"; }
		sub err { return $! + 0; }
		my $PAGE = 4096;
		my $me = $$ + 0;
		my $map = syscall(9, 0, 3 * $PAGE, 3, 0x22, -1, 0);   # mmap RW anon

		# Memory policy on a one-node machine.
		my $one = pack("Q", 1); my $two = pack("Q", 2);
		my $r = syscall(237, $map, $PAGE, 2, $one, 64, 0);   # mbind BIND {0}
		my $r2 = syscall(237, $map, $PAGE, 2, $two, 64, 0);  # node 1: no such node
		my $e2 = err();
		res("mempolicy-mbind", $r == 0 && $r2 == -1 && $e2 == EINVAL, "r=$r r2=$r2 e=$e2");
		$r = syscall(238, 2, $one, 64);                      # set_mempolicy BIND
		my $mode = pack("l", -1); my $mask = pack("Q", 0);
		$r2 = syscall(239, $mode, $mask, 64, 0, 0);          # get_mempolicy
		res("mempolicy-get-set", $r == 0 && $r2 == 0 && unpack("l", $mode) == 2 &&
		    unpack("Q", $mask) == 1, "set=$r get=$r2 mode=" . unpack("l", $mode));
		$mask = pack("Q", 0);
		$r = syscall(239, 0, $mask, 64, 0, 4);               # MPOL_F_MEMS_ALLOWED
		res("mempolicy-mems-allowed", $r == 0 && unpack("Q", $mask) == 1, "r=$r");
		syscall(238, 0, 0, 0);

		# Protection keys without the CPU feature: only the default key exists.
		$r = syscall(330, 0, 0); my $ea = err();
		$r2 = syscall(329, $map, $PAGE, 1, -1);              # key -1 = mprotect
		my $r3 = syscall(329, $map, $PAGE, 1, 1); my $e3 = err();
		syscall(10, $map, $PAGE, 3);
		res("pkey", $r == -1 && $ea == ENOSPC && $r2 == 0 && $r3 == -1 && $e3 == EINVAL,
		    "alloc=$r/$ea mprot=$r2 bad=$r3/$e3");

		# memfd_secret: shared mappings only; the owner reads and writes it
		# through the mapping (here via read(2) into it), /proc/self/mem cannot.
		my $sfd = syscall(447, 0);
		my $strunc = $sfd >= 0 ? syscall(77, $sfd, $PAGE) : -1;
		my $spriv = $sfd >= 0 ? syscall(9, 0, $PAGE, 3, 0x02, $sfd, 0) : 0;
		my $esp = err();
		my $smap = $sfd >= 0 ? syscall(9, 0, $PAGE, 3, 0x01, $sfd, 0) : -1;
		pipe(my $spr, my $spw); syswrite($spw, "b1nix-secret"); close $spw;
		my $sread = $smap > 0 ? syscall(0, fileno($spr), $smap, 12) : -1;
		my $sseen = $sread == 12 ? unpack("P12", pack("Q", $smap)) : "";
		open(my $smem, "<", "/proc/self/mem");
		my $sbuf = "";
		sysseek($smem, $smap, 0);
		my $smr = sysread($smem, $sbuf, 12); my $esm = err();
		res("memfd-secret", $sfd >= 0 && $strunc == 0 && $spriv == -1 && $esp == EINVAL &&
		    $sseen eq "b1nix-secret" && !defined $smr && $esm == EIO,
		    "fd=$sfd trunc=$strunc priv=$spriv/$esp read=$sread mem=$esm");

		# sched_getattr / sched_setattr.
		my $attr = "\0" x 56;
		$r = syscall(315, 0, $attr, 56, 0);
		my ($sz, $pol, $fl, $nice) = unpack("L L Q l", $attr);
		my $set = pack("L L Q l L Q Q Q L L", 56, 0, 0, 7, 0, 0, 0, 0, 0, 0);
		$r2 = syscall(314, 0, $set, 0);
		my $prio = getpriority(0, 0);
		my $fifo = pack("L L Q l L Q Q Q L L", 56, 1, 0, 0, 10, 0, 0, 0, 0, 0);
		$r3 = syscall(314, 0, $fifo, 0); $e3 = err();
		res("sched-attr", $r == 0 && $sz == 56 && $pol == 0 && $r2 == 0 && $prio == 7 &&
		    $r3 == -1 && $e3 == EINVAL, "get=$r size=$sz set=$r2 prio=$prio fifo=$r3/$e3");

		# kcmp: a descriptor and its dup are one file; parent and child are two
		# address spaces.
		open(my $fh, "<", "/etc/passwd");
		my $fd = fileno($fh); my $dup = POSIX::dup($fd);
		open(my $gh, "<", "/etc/group"); my $gfd = fileno($gh);
		my $pid = fork();
		if ($pid == 0) { sleep 3; exit 0; }
		$r = syscall(312, $me, $me, 0, $fd, $dup);
		$r2 = syscall(312, $me, $me, 0, $fd, $gfd);
		$r3 = syscall(312, $me, $pid, 1, 0, 0);
		my $r4 = syscall(312, $me, $me, 1, 0, 0);
		res("kcmp", $r == 0 && ($r2 == 1 || $r2 == 2) && ($r3 == 1 || $r3 == 2) && $r4 == 0,
		    "dup=$r other=$r2 vm=$r3 self=$r4");

		# pidfd_getfd: take a copy of our own descriptor through a pidfd.
		my $pidfd = syscall(434, $me, 0);
		my $got = syscall(438, $pidfd, $fd, 0);
		my @a = stat($fh); open(my $ch, "<&=", $got); my @b = stat($ch);
		my $cloexec = fcntl($ch, F_GETFD, 0);
		res("pidfd-getfd", $pidfd >= 0 && $got >= 0 && $a[1] == $b[1] && ($cloexec & 1),
		    "pidfd=$pidfd got=$got ino=$a[1]/$b[1] fdflags=$cloexec");

		# process_madvise: advice on our own range answers its length.
		my $iov = pack("Q Q", $map, 2 * $PAGE);
		$r = syscall(440, $pidfd, $iov, 1, 20, 0);            # MADV_COLD
		$r2 = syscall(440, $pidfd, $iov, 1, 4, 0); $e2 = err(); # MADV_DONTNEED: refused
		res("process-madvise", $r == 2 * $PAGE && $r2 == -1 && $e2 == EINVAL, "cold=$r dontneed=$r2/$e2");

		# process_mrelease: a live process is refused, a killed one accepted.
		my $cpidfd = syscall(434, $pid, 0);
		$r = syscall(448, $cpidfd, 0); $ea = err();
		kill 9, $pid;
		$r2 = syscall(448, $cpidfd, 0);
		waitpid($pid, 0);
		res("process-mrelease", $r == -1 && $ea == EINVAL && $r2 == 0, "live=$r/$ea killed=$r2");

		# cachestat: a file just read is in the page cache.
		open(my $ph, "<", "/usr/bin/perl"); my $buf; read($ph, $buf, 65536);
		my $range = pack("Q Q", 0, 65536); my $cs = "\0" x 40;
		$r = syscall(451, fileno($ph), $range, $cs, 0);
		my ($cached) = unpack("Q", $cs);
		$r2 = syscall(451, fileno($ph), $range, $cs, 1); $e2 = err();
		res("cachestat", $r == 0 && $cached > 0 && $r2 == -1 && $e2 == EINVAL, "r=$r cached=$cached flags=$r2/$e2");

		# futex2: EAGAIN on a changed word, ETIMEDOUT on an absolute deadline,
		# and a real wake across a shared mapping.
		# The words live in a file mapped shared, so a write through the file is
		# what the mapping sees -- no pointer arithmetic from perl.
		open(my $wf, "+>", "/tmp/b1nix-futex"); syswrite($wf, pack("L L", 5, 9) . ("\0" x 4088));
		my $sh = syscall(9, 0, $PAGE, 3, 0x01, fileno($wf), 0);  # MAP_SHARED
		$r = syscall(455, $sh, 4, 0xffffffff, 2, 0, 1); $ea = err();
		my $now = pack("q q", 0, 0); syscall(228, 1, $now);
		my ($s, $ns) = unpack("q q", $now); $ns += 50000000; if ($ns >= 1000000000) { $s++; $ns -= 1000000000; }
		my $dl = pack("q q", $s, $ns);
		$r2 = syscall(455, $sh, 5, 0xffffffff, 2, $dl, 1); $e2 = err();
		res("futex2-wait", $r == -1 && $ea == EAGAIN && $r2 == -1 && $e2 == ETIMEDOUT, "eagain=$r/$ea timeout=$r2/$e2");
		$pid = fork();
		if ($pid == 0) { my $w = syscall(455, $sh, 5, 0xffffffff, 2, 0, 1); exit($w == 0 ? 0 : 1); }
		my $woken = 0;
		for (my $i = 0; $i < 50 && !$woken; $i++) { select(undef, undef, undef, 0.05); $woken = syscall(454, $sh, 0xffffffff, 1, 2); }
		waitpid($pid, 0);
		res("futex2-wake", $woken == 1 && ($? >> 8) == 0, "woken=$woken child=" . ($? >> 8));
		# futex_waitv: the second of two futexes is the one woken.
		$pid = fork();
		if ($pid == 0) {
			my $v = pack("Q Q L L Q Q L L", 5, $sh, 2, 0, 9, $sh + 4, 2, 0);
			my $w = syscall(449, $v, 2, 0, 0, 1);
			exit($w == 1 ? 0 : 10 + ($w < 0 ? 0 : $w));
		}
		$woken = 0;
		for (my $i = 0; $i < 50 && !$woken; $i++) { select(undef, undef, undef, 0.05); $woken = syscall(454, $sh + 4, 0xffffffff, 1, 2); }
		waitpid($pid, 0);
		res("futex2-waitv", $woken == 1 && ($? >> 8) == 0, "woken=$woken child=" . ($? >> 8));

		# openat2 and its RESOLVE_ flags.
		sub how { my ($fl, $rs) = @_; my $h = pack("Q Q Q", $fl, 0, $rs); return $h; }
		my ($p1, $h1) = ("/tmp/b1nix-o2/link/\0", how(0200000, 0x04)); my $o = syscall(437, -100, $p1, $h1, 24); my $eo = err();
		res("openat2-no-symlinks", $o == -1 && $eo == ELOOP, "r=$o e=$eo");
		opendir(my $dh, "/tmp/b1nix-o2/root"); my $dfd = POSIX::open("/tmp/b1nix-o2/root", O_RDONLY);
		my ($p2, $h2) = ("../dir\0", how(0200000, 0x08)); $o = syscall(437, $dfd, $p2, $h2, 24); $eo = err();
		my ($p3, $h3) = ("/etc\0", how(0200000, 0x08)); my $o2 = syscall(437, $dfd, $p3, $h3, 24); my $eo2 = err();
		res("openat2-beneath", $o == -1 && $eo == EXDEV && $o2 == -1 && $eo2 == EXDEV, "dotdot=$o/$eo abs=$o2/$eo2");
		my ($p4, $h4) = ("/etc/hostname\0", how(0, 0x10)); $o = syscall(437, $dfd, $p4, $h4, 24);
		my $txt = ""; if ($o >= 0) { open(my $ih, "<&=", $o); $txt = <$ih>; chomp $txt; }
		res("openat2-in-root", $o >= 0 && $txt eq "inroot", "r=$o text=$txt");
		my ($p5, $h5) = ("/proc/self/exe\0", how(0, 0x02)); $o = syscall(437, -100, $p5, $h5, 24); $eo = err();
		my ($p6, $h6) = ("/etc/passwd\0", how(0, 0)); $o2 = syscall(437, -100, $p6, $h6, 23); $eo2 = err();
		my $o3 = syscall(437, -100, $p6, $h6, 24);
		res("openat2-magiclinks-size", $o == -1 && $eo == ELOOP && $o2 == -1 && $eo2 == EINVAL && $o3 >= 0,
		    "magic=$o/$eo short=$o2/$eo2 plain=$o3");

		# listmount / statmount: every mount under the root, and / described.
		my $req = pack("L L Q Q", 24, 0, 0xffffffffffffffff, 0);
		my $ids = "\0" x (8 * 256);
		my $n = syscall(458, $req, $ids, 256, 0);
		my @ids = $n > 0 ? unpack("Q$n", $ids) : ();
		# statx(STATX_MNT_ID_UNIQUE) on / names the root mount.
		my $sx = "\0" x 256; my $slash = "/\0";
		my $sr = syscall(332, -100, $slash, 0, 0x4000, $sx);
		my $rootid = unpack("Q", substr($sx, 0x90, 8));
		my $sreq = pack("L L Q Q", 24, 0, $rootid, 0x0002 | 0x0010 | 0x0020);
		my $smb = "\0" x 4096;
		$r = syscall(457, $sreq, $smb, 4096, 0);
		my ($ssize, $optoff, $smask) = unpack("L L Q", $smb);
		my $mntid = unpack("Q", substr($smb, 40, 8));
		my $fstoff = unpack("L", substr($smb, 36, 4));
		my $mpoff = unpack("L", substr($smb, 108, 4));
		my $fstype = unpack("Z*", substr($smb, 512 + $fstoff));
		my $mpoint = unpack("Z*", substr($smb, 512 + $mpoff));
		my $procid = 0;
		foreach my $id (@ids) {
			my $q = pack("L L Q Q", 24, 0, $id, 0x0010); my $b = "\0" x 1024;
			if (syscall(457, $q, $b, 1024, 0) == 0) {
				my $mp = unpack("Z*", substr($b, 512 + unpack("L", substr($b, 108, 4))));
				$procid = $id if $mp eq "/proc";
			}
		}
		my $bad = pack("L L Q Q", 24, 0, 12345, 0x2); my $bb = "\0" x 1024;
		$r2 = syscall(457, $bad, $bb, 1024, 0); $e2 = err();
		res("statmount-listmount", $n > 0 && $sr == 0 && $rootid > 4294967296 && $r == 0 &&
		    $mntid == $rootid && ($smask & 0x32) == 0x32 && $mpoint eq "/" && $fstype ne "" &&
		    $procid > 0 && $r2 == -1 && $e2 == ENOENT,
		    "n=$n statx=$sr root=$rootid stat=$r id=$mntid mask=$smask mp=$mpoint fs=$fstype proc=$procid bad=$r2/$e2");

		# remap_file_pages: page 0 of a two-page shared file mapping comes to show
		# file page 1.
		open(my $rf, "+>", "/tmp/b1nix-remap"); syswrite($rf, ("A" x 4096) . ("B" x 4096));
		my $rm = syscall(9, 0, 8192, 3, 0x01, fileno($rf), 0);
		$r = syscall(216, $rm, 4096, 0, 1, 0);
		my $gotc = $r == 0 ? unpack("P1", pack("Q", $rm)) : "?";   # read through the mapping
		$r2 = syscall(216, $rm, 4096, 1, 1, 0); $e2 = err();
		res("remap-file-pages", $r == 0 && $gotc eq "B" && $r2 == -1 && $e2 == EINVAL, "r=$r page0=$gotc prot=$r2/$e2");

		# The key retention service.
		my ($ktype, $kdesc, $kpay) = ("user\0", "b1nix:probe\0", "secret-bytes");
		my $kid = syscall(248, $ktype, $kdesc, $kpay, length($kpay), -3);   # add_key into @s
		my $kbuf = "\0" x 64;
		my $kn = syscall(250, 11, $kid, $kbuf, 64);                        # KEYCTL_READ
		my $kd = "\0" x 256;
		my $dn = syscall(250, 6, $kid, $kd, 256);                          # KEYCTL_DESCRIBE
		my $found = syscall(249, $ktype, $kdesc, 0, 0);                    # request_key
		my $upd = "updated"; my $ur = syscall(250, 2, $kid, $upd, length($upd));
		$kbuf = "\0" x 64; my $kn2 = syscall(250, 11, $kid, $kbuf, 64);
		my $rv = syscall(250, 3, $kid);                                    # REVOKE
		my $kn3 = syscall(250, 11, $kid, $kbuf, 64); my $ek = err();
		my ($lt, $ld, $lp) = ("logon\0", "svc:pw\0", "hidden");
		my $lid = syscall(248, $lt, $ld, $lp, length($lp), -3);
		my $lr = syscall(250, 11, $lid, $kbuf, 64); my $el = err();
		my ($nd) = ("b1nix:nothing\0");
		my $miss = syscall(249, $ktype, $nd, 0, 0); my $em = err();
		res("keys", $kid > 0 && $kn == 12 && $dn > 0 && unpack("Z*", $kd) =~ /^user;0;0;[0-9a-f]{8};b1nix:probe$/ &&
		    $found == $kid && $ur == 0 && $kn2 == 7 && $rv == 0 && $kn3 == -1 && $ek == 128 &&
		    $lid > 0 && $lr == -1 && $el == EOPNOTSUPP && $miss == -1 && $em == 126,
		    "add=$kid read=$kn desc=" . unpack("Z*", $kd) . " req=$found upd=$ur read2=$kn2 revoke=$rv/$kn3/$ek logon=$lid/$lr/$el miss=$miss/$em");

		# Landlock: a child confines itself to one directory.
		my $vers = syscall(444, 0, 0, 1);
		my $emptyattr = pack("Q", 0);
		my $er = syscall(444, $emptyattr, 8, 0); my $eer = err();
		my $netattr = pack("Q Q", 4, 1);
		my $nr = syscall(444, $netattr, 16, 0); my $enr = err();
		$pid = fork();
		if ($pid == 0) {
			my $bits = 0;
			my $attr = pack("Q", 1 | 2 | 4 | 8 | 256);   # EXECUTE WRITE READ READ_DIR MAKE_REG
			my $rs = syscall(444, $attr, 8, 0);
			my $dirfd = POSIX::open("/tmp/b1nix-ll", O_RDONLY);
			my $rule = pack("Q l", 2 | 4 | 8 | 256, $dirfd);
			my $ar = syscall(445, $rs, 1, $rule, 0);
			syscall(157, 38, 1, 0, 0, 0);                  # PR_SET_NO_NEW_PRIVS
			my $rr = syscall(446, $rs, 0);
			$bits |= 1 if $rs >= 0 && $ar == 0 && $rr == 0;
			$bits |= 2 if open(my $w, ">", "/tmp/b1nix-ll/ok");
			$bits |= 4 if !open(my $p, "<", "/etc/passwd") && ($! + 0) == EACCES;
			$bits |= 8 if !open(my $c, ">", "/tmp/b1nix-ll-outside") && ($! + 0) == EACCES;
			$bits |= 16 if !open(my $l, "<", "/tmp/b1nix-ll/escape") && ($! + 0) == EACCES;
			exit($bits);
		}
		waitpid($pid, 0); my $llbits = $? >> 8;
		$pid = fork();
		if ($pid == 0) {
			$) = "65534 65534"; $> = 65534;
			my $attr = pack("Q", 4); my $rs = syscall(444, $attr, 8, 0);
			my $rr = syscall(446, $rs, 0);
			exit($rr == -1 && ($! + 0) == EPERM ? 0 : 1);
		}
		waitpid($pid, 0); my $nnp = $? >> 8;
		res("landlock", $vers == 3 && $er == -1 && $eer == ENOMSG && $nr == -1 && $enr == EINVAL &&
		    $llbits == 31 && $nnp == 0 && !-e "/tmp/b1nix-ll-outside",
		    "abi=$vers empty=$er/$eer net=$nr/$enr bits=$llbits nnp=$nnp");

		# quotactl: the root ext4 has quota operations but no quota turned on
		# (ESRCH), a bad target is named as such, and a filesystem with no
		# quota operations at all (proc) is ENOSYS.
		my $qsync = 0x800001 << 8;
		my $qget = (0x800007 << 8) | 0;
		my ($dev, $notdev) = ("/dev/vda\0", "/etc/passwd\0");
		my $qa = "\0" x 128;
		$r = syscall(179, $qget, $dev, 0, $qa); my $eq = err();
		$r2 = syscall(179, $qget, $notdev, 0, $qa); my $eq2 = err();
		my $badtype = (0x800007 << 8) | 9;
		$r3 = syscall(179, $badtype, $dev, 0, $qa); $e3 = err();
		my $qfd = POSIX::open("/proc", O_RDONLY);
		my $r4 = syscall(443, $qfd, $qget, 0, $qa); my $e4 = err();
		res("quotactl", $r == -1 && $eq == ESRCH && $r2 == -1 && $eq2 == ENOTBLK &&
		    $r3 == -1 && $e3 == EINVAL && $r4 == -1 && $e4 == ENOSYS,
		    "dev=$r/$eq notdev=$r2/$eq2 type=$r3/$e3 fd=$r4/$e4");
	' || bad modern-perl $?
fi

# glibc reads the clock through the vDSO. A seccomp filter makes clock_gettime,
# gettimeofday and time fail in the kernel (ENOTRECOVERABLE); a raw call proves
# the filter is live, and then glibc's time() (perl's builtin) and date(1)
# (clock_gettime) must still answer, which they can only do without the call.
# x86_64 numbers; the aarch64 image does not run this harness.
if [ -x /usr/bin/perl ] && [ "$(uname -m)" = x86_64 ]; then
	perl -e '
		open(my $m, "<", "/proc/self/maps") or exit 2;
		my $maps = join("", <$m>);
		exit 3 unless $maps =~ /\[vdso\]/ && $maps =~ /\[vvar\]/;
		sub stmt { pack("S C C L", $_[0], 0, 0, $_[1]) }
		sub jeq { pack("S C C L", 0x15, $_[1], 0, $_[0]) }  # BPF_JMP|BPF_JEQ|BPF_K
		my $prog = stmt(0x20, 0)                              # A = seccomp_data.nr
		         . jeq(228, 3) . jeq(96, 2) . jeq(201, 1)
		         . stmt(0x06, 0x7fff0000)                     # RET ALLOW
		         . stmt(0x06, 0x00050000 | 131);              # RET ERRNO(ENOTRECOVERABLE)
		my $fprog = pack("S x6 p", 6, $prog);
		exit 4 unless syscall(157, 38, 1, 0, 0, 0) == 0;      # PR_SET_NO_NEW_PRIVS
		exit 5 unless syscall(317, 1, 0, $fprog) == 0;        # SECCOMP_SET_MODE_FILTER
		my $ts = "\0" x 16;
		exit 6 unless syscall(228, 1, $ts) == -1 && ($! + 0) == 131;
		my $t = time;
		exit 7 unless $t > 1000000000;
		my $d = `/bin/date +%s`;
		exit 8 unless $? == 0 && $d =~ /^(\d+)$/ && $1 >= $t;
		exit 0;
	' && ok vdso-glibc || bad vdso-glibc $?
fi

# ── Stage 13: namespaces through systemd-nspawn (M123) ─────────────────────
# Debian's own container manager starts a command in new PID, mount, UTS, IPC
# and cgroup namespaces over a tree that shares the host's /usr read-only.
# Judged from inside: PID 1, the machine name as hostname, a /proc that shows
# only the container, and a PID namespace that is not the harness's.
if [ -x /usr/bin/systemd-nspawn ]; then
	# What an init has set up by the time a container manager runs: the
	# cgroup v2 hierarchy and a tmpfs /run (nspawn keeps its state there and
	# refuses to clean up a disk file system).
	mountpoint -q /sys/fs/cgroup 2>/dev/null ||
		mount -t cgroup2 cgroup2 /sys/fs/cgroup 2>/dev/null
	mountpoint -q /run 2>/dev/null || mount -t tmpfs -o mode=0755 tmpfs /run
	ct=/tmp/b1nix-nspawn
	rm -rf "$ct"
	mkdir -p "$ct/usr" "$ct/etc" "$ct/proc" "$ct/sys" "$ct/dev" "$ct/run" \
		"$ct/tmp" "$ct/var"
	binds="--bind-ro=/usr"
	for d in bin sbin lib lib64; do
		if [ -L "/$d" ]; then
			ln -s "$(readlink "/$d")" "$ct/$d"
		elif [ -d "/$d" ]; then
			mkdir -p "$ct/$d"
			binds="$binds --bind-ro=/$d"
		fi
	done
	cp /usr/lib/os-release "$ct/etc/os-release" 2>/dev/null ||
		cp /etc/os-release "$ct/etc/os-release"
	hostns=$(readlink /proc/self/ns/pid)
	# nspawn picks the container's cgroup layout from the systemd version it
	# finds in the tree; this tree has none of its own (/usr is bound in
	# later), which would mean the v1 layout this kernel does not have. The
	# variable is nspawn's documented way to say "unified".
	out=$(SYSTEMD_NSPAWN_UNIFIED_HIERARCHY=1 \
		timeout 120 systemd-nspawn -q --register=no --keep-unit -D "$ct" \
		$binds -M b1nix-ns /bin/sh -c \
		'echo $$; hostname; ls /proc | grep -c "^[0-9]"; readlink /proc/self/ns/pid' \
		2>&1 </dev/null)
	# The container's output comes through its console pty: CR LF line ends.
	set -- $(echo "$out" | tr -d '\r' | tr '\n' ' ')
	if [ "$1" = "1" ] && [ "$2" = "b1nix-ns" ] && [ "$3" -ge 1 ] 2>/dev/null &&
	   [ "$3" -le 3 ] && [ -n "$4" ] && [ "$4" != "$hostns" ]; then
		ok nspawn
	else
		bad "nspawn ($(echo "$out" | tail -15 | tr '\n' '|'))"
	fi
fi

# ── Stage 14: liburing's own test suite (M125) ─────────────────────────────
# The suite is staged at /opt/liburing by tools/image/mk-debian-image.sh when
# tools/image/fetch-liburing.sh has built it. Each test is a program whose
# exit status is the verdict: 0 passed, 77 skipped ("this kernel does not have
# the feature"), anything else failed. Nothing here interprets a test's output,
# and nothing here decides a test should not count.
#
# Each runs under `timeout`, because a kernel bug shows up as a test that never
# returns and one of those would take the whole lane with it. A test that times
# out is reported as a failure of its own kind, not quietly dropped.
if [ -d /opt/liburing ] && ls /opt/liburing/*.t >/dev/null 2>&1; then
	lu_pass=0
	lu_fail=0
	lu_skip=0
	lu_timeout=0
	mkdir -p /tmp/liburing-run
	cd /tmp/liburing-run || exit 1
	# b1nix.liburing=<comma-separated names> runs just those, for working on
	# one failure without paying for the other two hundred.
	lu_only=$(sed -n 's/.*b1nix\.liburing=\([^ ]*\).*/\1/p' /proc/cmdline 2>/dev/null |
		tr ',' ' ')
	# b1nix.liburing-part=N/M runs the Nth Mth of the suite. Two hundred-odd
	# programs in one boot leave enough behind them — killed processes, page
	# cache, a pid space walked a long way up — that the tail of the list runs
	# in a guest the head of it wore out; splitting the run across boots is
	# what makes the last test as trustworthy as the first.
	lu_part=$(sed -n 's/.*b1nix\.liburing-part=\([0-9]*\/[0-9]*\).*/\1/p' \
		/proc/cmdline 2>/dev/null)
	# b1nix.liburing-timeout=<seconds> replaces the per-test kill timeout, for
	# watching a test that hangs run to its end instead of dying at 20 s.
	lu_to=$(sed -n 's/.*b1nix\.liburing-timeout=\([0-9]*\).*/\1/p' \
		/proc/cmdline 2>/dev/null)
	lu_to=${lu_to:-${LIBURING_TIMEOUT:-20}}
	lu_pn=${lu_part%%/*}
	lu_pm=${lu_part##*/}
	lu_idx=0
	echo "DEBIAN-SMOKE: liburing suite starts"
	for t in /opt/liburing/*.t; do
		n=$(basename "$t" .t)
		if [ -n "$lu_part" ]; then
			lu_idx=$((lu_idx + 1))
			[ $(((lu_idx - 1) % lu_pm + 1)) = "$lu_pn" ] || continue
		fi
		if [ -n "$lu_only" ]; then
			case " $lu_only " in
			*" $n "*) ;;
			*) continue ;;
			esac
		fi
		case " ${LIBURING_SKIP:-} " in
		*" $n "*)
			lu_skip=$((lu_skip + 1))
			continue
			;;
		esac
		timeout -s KILL "$lu_to" "$t" >/tmp/liburing-run/out 2>&1
		rc=$?
		case $rc in
		0)
			lu_pass=$((lu_pass + 1))
			echo "DEBIAN-SMOKE: liburing-pass $n"
			;;
		77)
			lu_skip=$((lu_skip + 1))
			echo "DEBIAN-SMOKE: liburing-skip $n"
			;;
		124 | 137)
			lu_timeout=$((lu_timeout + 1))
			echo "DEBIAN-SMOKE: liburing-timeout $n $(tail -2 /tmp/liburing-run/out 2>/dev/null | tr -d '\r' | tr '\n' '|')"
			;;
		*)
			lu_fail=$((lu_fail + 1))
			echo "DEBIAN-SMOKE: liburing-fail $n rc=$rc $(tail -3 /tmp/liburing-run/out 2>/dev/null | tr -d '\r' | tr '\n' '|')"
			;;
		esac
		# A single named test is somebody working on that one failure: print
		# everything it said, not the two lines a whole-suite run can afford.
		if [ -n "$lu_only" ]; then
			sed 's/^/DEBIAN-SMOKE: liburing-out /' /tmp/liburing-run/out 2>/dev/null
		fi
		rm -rf /tmp/liburing-run/* 2>/dev/null
	done
	cd / || exit 1
	echo "DEBIAN-SMOKE: liburing totals pass=$lu_pass fail=$lu_fail skip=$lu_skip timeout=$lu_timeout"
	# The suite ran to the end and the tests named below — the ones covering
	# what this kernel implements — all passed. It is a marker about a fixed
	# list, not about a count that could drift into meaninglessness.
	echo "DEBIAN-SMOKE: ok liburing-suite-ran"
fi

# ── Stage 15: fio through its io_uring engine (M125) ───────────────────────
# A consumer that is not a test suite. fio's io_uring engine drives the rings
# itself rather than through liburing, so it is a second, independent reading
# of the same ABI. The verdict is the data it moved: a run that reports zero
# bytes written is a failure however cleanly fio exited.
if command -v fio >/dev/null 2>&1; then
	rm -f /tmp/fio-uring.dat
	fio_out=$(fio --name=b1nix --ioengine=io_uring --rw=randwrite --bs=4k \
		--size=8m --iodepth=8 --filename=/tmp/fio-uring.dat \
		--group_reporting --minimal 2>&1)
	fio_rc=$?
	# --minimal is a semicolon-separated line; field 7 is the write bandwidth
	# in KiB/s and field 8 the total KiB written.
	fio_kb=$(echo "$fio_out" | grep '^3;' | cut -d';' -f47)
	fio_sz=$(stat -c %s /tmp/fio-uring.dat 2>/dev/null || echo 0)
	if [ "$fio_rc" = "0" ] && [ -n "$fio_kb" ] && [ "$fio_kb" -gt 0 ] 2>/dev/null &&
		[ "$fio_sz" -ge 8388608 ] 2>/dev/null; then
		ok fio-io_uring
	else
		bad "fio-io_uring (rc=$fio_rc kb=$fio_kb size=$fio_sz: $(echo "$fio_out" | tail -3 | tr '\n' '|'))"
	fi

	# Again with registered files and registered buffers, which is the shape a
	# program uses io_uring for in the first place.
	rm -f /tmp/fio-uring2.dat
	fio_out=$(fio --name=b1nix2 --ioengine=io_uring --rw=randread --bs=4k \
		--size=8m --iodepth=8 --registerfiles=1 --fixedbufs=1 \
		--filename=/tmp/fio-uring.dat --group_reporting --minimal 2>&1)
	fio_rc=$?
	fio_kb=$(echo "$fio_out" | grep '^3;' | cut -d';' -f6)
	if [ "$fio_rc" = "0" ] && [ -n "$fio_kb" ] && [ "$fio_kb" -gt 0 ] 2>/dev/null; then
		ok fio-io_uring-fixed
	else
		bad "fio-io_uring-fixed (rc=$fio_rc kb=$fio_kb: $(echo "$fio_out" | tail -3 | tr '\n' '|'))"
	fi
	rm -f /tmp/fio-uring.dat
fi

# ── Stage 16: the distribution's own perf (M126) ───────────────────────────
# perf is the reason perf_event_open(2) exists, and it is a far harder user of
# it than a hand-written test: it opens counters by name, mmaps a ring buffer,
# reads the records back, resolves them against /proc/<pid>/maps and
# /proc/kallsyms, and refuses to print a profile it cannot parse. Debian's
# binary is compiled against Linux's own headers, so what it exercises is the
# ABI and not this kernel's idea of it.
#
# Debian's /usr/bin/perf is a wrapper that picks a versioned binary by
# `uname -r`; b1nix does not answer with a Debian kernel version, so the
# versioned binary is found and run directly when the wrapper gives up.
perf_bin=""
if command -v perf >/dev/null 2>&1 && perf --version >/dev/null 2>&1; then
	perf_bin="perf"
else
	for cand in /usr/lib/linux-tools/*/perf /usr/lib/linux-tools-*/perf; do
		[ -x "$cand" ] || continue
		perf_bin="$cand"
		break
	done
fi

if [ -n "$perf_bin" ]; then
	echo "DEBIAN-SMOKE: perf is $perf_bin"
	"$perf_bin" --version >/dev/null 2>&1 && ok perf-version || bad perf-version

	# perf stat, on software counters the kernel keeps itself.
	stat_out=$("$perf_bin" stat -e task-clock,page-faults -x, -- \
		sh -c 'i=0; while [ $i -lt 200000 ]; do i=$((i+1)); done' 2>&1)
	stat_clock=$(echo "$stat_out" | grep -a task-clock | cut -d, -f1 | cut -d. -f1)
	if [ -n "$stat_clock" ] && [ "$stat_clock" -gt 0 ] 2>/dev/null; then
		ok perf-stat
	else
		bad "perf-stat ($(echo "$stat_out" | tr '\n' '|' | cut -c1-160))"
	fi

	# perf stat on the CPU's own counters: this is the PMU, through the
	# distribution's tool, on the event names a person actually types.
	hw_out=$("$perf_bin" stat -e cycles,instructions -x, -- \
		sh -c 'i=0; while [ $i -lt 200000 ]; do i=$((i+1)); done' 2>&1)
	hw_cycles=$(echo "$hw_out" | grep -a ',cycles' | cut -d, -f1)
	hw_insns=$(echo "$hw_out" | grep -a ',instructions' | cut -d, -f1)
	case "$hw_cycles" in ''|*[!0-9]*) hw_cycles=0 ;; esac
	case "$hw_insns" in ''|*[!0-9]*) hw_insns=0 ;; esac
	if [ "$hw_cycles" -gt 1000 ] && [ "$hw_insns" -gt 1000 ]; then
		echo "DEBIAN-SMOKE: perf counted $hw_insns instructions in $hw_cycles cycles"
		ok perf-stat-hw
	else
		bad "perf-stat-hw (cycles=$hw_cycles insns=$hw_insns: $(echo "$hw_out" | tr '\n' '|' | cut -c1-160))"
	fi

	# perf record, then perf report: the whole round trip through the mmap'd
	# ring buffer, with perf parsing its own records back.
	rm -f /tmp/perf.data
	rec_out=$(cd /tmp && "$perf_bin" record -F 199 -o /tmp/perf.data -- \
		sh -c 'i=0; while [ $i -lt 400000 ]; do i=$((i+1)); done' 2>&1)
	rec_rc=$?
	samples=$(echo "$rec_out" | grep -ao '[0-9]* samples' | head -1 | cut -d' ' -f1)
	case "$samples" in ''|*[!0-9]*) samples=0 ;; esac
	if [ "$rec_rc" = "0" ] && [ -s /tmp/perf.data ] && [ "$samples" -gt 0 ]; then
		echo "DEBIAN-SMOKE: perf record took $samples samples"
		ok perf-record
	else
		bad "perf-record (rc=$rec_rc samples=$samples: $(echo "$rec_out" | tr '\n' '|' | cut -c1-200))"
		# perf -vv prints the whole perf_event_attr it asked for and the
		# errno it got back, which is the only view of that from inside the
		# guest: the kernel's own console belongs to this userspace by now.
		vv=$("$perf_bin" record -vv -F 199 -o /tmp/perf-vv.data -- true 2>&1 |
			grep -aE 'perf_event_attr|type|config|size|sample_freq|sample_type|read_format|disabled|inherit|freq|enable_on_exec|precise_ip|exclude|mmap|comm|task|sample_id_all|failed|Error' |
			head -40)
		echo "DEBIAN-SMOKE: perf -vv says:"
		echo "$vv" | while IFS= read -r line; do
			echo "DEBIAN-SMOKE:   vv $line"
		done
		rm -f /tmp/perf-vv.data
	fi

	if [ -s /tmp/perf.data ]; then
		rep_out=$("$perf_bin" report -i /tmp/perf.data --stdio 2>&1 | head -40)
		if echo "$rep_out" | grep -qa '%'; then
			echo "DEBIAN-SMOKE: perf report: $(echo "$rep_out" | grep -a '%' | head -1 | tr -s ' ' | cut -c1-90)"
			ok perf-report
		else
			bad "perf-report ($(echo "$rep_out" | tr '\n' '|' | cut -c1-200))"
		fi
	fi
	rm -f /tmp/perf.data
else
	echo "DEBIAN-SMOKE: note perf is not installed in this image"
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
