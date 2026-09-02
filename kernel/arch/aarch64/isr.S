.section .text
.global vector_table_el1
.global x86_user_jump
.type x86_user_jump, %function

.align 11

// ELR_EL1/SPSR_EL1 are per-CPU EL1 system registers, not GPRs — unlike
// x86_64 (where the CPU pushes RIP/RFLAGS onto the interrupted task's OWN
// stack at interrupt entry, so they ride along for free across a reschedule),
// AArch64 hardware leaves them live in shared system registers. A syscall or
// timer tick that calls scheduler_yield() and switches to a different task
// runs *that* task's own exceptions/syscalls in between, which legitimately
// overwrite ELR_EL1/SPSR_EL1 for its own purposes. When the original task is
// later resumed and this exception handler eventually reaches `eret`, it
// must not `eret` using whatever some OTHER task last left in ELR_EL1 —
// found via a page mapping loop that hung at a different, non-deterministic
// point on every run (varying by exactly when a timer tick landed). Save and
// restore them on the *stack* — memory that belongs to this task and is
// therefore correctly preserved across any intervening reschedule, same as
// x86_64 gets from hardware automatically.
// Layout matches struct interrupt_frame (kernel/include/b1nix/arch_aarch64.h)
// field-for-field: x0..x30, esr, elr, spsr, far, sp_el0, tpidr_el0, pad — so
// `sp` right after SAVE_REGS can be handed to C as a real
// `struct interrupt_frame *`, not NULL.
// Per-CPU block offsets, reached through TPIDR_EL1 (kernel/arch/aarch64/smp.c
// fills it in, one per CPU). Must match struct aarch64_pcpu_asm, which asserts
// them. Slot 0 is dead weight now -- see the note below -- and stays only so the
// two fault-stack offsets keep the values the struct gives them.
.equ PCPU_KSTACK_TOP, 0
.equ PCPU_FAULT_TOP,  8
.equ PCPU_FAULT_SP,   16
.equ PCPU_FAULT_X1,   24

// SP_EL1 belongs to the task, not to the CPU.
//
// There used to be an EL0_KSTACK_RESET here, applied on every EL0 boundary in
// both directions: it read a kernel stack top out of a per-CPU slot and wrote
// it into SP. That was x86_64's TSS.rsp0 transplanted onto an architecture that
// does not need it -- EL1 has its own SP, it survives an eret to EL0 and the
// next exception back, and arch_context_switch already carries it with the
// task. What the reset actually bought was cover for the two paths that eret
// WITHOUT a matching SAVE_REGS (the first jump to EL0, and the clone child's
// frame return); those now put SP_EL1 where it belongs themselves, from a value
// their caller supplies, so nothing has to be repaired globally afterwards.
//
// The cost of the reset was not theoretical: a stale per-CPU value writes the
// entering task's register frame into ANOTHER task's kernel stack. It also
// borrowed TPIDRRO_EL0 -- architecturally the user-visible read-only thread
// pointer -- as a scratch register between two instructions.

.macro SAVE_REGS
	sub sp, sp, #304
	stp x0, x1, [sp, #16 * 0]
	stp x2, x3, [sp, #16 * 1]
	stp x4, x5, [sp, #16 * 2]
	stp x6, x7, [sp, #16 * 3]
	stp x8, x9, [sp, #16 * 4]
	stp x10, x11, [sp, #16 * 5]
	stp x12, x13, [sp, #16 * 6]
	stp x14, x15, [sp, #16 * 7]
	stp x16, x17, [sp, #16 * 8]
	stp x18, x19, [sp, #16 * 9]
	stp x20, x21, [sp, #16 * 10]
	stp x22, x23, [sp, #16 * 11]
	stp x24, x25, [sp, #16 * 12]
	stp x26, x27, [sp, #16 * 13]
	stp x28, x29, [sp, #16 * 14]
	str x30, [sp, #16 * 15]
	mrs x0, esr_el1
	str x0, [sp, #16 * 15 + 8]
	mrs x0, elr_el1
	mrs x1, spsr_el1
	stp x0, x1, [sp, #16 * 16]
	mrs x0, far_el1
	mrs x1, sp_el0
	stp x0, x1, [sp, #16 * 17]
	mrs x0, tpidr_el0
	str x0, [sp, #16 * 18]
.endm

.macro RESTORE_REGS
	// IRQs off for the whole return sequence. Everything below is per-task
	// CPU state that lives in single registers -- ELR_EL1, SPSR_EL1, SP_EL0,
	// TPIDR_EL0. A timer tick landing anywhere in here reschedules,
	// and the task that runs next overwrites every one of them; when this one
	// resumes it erets with another task's values. TPIDR_EL1 is the worst of
	// them because no frame saves it. The eret restores PSTATE from SPSR_EL1,
	// so the interrupted context gets its own DAIF back regardless.
	msr daifset, #3
	// far_el1 is diagnostic-only (never written back); sp_el0 and tpidr_el0
	// are the interrupted task's own user stack pointer and TLS thread
	// pointer, and — like ELR/SPSR — may have been clobbered by a different
	// task's syscalls/exceptions running in between if this task was
	// rescheduled, so both must be restored too.
	ldr x0, [sp, #16 * 18]
	msr tpidr_el0, x0
	ldp x0, x1, [sp, #16 * 17]
	msr sp_el0, x1
	ldp x0, x1, [sp, #16 * 16]
	msr elr_el1, x0
	msr spsr_el1, x1
	ldp x0, x1, [sp, #16 * 0]
	ldp x2, x3, [sp, #16 * 1]
	ldp x4, x5, [sp, #16 * 2]
	ldp x6, x7, [sp, #16 * 3]
	ldp x8, x9, [sp, #16 * 4]
	ldp x10, x11, [sp, #16 * 5]
	ldp x12, x13, [sp, #16 * 6]
	ldp x14, x15, [sp, #16 * 7]
	ldp x16, x17, [sp, #16 * 8]
	ldp x18, x19, [sp, #16 * 9]
	ldp x20, x21, [sp, #16 * 10]
	ldp x22, x23, [sp, #16 * 11]
	ldp x24, x25, [sp, #16 * 12]
	ldp x26, x27, [sp, #16 * 13]
	ldp x28, x29, [sp, #16 * 14]
	ldr x30, [sp, #16 * 15]
	add sp, sp, #304
.endm

.macro VECT_EMPTY
	.align 7
	b .
.endm

vector_table_el1:
	// Current EL with SP0
	VECT_EMPTY // Sync
	VECT_EMPTY // IRQ
	VECT_EMPTY // FIQ
	VECT_EMPTY // SError

	// Current EL with SPx
	.align 7
	b sync_el1
	.align 7
	b irq_el1
	.align 7
	b fiq_entry
	.align 7
	b serror_entry

	// Lower EL using AArch64
	.align 7
	b sync_el0
	.align 7
	b irq_el0
	.align 7
	b fiq_entry
	.align 7
	b serror_entry

	// Lower EL using AArch32
	VECT_EMPTY
	VECT_EMPTY
	VECT_EMPTY
	VECT_EMPTY

// Synchronous exceptions taken at EL1 run on the stack the interrupted kernel
// code was using -- the same shape as irq_el1 -- because this path legitimately
// RETURNS and legitimately BLOCKS: a kernel access to a user address
// (syscall_copyin/copyout, demand paging, COW) faults here and is serviced
// normally, and aarch64_sync_handler is free to sleep or reschedule while doing
// it.
//
// It used to switch to a per-CPU "fault stack" and park the interrupted SP_EL1
// in a single per-CPU word, so a bad SP could not turn SAVE_REGS itself into a
// nested fault that overwrote ESR/ELR/FAR. That is a real diagnostic win for
// the unrecoverable case and a correctness disaster for the ordinary one: the
// handler can context-switch, so a task would leave with its saved kernel SP
// pointing INTO the shared fault stack, the next task to fault on the same CPU
// would write over those frames, and the single saved-SP word would be
// restored into the wrong task. Observed as a task whose context.sp lay inside
// another task's stack, and as a garbage x29 in scheduler_yield right after
// pick_next_task -- a `str` through a frame pointer that no longer existed.
//
// A genuinely bad SP_EL1 still gets the dedicated stack, chosen by an
// alignment test below, so the dump for the fatal case is unchanged.

sync_el1:
	// Decide, before touching memory, whether the interrupted SP is usable.
	// Every kernel stack pointer is 16-byte aligned; the corrupt ones this is
	// here for are not (an SP of 0x400a68e4 -- a text address -- is what a
	// clobbered context.sp looks like). Cheap, needs no constant, and it is
	// the difference between "servicing an ordinary fault" and "SAVE_REGS
	// faults while building its own frame and the nested exception overwrites
	// the ESR/ELR/FAR that describe the first one".
	msr tpidrro_el0, x0          // free up x0
	mov x0, sp
	tst x0, #15
	b.ne 8f                      // unusable SP: take the dedicated stack

	mrs x0, tpidrro_el0          // x0 restored
	SAVE_REGS
	mrs x0, esr_el1
	mrs x1, elr_el1
	mrs x2, far_el1
	mov x3, sp
	bl aarch64_sync_handler
	RESTORE_REGS
	eret

	// Unusable SP. Switch to this CPU's fault stack purely so the dump is
	// readable; this path does not return -- a kernel fault whose stack
	// pointer is gone is already fatal, and not restoring SP is what keeps
	// the switch off the ordinary, recoverable path.
8:
	mrs x0, tpidr_el1
	cbz x0, 9f                   // no per-CPU block yet: nothing better to do
	// Park the bad SP itself. Once we move off it the frame no longer says
	// what it was, and that value is the whole content of the report. x1 is
	// still the interrupted context's, so it goes and comes back through the
	// per-CPU block rather than being clobbered out of the register dump.
	str x1, [x0, #PCPU_FAULT_X1]
	mov x1, sp
	str x1, [x0, #PCPU_FAULT_SP]
	ldr x1, [x0, #PCPU_FAULT_X1]
	ldr x0, [x0, #PCPU_FAULT_TOP]
	cbz x0, 9f
	mov sp, x0
9:
	mrs x0, tpidrro_el0          // x0 restored
	SAVE_REGS
	mrs x0, esr_el1
	mrs x1, elr_el1
	mrs x2, far_el1
	mov x3, sp
	bl aarch64_sync_handler
	RESTORE_REGS
	eret

irq_el1:
	SAVE_REGS
	mov x0, sp                  // frame: SAVE_REGS just built a real
	                            // struct interrupt_frame here
	bl aarch64_irq_handler
	RESTORE_REGS
	eret

sync_el0:
	SAVE_REGS
	mrs x0, esr_el1
	mrs x1, elr_el1
	mrs x2, far_el1

	// Check Exception Class (EC) in esr_el1 (bits [31:26])
	lsr x3, x0, #26
	cmp x3, #0x15       // 0x15 = SVC instruction in AArch64 state
	b.ne 1f

	// Syscall dispatch:
	// x8 = syscall number
	// x0..x5 = syscall arguments
	mov x7, sp                  // frame: SAVE_REGS just built a real
	                             // struct interrupt_frame at sp — hand it to
	                             // C instead of NULL (needed for signal
	                             // delivery, ptrace, and fork/clone resume).
	                             // None of the loads below touch x7.
	ldr x0, [sp, #16 * 4]      // saved x8 -> number
	ldp x1, x2, [sp, #16 * 0]   // saved x0, x1 -> arg0, arg1
	ldp x3, x4, [sp, #16 * 1]   // saved x2, x3 -> arg2, arg3
	ldp x5, x6, [sp, #16 * 2]   // saved x4, x5 -> arg4, arg5

	// Unmask IRQs for the body of the syscall, the way x86_64's entry does
	// (`sti` in syscall_entry.S). The vectors are entered with them masked,
	// and leaving them masked for the whole call made every syscall
	// non-preemptible — which would merely be slow, except that
	// scheduler_can_block() is defined as interrupts_enabled(). Every caller
	// asking "may I park here?" was told no in ordinary process context and
	// spun instead, with no timer left to end the spin: a read() waiting on a
	// DRM event span the CPU forever with the event's sender unable to run.
	//
	// This is the SVC path only. The fault vectors stay masked on purpose —
	// see docs/aarch64-parity.md on why blocking work must not happen there.
	msr daifclr, #2

	bl syscall_dispatch_impl

	// Masked again before the return sequence touches the frame, matching the
	// state RESTORE_REGS/eret expect (x86_64 does the same with cli).
	msr daifset, #2

	// Do NOT write x0 back into the frame here. syscall_dispatch_impl already
	// publishes the return value (frame->x0 = ret) and then runs the
	// return-to-user work that may legitimately REPLACE it: signal delivery
	// puts the signal number in x0 for the handler, and a restarted syscall
	// must keep its original arg0. Storing the C return value on top of that
	// handed every handler entered from a syscall return the syscall's result
	// instead of its signo — OpenRC reported "caught unknown signal 0" and
	// "caught unknown signal -4" (-EINTR).
	RESTORE_REGS
	eret

1:
	mov x3, sp                  // saved_regs: x3 was clobbered by the EC
	                             // check above, but aarch64_sync_handler's
	                             // 4th arg must be the interrupt frame.
	bl aarch64_sync_handler
	RESTORE_REGS
	eret

irq_el0:
	SAVE_REGS
	mov x0, sp                  // frame: the handler delivers a pending signal
	                            // through it on the way back to EL0
	bl aarch64_irq_handler
	RESTORE_REGS
	eret

// FIQ and SError used to be VECT_EMPTY, i.e. `b .`.
//
// That is not "unused": an SError is how a CPU is told about an asynchronous
// external abort, and an FIQ can be routed to EL1 by firmware. Spinning in
// place on either parks that CPU for ever, holding whatever it held, with
// nothing printed -- indistinguishable from the corruption this port has been
// chasing, and the reason those two entries had to stop being a guess. Report
// which one it was and what the syndrome says, then panic.
// Neither builds a register frame, and that is the point: these can arrive with
// SP_EL1 pointing anywhere (an SError is asynchronous, so it is not related to
// what the CPU was doing), and SAVE_REGS on a bad SP faults while building its
// own frame -- which is what the first attempt did, producing a QEMU-level
// exception storm and a 1.1 GB log with nothing printed. Move to this CPU's
// dedicated fault stack first, the same way sync_el1 does for an unusable SP,
// then call C. Nothing returns from here.
fiq_entry:
	mrs x0, tpidr_el1
	cbz x0, 1f
	ldr x0, [x0, #PCPU_FAULT_TOP]
	cbz x0, 1f
	mov sp, x0
1:
	mov x0, #0
	bl aarch64_async_vector
	b .

serror_entry:
	mrs x0, tpidr_el1
	cbz x0, 2f
	ldr x0, [x0, #PCPU_FAULT_TOP]
	cbz x0, 2f
	mov sp, x0
2:
	mov x0, #1
	bl aarch64_async_vector
	b .

// fork(2) resume: scheduler_fork_current() (kernel/sched/scheduler.c) builds
// a full struct interrupt_frame at the same fixed offset from the child's
// kernel_stack_ptr that SAVE_REGS uses on ordinary syscall entry (copied from
// the parent's own live syscall frame, with x0 forced to 0 — fork's child
// return value), points context.sp at it, and context.lr here. The very
// first thing that runs on this task is this trampoline: it's just the
// RESTORE_REGS+eret tail of sync_el0, dropping straight into the child's
// userspace continuation as if the fork syscall itself returned there.
.global aarch64_fork_child_trampoline
.type aarch64_fork_child_trampoline, %function
aarch64_fork_child_trampoline:
	RESTORE_REGS
	eret

x86_user_jump:
	// x0 = entry, x1 = stack, x2 = argc, x3 = argv, x4 = kernel stack top
	// IRQs off across the ELR/SPSR write + eret — see the note in
	// aarch64_user_thread_jump.
	msr daifset, #2
	msr sp_el0, x1
	msr elr_el1, x0
	// This runs on the task's own kernel stack, part-way down its C frames, and
	// erets from there — so SP_EL1 has to be put back at the top explicitly.
	// The value comes from the caller, which knows whose task this is; it used
	// to be read out of a per-CPU slot, which is the same value only as long as
	// nothing has published a different one since.
	mov sp, x4
	mov x4, #0
	msr spsr_el1, x4

	mov x0, x1
	mov x1, #0
	mov x2, #0
	mov x3, #0
	mov x4, #0
	mov x5, #0
	mov x6, #0
	mov x7, #0
	mov x8, #0
	mov x9, #0
	mov x10, #0
	mov x11, #0
	mov x12, #0
	mov x13, #0
	mov x14, #0
	mov x15, #0
	mov x16, #0
	mov x17, #0
	mov x18, #0
	mov x19, #0
	mov x20, #0
	mov x21, #0
	mov x22, #0
	mov x23, #0
	mov x24, #0
	mov x25, #0
	mov x26, #0
	mov x27, #0
	mov x28, #0
	mov x29, #0
	mov x30, #0
	eret


// aarch64_user_thread_jump(entry, sp, arg, lr) — enter EL0 at `entry` on stack
// `sp` with x0 = `arg`, x30 = `lr` and every other GPR cleared.
//
// Used for clone(2) children (kernel/sched/scheduler.c clone_thread_kentry).
// It differs from x86_user_jump only in what lands in x0: a process start
// wants the stack pointer there (musl's _start reads argc/argv off it), a
// cloned thread wants its argument — and for a musl clone the argument is 0,
// which is exactly the clone return value the child must observe (musl's
// __clone branches on it and picks fn/arg back off the new stack).
.global aarch64_user_thread_jump
.type aarch64_user_thread_jump, %function
aarch64_user_thread_jump:
	// x4 = kernel stack top. Same reason as x86_user_jump: this runs on the
	// task's own kernel stack, part-way down its C frames, and erets from
	// there, so SP_EL1 is put back at the top from a value the caller supplies.
	mov sp, x4
	// Mask IRQs first: ELR_EL1/SPSR_EL1 are shared system registers, so a
	// timer tick landing between the msr's and the eret reschedules, and the
	// task that eventually resumes this code erets with whatever ELR another
	// task left behind (observed as every clone child faulting at PC 0).
	// The eret restores DAIF from SPSR_EL1 = 0, so EL0 runs with IRQs on.
	msr daifset, #2
	msr sp_el0, x1
	msr elr_el1, x0
	mov x4, #0
	msr spsr_el1, x4
	mov x0, x2
	// x30 must survive: musl's vfork resumes the child right after the svc
	// and immediately `ret`s through it.
	mov x1, x3
	mov x3, #0
	mov x2, #0
	mov x3, #0
	mov x4, #0
	mov x5, #0
	mov x6, #0
	mov x7, #0
	mov x8, #0
	mov x9, #0
	mov x10, #0
	mov x11, #0
	mov x12, #0
	mov x13, #0
	mov x14, #0
	mov x15, #0
	mov x16, #0
	mov x17, #0
	mov x18, #0
	mov x19, #0
	mov x20, #0
	mov x21, #0
	mov x22, #0
	mov x23, #0
	mov x24, #0
	mov x25, #0
	mov x26, #0
	mov x27, #0
	mov x28, #0
	mov x29, #0
	// x30 last: musl's vfork resumes the child right after the svc and
	// immediately branches to __syscall_ret, which returns through it.
	mov x30, x1
	mov x1, #0
	eret

// aarch64_eret_frame(struct interrupt_frame *f) — return to EL0 through a
// caller-supplied frame. Used by clone(2) children (clone_thread_kentry):
// unlike a fresh process start, a clone child must inherit the parent's whole
// register file — musl's vfork returns into a function whose callee-saved
// registers (x19..x28) are live, so entering with them cleared crashed
// immediately. `f` must sit on this task's own kernel stack and be 16-byte
// aligned; RESTORE_REGS consumes it in place.
.global aarch64_eret_frame
.type aarch64_eret_frame, %function
aarch64_eret_frame:
	msr daifset, #2
	mov sp, x0
	RESTORE_REGS
	eret

/* Dedicated stack for synchronous exceptions taken at EL1 (see sync_el1), plus
 * the slot holding the SP_EL1 that was interrupted. */
.section .bss
.align 12
/* The EL1 fault stacks live in C now (kernel/arch/aarch64/smp.c), one per CPU,
 * reached through the per-CPU block above. A single shared stack was one of
 * the things that made a second CPU running processes unsafe: an EL1 fault is
 * an ordinary event — any copyin/copyout of an untouched user page takes one —
 * so two CPUs would have built their entry frames on the same memory. */
