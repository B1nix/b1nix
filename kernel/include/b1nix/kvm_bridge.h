/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef B1NIX_KVM_BRIDGE_H
#define B1NIX_KVM_BRIDGE_H
/*
 * What Linux's KVM needs from b1nix, as plain functions (M131).
 *
 * KVM itself and the Linux API it is written against are built with the
 * linuxkpi headers (kernel/lkpi/kvm_*.c); those cannot include b1nix's own
 * headers without the two type systems colliding. This is the seam: the
 * functions here are implemented natively in kernel/virt/kvm_bridge.c and
 * take only basic types, so both sides can include this header.
 */
#include <b1nix/types.h>

/* ── CPUs ─────────────────────────────────────────────────────────────── */
int b1nix_kvm_cpu_count(void);
const char *b1nix_kvm_cmdline(void);
int b1nix_kvm_this_cpu(void);
int b1nix_kvm_cpu_present(int cpu);
void b1nix_kvm_kick_cpu(int cpu);
u32 b1nix_kvm_cpu_apic_id(int cpu);          /* 0xffffffff when absent */
u64 b1nix_kvm_cpu_gs_base(int cpu);          /* the kernel GS base of that CPU */
u64 b1nix_kvm_cpu_tss_base(int cpu);
/* KVM's %gs-read per-CPU values, mirrored into struct percpu. */
void b1nix_kvm_percpu_mirror(int cpu, u64 spec_ctrl, u64 svm_hsave_pa);
void b1nix_kvm_send_ipi(int cpu, u32 vector);
int b1nix_kvm_call_on_cpu(int cpu, void (*fn)(void *), void *info, int wait);
int b1nix_kvm_rdmsr_safe(u32 msr, u64 *val);
int b1nix_kvm_wrmsr_safe(u32 msr, u64 val);
void b1nix_kvm_udelay(u64 us);
u32 b1nix_kvm_tsc_khz(void);

/* ── memory ───────────────────────────────────────────────────────────── */
u64 b1nix_kvm_direct_map_base(void);
u64 b1nix_kvm_virt_to_phys(const void *va);
int b1nix_kvm_la57(void);
/* Is [start, end) all RAM in the firmware's memory map? */
int b1nix_kvm_range_is_ram(u64 start, u64 end);
u64 b1nix_kvm_zero_page_phys(void);

/* ── the calling process's address space ──────────────────────────────── */
u64 b1nix_kvm_current_pml4(void);
/* Where va is mapped in the address space: the physical address, whether it
 * is writable. 0, or -1 when nothing is mapped there. */
int b1nix_kvm_user_lookup(u64 pml4, u64 va, u64 *phys, int *writable);
/* Fault va in for the CURRENT task, as a user access would (write: break
 * copy-on-write too). 0, or a negative errno. */
int b1nix_kvm_fault_in(u64 va, int write);
/* The mapping around va in the current task: its bounds and PROT_* bits. 0,
 * or -1 when va is not mapped. */
int b1nix_kvm_vma(u64 va, u64 *start, u64 *end, u32 *prot);
/* mmap/munmap on behalf of the current task (anonymous only). */
u64 b1nix_kvm_mmap_anon(u64 addr, u64 len, int prot, int flags);
int b1nix_kvm_munmap(u64 addr, u64 len);

/* ── tasks ────────────────────────────────────────────────────────────── */
void *b1nix_kvm_current_task(void);
/* Is the current task still runnable (preempted, not going to sleep)? */
int b1nix_kvm_current_runnable(void);
u64 b1nix_kvm_sigmask_get(void);
void b1nix_kvm_sigmask_set(u64 mask);
/* A signal the current task has not blocked is pending; a SIGKILL is. */
int b1nix_kvm_signal_pending(void);
int b1nix_kvm_fatal_signal_pending(void);
int b1nix_kvm_other_work_pending(void);
/* The scheduler wants the current task off its CPU (a tick found it not
 * preemptible). */
int b1nix_kvm_need_resched(void);
void b1nix_kvm_wake(void *task);

/* ── the task's FPU image ─────────────────────────────────────────────── */
/* The XSAVE area the scheduler saves this task's registers into (allocated
 * if the task has none yet); and replacing it, which is how a vCPU's guest
 * image rides along a context switch while KVM_RUN has it loaded. */
void *b1nix_kvm_task_xsave_area(void);
void b1nix_kvm_task_set_xsave_area(void *area);
u64 b1nix_kvm_xsave_size(void);
u64 b1nix_kvm_xsave_mask(void);
void b1nix_kvm_xsave(void *area, u64 mask);
void b1nix_kvm_xrstor(void *area, u64 mask);

/* ── hooks KVM's glue installs in the kernel ──────────────────────────── */
struct b1nix_kvm_hooks {
	/* around a context switch, interrupts off */
	void (*sched_out)(void *prev, void *next);
	void (*sched_in)(void *task, int cpu);
	/* last thing before this CPU returns to user mode, interrupts off;
	 * only called once armed with b1nix_kvm_arm_user_return() */
	void (*return_to_user)(void);
	/* a user address space's mappings in [start, end) changed or went away
	 * (before the frames are freed); pml4 0 means every address space */
	void (*mm_invalidate)(u64 pml4, u64 start, u64 end, int blockable);
	/* the address space is being torn down */
	void (*mm_release)(u64 pml4);
	/* every CPU is about to be parked (a sleep that may lose their state),
	 * and they are all back; each is called on one CPU with the others
	 * still (or again) taking interrupts */
	void (*cpus_down)(void);
	void (*cpus_up)(void);
};
void b1nix_kvm_set_hooks(const struct b1nix_kvm_hooks *hooks);

/* ── the task's FP/SIMD registers (arm64) ─────────────────────────────── */
void b1nix_kvm_fp_flush_task(void);
int b1nix_kvm_fp_foreign(void);
/* Save / load the registers in Linux's struct user_fpsimd_state layout. */
void b1nix_kvm_fp_save(void *area);
void b1nix_kvm_fp_load(void *area);

/* ── arm64: CPUs, GIC, RAM, fixmap (kernel/virt/kvm_bridge_arm64.c) ──── */
u64 b1nix_kvm_cpu_mpidr(int cpu);
int b1nix_kvm_at_el2(void);
/* Where KVM's per-CPU variables lie on a CPU, as an offset from their
 * template (what KVM's hyp assembly adds to a per-CPU symbol). */
void b1nix_kvm_set_percpu_off(int cpu, u64 off);
u64 b1nix_kvm_host_vectors(void);
/* The timer output at a device-tree position, the vGIC maintenance
 * interrupt: INTIDs, 0 when absent. */
u32 b1nix_kvm_timer_irq(int index);
u32 b1nix_kvm_timer_irq_flags(int index);	/* IRQ_TYPE_* */
u32 b1nix_kvm_gic_maint_irq(void);
u32 b1nix_kvm_gic_maint_irq_flags(void);
int b1nix_kvm_gic_v3(void);
int b1nix_kvm_gic_eoimode_split(void);
/* A handler for a GIC interrupt, run in interrupt context on the CPU that
 * took it; enable/disable are this CPU's for a PPI. */
int b1nix_kvm_irq_register(u32 irq, int (*fn)(void *), void *ctx);
int b1nix_kvm_irq_unregister(u32 irq, int (*fn)(void *), void *ctx);
void b1nix_kvm_irq_enable(u32 irq);
void b1nix_kvm_irq_disable(u32 irq);
/* active: 1 the active bit, 0 the pending bit. */
int b1nix_kvm_irq_state(u32 irq, int active, int *state);
int b1nix_kvm_irq_set_state(u32 irq, int active, int on);
void b1nix_kvm_irq_set_forwarded(u32 irq, int on);
int b1nix_kvm_ram_region(int index, u64 *base, u64 *size);
u64 b1nix_kvm_direct_map_end(void);
u64 b1nix_kvm_alloc_block(int order);
u64 b1nix_kvm_fixmap_top(void);
/* Map the page at pa at va, or unmap va when pa is 0. */
void b1nix_kvm_fixmap_set(u64 va, u64 pa, int writable);
/* This CPU must run return_to_user before it next enters user mode. */
void b1nix_kvm_arm_user_return(void);
#endif
