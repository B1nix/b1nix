/* M40 — a tiny *static Linux x86_64* ELF used to validate b1nix's Linux ABI
 * compatibility layer end to end.
 *
 * It deliberately uses the LINUX x86_64 syscall NUMBERS (write=1, exit_group=
 * 231). write=1 happens to coincide with b1nix's native number, but exit_group
 * (231) has no native b1nix number (native exit is 60). The kernel's Linux
 * personality translation layer maps these to the b1nix handlers; without that
 * layer the exit would mis-dispatch and the binary would never terminate.
 *
 * It embeds a real GNU `.note.ABI-tag` (NT_GNU_ABI_TAG) declaring OS=Linux —
 * the note genuine glibc/Linux static binaries carry and b1nix's own
 * freestanding userspace never emits. The in-kernel ELF loader keys the "linux"
 * personality off this note (or off EI_OSABI==ELFOSABI_LINUX).
 *
 * Assembled and linked by tools/blobs/build-linux-hello.sh into a committed blob
 * (tools/blobs/linux_hello.bin) embedded into the initramfs as
 * /bin/m40-linux-hello. No b1nix toolchain or libc is involved.
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

	/* Writable scratch kept in .data so it is loaded without relying on .bss
	 * zero-fill. ELF is ET_EXEC (fixed VA) so the handler address is a valid
	 * static initialiser for the Linux struct sigaction. */
	.section .data
	.align 8
sigact:
	.quad sigusr1_handler   /* sa_handler */
	.quad 0                 /* sa_flags   */
	.quad 0                 /* sa_restorer (b1nix uses the kernel trampoline) */
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

	.section .text
	.globl _start
_start:
	/* write(1, msg, msg_len)  — Linux x86_64 nr = 1 */
	movq $1, %rax
	movq $1, %rdi
	leaq msg(%rip), %rsi
	movq $msg_len, %rdx
	syscall

	/* fstat(1, &statbuf)  — Linux x86_64 nr = 5. Exercises the kernel's Linux
	 * struct-stat translation: read st_mode at the LINUX offset (24) and confirm
	 * its S_IFMT type bits are set. Without translation, offset 24 aliases
	 * b1nix's st_uid (0 for root) and the type bits would be clear — so this
	 * fails loudly if the layout is not converted. */
	movq $5, %rax
	movq $1, %rdi
	leaq statbuf(%rip), %rsi
	syscall
	testq %rax, %rax
	jnz .Lfstat_fail
	movl statbuf+24(%rip), %eax   /* Linux struct stat: st_mode at offset 24 */
	andl $0xF000, %eax            /* S_IFMT */
	testl %eax, %eax
	jz .Lfstat_fail

	movq $1, %rax
	movq $1, %rdi
	leaq okmsg(%rip), %rsi
	movq $okmsg_len, %rdx
	syscall
	jmp .Luname

.Lfstat_fail:
	movq $1, %rax
	movq $1, %rdi
	leaq failmsg(%rip), %rsi
	movq $failmsg_len, %rdx
	syscall
	jmp .Lexit

.Luname:
	/* uname(&utsbuf)  — Linux x86_64 nr = 63. Validates the struct utsname
	 * translation: machine[] sits at offset 260 in the Linux layout (6×65) but
	 * at 128 in b1nix's (5×32), and b1nix's struct is only 160 bytes — so byte
	 * 260 stays zero unless the kernel widened the result. Check it is set. */
	movq $63, %rax
	leaq utsbuf(%rip), %rdi
	syscall
	testq %rax, %rax
	jnz .Luname_fail
	movzbl utsbuf+260(%rip), %eax  /* Linux struct utsname: machine[0] at 260 */
	testl %eax, %eax
	jz .Luname_fail

	movq $1, %rax
	movq $1, %rdi
	leaq unameok(%rip), %rsi
	movq $unameok_len, %rdx
	syscall
	jmp .Lgetdents

.Luname_fail:
	movq $1, %rax
	movq $1, %rdi
	leaq unamefail(%rip), %rsi
	movq $unamefail_len, %rdx
	syscall
	jmp .Lexit

.Lgetdents:
	/* open("/bin", O_RDONLY) — Linux x86_64 nr = 2 */
	movq $2, %rax
	leaq dirpath(%rip), %rdi
	xorq %rsi, %rsi
	xorq %rdx, %rdx
	syscall
	testq %rax, %rax
	js .Lgd_fail              /* fd < 0 */
	movq %rax, %r12          /* save fd */

	/* getdents64(fd, dbuf, 1024) — Linux x86_64 nr = 217. Validates the
	 * linux_dirent64 repacking: d_reclen (u16 at offset 16) must be a real
	 * record length and d_name (offset 19) must be non-empty. Without
	 * translation the native fixed struct dirent puts its `type` field (1/2/3)
	 * at offset 16, which is < 20 — so this check fails loudly. */
	movq $217, %rax
	movq %r12, %rdi
	leaq dbuf(%rip), %rsi
	movq $1024, %rdx
	syscall
	testq %rax, %rax
	jle .Lgd_fail            /* <= 0: error or empty */
	movzwl dbuf+16(%rip), %ecx   /* d_reclen */
	cmpl $20, %ecx
	jb .Lgd_fail
	movzbl dbuf+19(%rip), %ecx   /* d_name[0] */
	testl %ecx, %ecx
	jz .Lgd_fail

	/* close(fd) — Linux x86_64 nr = 3 */
	movq $3, %rax
	movq %r12, %rdi
	syscall

	movq $1, %rax
	movq $1, %rdi
	leaq gdok(%rip), %rsi
	movq $gdok_len, %rdx
	syscall
	jmp .Lprctl

.Lgd_fail:
	movq $1, %rax
	movq $1, %rdi
	leaq gdfail(%rip), %rsi
	movq $gdfail_len, %rdx
	syscall
	jmp .Lexit

.Lprctl:
	/* arch_prctl(ARCH_SET_FS, &tlsblock) — Linux x86_64 nr = 158. Sets the FS
	 * base; unmapped without the M40 translation (would return -ENOSYS). Then
	 * round-trip a sentinel through %fs:0 to prove the base took effect. */
	movq $158, %rax
	movq $0x1002, %rdi          /* ARCH_SET_FS */
	leaq tlsblock(%rip), %rsi
	syscall
	testq %rax, %rax
	jnz .Lprctl_fail
	movq $0x1122334455667788, %r13
	movq %r13, %fs:0            /* store at fs_base+0 == tlsblock[0] */
	movq %fs:0, %r14
	cmpq %r13, %r14
	jne .Lprctl_fail

	movq $1, %rax
	movq $1, %rdi
	leaq prctlok(%rip), %rsi
	movq $prctlok_len, %rdx
	syscall
	jmp .Lmmap

.Lprctl_fail:
	movq $1, %rax
	movq $1, %rdi
	leaq prctlfail(%rip), %rsi
	movq $prctlfail_len, %rdx
	syscall
	jmp .Lexit

.Lmmap:
	/* mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)
	 * — Linux x86_64 nr = 9. The 4th arg goes in r10 (syscall clobbers rcx).
	 * Linux returns the address, or a value in [-4095,-1] on error. Confirms a
	 * Linux binary can get anonymous memory (flags/prot already match b1nix). */
	movq $9, %rax
	xorq %rdi, %rdi
	movq $4096, %rsi
	movq $3, %rdx              /* PROT_READ|PROT_WRITE */
	movq $0x22, %r10           /* MAP_PRIVATE|MAP_ANONYMOUS */
	movq $-1, %r8
	xorq %r9, %r9
	syscall
	cmpq $-4096, %rax          /* unsigned >= -4096 ⇒ -errno */
	jae .Lmmap_fail
	movq %rax, %r15            /* save mapping */
	movq $0xdeadbeefcafef00d, %rcx
	movq %rcx, (%r15)          /* write then read back (faults the page in) */
	cmpq %rcx, (%r15)
	jne .Lmmap_fail
	/* munmap(addr, 4096) — nr = 11 */
	movq $11, %rax
	movq %r15, %rdi
	movq $4096, %rsi
	syscall
	movq $1, %rax
	movq $1, %rdi
	leaq mmapok(%rip), %rsi
	movq $mmapok_len, %rdx
	syscall
	jmp .Lclock

.Lmmap_fail:
	movq $1, %rax
	movq $1, %rdi
	leaq mmapfail(%rip), %rsi
	movq $mmapfail_len, %rdx
	syscall
	jmp .Lexit

.Lclock:
	/* clock_gettime(CLOCK_MONOTONIC, &tsbuf) — Linux x86_64 nr = 228. timespec
	 * is identical to b1nix's (two 64-bit fields), so the already-mapped call
	 * works; confirm rax==0 and a non-zero time was written. */
	movq $228, %rax
	movq $1, %rdi              /* CLOCK_MONOTONIC */
	leaq tsbuf(%rip), %rsi
	syscall
	testq %rax, %rax
	jnz .Lclock_fail
	movq tsbuf(%rip), %rcx     /* tv_sec */
	orq tsbuf+8(%rip), %rcx    /* | tv_nsec */
	testq %rcx, %rcx
	jz .Lclock_fail
	movq $1, %rax
	movq $1, %rdi
	leaq clockok(%rip), %rsi
	movq $clockok_len, %rdx
	syscall
	jmp .Lgettid

.Lclock_fail:
	movq $1, %rax
	movq $1, %rdi
	leaq clockfail(%rip), %rsi
	movq $clockfail_len, %rdx
	syscall
	jmp .Lexit

.Lgettid:
	/* gettid() — Linux x86_64 nr = 186. Newly mapped to the native SYS_GETTID;
	 * unmapped it would return -ENOSYS. A valid thread id is positive. */
	movq $186, %rax
	syscall
	testq %rax, %rax
	jle .Lgettid_fail
	movq $1, %rax
	movq $1, %rdi
	leaq gettidok(%rip), %rsi
	movq $gettidok_len, %rdx
	syscall
	jmp .Lsignal

.Lgettid_fail:
	movq $1, %rax
	movq $1, %rdi
	leaq gettidfail(%rip), %rsi
	movq $gettidfail_len, %rdx
	syscall
	jmp .Lexit

.Lsignal:
	/* End-to-end Linux signal test. Install a SIGUSR1 (Linux nr 10) handler,
	 * send it to ourselves, and confirm the handler ran AND received the Linux
	 * signo (10). Exercises the signo remap (install + kill), delivery (rdi
	 * remap) and the Linux sigreturn trampoline. b1nix numbers SIGUSR1 as 19, so
	 * none of this works without the remap. */
	/* rt_sigaction(10, &sigact, NULL, 8) — nr = 13 (4th arg in r10) */
	movq $13, %rax
	movq $10, %rdi
	leaq sigact(%rip), %rsi
	xorq %rdx, %rdx
	movq $8, %r10
	syscall
	testq %rax, %rax
	jnz .Lsignal_fail
	/* getpid() — nr = 39 */
	movq $39, %rax
	syscall
	movq %rax, %rdi
	/* kill(pid, 10) — nr = 62. On return the kernel delivers the signal: the
	 * handler runs, then the trampoline's rt_sigreturn resumes us here. */
	movq $62, %rax
	movq $10, %rsi
	syscall
	testq %rax, %rax
	jnz .Lsignal_fail
	movl sigflag(%rip), %eax
	cmpl $10, %eax           /* handler must have seen Linux SIGUSR1 == 10 */
	jne .Lsignal_fail
	movq $1, %rax
	movq $1, %rdi
	leaq signalok(%rip), %rsi
	movq $signalok_len, %rdx
	syscall
	jmp .Lsigmask

.Lsignal_fail:
	movq $1, %rax
	movq $1, %rdi
	leaq signalfail(%rip), %rsi
	movq $signalfail_len, %rdx
	syscall
	jmp .Lexit

.Lsigmask:
	/* rt_sigprocmask bit-remap test. Block Linux SIGUSR1 (bit 9), send it to
	 * ourselves — the handler must NOT run while blocked — then unblock and
	 * confirm it runs. b1nix numbers SIGUSR1 as 19 and swaps SIG_UNBLOCK/
	 * SIG_SETMASK, so this only works with the sigset and `how` remaps. */
	movl $0, sigflag(%rip)
	/* rt_sigprocmask(SIG_BLOCK=0, &blockset, NULL, 8) — nr=14 */
	movq $14, %rax
	xorq %rdi, %rdi
	leaq blockset(%rip), %rsi
	xorq %rdx, %rdx
	movq $8, %r10
	syscall
	testq %rax, %rax
	jnz .Lsigmask_fail
	/* kill(getpid(), SIGUSR1=10) while blocked */
	movq $39, %rax
	syscall
	movq %rax, %rdi
	movq $62, %rax
	movq $10, %rsi
	syscall
	movl sigflag(%rip), %eax
	testl %eax, %eax         /* blocked: handler must NOT have run */
	jnz .Lsigmask_fail
	/* rt_sigprocmask(SIG_UNBLOCK=1, &blockset, NULL, 8) — pending now delivers */
	movq $14, %rax
	movq $1, %rdi
	leaq blockset(%rip), %rsi
	xorq %rdx, %rdx
	movq $8, %r10
	syscall
	movl sigflag(%rip), %eax
	cmpl $10, %eax           /* now the handler ran with Linux signo 10 */
	jne .Lsigmask_fail
	movq $1, %rax
	movq $1, %rdi
	leaq sigmaskok(%rip), %rsi
	movq $sigmaskok_len, %rdx
	syscall
	jmp .Ltgkill

.Lsigmask_fail:
	movq $1, %rax
	movq $1, %rdi
	leaq sigmaskfail(%rip), %rsi
	movq $sigmaskfail_len, %rdx
	syscall
	jmp .Lexit

.Ltgkill:
	/* tgkill(getpid(), gettid(), SIGUSR1=10) — Linux nr 234. Self-signal via the
	 * thread-targeted kill (used by glibc raise/pthread_kill). Reuses the
	 * installed SIGUSR1 handler; confirm it ran with the remapped signo. */
	movl $0, sigflag(%rip)
	movq $39, %rax           /* getpid -> tgid */
	syscall
	movq %rax, %r12
	movq $186, %rax          /* gettid -> tid */
	syscall
	movq %rax, %r13
	movq $234, %rax
	movq %r12, %rdi
	movq %r13, %rsi
	movq $10, %rdx
	syscall
	testq %rax, %rax
	jnz .Ltgkill_fail
	movl sigflag(%rip), %eax
	cmpl $10, %eax
	jne .Ltgkill_fail
	movq $1, %rax
	movq $1, %rdi
	leaq tgkillok(%rip), %rsi
	movq $tgkillok_len, %rdx
	syscall
	jmp .Lalarm

.Ltgkill_fail:
	movq $1, %rax
	movq $1, %rdi
	leaq tgkillfail(%rip), %rsi
	movq $tgkillfail_len, %rdx
	syscall
	jmp .Lexit

.Lalarm:
	/* alarm(0) — Linux nr 37. Cancels any pending timer and returns the seconds
	 * left on the previous one (0 here). Newly mapped to SYS_ALARM; unmapped it
	 * would return -ENOSYS. Representative check for the sync/fchdir/setpriority
	 * batch added alongside it. */
	movq $37, %rax
	xorq %rdi, %rdi
	syscall
	testq %rax, %rax
	jnz .Lalarm_fail
	movq $1, %rax
	movq $1, %rdi
	leaq alarmok(%rip), %rsi
	movq $alarmok_len, %rdx
	syscall
	jmp .Lsiginfo

.Lalarm_fail:
	movq $1, %rax
	movq $1, %rdi
	leaq alarmfail(%rip), %rsi
	movq $alarmfail_len, %rdx
	syscall
	jmp .Lexit

.Lsiginfo:
	/* SA_SIGINFO test: install a 3-arg handler for SIGUSR2 (Linux 12), signal
	 * ourselves, and confirm the handler read si_signo == 12 out of the
	 * kernel-built siginfo_t (proving RSI pointed at a valid Linux siginfo). */
	movl $0, siginfo_flag(%rip)
	/* rt_sigaction(SIGUSR2=12, &sigact_si, NULL, 8) — nr=13, sa_flags=SA_SIGINFO */
	movq $13, %rax
	movq $12, %rdi
	leaq sigact_si(%rip), %rsi
	xorq %rdx, %rdx
	movq $8, %r10
	syscall
	testq %rax, %rax
	jnz .Lsiginfo_fail
	movq $39, %rax           /* getpid */
	syscall
	movq %rax, %rdi
	movq $62, %rax           /* kill(pid, SIGUSR2=12) */
	movq $12, %rsi
	syscall
	testq %rax, %rax
	jnz .Lsiginfo_fail
	movl siginfo_flag(%rip), %eax
	cmpl $12, %eax           /* handler read info->si_signo == Linux SIGUSR2 */
	jne .Lsiginfo_fail
	movq $1, %rax
	movq $1, %rdi
	leaq siginfook(%rip), %rsi
	movq $siginfook_len, %rdx
	syscall
	jmp .Lexit

.Lsiginfo_fail:
	movq $1, %rax
	movq $1, %rdi
	leaq siginfofail(%rip), %rsi
	movq $siginfofail_len, %rdx
	syscall

.Lexit:
	/* exit_group(0)  — Linux x86_64 nr = 231 */
	movq $231, %rax
	xorq %rdi, %rdi
	syscall

	/* Must not fall through; loop just in case. */
1:	jmp 1b

	/* SIGUSR1 handler (1-arg): record the signo, then return via the trampoline. */
sigusr1_handler:
	movl %edi, sigflag(%rip)
	ret

	/* SIGUSR2 handler (3-arg, SA_SIGINFO): read si_signo from the siginfo_t in
	 * RSI to prove the kernel built and passed it. */
siginfo_handler:
	movl (%rsi), %eax       /* siginfo->si_signo (offset 0) */
	movl %eax, siginfo_flag(%rip)
	ret
