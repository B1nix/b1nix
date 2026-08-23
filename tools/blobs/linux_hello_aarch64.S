/* M40 — a tiny *static Linux aarch64* ELF used to validate b1nix's Linux ABI
 * compatibility layer end to end. Counterpart of linux_hello.S, which does the
 * same job for x86_64.
 *
 * It uses the LINUX asm-generic syscall NUMBERS (write=64, exit_group=94), not
 * b1nix's native ones, and carries a real GNU .note.ABI-tag declaring OS=Linux
 * — the note a genuine Linux static binary has and b1nix's own freestanding
 * userspace never emits. The loader keys the "linux" personality off that note
 * or off EI_OSABI==ELFOSABI_LINUX.
 *
 * Differences from the x86_64 blob, all forced by the architecture rather than
 * by choice:
 *   - asm-generic has no open/getdents; openat(AT_FDCWD) and getdents64 are the
 *     calls that exist, so those two steps drive those.
 *   - arch_prctl does not exist either: the TLS register is TPIDR_EL0, written
 *     directly, so the equivalent check is that the kernel preserves it across
 *     a syscall and a signal.
 *
 * Assembled and linked by tools/blobs/build-linux-hello.sh into a committed blob
 * (tools/blobs/linux_hello_aarch64.bin) staged as /bin/m40-linux-hello.
 */

	.section .note.ABI-tag, "a"
	.align 4
	.long 4              /* n_namesz: "GNU\0" */
	.long 16             /* n_descsz: 4 words */
	.long 1              /* n_type:   NT_GNU_ABI_TAG */
	.asciz "GNU"
	.align 4
	.long 0              /* OS: 0 = Linux */
	.long 3              /* major */
	.long 2              /* minor */
	.long 0              /* subminor */

	.section .rodata
msg:
	.ascii "M40-LINUX: hello from a static linux binary\n"
	.set msg_len, . - msg
okmsg:
	.ascii "M40-LINUX: ok fstat\n"
	.set okmsg_len, . - okmsg
failmsg:
	.ascii "M40-LINUX: fail fstat\n"
	.set failmsg_len, . - failmsg
unameok:
	.ascii "M40-LINUX: ok uname\n"
	.set unameok_len, . - unameok
unamefail:
	.ascii "M40-LINUX: fail uname\n"
	.set unamefail_len, . - unamefail
gdok:
	.ascii "M40-LINUX: ok getdents64\n"
	.set gdok_len, . - gdok
gdfail:
	.ascii "M40-LINUX: fail getdents64\n"
	.set gdfail_len, . - gdfail
prctlok:
	.ascii "M40-LINUX: ok arch-prctl\n"
	.set prctlok_len, . - prctlok
prctlfail:
	.ascii "M40-LINUX: fail arch-prctl\n"
	.set prctlfail_len, . - prctlfail
mmapok:
	.ascii "M40-LINUX: ok mmap\n"
	.set mmapok_len, . - mmapok
mmapfail:
	.ascii "M40-LINUX: fail mmap\n"
	.set mmapfail_len, . - mmapfail
clockok:
	.ascii "M40-LINUX: ok clock\n"
	.set clockok_len, . - clockok
clockfail:
	.ascii "M40-LINUX: fail clock\n"
	.set clockfail_len, . - clockfail
gettidok:
	.ascii "M40-LINUX: ok gettid\n"
	.set gettidok_len, . - gettidok
gettidfail:
	.ascii "M40-LINUX: fail gettid\n"
	.set gettidfail_len, . - gettidfail
signalok:
	.ascii "M40-LINUX: ok signal\n"
	.set signalok_len, . - signalok
signalfail:
	.ascii "M40-LINUX: fail signal\n"
	.set signalfail_len, . - signalfail
sigmaskok:
	.ascii "M40-LINUX: ok sigmask\n"
	.set sigmaskok_len, . - sigmaskok
sigmaskfail:
	.ascii "M40-LINUX: fail sigmask\n"
	.set sigmaskfail_len, . - sigmaskfail
tgkillok:
	.ascii "M40-LINUX: ok tgkill\n"
	.set tgkillok_len, . - tgkillok
tgkillfail:
	.ascii "M40-LINUX: fail tgkill\n"
	.set tgkillfail_len, . - tgkillfail
alarmok:
	.ascii "M40-LINUX: ok alarm\n"
	.set alarmok_len, . - alarmok
alarmfail:
	.ascii "M40-LINUX: fail alarm\n"
	.set alarmfail_len, . - alarmfail
siginfook:
	.ascii "M40-LINUX: ok siginfo\n"
	.set siginfook_len, . - siginfook
siginfofail:
	.ascii "M40-LINUX: fail siginfo\n"
	.set siginfofail_len, . - siginfofail
dirpath:
	.asciz "/bin"

	.section .data
	.align 8
sigact:
	.quad sigusr1_handler   /* sa_handler */
	.quad 0                 /* sa_flags   */
	.quad 0                 /* sa_restorer */
	.quad 0                 /* sa_mask    */
sigact_si:
	.quad siginfo_handler   /* sa_sigaction */
	.quad 4                 /* sa_flags = SA_SIGINFO */
	.quad 0                 /* sa_restorer */
	.quad 0                 /* sa_mask     */
sigflag:
	.long 0
siginfo_flag:
	.long 0
	.align 8
blockset:
	.quad 0x200             /* sigset_t with Linux SIGUSR1 (bit 9) set */
statbuf:
	.skip 144
utsbuf:
	.skip 390
dbuf:
	.skip 1024
tlsblock:
	.skip 16
tsbuf:
	.skip 16

/* write(1, buf, len). Clobbers x0-x2, x8. */
.macro EMIT sym, len
	mov x8, #64
	mov x0, #1
	adrp x1, \sym
	add x1, x1, :lo12:\sym
	mov x2, #\len
	svc #0
.endm

	.section .text
	.globl _start
_start:
	EMIT msg, msg_len

	/* fstat(1, &statbuf) — asm-generic nr 80. st_mode sits at offset 16 in the
	 * asm-generic layout (dev 0, ino 8, mode 16, nlink 20, uid 24) — NOT at 24
	 * the way x86_64 spells it; that offset is st_uid here, which is 0 for root
	 * and would read as "no type bits". Confirm the S_IFMT bits are set. */
	mov x8, #80
	mov x0, #1
	adrp x1, statbuf
	add x1, x1, :lo12:statbuf
	svc #0
	cbnz x0, .Lfstat_fail
	adrp x9, statbuf
	add x9, x9, :lo12:statbuf
	ldr w10, [x9, #16]
	and w10, w10, #0xF000
	cbz w10, .Lfstat_fail
	EMIT okmsg, okmsg_len
	b .Luname

.Lfstat_fail:
	EMIT failmsg, failmsg_len
	b .Lexit

.Luname:
	/* uname(&utsbuf) — nr 160. machine[] sits at 260 in the Linux layout but
	 * at 128 in b1nix's 160-byte struct, so byte 260 stays zero unless the
	 * kernel widened the result. */
	mov x8, #160
	adrp x0, utsbuf
	add x0, x0, :lo12:utsbuf
	svc #0
	cbnz x0, .Luname_fail
	adrp x9, utsbuf
	add x9, x9, :lo12:utsbuf
	ldrb w10, [x9, #260]
	cbz w10, .Luname_fail
	EMIT unameok, unameok_len
	b .Lgetdents

.Luname_fail:
	EMIT unamefail, unamefail_len
	b .Lexit

.Lgetdents:
	/* openat(AT_FDCWD, "/bin", O_RDONLY) — nr 56. asm-generic has no open. */
	mov x8, #56
	mov x0, #-100            /* AT_FDCWD */
	adrp x1, dirpath
	add x1, x1, :lo12:dirpath
	mov x2, #0
	mov x3, #0
	svc #0
	tbnz x0, #63, .Lgd_fail  /* fd < 0 */
	mov x19, x0              /* save fd */

	/* getdents64(fd, dbuf, 1024) — nr 61. d_reclen (u16 at 16) must be a real
	 * record length and d_name (19) non-empty; the native fixed dirent would
	 * put its type field (1/2/3) at offset 16. */
	mov x8, #61
	mov x0, x19
	adrp x1, dbuf
	add x1, x1, :lo12:dbuf
	mov x2, #1024
	svc #0
	cmp x0, #0
	b.le .Lgd_fail
	adrp x9, dbuf
	add x9, x9, :lo12:dbuf
	ldrh w10, [x9, #16]
	cmp w10, #20
	b.lo .Lgd_fail
	ldrb w10, [x9, #19]
	cbz w10, .Lgd_fail

	/* close(fd) — nr 57 */
	mov x8, #57
	mov x0, x19
	svc #0

	EMIT gdok, gdok_len
	b .Lprctl

.Lgd_fail:
	EMIT gdfail, gdfail_len
	b .Lexit

.Lprctl:
	/* The TLS-pointer equivalent. x86_64 needs arch_prctl(ARCH_SET_FS) because
	 * only the kernel can write FS_BASE; AArch64 lets EL0 write TPIDR_EL0
	 * directly, so there is no syscall to check. What still has to hold — and
	 * is what the x86 check is really about — is that the kernel PRESERVES the
	 * thread pointer across a syscall. Set it, make a syscall, read it back. */
	adrp x9, tlsblock
	add x9, x9, :lo12:tlsblock
	msr tpidr_el0, x9
	mov x8, #172             /* getpid */
	svc #0
	mrs x10, tpidr_el0
	cmp x9, x10
	b.ne .Lprctl_fail
	/* And that the block it points at is really ours. */
	movz x11, #0x7788
	movk x11, #0x5566, lsl #16
	movk x11, #0x3344, lsl #32
	movk x11, #0x1122, lsl #48
	str x11, [x9]
	ldr x12, [x9]
	cmp x11, x12
	b.ne .Lprctl_fail
	EMIT prctlok, prctlok_len
	b .Lmmap

.Lprctl_fail:
	EMIT prctlfail, prctlfail_len
	b .Lexit

.Lmmap:
	/* mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)
	 * — nr 222. Returns the address, or -errno in [-4095,-1]. */
	mov x8, #222
	mov x0, #0
	mov x1, #4096
	mov x2, #3               /* PROT_READ|PROT_WRITE */
	mov x3, #0x22            /* MAP_PRIVATE|MAP_ANONYMOUS */
	mov x4, #-1
	mov x5, #0
	svc #0
	cmn x0, #4096            /* x0 >= -4095 (unsigned view) => error */
	b.hi .Lmmap_fail
	mov x20, x0
	movz x11, #0xf00d
	movk x11, #0xcafe, lsl #16
	movk x11, #0xbeef, lsl #32
	movk x11, #0xdead, lsl #48
	str x11, [x20]
	ldr x12, [x20]
	cmp x11, x12
	b.ne .Lmmap_fail
	/* munmap(addr, 4096) — nr 215 */
	mov x8, #215
	mov x0, x20
	mov x1, #4096
	svc #0
	EMIT mmapok, mmapok_len
	b .Lclock

.Lmmap_fail:
	EMIT mmapfail, mmapfail_len
	b .Lexit

.Lclock:
	/* clock_gettime(CLOCK_MONOTONIC, &tsbuf) — nr 113. */
	mov x8, #113
	mov x0, #1
	adrp x1, tsbuf
	add x1, x1, :lo12:tsbuf
	svc #0
	cbnz x0, .Lclock_fail
	adrp x9, tsbuf
	add x9, x9, :lo12:tsbuf
	ldr x10, [x9]
	ldr x11, [x9, #8]
	orr x10, x10, x11
	cbz x10, .Lclock_fail
	EMIT clockok, clockok_len
	b .Lgettid

.Lclock_fail:
	EMIT clockfail, clockfail_len
	b .Lexit

.Lgettid:
	/* gettid() — nr 178. A valid thread id is positive. */
	mov x8, #178
	svc #0
	cmp x0, #0
	b.le .Lgettid_fail
	EMIT gettidok, gettidok_len
	b .Lsignal

.Lgettid_fail:
	EMIT gettidfail, gettidfail_len
	b .Lexit

.Lsignal:
	/* rt_sigaction(SIGUSR1=10, &sigact, NULL, 8) — nr 134. b1nix numbers
	 * SIGUSR1 as 19, so install + kill + delivery all depend on the remap. */
	mov x8, #134
	mov x0, #10
	adrp x1, sigact
	add x1, x1, :lo12:sigact
	mov x2, #0
	mov x3, #8
	svc #0
	cbnz x0, .Lsignal_fail
	mov x8, #172             /* getpid */
	svc #0
	mov x19, x0
	mov x8, #129             /* kill(pid, 10) */
	mov x0, x19
	mov x1, #10
	svc #0
	cbnz x0, .Lsignal_fail
	adrp x9, sigflag
	add x9, x9, :lo12:sigflag
	ldr w10, [x9]
	cmp w10, #10
	b.ne .Lsignal_fail
	EMIT signalok, signalok_len
	b .Lsigmask

.Lsignal_fail:
	EMIT signalfail, signalfail_len
	b .Lexit

.Lsigmask:
	/* Block SIGUSR1, signal ourselves (handler must NOT run), unblock (it must
	 * then run). Depends on the sigset bit remap and the how-value remap. */
	adrp x9, sigflag
	add x9, x9, :lo12:sigflag
	str wzr, [x9]
	mov x8, #135             /* rt_sigprocmask(SIG_BLOCK=0, &blockset, NULL, 8) */
	mov x0, #0
	adrp x1, blockset
	add x1, x1, :lo12:blockset
	mov x2, #0
	mov x3, #8
	svc #0
	cbnz x0, .Lsigmask_fail
	mov x8, #172
	svc #0
	mov x19, x0
	mov x8, #129
	mov x0, x19
	mov x1, #10
	svc #0
	adrp x9, sigflag
	add x9, x9, :lo12:sigflag
	ldr w10, [x9]
	cbnz w10, .Lsigmask_fail /* blocked: must not have run */
	mov x8, #135             /* SIG_UNBLOCK=1 */
	mov x0, #1
	adrp x1, blockset
	add x1, x1, :lo12:blockset
	mov x2, #0
	mov x3, #8
	svc #0
	adrp x9, sigflag
	add x9, x9, :lo12:sigflag
	ldr w10, [x9]
	cmp w10, #10
	b.ne .Lsigmask_fail
	EMIT sigmaskok, sigmaskok_len
	b .Ltgkill

.Lsigmask_fail:
	EMIT sigmaskfail, sigmaskfail_len
	b .Lexit

.Ltgkill:
	/* tgkill(pid, tid, SIGUSR1) — nr 131. */
	adrp x9, sigflag
	add x9, x9, :lo12:sigflag
	str wzr, [x9]
	mov x8, #172
	svc #0
	mov x19, x0
	mov x8, #178
	svc #0
	mov x20, x0
	mov x8, #131
	mov x0, x19
	mov x1, x20
	mov x2, #10
	svc #0
	cbnz x0, .Ltgkill_fail
	adrp x9, sigflag
	add x9, x9, :lo12:sigflag
	ldr w10, [x9]
	cmp w10, #10
	b.ne .Ltgkill_fail
	EMIT tgkillok, tgkillok_len
	b .Lalarm

.Ltgkill_fail:
	EMIT tgkillfail, tgkillfail_len
	b .Lexit

.Lalarm:
	/* asm-generic has no alarm(2). getitimer(ITIMER_REAL=0, &buf) — nr 102 —
	 * tests the same property: the call is mapped to a native handler rather
	 * than returning -ENOSYS. It needs a real struct itimerval (4 words), so
	 * borrow dbuf. */
	mov x8, #102
	mov x0, #0
	adrp x1, dbuf
	add x1, x1, :lo12:dbuf
	svc #0
	cbnz x0, .Lalarm_fail
	EMIT alarmok, alarmok_len
	b .Lsiginfo

.Lalarm_fail:
	EMIT alarmfail, alarmfail_len
	b .Lexit

.Lsiginfo:
	/* SA_SIGINFO: install a 3-arg handler for SIGUSR2 (Linux 12) and confirm it
	 * read si_signo == 12 out of the kernel-built siginfo_t. */
	adrp x9, siginfo_flag
	add x9, x9, :lo12:siginfo_flag
	str wzr, [x9]
	mov x8, #134
	mov x0, #12
	adrp x1, sigact_si
	add x1, x1, :lo12:sigact_si
	mov x2, #0
	mov x3, #8
	svc #0
	cbnz x0, .Lsiginfo_fail
	mov x8, #172
	svc #0
	mov x19, x0
	mov x8, #129
	mov x0, x19
	mov x1, #12
	svc #0
	cbnz x0, .Lsiginfo_fail
	adrp x9, siginfo_flag
	add x9, x9, :lo12:siginfo_flag
	ldr w10, [x9]
	cmp w10, #12
	b.ne .Lsiginfo_fail
	EMIT siginfook, siginfook_len
	b .Lexit

.Lsiginfo_fail:
	EMIT siginfofail, siginfofail_len

.Lexit:
	/* exit_group(0) — nr 94 */
	mov x8, #94
	mov x0, #0
	svc #0
1:	b 1b

	/* SIGUSR1 handler (1-arg): record the signo the kernel passed. */
sigusr1_handler:
	adrp x9, sigflag
	add x9, x9, :lo12:sigflag
	str w0, [x9]
	ret

	/* SIGUSR2 handler (SA_SIGINFO): si_signo is the first word of siginfo_t,
	 * passed in x1. */
siginfo_handler:
	ldr w9, [x1]
	adrp x10, siginfo_flag
	add x10, x10, :lo12:siginfo_flag
	str w9, [x10]
	ret
