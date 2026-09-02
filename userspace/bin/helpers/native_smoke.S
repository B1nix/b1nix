.section .text
.global _start
.type _start, @function

_start:
#ifdef __aarch64__
	/* AArch64: number in x8, args x0..x5, trap with `svc #0`. Same three
	 * steps as the x86_64 path below — write the message, then exit. */
	mov x8, #64              /* asm-generic write */
	mov x0, #1               /* fd = 1 (stdout) */
	adrp x1, message
	add x1, x1, :lo12:message
	mov x2, #message_len
	svc #0

	mov x8, #93              /* asm-generic exit */
	mov x0, #0               /* status = 0 */
	svc #0
#elif defined(__x86_64__)
	movq $1, %rax            /* SYS_WRITE */
	movq $1, %rdi            /* fd = 1 (stdout) */
	leaq message(%rip), %rsi /* buf address */
	movq $message_len, %rdx  /* byte count */
	syscall

	movq $60, %rax            /* SYS_EXIT (60) */
	xorq %rdi, %rdi          /* status = 0 */
	syscall
#else
	movl $1, %eax            /* SYS_WRITE */
	movl $1, %ebx            /* fd = 1 (stdout) */
	movl $message, %ecx      /* buf address */
	movl $message_len, %edx  /* byte count */
	int $0x80

	movl $60, %eax           /* SYS_EXIT */
	xorl %ebx, %ebx          /* status = 0 */
	int $0x80
#endif


.section .rodata
message:
	.ascii "NATIVE-SMOKE: ok\n"
.set message_len, . - message
