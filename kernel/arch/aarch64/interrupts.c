#include <b1nix/types.h>
#include <b1nix/fb_console.h>
#include <b1nix/console.h>
#include <b1nix/panic.h>
#include <b1nix/sched.h>
#include <b1nix/irq.h>
#include <b1nix/spinlock.h>
#include <b1nix/mm.h>
#include <b1nix/arch.h>
#include <b1nix/ptrace.h>
#include <b1nix/watchdog.h>
#include <b1nix/bootinfo.h>
#include <b1nix/gdbstub.h>
#include <b1nix/rseq.h>
#include <b1nix/serial_tty.h>
#include <b1nix/gicv3.h>
#include <b1nix/net.h>
#include "platform.h"

/* GICv2, wherever this board puts it. Initialized via platform_gicd_base() /
 * platform_gicc_base() and updated from device tree if needed. */
static u64 g_gicd = 0;
static u64 g_gicc = 0;
#define GICD_BASE g_gicd
#define GICC_BASE g_gicc

#define GICD_CTLR        (*(volatile u32 *)(GICD_BASE + 0x000))
#define GICD_TYPER       (*(volatile u32 *)(GICD_BASE + 0x004))
#define GICD_IGROUPR(n)  (*(volatile u32 *)(GICD_BASE + 0x080 + (n) * 4))
#define GICD_ISENABLER(n) (*(volatile u32 *)(GICD_BASE + 0x100 + (n) * 4))
#define GICD_ICENABLER(n) (*(volatile u32 *)(GICD_BASE + 0x180 + (n) * 4))
#define GICD_IPRIORITYR(n) (*(volatile u32 *)(GICD_BASE + 0x400 + (n) * 4))
#define GICD_ITARGETSR(n) (*(volatile u32 *)(GICD_BASE + 0x800 + (n) * 4))
#define GICD_ICFGR(n)    (*(volatile u32 *)(GICD_BASE + 0xc00 + (n) * 4))

#define GICC_CTLR        (*(volatile u32 *)(GICC_BASE + 0x000))
#define GICC_PMR         (*(volatile u32 *)(GICC_BASE + 0x004))
#define GICC_IAR         (*(volatile u32 *)(GICC_BASE + 0x00c))
#define GICC_EOIR        (*(volatile u32 *)(GICC_BASE + 0x010))

#define TIMER_IRQ 27

extern void vector_table_el1(void);


/* ── Generic device-IRQ dispatch (mirrors kernel/arch/x86_64/interrupts.c's
 * M70 table) ── device drivers register a completion handler against a GIC
 * interrupt ID instead of the dispatcher hard-coding one. QEMU virt's
 * virtio-mmio transports sit at SPI 16..47, i.e. GIC INTID 48..79
 * (kernel/dev/virtio_blk_mmio.c) — size the table generously above that. */
#define IRQ_LINES 128
#define IRQ_SHARERS 4
struct irq_action {
  irq_handler_fn fn;
  void *ctx;
};
static struct irq_action g_irq_actions[IRQ_LINES][IRQ_SHARERS];
static spinlock_t g_irq_lock = SPINLOCK_INIT;

int irq_register_handler(u8 irq, irq_handler_fn fn, void *ctx) {
  if (irq >= IRQ_LINES || fn == 0)
    return -1;
  u64 flags;
  spin_lock_irqsave(&g_irq_lock, &flags);
  for (int i = 0; i < IRQ_SHARERS; i++) {
    if (g_irq_actions[irq][i].fn == 0) {
      g_irq_actions[irq][i].ctx = ctx;
      __atomic_store_n(&g_irq_actions[irq][i].fn, fn, __ATOMIC_RELEASE);
      spin_unlock_irqrestore(&g_irq_lock, flags);
      return 0;
    }
  }
  spin_unlock_irqrestore(&g_irq_lock, flags);
  return -1;
}

int irq_unregister_handler(u8 irq, irq_handler_fn fn, void *ctx) {
  if (irq >= IRQ_LINES || fn == 0)
    return -1;
  u64 flags;
  spin_lock_irqsave(&g_irq_lock, &flags);
  for (int i = 0; i < IRQ_SHARERS; i++) {
    if (g_irq_actions[irq][i].fn == fn && g_irq_actions[irq][i].ctx == ctx) {
      __atomic_store_n(&g_irq_actions[irq][i].fn, (irq_handler_fn)0,
                       __ATOMIC_RELEASE);
      g_irq_actions[irq][i].ctx = 0;
      spin_unlock_irqrestore(&g_irq_lock, flags);
      return 0;
    }
  }
  spin_unlock_irqrestore(&g_irq_lock, flags);
  return -1;
}

int irq_dispatch(int irq) {
  if (irq < 0 || irq >= IRQ_LINES)
    return 0;
  int handled = 0;
  for (int i = 0; i < IRQ_SHARERS; i++) {
    irq_handler_fn fn =
        __atomic_load_n(&g_irq_actions[irq][i].fn, __ATOMIC_ACQUIRE);
    if (fn)
      handled |= fn(g_irq_actions[irq][i].ctx);
  }
  return handled;
}

/* GICD_ISENABLER/IPRIORITYR/ITARGETSR are already programmed for every SPI
 * line at boot (gic_init() below loops over all of them) — unmasking a
 * specific line for a driver only needs the enable bit set. */
void irq_unmask(u8 irq) {
  /* On a GICv3 the enable bit for an SGI or a PPI lives in this CPU's
   * redistributor, not in the distributor — a write here would be dropped, and
   * the timer would simply never fire. */
  if (gicv3_present()) {
    gicv3_enable_irq(irq);
    return;
  }
  GICD_ISENABLER(irq / 32) = 1u << (irq % 32);
}

void gic_cpu_init(void);

static void gic_init(void)
{
	/* A GICv3 board takes an entirely different path: its CPU interface is a
	 * set of system registers rather than a memory-mapped block, and it is the
	 * only kind that can carry an ITS — which is what message-signalled
	 * interrupts need. gicv3_init() reports -1 on a v2 machine and nothing
	 * below changes. */
	if (gicv3_init() == 0)
		return;

	g_gicd = platform_gicd_base();
	g_gicc = platform_gicc_base();
	if (fdt_gicd_base())
		g_gicd = fdt_gicd_base();
	if (fdt_gicc_base())
		g_gicc = fdt_gicc_base();

	GICD_CTLR = 0;

	u32 typer = GICD_TYPER;
	u32 lines = (typer & 0x1f) + 1;

	for (u32 i = 0; i < lines; i++) {
		GICD_ICENABLER(i) = 0xffffffff;
	}

	for (u32 i = 8; i < lines * 8; i++) {
		GICD_ITARGETSR(i) = 0x01010101;
	}

	for (u32 i = 0; i < lines * 8; i++) {
		GICD_IPRIORITYR(i) = 0xa0a0a0a0;
	}

	GICD_CTLR = 1;

	gic_cpu_init();
}

/* The CPU interface and the priority mask are banked per CPU: a secondary that
 * has not programmed them takes no interrupt at all, however the distributor is
 * configured. Called once per CPU — by gic_init() on the boot processor, and by
 * aarch64_ap_main() on each of the others. */
void gic_cpu_init(void)
{
	if (gicv3_present()) {
		gicv3_cpu_init();
		return;
	}
	GICC_PMR = 0xf0;
	GICC_CTLR = 1;
}

static void timer_init(void)
{
	irq_unmask(TIMER_IRQ);

	u64 freq;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));

	u64 interval = freq / 100;
	__asm__ volatile("msr cntv_tval_el0, %0" : : "r"(interval));

	__asm__ volatile("msr cntv_ctl_el0, %0" : : "r"(1ULL));
}

/* The virtual timer and its interrupt are per CPU: CNTV_* are banked, and a
 * PPI's enable bit has one copy per CPU interface, so this write lands on the
 * calling CPU alone. A secondary that skips this runs without a scheduler
 * tick — fine while it only steals kernel workers, not once it runs a process
 * that has to be preempted. */
void timer_init_cpu(void)
{
	u64 freq;

	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
	if (!freq)
		return;
	irq_unmask(TIMER_IRQ);
	__asm__ volatile("msr cntv_tval_el0, %0" : : "r"(freq / 100));
	__asm__ volatile("msr cntv_ctl_el0, %0" : : "r"(1ULL));
}

static void gic_eoi(u32 iar)
{
	if (gicv3_present())
		gicv3_eoi(iar);
	else
		GICC_EOIR = iar;
}

/* Affinity-0 of the CPU that booted, so the tick can tell the housekeeping
 * CPU from the others without a lookup. */
u8 g_boot_aff0;

void interrupts_init(void)
{
	{
		u64 v;
		__asm__ volatile("mrs %0, mpidr_el1" : "=r"(v));
		g_boot_aff0 = (u8)(v & 0xff);
	}

	/* sync_el1 runs on its own stack so a corrupt SP_EL1 cannot make the entry
	 * frame fault and lose the first exception's ESR/ELR/FAR. One such stack
	 * per CPU; this installs the boot processor's block. */
	aarch64_pcpu_asm_init(0);

	__asm__ volatile("msr vbar_el1, %0" : : "r"((u64)vector_table_el1));
	
	gic_init();
	/* The ITS only exists on a GICv3 machine, and only matters to devices that
	 * can raise a message interrupt — a board without one keeps using its
	 * INTx lines and never notices. */
	its_init();
	timer_init();

	__asm__ volatile("msr daifclr, #2");
	console_write("aarch64: interrupts enabled\n");
}

/* A pending signal is delivered on the way back to EL0 from here, exactly as
 * x86_64 does from its timer vector (kernel/arch/x86_64/interrupts.c). Without
 * it, the only place this arch ever looked at pending signals was the syscall
 * and fault paths — so a task in a pure compute loop could not be signalled at
 * all. That is not theoretical: m32_smoke's TLS section SIGTERMs its
 * mbedTLS server and then waits for it, and a server still inside its (long,
 * syscall-free) key setup never saw the signal, so the waitpid never returned
 * and took every check after it in the lane down with it. */
static void irq_return_to_user(struct interrupt_frame *frame)
{
	if (!frame || (frame->spsr & 0xFULL) != 0)
		return; /* interrupted EL1 — no user frame to deliver through */
	/* rseq(2): the tick may have preempted the task, which is exactly when a
	 * critical section has to be restarted at its abort handler rather than
	 * resumed in the middle. x86_64 does this from its timer vector; nothing
	 * called it here at all, so an rseq section on this arch always ran to
	 * completion and M40's rseq-abort check never saw a restart. */
	rseq_on_return_to_user(frame);
	arch_check_and_deliver_signals(frame);
}

void aarch64_irq_handler(struct interrupt_frame *frame)
{
	u32 iar = gicv3_present() ? gicv3_ack() : GICC_IAR;
	/* v3 acknowledges a 24-bit INTID; v2's is 10 bits and the rest of the
	 * word is the sending CPU for an SGI. */
	u32 irq = gicv3_present() ? (iar & 0xffffffu) : (iar & 0x3ff);

	/* 1023 is the "no interrupt pending" answer on both versions; there is
	 * nothing to end. */
	if (irq == 1023) {
		return;
	}

	if (irq == TIMER_IRQ) {
		u64 freq;
		__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
		u64 interval = freq / 100;
		__asm__ volatile("msr cntv_tval_el0, %0" : : "r"(interval));

		/* EOI BEFORE scheduler_on_timer_tick, mirroring the x86_64 LAPIC
		 * fix (M28 #8, kernel/arch/x86_64/interrupts.c). scheduler_on_timer_tick
		 * may context-switch away via scheduler_yield; the task that resumes
		 * this call frame does so long after the tick, so an EOIR placed
		 * after it leaves this interrupt marked active in the GIC the whole
		 * time. Per GICv2, an active PPI is not re-forwarded to the CPU
		 * interface until it is deactivated — so the next timer expiry can
		 * assert cntv_ctl.ISTATUS but the GIC withholds delivery, and the
		 * CPU's next wfi never wakes. EOI first so the GIC unblocks
		 * immediately, then run the (preemptible) tick work. */
		gic_eoi(iar);

		/* Only the boot CPU runs the housekeeping half of the tick: the
		 * watchdog counts wall time once, the serial drain owns a device, and
		 * scheduler_on_timer_tick advances the machine's tick counter — run it
		 * on every CPU and wall time passes N times too fast, which is what
		 * shortened every sleep and doubled every rusage figure the first time
		 * a secondary armed its own timer. x86_64 draws the same line
		 * (kernel/arch/x86_64/interrupts.c, vector 64).
		 *
		 * A secondary still takes the interrupt, and still does the
		 * return-to-user work below — rseq restart and signal delivery — which
		 * is the half that has to happen on the CPU the task runs on. */
		{
			u64 aff0;
			__asm__ volatile("mrs %0, mpidr_el1" : "=r"(aff0));
			if ((aff0 & 0xff) == g_boot_aff0) {
				watchdog_tick();
				serial_tty_tick();
				scheduler_on_timer_tick();
			}
		}
		irq_return_to_user(frame);
		return;
	} else if (its_vector_for_lpi(irq) >= 0) {
		/* An LPI: the ITS translated some device's write into it, and the
		 * vector it belongs to has exactly one owner. */
		msi_dispatch(its_vector_for_lpi(irq));
		gic_eoi(iar);
		irq_return_to_user(frame);
		return;
	} else {
		/* NICs first, then the registered block/DMA handlers — the same order
		 * the x86_64 vector uses, and for the same reason: a shared,
		 * level-triggered INTx line stays asserted until every interface on it
		 * has had its cause register read. */
		int handled = net_handle_irq((int)irq);

		handled |= irq_dispatch((int)irq);
		/* A line no driver claimed is an ordinary shared-INTx artifact, not a
		 * fault; only a line nothing is registered on at all is worth a word. */
		(void)handled;
	}

	gic_eoi(iar);
	irq_return_to_user(frame);
}

/* ESR_EL1.EC values this handler cares about (ARM ARM D17.2.37). */
#define EC_INSN_ABORT_LOWER 0x20
#define EC_INSN_ABORT_SAME  0x21
#define EC_DATA_ABORT_LOWER 0x24
#define EC_DATA_ABORT_SAME  0x25
#define EC_BRK              0x3c

void aarch64_sync_handler(u64 esr, u64 elr, u64 far, u64 *saved_regs)
{
	struct interrupt_frame *frame = (struct interrupt_frame *)saved_regs;
	u32 ec = (u32)(esr >> 26) & 0x3f;
	/* Which EL the exception came FROM is SPSR_EL1.M[3:0] (0 = EL0t), not the
	 * exception class: an unknown-reason or illegal-state exception taken from
	 * userspace has no "lower EL" EC of its own, and classifying those as
	 * kernel faults panicked the machine over a bad user instruction. */
	int from_el0 = (frame->spsr & 0xFULL) == 0;
	int is_abort = (ec == EC_INSN_ABORT_LOWER || ec == EC_DATA_ABORT_LOWER ||
	                ec == EC_INSN_ABORT_SAME || ec == EC_DATA_ABORT_SAME);

	/* M36: BRK is this arch's int3 — route it to the GDB serial stub when the
	 * kernel was booted with b1nix.gdb. Off by default, so an ordinary or test
	 * boot never blocks waiting on a host debugger. */
	if (ec == EC_BRK && bootinfo_has_flag("b1nix.gdb")) {
		gdb_stub_enter(frame);
		/* BRK does not advance PC, so resuming without stepping over it
		 * would re-enter the stub forever. */
		frame->elr += 4;
		return;
	}

	if (is_abort) {
		/* Translate ESR into the x86-shaped error_code the shared VM code
		 * speaks: bit0 = page present (fault was a permission/access fault,
		 * not a missing translation), bit1 = write, bit2 = from userspace. */
		u32 dfsc = (u32)esr & 0x3f;
		int translation_fault = (dfsc & 0x3c) == 0x04; /* 0b0001xx */
		u64 error_code = (translation_fault ? 0 : 1) |
		                 ((esr & (1ULL << 6)) ? 2 : 0) |
		                 (from_el0 ? 4 : 0);
		if (vmm_handle_page_fault(far, error_code) == 0) {
			if (from_el0)
				arch_check_and_deliver_signals(frame);
			return;
		}
	}

	/* A userspace fault kills the faulting task; only a kernel-mode fault is
	 * fatal to the machine. Before this, every stray user pointer took the
	 * whole kernel down. */
	if (from_el0) {
		int sig = SIGTERM;
		if (ec == EC_INSN_ABORT_LOWER || ec == EC_DATA_ABORT_LOWER)
			sig = SIGSEGV;
		else if (ec == 0x00 /* EC_UNKNOWN */ || ec == 0x26 /* SP alignment */ || ec == 0x22 /* PC alignment */)
			sig = SIGILL;
		else
			sig = SIGILL;

		if (sig == SIGSEGV) {
			u32 dfsc = (u32)esr & 0x3f;
			int translation_fault = (dfsc & 0x3c) == 0x04;
			ptrace_record_fault(current_task, sig, far,
			                    translation_fault ? B1NIX_SEGV_MAPERR : B1NIX_SEGV_ACCERR);
		} else {
			ptrace_record_fault(current_task, sig, elr, B1NIX_SI_KERNEL);
		}

		usize pid = scheduler_get_pid();
		struct sigaction *sa = &current_task->sigactions[sig - 1];
		int is_blocked = (current_task->blocked_signals >> (sig - 1)) & 1ULL;
		int has_handler = (sa->sa_handler != SIG_DFL && sa->sa_handler != SIG_IGN);

		if (!has_handler && ptrace_is_traced(current_task)) {
			scheduler_kill(pid, sig);
			arch_check_and_deliver_signals(frame);
			scheduler_yield();
			return;
		}

		if (has_handler && !is_blocked) {
			scheduler_kill(pid, sig);
			arch_check_and_deliver_signals(frame);
			scheduler_yield();
			return;
		}

		console_write("[FATAL] task '");
		console_write(current_task && current_task->name ? current_task->name : "?");
		console_write("' (pid ");
		console_write_dec((u32)pid);
		console_write("): unhandled fault at elr=0x");
		console_write_hex64(elr);
		console_write(" far=0x");
		console_write_hex64(far);
		console_write(" esr=0x");
		console_write_hex64(esr);
		console_write(" lr=0x");
		console_write_hex64(saved_regs[30]);
		console_write(" sp=0x");
		console_write_hex64(frame->sp_el0);
		console_write(" — terminating\n");
		/* M35: the address space is still live here, so dump an ELF core
		 * before the task is torn down (same point x86_64 does it). */
		{
			extern void coredump_write(struct interrupt_frame *frame, int sig);
			coredump_write(frame, sig);
			console_write("coredump: wrote /tmp/core\n");
		}
		scheduler_exit_current(TASK_EXIT_SIGNALED | sig);
		arch_halt();
	}

	console_write("\ninterrupted SP_EL1: 0x");
	console_write_hex64(aarch64_el1_fault_sp());
	console_write(" task='");
	console_write(current_task && current_task->name ? current_task->name : "?");
	console_write("' stack=0x");
	console_write_hex64(current_task ? (u64)(usize)current_task->stack : 0);
	console_write(" ksp=0x");
	console_write_hex64(current_task ? current_task->kernel_stack_ptr : 0);
	console_write("\n");

	/* An EL1 abort on a kernel global means the translation for kernel VAs
	 * went missing under this task, so name the address space and the walk
	 * result rather than leaving a bare "translation fault, FAR=0". */
	{

		u64 ttbr0 = 0, ttbr1 = 0, spsr = 0, sctlr = 0;
		__asm__ volatile("mrs %0, ttbr0_el1" : "=r"(ttbr0));
		__asm__ volatile("mrs %0, ttbr1_el1" : "=r"(ttbr1));
		__asm__ volatile("mrs %0, spsr_el1" : "=r"(spsr));
		__asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
		console_write("EL1 state: ttbr0=0x");
		console_write_hex64(ttbr0);
		console_write(" ttbr1=0x");
		console_write_hex64(ttbr1);
		console_write(" spsr=0x");
		console_write_hex64(spsr);
		console_write(" sctlr=0x");
		console_write_hex64(sctlr);
		console_write(" task_root=0x");
		console_write_hex64(current_task ? current_task->pml4_phys : 0);
		console_write("\n  walk(elr)=0x");
		console_write_hex64(vmm_virt_to_phys((void *)(usize)(elr & ~0xfffULL)));
		console_write(" walk(kstack_top_var)=0x");
		console_write_hex64(aarch64_kstack_top());
		console_write(" kstack_top=0x");
		console_write_hex64(aarch64_kstack_top());
		console_write("\n  leaf(far)=0x");
		console_write_hex64(paging_leaf_pte(far));
		console_write("\n");
	}

	console_write("\nException!\nESR: ");
	console_write_hex64(esr);
	console_write("\nELR: ");
	console_write_hex64(elr);
	console_write("\nFAR: ");
	console_write_hex64(far);
	console_write("\n");
	for (int i = 0; i < 30; i += 2) {
		console_write("x");
		console_write_dec((u32)i);
		console_write("=");
		console_write_hex64(saved_regs[i]);
		console_write(" x");
		console_write_dec((u32)(i + 1));
		console_write("=");
		console_write_hex64(saved_regs[i + 1]);
		console_write("\n");
	}
	console_write("lr(x30)=");
	console_write_hex64(saved_regs[30]);
	console_write("\n");
	panic("unhandled synchronous exception");
}
