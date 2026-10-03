// SPDX-License-Identifier: GPL-2.0-only
/*
 * b1nix's side of the KVM seam, x86 part (M131): APIC IDs and IPIs, the
 * GS/TSS bases and per-CPU mirrors a VMCS and the entry code read, MSRs, the
 * TSC rate, 5-level paging and the task's XSAVE image. See
 * <b1nix/kvm_bridge.h>.
 */
#include <b1nix/kvm_bridge.h>
#include <b1nix/arch.h>
#include <b1nix/lapic.h>
#include <b1nix/mm.h>
#include <b1nix/sched.h>

extern int paging_la57(void);
int arch_rdmsr_safe(u32 msr, u64 *out);
int arch_wrmsr_safe(u32 msr, u64 value);

int b1nix_kvm_cpu_present(int cpu)
{
	return b1nix_kvm_cpu_apic_id(cpu) != 0xffffffffu;
}

/* A kick out of guest mode: any interrupt makes the target CPU leave the
 * guest, and the reschedule vector does nothing else. */
void b1nix_kvm_kick_cpu(int cpu)
{
	b1nix_kvm_send_ipi(cpu, 0x42);
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


int b1nix_kvm_rdmsr_safe(u32 msr, u64 *val) { return arch_rdmsr_safe(msr, val); }
int b1nix_kvm_wrmsr_safe(u32 msr, u64 val) { return arch_wrmsr_safe(msr, val); }

u32 b1nix_kvm_tsc_khz(void) { return arch_tsc_khz(); }


int b1nix_kvm_la57(void) { return paging_la57(); }


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
