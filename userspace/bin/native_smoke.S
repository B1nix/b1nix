.section .text
.global _start
.type _start, @function

_start:
	movq $1, %rax            /* SYS_WRITE */
	movq $1, %rdi            /* fd = 1 (stdout) */
	leaq message(%rip), %rsi /* buf address */
	movq $message_len, %rdx  /* byte count */
	syscall

	movq $60, %rax            /* SYS_EXIT (60) */
	xorq %rdi, %rdi          /* status = 0 */
	syscall


.section .rodata
message:
	.ascii "NATIVE-SMOKE: ok\n"
.set message_len, . - message
