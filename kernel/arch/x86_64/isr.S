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

.macro IRQ vector
.global isr\vector
.type isr\vector, @function
isr\vector:
	pushq $0
	pushq $\vector
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

/* M98: MSI / MSI-X vectors 48..63. A message-signalled interrupt is not routed
 * through the IOAPIC and has no legacy line behind it, so it gets its own
 * vector range and its own dispatch table (msi_alloc_vector in interrupts.c)
 * rather than borrowing one of the 16 legacy lines a real device also uses. */
IRQ 48
IRQ 49
IRQ 50
IRQ 51
IRQ 52
IRQ 53
IRQ 54
IRQ 55
IRQ 56
IRQ 57
IRQ 58
IRQ 59
IRQ 60
IRQ 61
IRQ 62
IRQ 63

/* LAPIC timer (per-CPU scheduler tick) — vector 0x40 = 64.
 * Drives scheduler_on_timer_tick on the BSP; on APs it's a per-CPU EOI today
 * (cooperative scheduler — no preemption from ISR) and the mechanism through
 * which we'll later enable preemption once VFS chain walks are rwlock-safe. */
IRQ 64

/* TLB shootdown IPI — vector 0x41 = 65. Targets invalidate the requested
 * vaddr range from their local TLB and decrement the pending counter so the
 * initiator can wait synchronously. See kernel/arch/x86_64/tlb.c (M28 #5). */
IRQ 65

/* Reschedule IPI — vector 0x42 = 66. No-op handler whose only effect is to
 * pull the target out of `sti; hlt` so it re-polls the global runqueue
 * (M28 #6). EOI inside the handler; never takes the BKL. */
IRQ 66

/* LAPIC spurious vector — must match LAPIC_SPURIOUS_VECTOR (0xFF). The CPU
 * delivers this when a pending interrupt is cancelled mid-flight; per Intel
 * SDM the handler must NOT issue an EOI. Without an IDT gate the CPU would
 * triple-fault, so route it to a no-op stub. */
IRQ 255

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
