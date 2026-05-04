.section .text
.global arch_context_switch
.type arch_context_switch, %function

// void arch_context_switch(struct cpu_context *old_context, struct cpu_context *new_context);
// x0 = old_context
// x1 = new_context

arch_context_switch:
	// Save callee-saved registers to old_context (x0)
	// cpu_context structure: rsp, rbp, rbx, r12-r15 for x86
	// For aarch64, we need to adapt struct cpu_context or just save x19-x28, fp, lr, sp
	// Wait, the scheduler uses `struct cpu_context` defined in scheduler.c
	// Let's match the struct in scheduler.c:
	// u64 rsp; u64 rbp; u64 rbx; u64 r12; u64 r13; u64 r14; u64 r15;
	// This is 7x8 = 56 bytes.
	// For AArch64, we need more. We must modify `scheduler.c` to have arch-dependent cpu_context or a big enough array.
	// Actually, let's just save sp, x19-x29, x30.
	// We'll define a larger struct in a bit. For now, let's just write to the pointer assuming it's big enough.
	// The x86 cpu_context is 56 bytes.
	// Let's change struct cpu_context in scheduler.c to use `void *` or be arch-specific.
	// To avoid changing scheduler.c too much, we'll redefine `struct cpu_context` in scheduler.c to use a union or `#ifdef`.
	// For AArch64:
	// struct cpu_context { u64 x19, x20, x21, x22, x23, x24, x25, x26, x27, x28, fp, lr, sp; };
	// That is 13 * 8 = 104 bytes.

	stp x19, x20, [x0, #0]
	stp x21, x22, [x0, #16]
	stp x23, x24, [x0, #32]
	stp x25, x26, [x0, #48]
	stp x27, x28, [x0, #64]
	stp x29, x30, [x0, #80]
	mov x2, sp
	str x2, [x0, #96]

	// Restore from new_context (x1)
	ldp x19, x20, [x1, #0]
	ldp x21, x22, [x1, #16]
	ldp x23, x24, [x1, #32]
	ldp x25, x26, [x1, #48]
	ldp x27, x28, [x1, #64]
	ldp x29, x30, [x1, #80]
	ldr x2, [x1, #96]
	mov sp, x2

	ret
