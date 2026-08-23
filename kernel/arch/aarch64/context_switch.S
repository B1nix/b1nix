.section .text
.global arch_context_switch
.type arch_context_switch, %function

// void arch_context_switch(struct cpu_context *old_context,
//                           struct cpu_context *new_context,
//                           volatile int *released_publish);
// x0 = old_context
// x1 = new_context
// x2 = released_publish (may be NULL) — stored with 1 AFTER the sp swap
//      below, mirroring kernel/arch/x86_64/context_switch.S: a reaper must
//      never observe stack_released==1 while this CPU is still using the
//      old task's kernel stack as sp.

arch_context_switch:
	stp x19, x20, [x0, #0]
	stp x21, x22, [x0, #16]
	stp x23, x24, [x0, #32]
	stp x25, x26, [x0, #48]
	stp x27, x28, [x0, #64]
	stp x29, x30, [x0, #80]
	mov x3, sp
	str x3, [x0, #96]

	// Restore from new_context (x1)
	ldp x19, x20, [x1, #0]
	ldp x21, x22, [x1, #16]
	ldp x23, x24, [x1, #32]
	ldp x25, x26, [x1, #48]
	ldp x27, x28, [x1, #64]
	ldp x29, x30, [x1, #80]
	ldr x3, [x1, #96]
	mov sp, x3

	cbz x2, 1f
	mov w3, #1
	str w3, [x2]
1:
	ret
