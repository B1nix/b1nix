// SPDX-License-Identifier: GPL-2.0-only
/*
 * b1nix's side of the KVM seam (M131): see <b1nix/kvm_bridge.h>. The part
 * every architecture shares; kvm_bridge_x86.c and kvm_bridge_arm64.c hold
 * the rest.
 */
#include <b1nix/kvm_bridge.h>
#include <b1nix/arch.h>
#include <b1nix/bootinfo.h>
#include <b1nix/ipi.h>
#include <b1nix/mm.h>
#include <b1nix/ktime.h>
#include <b1nix/sched.h>
#include <b1nix/errno.h>
#include <b1nix/kvm_hooks.h>

int b1nix_kvm_cpu_count(void) { return g_max_cpus; }

int b1nix_kvm_this_cpu(void)
{
	struct percpu *p = get_percpu();

	return p ? (int)p->cpu_id : 0;
}

int b1nix_kvm_call_on_cpu(int cpu, void (*fn)(void *), void *info, int wait)
{
	return smp_call_on_cpu(cpu, fn, info, wait);
}

void b1nix_kvm_udelay(u64 us)
{
	u64 end = ktime_monotonic_ns() + us * 1000;

	while (ktime_monotonic_ns() < end)
		cpu_relax();
}

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

u64 b1nix_kvm_virt_to_phys(const void *va) { return vmm_virt_to_phys((void *)va); }
u64 b1nix_kvm_direct_map_base(void) { return DIRECT_MAP_BASE; }

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

/* ── the task's FP/SIMD registers (arm64 KVM) ─────────────────────────── */

/* Save the current task's registers and mark them foreign; they are loaded
 * back on the way to user mode, which this arms. */
void b1nix_kvm_fp_flush_task(void)
{
	sched_fpu_flush_current();
	b1nix_kvm_arm_user_return();
}

int b1nix_kvm_fp_foreign(void) { return sched_fpu_foreign(); }

/* The kernel command line, for KVM's early parameters. */
const char *b1nix_kvm_cmdline(void)
{
	const char *c = bootinfo_cmdline();

	return c ? c : "";
}
