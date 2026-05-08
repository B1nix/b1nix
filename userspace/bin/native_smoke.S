.section .text
.global _start
.type _start, @function

_start:
	movq $1, %rdi    /* SYS_WRITE */
	movq $1, %rsi    /* fd = 1 (stdout) */
	leaq message(%rip), %rdx /* buf */
	movq $message_len, %r10  /* count */
	xorq %r8, %r8
	syscall

	movq $2, %rdi
	xorq %rsi, %rsi
	xorq %rdx, %rdx
	xorq %r10, %r10
	xorq %r8, %r8
	syscall

1:
	hlt
	jmp 1b

.section .rodata
message:
	.ascii "NATIVE-SMOKE: ok\n"
.set message_len, . - message

