.section .text
.global _start
.type _start, @function

_start:
#ifdef __x86_64__
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
