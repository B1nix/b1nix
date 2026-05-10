.section .text
.global _start
.type _start, @function

_start:
	movq $1, %rax            /* SYS_WRITE */
	movq $1, %rdi            /* fd = 1 (stdout) */
	leaq message(%rip), %rsi /* buf address */
	movq $message_len, %rdx  /* byte count */
	syscall

	movq $2, %rax            /* SYS_EXIT (B1NIX uses 2, Linux uses 60) */
	xorq %rdi, %rdi          /* status = 0 */
	syscall

1:
	pause
	jmp 1b

.section .rodata
message:
	.ascii "NATIVE-SMOKE: ok\n"
.set message_len, . - message
