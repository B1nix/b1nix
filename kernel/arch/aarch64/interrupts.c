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

/* The virtual timer's INTID, from the board's device tree — see
 * fdt_timer_virt_irq(). The old constant was QEMU virt's PPI 11; a Snapdragon
 * wires its timers to PPIs 1/2/3/0, so the same timer is INTID 19 there and
 * nothing ever arrived on 27. */
u32 fdt_timer_virt_irq(void);
#define TIMER_IRQ (fdt_timer_virt_irq())

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

/* Re-arm the virtual timer for the NEXT tick.
 *
 * CVAL, not TVAL. Writing TVAL sets the deadline to "now + interval", so every
 * tick serviced late loses the lateness for ever and the tick count drifts
 * below real time without bound -- which is exactly what M110's
 * clock-uptime-agree caught on the busiest lane, where /proc/uptime (a tick
 * count) had fallen more than a second behind the monotonic clock the log
 * stamps use. Advancing the previous DEADLINE by one interval keeps the
 * period anchored to the counter, so lateness is absorbed by the next tick
 * instead of accumulating.
 *
 * The catch-up guard matters as much as the accumulation: after a long stall
 * (a host that descheduled the whole vCPU, a debugger) the accumulated
 * deadline can be many intervals in the past, and re-arming to it would
 * deliver that whole backlog back to back with no time to make progress
 * between them. Past a backlog of one interval, resynchronise to the counter
 * and drop the missed ticks -- losing them knowingly rather than servicing a
 * storm. */
/* The virtual timer's interval, derived from the rate the scheduler says it
 * ticks at rather than from a literal.
 *
 * There were three copies of `freq / 100` in this file, and the scheduler
 * separately reports 100 through sched_tick_hz(). Two independent statements of
 * the same constant is how a tick rate goes wrong quietly: everything built on
 * SCHED_TICKS_PER_SEC (sleeps, timeouts, the silence watchdog) is scaled by the
 * scheduler's number, while the hardware is armed with this one, and nothing
 * checks that they agree. Ask once, here. */
static u64 timer_interval_ticks(u64 freq)
{
	u32 hz = sched_tick_hz();

	return hz ? freq / hz : freq / 100;
}

/* `b1nix.dynticks[=<max-idle-ticks>]`: program the next timer interrupt for the
 * next deadline anyone actually has, instead of one fixed period from now.
 *
 * CNTV_CVAL_EL0 is an absolute compare register, so this needs no new hardware
 * support -- it is the same one-shot mode FreeBSD's eventtimer(9) and Linux's
 * clockevent use. What made it safe here is that scheduler_ticks already
 * follows the monotonic clock rather than counting interrupts (see
 * scheduler_on_timer_tick), so a skipped interrupt does not lose time: the next
 * one advances the counter to wherever the clock says we are.
 *
 * The sleep is capped anyway. Some periodic work does not register a deadline
 * with anybody -- the guest watchdog, the serial receive drain -- and the cap
 * bounds how late that gets rather than requiring every such site to be found
 * first. Default 10 ticks (100 ms); `b1nix.dynticks=1` gives back the fixed
 * periodic beat, which is what a bisect wants. */
static u64 dynticks_cap(void)
{
	static u64 cap = ~0ull;

	if (cap == ~0ull) {
		char buf[24];

		/* OFF by default, and the reason is worth keeping.
		 *
		 * It was on for exactly one commit. Two full suites passed with it,
		 * and both were lying: a test-support thread (m47-input-inject) was
		 * polling every 2 ticks and its beat kept the timer programmed often
		 * enough to hide what one-shot costs. The moment that poller was fixed
		 * to wait for an event, six checks failed reproducibly -- all of them
		 * signal latency: a signal to a task asleep in nanosleep, ppoll's
		 * sub-millisecond timeout, waitpid's EINTR.
		 *
		 * That is the real precondition, and it is not "find the pollers": a
		 * task asleep on a deadline must be woken when a signal is POSTED, not
		 * when its own timer happens to fire. Waking &task->pending_signals
		 * from the posting sites was tried and broke eight other checks,
		 * because which waits a signal may interrupt is a policy this kernel
		 * already implements carefully (SIGCHLD must not cut waitpid short).
		 * Until signal posting wakes sleepers through THAT policy, one-shot
		 * ticks are not correct here. */
		cap = 0;
		if (bootinfo_has_flag("b1nix.dynticks"))
			cap = 10;
		if (bootinfo_get_kv("b1nix.dynticks", buf, sizeof(buf)) == 0) {
			u64 v = 0;
			const char *p = buf;

			while (*p >= '0' && *p <= '9')
				v = v * 10 + (u64)(*p++ - '0');
			if (v)
				cap = v;
		}
	}
	return cap;
}

static void timer_rearm(void)
{
	u64 freq, cval, now;

	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
	if (!freq)
		return;

	u64 interval = timer_interval_ticks(freq);
	u64 cap = dynticks_cap();

	__asm__ volatile("mrs %0, cntv_cval_el0" : "=r"(cval));
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(now));

	if (cap) {
		u64 next = sched_next_deadline_tick();
		u64 nowt = scheduler_get_ticks();
		u64 skip = cap;
		int capped = 1;

		if (next > nowt && next - nowt < skip) {
			skip = next - nowt;
			capped = 0;
		}
		if (!skip)
			skip = 1;
		/* Jitter the idle interval, and only the idle one.
		 *
		 * This is what BSD gets from running statclock at a frequency
		 * deliberately not commensurate with hz: if the sampling instant sits
		 * on a fixed grid, periodic work aliases against it and the profile
		 * lies. It already bit us -- a 100 Hz sampler could not see net_task
		 * wake, poll and sleep inside one tick, and two versions of the idle
		 * profiler drew confident conclusions from that blind spot.
		 *
		 * Applied only when nothing is due, so a real deadline is never moved.
		 * xorshift on the counter: cheap, and its exact quality does not
		 * matter -- any irregularity breaks the harmonic. */
		if (capped && skip > 2) {
			static u64 seed = 0x9e3779b97f4a7c15ull;

			seed ^= seed << 13;
			seed ^= seed >> 7;
			seed ^= seed << 17;
			skip -= seed % (skip / 2);
		}
		cval += interval * skip;
		if (cval <= now)
			cval = now + interval * skip;
		__asm__ volatile("msr cntv_cval_el0, %0" : : "r"(cval));
		return;
	}

	cval += interval;
	if (cval <= now)
		cval = now + interval;
	__asm__ volatile("msr cntv_cval_el0, %0" : : "r"(cval));
}

static void timer_init(void)
{
	irq_unmask(TIMER_IRQ);

	u64 freq;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));

	u64 interval = timer_interval_ticks(freq);
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
	__asm__ volatile("msr cntv_tval_el0, %0" : : "r"(timer_interval_ticks(freq)));
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
	/* `frame` is the interrupt frame SAVE_REGS built, so it is a kernel stack
	 * pointer: 16-byte aligned, and above the base of RAM (this port runs
	 * identity-mapped, so stacks are ordinary low addresses). A null check
	 * alone is not
	 * enough -- this has arrived as the integer 0x1A, small enough to be an
	 * interrupt ID and non-zero enough to pass `!frame`, and the read of
	 * frame->spsr at +0x108 then faulted on 0x122. The caller loads it from
	 * [x29,#-0x8], four bytes above where it keeps `irq`, so the shape of the
	 * corruption is a slot written at the wrong offset. Report it here, where
	 * the value and the CPU that produced it are both still in hand. */
	if (frame && ((u64)(usize)frame < 0x40000000ULL ||
	              ((u64)(usize)frame & 15))) {
		static volatile int reported;

		if (!__atomic_exchange_n(&reported, 1, __ATOMIC_ACQ_REL)) {
			struct percpu *pc = get_percpu();

			console_write("IRQ-FRAME-BOGUS: frame=0x");
			console_write_hex64((u64)(usize)frame);
			console_write(" cpu=");
			console_write_dec(pc ? (u64)pc->cpu_id : 99);
			console_write(" task=");
			console_write(current_task && current_task->name
			                  ? current_task->name
			                  : "(none)");
			console_write(" pid=");
			console_write_dec(current_task ? (u64)current_task->id : 0);
			console_write("\n");
		}
		return;
	}
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

/* M86: an exception taken at EL0 closes that task's user-time interval; the
 * handler's own cost is charged to it as system time, exactly as x86_64 does in
 * x86_irq_handler / x86_exception_handler. Without these two boundaries the
 * ONLY accounting stamp on this arch was the syscall path, so a compute-bound
 * thread that makes no syscalls had every one of its intervals closed by the
 * context switch instead -- and sched_acct_on_switch credits an interval it
 * ends to SYSTEM time. A thread that spent 150 ms burning CPU in userspace
 * therefore reported almost no utime at all, which is what failed M86's
 * rusage-self-group and times-process. Both handlers have several early
 * returns, so the boundary lives in a wrapper rather than being repeated. */
static void aarch64_irq_handler_inner(struct interrupt_frame *frame);

/* FIQ (kind 0) and SError (kind 1), which the vector table used to answer with
 * an infinite loop. Both are fatal here -- this kernel routes no FIQ and has no
 * recovery for an asynchronous external abort -- but they must SAY so: a CPU
 * spinning silently in a vector looks exactly like a CPU corrupted by something
 * else, and telling those two apart is most of the work. ESR_EL1 carries the
 * SError syndrome (with ISV/IDS set when the implementation provides one), so
 * print it before stopping. */
void aarch64_async_vector(u64 kind)
{
	u64 esr = 0, elr = 0, far = 0;

	__asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
	__asm__ volatile("mrs %0, elr_el1" : "=r"(elr));
	__asm__ volatile("mrs %0, far_el1" : "=r"(far));

	console_write(kind ? "SERROR: " : "FIQ: ");
	console_write("esr=0x");
	console_write_hex64(esr);
	console_write(" elr=0x");
	console_write_hex64(elr);
	ksym_print(elr);
	console_write(" far=0x");
	console_write_hex64(far);
	{
		u64 spsr = 0;

		__asm__ volatile("mrs %0, spsr_el1" : "=r"(spsr));
		console_write(" spsr=0x");
		console_write_hex64(spsr);
	}
	{
		struct percpu *pc = get_percpu();

		console_write(" cpu=");
		console_write_dec(pc ? (u64)pc->cpu_id : 99);
	}
	console_write(" task=");
	console_write(current_task && current_task->name ? current_task->name
	                                                 : "(none)");
	console_write("/");
	console_write_dec(current_task ? (u64)current_task->id : 0);
	console_write("\n");
	panic(kind ? "aarch64: SError" : "aarch64: unexpected FIQ");
}

void aarch64_irq_handler(struct interrupt_frame *frame)
{
	int from_el0 = (frame->spsr & 0xFULL) == 0;

	if (from_el0)
		sched_acct_enter_kernel();
	aarch64_irq_handler_inner(frame);
	if (from_el0)
		sched_acct_leave_kernel();
}

static void aarch64_irq_handler_inner(struct interrupt_frame *frame)
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
		timer_rearm();

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

		/* Where this tick landed. The user/kernel/idle distribution is always
		 * kept; the kernel-ELR histogram only under b1nix.sysprof. Every core
		 * takes its own CNTV interrupt, so every core contributes — unlike the
		 * housekeeping below, which is deliberately the boot CPU's alone. */
		{
			extern void kprof_tick(u64 pc, int in_user, int in_idle, int cpu);
			struct percpu *pc = get_percpu();
			int in_user = (frame->spsr & 0xFULL) == 0; /* EL0t */
			int in_idle = pc && pc->idle_task &&
			              (struct task *)pc->cur_task ==
			                  (struct task *)pc->idle_task;

			kprof_tick(frame->elr, in_user, in_idle,
			           pc ? (int)pc->cpu_id : 0);
		}

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

static void decode_aarch64_exception(u64 esr, u64 elr, u64 far)
{
	u32 ec = (u32)((esr >> 26) & 0x3f);
	u32 iss = (u32)(esr & 0x1ffffff);
	u32 dfsc = iss & 0x3f;
	int wnr = (iss >> 6) & 1;

	console_write("\n=== AArch64 Exception Decode ===\n");

	const char *ec_name = "Unknown Reason";
	switch (ec) {
	case 0x00: ec_name = "Unknown Reason"; break;
	case 0x01: ec_name = "Trapped WFI/WFE Instruction"; break;
	case 0x0e: ec_name = "Illegal Execution State"; break;
	case 0x15: ec_name = "SVC (Syscall)"; break;
	case 0x20: ec_name = "Instruction Abort (User EL0)"; break;
	case 0x21: ec_name = "Instruction Abort (Kernel EL1)"; break;
	case 0x22: ec_name = "PC Alignment Fault"; break;
	case 0x24: ec_name = "Data Abort (User EL0)"; break;
	case 0x25: ec_name = "Data Abort (Kernel EL1 Crash)"; break;
	case 0x26: ec_name = "SP Alignment Fault"; break;
	case 0x3c: ec_name = "BRK Instruction (Breakpoint)"; break;
	}

	console_write("  Type: ");
	console_write(ec_name);
	console_write(" (EC=0x");
	console_write_hex64(ec);
	console_write(" ESR=0x");
	console_write_hex64(esr);
	console_write(")\n");

	if (ec == 0x20 || ec == 0x21 || ec == 0x24 || ec == 0x25) {
		const char *fault_desc = "Unknown Fault";
		switch (dfsc & 0x3c) {
		case 0x00: fault_desc = "Address Size Fault"; break;
		case 0x04: fault_desc = "Translation Fault (Page Unmapped/Missing)"; break;
		case 0x08: fault_desc = "Access Flag Fault"; break;
		case 0x0c: fault_desc = "Permission Fault (Access Denied / Read-Only)"; break;
		}
		if (dfsc == 0x10) fault_desc = "Synchronous External Abort";
		else if (dfsc == 0x21) fault_desc = "Alignment Fault";

		console_write("  Cause: ");
		console_write(fault_desc);
		console_write(" (DFSC=0x");
		console_write_hex64(dfsc);
		console_write(")");

		if (ec == 0x24 || ec == 0x25) {
			console_write(wnr ? " [Write]" : " [Read]");
		}
		console_write("\n");
	}

	console_write("  Fault Address (FAR): 0x");
	console_write_hex64(far);
	describe_address(far);
	console_write("\n");

	console_write("  Crash Site (ELR): 0x");
	console_write_hex64(elr);
	ksym_print(elr);
	console_write("\n");

	dump_code_around_pc(elr);
}



static void aarch64_sync_handler_inner(u64 esr, u64 elr, u64 far,
                                      u64 *saved_regs);

void aarch64_sync_handler(u64 esr, u64 elr, u64 far, u64 *saved_regs)
{
	struct interrupt_frame *frame = (struct interrupt_frame *)saved_regs;
	int from_el0 = (frame->spsr & 0xFULL) == 0;

	if (from_el0)
		sched_acct_enter_kernel();
	aarch64_sync_handler_inner(esr, elr, far, saved_regs);
	if (from_el0)
		sched_acct_leave_kernel();
}

static void aarch64_sync_handler_inner(u64 esr, u64 elr, u64 far,
                                       u64 *saved_regs)
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

	/* Did this entry land on the stack it should have?
	 *
	 * SAVE_REGS builds the frame immediately below SP_EL1, so on an EL0 entry
	 * the top of the frame must be exactly the task's kernel_stack_ptr. That is
	 * the invariant the old per-CPU stack reset enforced by overwriting SP on
	 * every boundary; with the reset gone the invariant has to hold on its own,
	 * so it is checked instead of assumed. A mismatch means a task's frame is
	 * being written somewhere other than its own stack, which is the corruption
	 * this branch has been chasing -- caught here, at the moment it happens,
	 * rather than deduced from a garbled saved register three switches later. */
	/* The same question for an EL1 exception, which is the half the EL0 check
	 * cannot see. irq_el1/sync_el1 build their frame at whatever SP_EL1 holds,
	 * so if SP is already on somebody else's stack the frame lands there and
	 * overwrites live locals. Report the first exception taken on a foreign
	 * stack, while the task table and the flight recorder still describe how it
	 * got there.
	 *
	 * This USED to be report-only, and the reason was not caution:
	 * scheduler_yield publishes `current_task = new_task` BEFORE calling
	 * arch_context_switch, deliberately, so no other CPU can claim the incoming
	 * task while its context is being loaded. Between those two points SP still
	 * belongs to the OUTGOING task, so "SP is not current_task's stack" was a
	 * legitimate, transient state, and panicking on it manufactured three
	 * panics a run out of a correct scheduler.
	 *
	 * The window is now a FACT rather than something to tolerate:
	 * sched_prev_task_this_cpu() names the task SP still belongs to for exactly
	 * that span and NULL outside it. An SP inside that task's stack is the
	 * known window; an SP anywhere else is the corruption this check exists
	 * for, and it panics again -- at the moment it happens, while the task
	 * table still describes how it got there, instead of surfacing three
	 * context switches later as a garbled register.
	 */
	if (!from_el0 && current_task && current_task->stack &&
	    current_task->kernel_stack_ptr) {
		u64 sp_now = (u64)(usize)frame;
		u64 own_lo = (u64)(usize)current_task->stack;
		u64 own_hi = current_task->kernel_stack_ptr;

		/* A frame on this CPU's EL1 fault stack is not a task running on
		 * somebody else's stack -- sync_el1 put it there precisely BECAUSE
		 * SP_EL1 was unusable. Reporting the fault stack as "foreign" hid the
		 * one number that matters (the SP that went bad, which is parked) and
		 * cost a run to work out. Report the parked SP instead. */
		if (aarch64_on_el1_fault_stack(sp_now))
			sp_now = aarch64_el1_fault_sp();

		/* The mid-switch window: SP still names the outgoing task. */
		struct task *prev = sched_prev_task_this_cpu();
		int in_switch_window = 0;

		if (prev && prev->stack && prev->kernel_stack_ptr) {
			u64 plo = (u64)(usize)prev->stack;
			u64 phi = prev->kernel_stack_ptr;

			in_switch_window = (sp_now >= plo && sp_now <= phi);
		}

		if (!in_switch_window && (sp_now < own_lo || sp_now > own_hi)) {
			/* Report once. This report runs ON the foreign stack, and the
			 * console it calls faults there too -- each fault re-enters here,
			 * re-reports, and recurses ~0x430 of stack per turn until the log
			 * is gigabytes of interleaved half-lines and the lane wedges on the
			 * watchdog with no readable dump at all. The first report is the
			 * only one worth anything; everything after it is this check
			 * tripping over its own output. */
			console_write("EL1-FOREIGN-STACK: pid ");
			console_write_dec((u64)current_task->id);
			console_write(" name=");
			console_write(current_task->name ? current_task->name : "(none)");
			console_write(" sp=0x");
			console_write_hex64(sp_now);
			console_write(" own=0x");
			console_write_hex64(own_lo);
			console_write("..0x");
			console_write_hex64(own_hi);
			console_write(" elr=0x");
			console_write_hex64(frame->elr);
			{
				struct percpu *fp = get_percpu();
				console_write(" cpu=");
				console_write_dec(fp ? (u64)fp->cpu_id : 99);
				console_write(" cur=0x");
				console_write_hex64((u64)(usize)(fp ? fp->cur_task : 0));
				console_write(" task=0x");
				console_write_hex64((u64)(usize)current_task);
				console_write(" ksp=0x");
				console_write_hex64(current_task->kernel_stack_ptr);
				console_write(" ctxsp=0x");
				console_write_hex64(current_task->context.sp);
				console_write(" idle=0x");
				console_write_hex64((u64)(usize)(fp ? fp->idle_task : 0));
				/* Whose stack is this really? A stale cur_task and a corrupted
				 * SP look identical from here; the owner tells them apart. */
				{
					struct task *ow = scheduler_task_owning_stack(sp_now);
					console_write(" sp_owner=");
					console_write(ow ? (ow->name ? ow->name : "(none)") : "(unowned)");
					console_write("/");
					console_write_dec(ow ? (u64)ow->id : 0);
					/* THE question this report could not answer.
					 *
					 * Two families explain a CPU on another task's stack, and
					 * they need opposite fixes: either that task is running
					 * RIGHT NOW on another CPU (two claimers of one task, so
					 * look at the claim paths), or it is running nowhere and
					 * this CPU resumed a stale saved context (so look at what
					 * writes ->context). Every CPU's cur_task, printed here,
					 * decides it -- and the owner's own state and lease say
					 * whether anything could legitimately have claimed it. */
					console_write(" owner_state=");
					console_write_dec(ow ? (u64)ow->state : 99);
					console_write(" owner_lease=");
					console_write_dec(ow ? (u64)__atomic_load_n(
					                           &ow->stack_released,
					                           __ATOMIC_ACQUIRE)
					                     : 99);
					console_write(" cur[");
					for (int c = 0; c < MAX_CPUS; c++) {
						struct percpu *pc = get_percpu_n(c);
						struct task *ct = pc ? (struct task *)pc->cur_task : 0;

						if (!pc)
							continue;
						if (c)
							console_write(",");
						console_write_dec((u64)c);
						console_write("=");
						console_write(ct ? (ct->name ? ct->name : "(none)")
						                 : "(none)");
						console_write("/");
						console_write_dec(ct ? (u64)ct->id : 0);
					}
					console_write("]");
				}
				console_write(" prev=");
				console_write(prev ? (prev->name ? prev->name : "(none)")
				                   : "(none)");
			}
			console_write("\n");
			panic("sched: EL1 exception on a foreign kernel stack");
		}
	}

	if (from_el0 && current_task && current_task->kernel_stack_ptr) {
		u64 frame_top = (u64)(usize)frame + sizeof(*frame);
		/* Two questions, and the weaker one is the interesting one.
		 *
		 * Exact equality says the entry landed exactly where SAVE_REGS should
		 * have put it, and a drift of one frame is a real defect. But the
		 * failure this is hunting is coarser and much more damaging: the frame
		 * is not on this task's stack AT ALL. That has been seen directly --
		 * `/bin/m32_smoke`, whose own stack is 0x1001034490..0x1001054450,
		 * entering with SP_EL1 at 0x1000b15890, inside the AP idle task's
		 * stack. Checking containment catches the first such entry rather than
		 * the rare moment the drift happens to become visible. */
		u64 own_lo = (u64)(usize)current_task->stack;
		u64 own_hi = current_task->kernel_stack_ptr;
		int foreign = own_lo && (frame_top <= own_lo || frame_top > own_hi);

		if (foreign || frame_top != current_task->kernel_stack_ptr) {
			console_write("KSTACK-MISMATCH: entry for pid ");
			console_write_dec((u64)current_task->id);
			console_write(" name=");
			console_write(current_task->name ? current_task->name : "(none)");
			console_write(" frame_top=0x");
			console_write_hex64(frame_top);
			console_write(" kernel_stack_ptr=0x");
			console_write_hex64(current_task->kernel_stack_ptr);
			/* TEMPPROBE: which CPU, and what was actually at EL0. */
			{
				struct percpu *mp = get_percpu();
				console_write(" cpu=");
				console_write_dec(mp ? (u64)mp->cpu_id : 99);
				console_write(" idle=");
				console_write_dec(mp && mp->idle_task == (void *)current_task ? 1 : 0);
				console_write(" ec=0x");
				console_write_hex64((u64)ec);
				console_write(" user_pc=0x");
				console_write_hex64(frame->elr);
				console_write(" user_sp=0x");
				console_write_hex64(frame->sp_el0);
				console_write(" stack=0x");
				console_write_hex64((u64)(usize)current_task->stack);
				{
					struct task *ow = scheduler_task_owning_stack(frame_top - 8);
					console_write(" sp_owner=");
					console_write(ow ? (ow->name ? ow->name : "(none)") : "(unowned)");
					console_write("/");
					console_write_dec(ow ? (u64)ow->id : 0);
					console_write(" owner_ksp=0x");
					console_write_hex64(ow ? ow->kernel_stack_ptr : 0);
					console_write(" owner_state=");
					console_write_dec(ow ? (u64)ow->state : 99);
				}
			}
			console_write("\n");
			/* Take the dump HERE. The frame this entry just built is sitting on
			 * top of live C frames belonging to whatever really owns this
			 * stack; letting it run on only buys a `ret` into a spilled
			 * boolean somewhere else entirely (observed: ELR=0x1, the return
			 * value of scheduler_yield in the AP idle loop). Panic while the
			 * task table and the per-CPU state still describe the moment. */
			if (foreign)
				panic("kstack mismatch: EL0 frame built on another task's stack");
		}
	}

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

	/* The interrupted SP_EL1 is where SAVE_REGS started, i.e. just above the
	 * frame it built. Derived rather than parked in a per-CPU word: the
	 * parking is what used to hand one task another task's stack (see the
	 * note on sync_el1 in isr.S). */
	console_write("\ninterrupted SP_EL1: 0x");
	console_write_hex64(frame ? (u64)(usize)frame + sizeof(struct interrupt_frame)
	                          : aarch64_el1_fault_sp());
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

	decode_aarch64_exception(esr, elr, far);

	console_write("\nRegisters (x0..x30):\n");
	for (int i = 0; i < 30; i += 2) {
		console_write("  x");
		console_write_dec((u32)i);
		console_write("=0x");
		console_write_hex64(saved_regs[i]);
		console_write("  x");
		console_write_dec((u32)(i + 1));
		console_write("=0x");
		console_write_hex64(saved_regs[i + 1]);
		console_write("\n");
	}
	console_write("  lr(x30)=0x");
	console_write_hex64(saved_regs[30]);
	ksym_print(saved_regs[30]);
	console_write("\n");

	panic_at("unhandled synchronous exception", __FILE__, __LINE__);
}


