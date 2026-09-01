#ifndef B1NIX_ARCH_AARCH64_H
#define B1NIX_ARCH_AARCH64_H

#include <b1nix/types.h>

struct interrupt_frame {
	u64 x0, x1, x2, x3, x4, x5, x6, x7;
	u64 x8, x9, x10, x11, x12, x13, x14, x15;
	u64 x16, x17, x18, x19, x20, x21, x22, x23;
	u64 x24, x25, x26, x27, x28, x29, x30;
	u64 esr;
	u64 elr;
	u64 spsr;
	u64 far;
	u64 sp_el0;
	/* TPIDR_EL0 is the aarch64 TLS thread-pointer register — like ELR/SPSR/
	 * SP_EL0, it's a live EL1-banked system register the hardware does NOT
	 * push onto the interrupted task's own stack, so a reschedule between
	 * this task's SAVE_REGS and RESTORE_REGS (another task's syscalls
	 * legitimately changing TPIDR_EL0 for its own TLS) would otherwise
	 * corrupt this task's thread pointer on resume. Save/restore it here for
	 * the same reason sp_el0 is. _pad keeps the frame 16-byte aligned.
	 */
	u64 tpidr_el0;
	u64 _pad;
	/* No rip/rsp/rax/cs/ss "compat" aliases: SAVE_REGS (isr.S) only ever
	 * populates through tpidr_el0, and sizeof(this struct) is relied on to
	 * locate the live frame on a task's kernel stack (scheduler_fork_current)
	 * — trailing fields the assembly never writes would silently desync that
	 * math. Use elr/sp_el0/x0 directly instead of rip/rsp/rax. */
} __attribute__((packed));

/* The per-CPU block the exception vectors read through TPIDR_EL1. */
void aarch64_pcpu_asm_init(u32 cpu);
void aarch64_set_kstack_top(u64 top);
u64 aarch64_kstack_top(void);
u64 aarch64_el1_fault_sp(void);
/* True if `sp` lies in this CPU's EL1 fault stack (see the definition). */
int aarch64_on_el1_fault_stack(u64 sp);

void arch_check_and_deliver_signals(struct interrupt_frame *frame);
u64 sys_sigreturn(struct interrupt_frame *frame);
void arch_backtrace(u64 fp, u64 lr);

#endif
