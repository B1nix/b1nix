.section .text
.code32

.extern x86_exception_handler
.extern x86_irq_handler

.macro ISR_NOERR vector
.global isr\vector
.type isr\vector, @function
isr\vector:
	pushl $0
	pushl $\vector
	jmp isr_common
.endm

.macro ISR_ERR vector
.global isr\vector
.type isr\vector, @function
isr\vector:
	pushl $\vector
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

.macro IRQ vector
.global isr\vector
.type isr\vector, @function
isr\vector:
	pushl $0
	pushl $\vector
	jmp irq_common
.endm

IRQ 32
IRQ 33
IRQ 34
IRQ 35
IRQ 36
IRQ 37
IRQ 38
IRQ 39
IRQ 40
IRQ 41
IRQ 42
IRQ 43
IRQ 44
IRQ 45
IRQ 46
IRQ 47

/* LAPIC timer */
IRQ 64
/* TLB shootdown IPI */
IRQ 65
/* Reschedule IPI */
IRQ 66
/* LAPIC spurious vector */
IRQ 255

.global isr_common
.type isr_common, @function
isr_common:
	cld
	pushl %esi
	pushl %edi
	pushl %ebp
	pushl %edx
	pushl %ecx
	pushl %ebx
	pushl %eax

	/* Reload segment registers for kernel mode */
	movw $0x10, %ax
	movw %ax, %ds
	movw %ax, %es
	movw %ax, %gs
	movw $0x38, %ax
	movw %ax, %fs

	/* Align stack to 16 bytes for System V ABI:
	 * Current ESP is at S. S - 4 (argument) - 12 (padding) = 16-aligned.
	 * S is 8-aligned? Let's check:
	 * S has interrupt_frame (56 bytes) on it.
	 * Interrupt frame contains:
	 * SS, ESP, EFLAGS, CS, EIP (20 bytes from CPU)
	 * error_code, vector (8 bytes)
	 * ESI, EDI, EBP, EDX, ECX, EBX, EAX (28 bytes)
	 * Total = 56 bytes.
	 * If TSS stack was 16-aligned, S is 16-aligned (multiple of 16).
	 * If we push argument (4 bytes), ESP is 12 mod 16.
	 * So we need 12 bytes of padding!
	 */
	subl $12, %esp
	leal 12(%esp), %eax  /* EAX points to interrupt_frame */
	pushl %eax           /* Push argument */

	call x86_exception_handler
	addl $16, %esp       /* Clean up argument and padding */

	popl %eax
	popl %ebx
	popl %ecx
	popl %edx
	popl %ebp
	popl %edi
	popl %esi
	addl $8, %esp       /* Clean up vector and error_code */

	/* Check if returning to user mode (RPL of CS is 3) */
	testl $3, 4(%esp)
	jz 1f
	/* Restore user segment registers */
	movw $0x23, %ax
	movw %ax, %ds
	movw %ax, %es
	movw %ax, %fs
	movw $0x33, %ax
	movw %ax, %gs
1:
	/* Return to user mode */
	iret

.global irq_common
.type irq_common, @function
irq_common:
	cld
	pushl %esi
	pushl %edi
	pushl %ebp
	pushl %edx
	pushl %ecx
	pushl %ebx
	pushl %eax

	/* Reload segment registers for kernel mode */
	movw $0x10, %ax
	movw %ax, %ds
	movw %ax, %es
	movw %ax, %gs
	movw $0x38, %ax
	movw %ax, %fs

	subl $12, %esp
	leal 12(%esp), %eax
	pushl %eax

	call x86_irq_handler
	addl $16, %esp

	popl %eax
	popl %ebx
	popl %ecx
	popl %edx
	popl %ebp
	popl %edi
	popl %esi
	addl $8, %esp

	/* Check if returning to user mode (RPL of CS is 3) */
	testl $3, 4(%esp)
	jz 1f
	/* Restore user segment registers */
	movw $0x23, %ax
	movw %ax, %ds
	movw %ax, %es
	movw %ax, %fs
	movw $0x33, %ax
	movw %ax, %gs
1:
	/* Return to user mode */
	iret
