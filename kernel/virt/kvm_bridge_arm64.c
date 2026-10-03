// SPDX-License-Identifier: GPL-2.0-only
/*
 * b1nix's side of the KVM seam, arm64 part (M131): CPUs by MPIDR, the GIC
 * interrupts a VHE host hands its guests (the EL1 timers, the vGIC
 * maintenance interrupt) and their state, RAM as KVM sizes it, the fixmap
 * nested KVM maps a VNCR page through, and the FP/SIMD register file. See
 * <b1nix/kvm_bridge.h>.
 */
#include <b1nix/kvm_bridge.h>
#include <b1nix/arch.h>
#include <b1nix/bootinfo.h>
#include <b1nix/gicv3.h>
#include <b1nix/irq.h>
#include <b1nix/lapic.h>
#include <b1nix/mm.h>

extern void arch_fpu_save(void *area);
extern void arch_fpu_restore(void *area);
extern void vector_table_el1(void);

/* ── CPUs ─────────────────────────────────────────────────────────────── */

int b1nix_kvm_cpu_present(int cpu)
{
	return get_percpu_n(cpu) != 0;
}

u64 b1nix_kvm_cpu_mpidr(int cpu)
{
	return aarch64_cpu_mpidr((u32)cpu);
}

/* A kick out of guest mode: the reschedule SGI, which does nothing but end
 * the CPU's wait or its guest's time slice. */
void b1nix_kvm_kick_cpu(int cpu)
{
	u64 mpidr = aarch64_cpu_mpidr((u32)cpu);

	if (mpidr)
		gicv3_send_sgi(mpidr, GICV3_SGI_RESCHED);
}

int b1nix_kvm_at_el2(void) { return arch_kernel_at_el2(); }

void b1nix_kvm_set_percpu_off(int cpu, u64 off)
{
	aarch64_set_kvm_percpu_off((u32)cpu, off);
}

/* The table VBAR points at while no guest runs. */
u64 b1nix_kvm_host_vectors(void) { return (u64)(usize)vector_table_el1; }

/* ── interrupts ───────────────────────────────────────────────────────── */

u32 b1nix_kvm_timer_irq(int index) { return fdt_timer_irq((u32)index); }
u32 b1nix_kvm_timer_irq_flags(int index) { return fdt_timer_irq_flags((u32)index); }
u32 b1nix_kvm_gic_maint_irq(void) { return fdt_gic_maint_irq(); }
u32 b1nix_kvm_gic_maint_irq_flags(void) { return fdt_gic_maint_irq_flags(); }
int b1nix_kvm_gic_v3(void) { return gicv3_present(); }
int b1nix_kvm_gic_eoimode_split(void) { return gicv3_eoimode_split(); }

int b1nix_kvm_irq_register(u32 irq, int (*fn)(void *), void *ctx)
{
	return irq_register_handler(irq, fn, ctx);
}

int b1nix_kvm_irq_unregister(u32 irq, int (*fn)(void *), void *ctx)
{
	return irq_unregister_handler(irq, fn, ctx);
}

void b1nix_kvm_irq_enable(u32 irq) { gicv3_enable_irq(irq); }
void b1nix_kvm_irq_disable(u32 irq) { gicv3_disable_irq(irq); }

int b1nix_kvm_irq_state(u32 irq, int active, int *state)
{
	return gicv3_irq_state(irq, active, state);
}

int b1nix_kvm_irq_set_state(u32 irq, int active, int on)
{
	return gicv3_set_irq_state(irq, active, on);
}

void b1nix_kvm_irq_set_forwarded(u32 irq, int on) { gicv3_set_forwarded(irq, on); }

/* ── RAM ──────────────────────────────────────────────────────────────── */

/* The i-th RAM region of the firmware's map; -1 past the last. */
int b1nix_kvm_ram_region(int index, u64 *base, u64 *size)
{
	const struct boot_info *bi = bootinfo_get();
	int n = 0;

	if (!bi)
		return -1;
	for (usize r = 0; r < bi->memory_region_count; r++) {
		const struct boot_memory_region *m = &bi->memory_regions[r];

		if (m->type != BOOT_MEMORY_AVAILABLE)
			continue;
		if (n++ == index) {
			*base = m->base;
			*size = m->length;
			return 0;
		}
	}
	return -1;
}

/* The end of the direct map: RAM above it has no linear address. */
u64 b1nix_kvm_direct_map_end(void) { return DIRECT_MAP_BASE + g_direct_map_size; }

/* 2^order contiguous zeroed frames, naturally aligned, or 0. */
u64 b1nix_kvm_alloc_block(int order) { return pmm_alloc_block(order); }

/* ── the fixmap ───────────────────────────────────────────────────────── */

/* Below the kernel heap, above the largest direct map: a window no other
 * mapping uses, inside the half every address space shares. */
u64 b1nix_kvm_fixmap_top(void) { return KHEAP_START - 0x100000ULL; }

void b1nix_kvm_fixmap_set(u64 va, u64 pa, int writable)
{
	if (pa)
		vmm_map_page(va, pa, VMM_PRESENT | (writable ? VMM_WRITABLE : 0) |
				     VMM_NO_EXECUTE);
	else
		vmm_unmap_page(va);
}

/* ── FP/SIMD ──────────────────────────────────────────────────────────── */

void b1nix_kvm_fp_save(void *area) { arch_fpu_save(area); }
void b1nix_kvm_fp_load(void *area) { arch_fpu_restore(area); }
