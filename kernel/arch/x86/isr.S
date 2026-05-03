.section .text
.code64

.extern x86_exception_handler
.extern x86_irq_handler

.macro ISR_NOERR vector
.global isr\vector
.type isr\vector, @function
isr\vector:
	pushq $0
	pushq $\vector
	jmp isr_common
.endm

.macro ISR_ERR vector
.global isr\vector
.type isr\vector, @function
isr\vector:
	pushq $\vector
	jmp isr_common
.endm

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR 8
ISR_NOERR 9
ISR_ERR 10
ISR_ERR 11
ISR_ERR 12
ISR_ERR 13
ISR_ERR 14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR 17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_ERR 30
ISR_NOERR 31

.global isr32
.type isr32, @function
isr32:
	pushq $0
	pushq $32
	jmp irq_common

.global isr33
.type isr33, @function
isr33:
	pushq $0
	pushq $33
	jmp irq_common

.global isr_common
.type isr_common, @function
isr_common:
	cld
	pushq %r15
	pushq %r14
	pushq %r13
	pushq %r12
	pushq %r11
	pushq %r10
	pushq %r9
	pushq %r8
	pushq %rsi
	pushq %rdi
	pushq %rbp
	pushq %rdx
	pushq %rcx
	pushq %rbx
	pushq %rax

	movq %rsp, %rdi
	call x86_exception_handler

	popq %rax
	popq %rbx
	popq %rcx
	popq %rdx
	popq %rbp
	popq %rdi
	popq %rsi
	popq %r8
	popq %r9
	popq %r10
	popq %r11
	popq %r12
	popq %r13
	popq %r14
	popq %r15
	addq $16, %rsp
	iretq

.global irq_common
.type irq_common, @function
irq_common:
	cld
	pushq %r15
	pushq %r14
	pushq %r13
	pushq %r12
	pushq %r11
	pushq %r10
	pushq %r9
	pushq %r8
	pushq %rsi
	pushq %rdi
	pushq %rbp
	pushq %rdx
	pushq %rcx
	pushq %rbx
	pushq %rax

	movq %rsp, %rdi
	call x86_irq_handler

	popq %rax
	popq %rbx
	popq %rcx
	popq %rdx
	popq %rbp
	popq %rdi
	popq %rsi
	popq %r8
	popq %r9
	popq %r10
	popq %r11
	popq %r12
	popq %r13
	popq %r14
	popq %r15
	addq $16, %rsp
	iretq
