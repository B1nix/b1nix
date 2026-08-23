/* Secondary-CPU bring-up, over PSCI or a spin-table.
 *
 * There is no trampoline to write here and no real mode to leave: firmware
 * (or the hypervisor — QEMU virt implements PSCI itself) parks every secondary
 * CPU before the kernel runs, and CPU_ON hands one of them an entry address and
 * a context id.
 *
 * The other way a board parks them, and the one a Raspberry Pi uses, is a
 * spin-table: each CPU sits in a loop polling the address its own device-tree
 * node names in `cpu-release-addr`, and starts executing at whatever address is
 * written there. Which of the two applies is the tree's to say, in
 * `enable-method` — asking PSCI on a board that has none is an exception, not
 * an error code. The CPU arrives at EL1 with the MMU off and caches cold, which
 * is what `_ap_start` in boot.S deals with; from `aarch64_ap_main` down, this
 * is ordinary C on a CPU that shares the kernel's address space.
 *
 * What runs on a secondary today is the work-stealing phase only: stealable,
 * self-contained kernel workers, with interrupts masked, exactly as x86_64's
 * ap_main does before it enables its userspace phase. Running userspace here
 * additionally needs per-CPU exception state and a review of every kernel path
 * this port has so far been able to leave unlocked because g_max_cpus was 1.
 */

#include <b1nix/arch.h>
#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/lapic.h>
#include <b1nix/mm.h>
#include <b1nix/panic.h>
#include <b1nix/runqueue.h>
#include <b1nix/sched.h>

/* PSCI 0.2+ 64-bit calls. CPU_ON takes (target MPIDR, entry point, context id)
 * and returns 0 on success; the entry point is physical, since the CPU it
 * wakes starts with translation off. */
#define PSCI_CPU_ON_64 0xc4000003u
#define PSCI_SUCCESS   0

extern void _ap_start(void);
extern void arch_context_switch(struct cpu_context *old, struct cpu_context *new,
                                volatile int *old_stack_released);
extern void timer_init_cpu(void);

/* The block the exception vectors reach through TPIDR_EL1. Offsets are
 * hardcoded in kernel/arch/aarch64/isr.S; the asserts below keep the two in
 * step. One per CPU, because the vectors' two jobs — resetting SP_EL1 on an
 * EL0 entry and finding a stack for an EL1 fault — are per CPU the moment a
 * second one runs a process. */
struct aarch64_pcpu_asm {
	u64 kstack_top;  /* +0  task kernel stack top, for the SP_EL1 reset */
	u64 fault_top;   /* +8  top of this CPU's EL1 fault stack */
	u64 fault_sp;    /* +16 SP_EL1 the current EL1 fault interrupted */
};
_Static_assert(__builtin_offsetof(struct aarch64_pcpu_asm, kstack_top) == 0, "PCPU_KSTACK_TOP");
_Static_assert(__builtin_offsetof(struct aarch64_pcpu_asm, fault_top) == 8, "PCPU_FAULT_TOP");
_Static_assert(__builtin_offsetof(struct aarch64_pcpu_asm, fault_sp) == 16, "PCPU_FAULT_SP");

#define AARCH64_ASM_CPUS 8
#define EL1_FAULT_STACK_SIZE 16384

static struct aarch64_pcpu_asm g_pcpu_asm[AARCH64_ASM_CPUS];
static u8 g_el1_fault_stacks[AARCH64_ASM_CPUS][EL1_FAULT_STACK_SIZE]
    __attribute__((aligned(16)));

/* Install this CPU's block. Must run before the CPU can take an exception from
 * EL0 or fault at EL1 — on the boot processor that means before the scheduler,
 * not just before userspace: the very first arch_set_kernel_stack() has to
 * land somewhere the vectors will read, and one that is silently dropped
 * leaves SP_EL1 wherever boot left it. */
void aarch64_pcpu_asm_init(u32 cpu)
{
	if (cpu >= AARCH64_ASM_CPUS)
		panic("aarch64: more CPUs than per-CPU exception blocks");

	g_pcpu_asm[cpu].kstack_top = 0;
	g_pcpu_asm[cpu].fault_top =
	    (u64)(usize)g_el1_fault_stacks[cpu] + EL1_FAULT_STACK_SIZE;
	g_pcpu_asm[cpu].fault_sp = 0;

	__asm__ volatile("msr tpidr_el1, %0" : : "r"(&g_pcpu_asm[cpu]) : "memory");
}

static struct aarch64_pcpu_asm *pcpu_asm_self(void)
{
	struct aarch64_pcpu_asm *p;

	__asm__ volatile("mrs %0, tpidr_el1" : "=r"(p));
	return p;
}

void aarch64_set_kstack_top(u64 top)
{
	struct aarch64_pcpu_asm *p = pcpu_asm_self();

	/* Temporary: whoever clobbers TPIDR_EL1 gets named here rather than
	 * surfacing as a corrupt stack pointer three context switches later. */
	if (p && (p < &g_pcpu_asm[0] || p >= &g_pcpu_asm[AARCH64_ASM_CPUS])) {
		console_write("pcpu: TPIDR_EL1 clobbered: 0x");
		console_write_hex64((u64)(usize)p);
		console_write("\n");
		panic("aarch64: per-CPU block pointer lost");
	}

	if (!p) {
		/* Before aarch64_pcpu_asm_init: install the boot CPU's block now
		 * rather than dropping the value. Losing the first publish is not a
		 * missed optimisation — the vectors then reset SP_EL1 to nothing. */
		aarch64_pcpu_asm_init(0);
		p = pcpu_asm_self();
	}
	p->kstack_top = top;
}

u64 aarch64_kstack_top(void)
{
	struct aarch64_pcpu_asm *p = pcpu_asm_self();

	return p ? p->kstack_top : 0;
}

u64 aarch64_el1_fault_sp(void)
{
	struct aarch64_pcpu_asm *p = pcpu_asm_self();

	return p ? p->fault_sp : 0;
}

/* Read by _ap_start with the MMU still off, so both must live in the kernel
 * image, whose virtual addresses are its physical ones. */
u64 g_ap_ttbr0;
u64 g_ap_sp[MAX_CPUS];

int g_max_cpus = 1;

static struct percpu g_percpu[MAX_CPUS];

/* MPIDR affinity level 0 -> CPU index. Read by _ap_start (boot.S) too: a
 * secondary released from a spin-table enters with x0 = 0, so it works out
 * which CPU it is from what it can see of itself.
 *
 * AArch64 has no equivalent of x86's GS-based per-CPU pointer this kernel can
 * use: TPIDR_EL1 is already the
 * scratch register the exception vectors need before any GPR is live (see
 * EL0_KSTACK_RESET in isr.S). So the current CPU identifies itself from
 * MPIDR_EL1, and this table turns that into an index in two instructions —
 * get_percpu() is on the syscall path and cannot afford a search.
 *
 * Aff0 alone is the index on every board that numbers its CPUs the way the
 * device tree here does; a CPU whose Aff0 collides with another cluster's
 * would need the full affinity value, and would land on entry 0 rather than
 * silently on a stranger's data — which is why the BSP claims entry 0 first. */
u8 g_aff0_to_cpu[256];

static u64 mpidr(void)
{
	u64 v;

	__asm__ volatile("mrs %0, mpidr_el1" : "=r"(v));
	return v & 0x00ffffffu;
}

struct percpu *aarch64_get_percpu(void)
{
	return &g_percpu[g_aff0_to_cpu[mpidr() & 0xff]];
}

struct percpu *get_percpu_n(int idx)
{
	if (idx < 0 || idx >= g_max_cpus)
		return 0;
	return &g_percpu[idx];
}

void percpu_init(void)
{
	for (int i = 0; i < 256; i++)
		g_aff0_to_cpu[i] = 0;

	g_percpu[0].cpu_id = 0;
	g_percpu[0].cur_task = 0;
	g_percpu[0].cpu_online = 1;
	g_aff0_to_cpu[mpidr() & 0xff] = 0;
}

int get_online_cpu_count(void)
{
	int n = 0;

	for (int i = 0; i < g_max_cpus; i++) {
		if (g_percpu[i].cpu_online)
			n++;
	}
	return n ? n : 1;
}

static long psci_cpu_on(u64 target_mpidr, u64 entry_phys, u64 context_id)
{
	register u64 x0 __asm__("x0") = PSCI_CPU_ON_64;
	register u64 x1 __asm__("x1") = target_mpidr;
	register u64 x2 __asm__("x2") = entry_phys;
	register u64 x3 __asm__("x3") = context_id;

	/* HVC is QEMU virt's conduit, and the only one this port has met. A board
	 * that uses SMC reports so in its device tree's psci node; when one turns
	 * up, read `method` there rather than trying both — an unimplemented HVC
	 * at EL1 is an exception, not a return value. */
	__asm__ volatile("hvc #0"
	                 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
	                 :
	                 : "memory");
	return (long)x0;
}

/* Release a CPU that its firmware parked in a spin-table: write the entry
 * point into the address its device-tree node named, then wake it. Boards that
 * park their secondaries this way poll that word with the MMU off, so the
 * write goes to the physical address as-is — which is what the identity map
 * makes it here — and an event is what gets a CPU sitting in WFE to look again.
 *
 * The second write is for a board whose firmware parks nothing at all (QEMU's
 * raspi4b starts every CPU at the kernel entry): boot.S catches those in a
 * parked loop of its own, on g_spin_release. Releasing both costs one store
 * and covers both. */
extern u64 g_spin_release[256];

static void spin_table_release(u64 target_mpidr, u64 release_addr)
{
	if (release_addr)
		*(volatile u64 *)(usize)release_addr = (u64)(usize)&_ap_start;

	g_spin_release[target_mpidr & 0xff] = (u64)(usize)&_ap_start;

	__asm__ volatile("dsb sy\n\tsev" ::: "memory");
}

/* Entered from _ap_start with this CPU's own stack and the kernel's page
 * tables. `cpu` is the index _ap_start read out of g_aff0_to_cpu. */
void aarch64_ap_main(u64 cpu)
{
	extern void gic_cpu_init(void);
	struct percpu *pcpu = &g_percpu[cpu];

	aarch64_pcpu_asm_init((u32)cpu);
	gic_cpu_init();

	/* Published before anything that can stall, so the BSP's wait below sees
	 * this CPU promptly. Nothing here writes to the console: the serial port
	 * and the log buffer are the BSP's until this CPU holds a lock over them,
	 * and a half-line from a secondary in the middle of the boot log has cost
	 * this project a marker before. */
	pcpu->cpu_id = (u32)cpu;
	pcpu->scheduler_started = 1;
	__atomic_store_n(&pcpu->cpu_online, 1, __ATOMIC_RELEASE);

	/* The idle context this CPU parks back into when a stolen worker
	 * finishes. aarch64_ap_main never returns, so the local outlives every
	 * worker that switches through it. */
	struct cpu_context idle_ctx;

	/* ── Phase 1: stealable kernel workers only ──
	 * Until the boot CPU has finished its own SMP self-tests, run nothing but
	 * self-contained workers, with interrupts masked. Same shape as x86_64's
	 * ap_main, and for the same reason: the work-stealing path is proven
	 * before anything user-visible depends on this CPU. */
	while (!g_ap_userspace_enabled) {
		interrupts_disable();

		struct task *t = rq_dequeue(&pcpu->runqueue);

		if (t && !(t->state == TASK_READY && t->stealable)) {
			rq_enqueue(&pcpu->runqueue, t);
			t = 0;
		}
		if (!t)
			t = sched_steal_task(); /* READY stealable workers only */

		if (t) {
			t->state = TASK_RUNNING;
			pcpu->cur_task = t;
			pcpu->sched_return_ctx = &idle_ctx;
			arch_context_switch(&idle_ctx, &t->context, (volatile int *)0);
			pcpu->cur_task = 0;
			sched_ap_reap_worker(t);
			interrupts_enable();
			continue;
		}

		/* Nothing to steal. WFE rather than a spin: the CPU sleeps until an
		 * event or an interrupt, and every enqueue that matters is followed by
		 * a SEV from the CPU that made the work runnable. A timeout is not
		 * needed — an unpaired wakeup only costs one trip round this loop. */
		interrupts_enable();
		__asm__ volatile("wfe" ::: "memory");
	}

	/* ── Phase 2: the full cooperative scheduler ──
	 * Ordinary processes now, picked from the global runqueue; the scheduler
	 * lets only ap_runnable tasks (userspace ELFs) onto a secondary.
	 *
	 * Three things had to become per-CPU first: the SP_EL1 reset the EL0 entry
	 * performs, the EL1 fault stack (both in the block above), and the
	 * scheduler tick, armed here for this CPU rather than inherited from the
	 * boot processor. */
	timer_init_cpu();

	struct task *idle = scheduler_setup_ap_idle((int)cpu, pcpu->kernel_stack_virt);

	pcpu->idle_task = idle;
	pcpu->cur_task = idle;
	pcpu->sched_return_ctx = 0;

	for (;;) {
		int switched = scheduler_yield();

		if (!switched)
			interrupts_enable_and_wait();
	}
}

/* Entered on a secondary CPU (through arch_context_switch, from the loop
 * above) once it has stolen a worker. Runs the worker on this CPU's stack and
 * parks back into the idle context.
 *
 * Deliberately not the cooperative scheduler's exit path: that one is the boot
 * CPU's and is not safe to run from here. The secondary tracks its worker
 * through its own percpu entry alone. */
void ap_worker_trampoline(void)
{
	struct percpu *pcpu = get_percpu();
	struct task *t = pcpu ? pcpu->cur_task : (struct task *)0;

	if (t && t->entry)
		t->entry(t->arg);
	if (t) {
		/* Claim stack_released before publishing DEAD, so a reaper cannot see
		 * the task as finished while the switch below is still on its stack. */
		t->stack_released = 0;
		t->state = TASK_DEAD;
	}

	arch_context_switch(&t->context, (struct cpu_context *)pcpu->sched_return_ctx,
	                    &t->stack_released);

	for (;;)
		__asm__ volatile("wfe");	/* unreachable */
}

/* Wake every CPU the device tree lists apart from the one already running.
 * Returns the number now online. */
int smp_boot_aps(void)
{
	u32 listed = fdt_cpu_count();

	if (listed <= 1) {
		console_write("smp: one CPU in the device tree\n");
		return 1;
	}
	/* Bounded by the per-CPU exception blocks: a CPU without one has nowhere
	 * to reset SP_EL1 from and no EL1 fault stack of its own. */
	if (listed > AARCH64_ASM_CPUS)
		listed = AARCH64_ASM_CPUS;

	/* The live kernel L0, not boot_l0: by now the heap, the MMIO window and
	 * every task's page tables live outside the boot identity map, so a CPU
	 * that came up on boot_l0 would fault on the first heap access. */
	g_ap_ttbr0 = paging_kernel_root_phys();

	u64 self = mpidr();
	u32 attempted = 0;

	for (u32 i = 0; i < listed; i++) {
		u64 target = fdt_cpu_mpidr(i);

		if (target == self)
			continue;

		/* One index per CPU ATTEMPTED, counting from 1 (the BSP owns 0).
		 * Deriving it from the success count reuses the slot of a CPU that
		 * failed to report in — and a CPU that is merely slow then boots onto
		 * another one's stack and per-CPU block. */
		u32 index = attempted + 1;

		attempted++;
		if (index >= MAX_CPUS)
			break;

		/* 16 KiB of kernel stack, the same size the BSP's is. Allocated
		 * before the call, because the CPU it wakes runs on it immediately. */
		void *stack = kmalloc(16384);
		if (!stack) {
			console_write("smp: out of memory for a secondary stack\n");
			break;
		}

		g_percpu[index].cpu_id = index;
		g_percpu[index].cur_task = 0;
		g_percpu[index].cpu_online = 0;
		g_ap_sp[index] = (u64)(usize)stack + 16384;
		/* The same stack, recorded for scheduler_setup_ap_idle: this CPU's
		 * idle task runs on it. */
		g_percpu[index].kernel_stack_virt = g_ap_sp[index];
		g_aff0_to_cpu[target & 0xff] = (u8)index;

		if (fdt_cpu_enable_method() == FDT_ENABLE_METHOD_SPIN_TABLE) {
			spin_table_release(target, fdt_cpu_release_addr(i));
		} else {
			long rc = psci_cpu_on(target, (u64)(usize)&_ap_start, index);

			if (rc != PSCI_SUCCESS) {
				console_write("smp: CPU_ON refused for cpu ");
				console_write_dec(index);
				console_write("\n");
				kfree(stack);
				continue;
			}
		}

		/* Wait for the CPU to say so itself rather than assuming the call
		 * that succeeded also arrived: CPU_ON returns as soon as the request
		 * is accepted, and a spin-table write returns before the CPU polls. The bound is generous and the failure is reported, not
		 * fatal — a machine that comes up with fewer CPUs than its tree lists
		 * is still a working machine. */
		int ready = 0;
		for (int spin = 0; spin < 500000 && !ready; spin++) {
			ready = __atomic_load_n(&g_percpu[index].cpu_online, __ATOMIC_ACQUIRE);
			cpu_relax();
		}
		if (!ready) {
			/* Its stack and block stay allocated on purpose: the CPU may
			 * simply be slow, and freeing them under a CPU that is still
			 * booting is worse than leaking 16 KiB. */
			console_write("smp: cpu ");
			console_write_dec(index);
			console_write(" never reported online\n");
			continue;
		}

		if ((int)(index + 1) > g_max_cpus)
			g_max_cpus = (int)(index + 1);
	}

	console_write("smp: ");
	console_write_dec((u64)get_online_cpu_count());
	console_write(" CPUs online\n");
	return get_online_cpu_count();
}

/* A CPU parked in the WFE above wakes on an event; making work runnable is
 * one. The x86_64 counterpart sends a reschedule IPI, which this does not need
 * — SEV is a broadcast event, and the loop re-checks the runqueues on waking. */
void ipi_reschedule_all(void)
{
	__asm__ volatile("sev" ::: "memory");
}
