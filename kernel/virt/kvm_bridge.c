// SPDX-License-Identifier: GPL-2.0-only
/*
 * b1nix's side of the KVM seam (M131): see <b1nix/kvm_bridge.h>.
 */
#include <b1nix/kvm_bridge.h>
#include <b1nix/arch.h>
#include <b1nix/bootinfo.h>
#include <b1nix/ipi.h>
#include <b1nix/lapic.h>
#include <b1nix/mm.h>
#include <b1nix/ktime.h>
#include <b1nix/sched.h>
#include <b1nix/errno.h>
#include <b1nix/kvm_hooks.h>

extern int paging_la57(void);
int arch_rdmsr_safe(u32 msr, u64 *out);
int arch_wrmsr_safe(u32 msr, u64 value);

int b1nix_kvm_cpu_count(void) { return g_max_cpus; }

int b1nix_kvm_this_cpu(void)
{
	struct percpu *p = get_percpu();

	return p ? (int)p->cpu_id : 0;
}

u32 b1nix_kvm_cpu_apic_id(int cpu)
{
	struct percpu *p = get_percpu_n(cpu);

	return p ? p->apic_id : 0xffffffffu;
}

/* b1nix never swaps GS: the kernel GS base of a CPU is always its struct
 * percpu, and that is what a VMCS must restore on VM exit. */
u64 b1nix_kvm_cpu_gs_base(int cpu)
{
	return (u64)(usize)get_percpu_n(cpu);
}

u64 b1nix_kvm_cpu_tss_base(int cpu) { return arch_tss_base(cpu); }

/* The two per-CPU values KVM's entry code reads through GS (see
 * <b1nix/percpu_kvm.h>). */
void b1nix_kvm_percpu_mirror(int cpu, u64 spec_ctrl, u64 svm_hsave_pa)
{
	struct percpu *p = get_percpu_n(cpu);

	if (!p)
		return;
	p->kvm_spec_ctrl = spec_ctrl;
	p->kvm_svm_hsave_pa = svm_hsave_pa;
}

void b1nix_kvm_send_ipi(int cpu, u32 vector)
{
	struct percpu *p = get_percpu_n(cpu);

	if (p)
		lapic_send_ipi(p->apic_id, (vector & 0xff) | LAPIC_ICR_FIXED);
}

int b1nix_kvm_call_on_cpu(int cpu, void (*fn)(void *), void *info, int wait)
{
	return smp_call_on_cpu(cpu, fn, info, wait);
}

int b1nix_kvm_rdmsr_safe(u32 msr, u64 *val) { return arch_rdmsr_safe(msr, val); }
int b1nix_kvm_wrmsr_safe(u32 msr, u64 val) { return arch_wrmsr_safe(msr, val); }

u32 b1nix_kvm_tsc_khz(void) { return arch_tsc_khz(); }

void b1nix_kvm_udelay(u64 us)
{
	u64 end = ktime_monotonic_ns() + us * 1000;

	while (ktime_monotonic_ns() < end)
		cpu_relax();
}

u64 b1nix_kvm_direct_map_base(void) { return DIRECT_MAP_BASE; }
u64 b1nix_kvm_virt_to_phys(const void *va) { return vmm_virt_to_phys((void *)va); }
int b1nix_kvm_la57(void) { return paging_la57(); }

int b1nix_kvm_range_is_ram(u64 start, u64 end)
{
	const struct boot_info *bi = bootinfo_get();

	if (!bi || end <= start)
		return 0;
	for (usize r = 0; r < bi->memory_region_count; r++) {
		const struct boot_memory_region *m = &bi->memory_regions[r];

		if (m->type == BOOT_MEMORY_AVAILABLE && start >= m->base &&
		    end <= m->base + m->length)
			return 1;
	}
	return 0;
}

u64 b1nix_kvm_zero_page_phys(void) { return pmm_zero_page(); }

/* ── the calling process's address space ──────────────────────────────── */

u64 b1nix_kvm_current_pml4(void)
{
	return current_task ? current_task->pml4_phys : 0;
}

int b1nix_kvm_user_lookup(u64 pml4, u64 va, u64 *phys, int *writable)
{
	u64 pa = paging_user_phys(pml4, va);

	if (!pa)
		return -1;
	*phys = pa;
	*writable = paging_user_writable(pml4, va);
	return 0;
}

/* The fault the CPU would have raised for this access, retried until the
 * page is in. A write to a present page is a present+write fault, which
 * breaks a copy-on-write page rather than leaving it shared; a page that is
 * not there yet is an absent-page fault, read or write, which is what brings
 * in guest memory the VMM has never touched. */
int b1nix_kvm_fault_in(u64 va, int write)
{
	u64 pml4 = b1nix_kvm_current_pml4();

	for (int tries = 0; tries < 4; tries++) {
		u64 err = PF_USER | (write ? PF_WRITE : 0);
		int r;

		if (paging_user_phys(pml4, va)) {
			if (!write || paging_user_writable(pml4, va))
				return 0;
			err |= PF_PRESENT;
		}
		r = vmm_handle_page_fault(va, err);
		if (r < 0)
			return -EFAULT;
	}
	return (paging_user_phys(pml4, va) &&
		(!write || paging_user_writable(pml4, va))) ? 0 : -EFAULT;
}

int b1nix_kvm_vma(u64 va, u64 *start, u64 *end, u32 *prot)
{
	struct vm_area *v;
	u64 flags;
	int r = -1;

	if (!current_task)
		return -1;
	vma_list_lock(&flags);
	v = vma_lookup(current_task, va);
	if (v) {
		*start = v->start;
		*end = v->end;
		*prot = v->prot;
		r = 0;
	}
	vma_list_unlock(flags);
	return r;
}

u64 b1nix_kvm_mmap_anon(u64 addr, u64 len, int prot, int flags)
{
	return syscall_mmap_current((void *)(usize)addr, (usize)len, prot, flags);
}

int b1nix_kvm_munmap(u64 addr, u64 len)
{
	return (int)syscall_munmap_current((void *)(usize)addr, (usize)len);
}

/* ── tasks ────────────────────────────────────────────────────────────── */

void *b1nix_kvm_current_task(void) { return current_task; }

int b1nix_kvm_current_runnable(void)
{
	return current_task && (current_task->state == TASK_RUNNING ||
				current_task->state == TASK_READY);
}

u64 b1nix_kvm_sigmask_get(void)
{
	return current_task ? current_task->blocked_signals : 0;
}

void b1nix_kvm_sigmask_set(u64 mask)
{
	/* SIGKILL and SIGSTOP cannot be blocked, whatever is asked. */
	if (current_task)
		current_task->blocked_signals = mask & ~((1ULL << (9 - 1)) | (1ULL << (19 - 1)));
}

int b1nix_kvm_signal_pending(void)
{
	return scheduler_signal_pending_any();
}

int b1nix_kvm_fatal_signal_pending(void)
{
	return current_task &&
	       (__atomic_load_n(&current_task->pending_signals, __ATOMIC_ACQUIRE) &
		(1ULL << (9 - 1))) != 0;
}

int b1nix_kvm_other_work_pending(void) { return sched_other_work_pending(); }
int b1nix_kvm_need_resched(void) { return scheduler_resched_pending(); }

void b1nix_kvm_wake(void *task)
{
	struct task *t = task;

	if (t)
		scheduler_wake_task(t->id);
}

/* ── the task's FPU image ─────────────────────────────────────────────── */

void *b1nix_kvm_task_xsave_area(void)
{
	if (!current_task || !task_fpu_alloc(current_task))
		return 0;
	return task_xsave_area(current_task);
}

void b1nix_kvm_task_set_xsave_area(void *area)
{
	if (current_task)
		task_set_xsave_area(current_task, area);
}

u64 b1nix_kvm_xsave_size(void) { return arch_xsave_area_size(); }
u64 b1nix_kvm_xsave_mask(void) { return arch_xsave_mask(); }
void b1nix_kvm_xsave(void *area, u64 mask) { arch_xsave(area, mask); }
void b1nix_kvm_xrstor(void *area, u64 mask) { arch_xrstor(area, mask); }
