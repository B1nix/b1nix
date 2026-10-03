// SPDX-License-Identifier: GPL-2.0-only
/*
 * The rest of the host arm64 KVM runs on (M131): RAM as memblock would
 * describe it, the fixmap, cache maintenance by address, the CPU map, the
 * generic timer KVM gives its guests, and the FP/SIMD register file shared
 * between the host's tasks and a vCPU.
 */
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/memblock.h>
#include <linux/percpu.h>
#include <linux/sched.h>
#include <linux/timekeeping.h>
#include <linux/timecounter.h>
#include <asm/arch_timer.h>
#include <asm/cacheflush.h>
#include <asm/cpufeature.h>
#include <asm/fixmap.h>
#include <asm/fpsimd.h>
#include <asm/memory.h>
#include <asm/smp_plat.h>
#include <asm/thread_info.h>
#include <clocksource/arm_arch_timer.h>
#include <b1nix/kvm_bridge.h>

/* ── RAM ──────────────────────────────────────────────────────────────── */

void *lkpi_high_memory(void)
{
	return (void *)(uintptr_t)b1nix_kvm_direct_map_end();
}

#define MEMBLOCK_MAX 32
static struct memblock_region memblock_regions[MEMBLOCK_MAX];

struct memblock_region *lkpi_memblock_region(unsigned int i)
{
	u64 base, size;

	if (i >= MEMBLOCK_MAX || b1nix_kvm_ram_region((int)i, &base, &size))
		return NULL;
	memblock_regions[i].base = base;
	memblock_regions[i].size = size;
	memblock_regions[i].flags = 0;
	return &memblock_regions[i];
}

phys_addr_t memblock_start_of_DRAM(void)
{
	phys_addr_t lowest = ~(phys_addr_t)0;
	struct memblock_region *r;

	for_each_mem_region(r)
		if (r->base < lowest)
			lowest = r->base;
	return lowest == ~(phys_addr_t)0 ? 0 : lowest;
}

/* Physically contiguous memory, aligned to @align, from the frame
 * allocator (memblock is gone once the kernel runs). 0 on failure. */
phys_addr_t memblock_phys_alloc(phys_addr_t size, phys_addr_t align)
{
	int order = get_order(max(size, align));

	return (phys_addr_t)b1nix_kvm_alloc_block(order);
}

/* RAM the direct map covers, as opposed to device memory. */
bool pfn_is_map_memory(unsigned long pfn)
{
	phys_addr_t pa = (phys_addr_t)pfn << PAGE_SHIFT;
	struct memblock_region *r;

	if (pa >= b1nix_kvm_direct_map_end())
		return false;
	for_each_mem_region(r)
		if (pa >= r->base && pa < r->base + r->size)
			return true;
	return false;
}

/* ── fixmap ───────────────────────────────────────────────────────────── */

unsigned long lkpi_fixaddr_top(void)
{
	return (unsigned long)b1nix_kvm_fixmap_top();
}

void __set_fixmap(enum fixed_addresses idx, phys_addr_t phys, pgprot_t prot)
{
	unsigned long va = __fix_to_virt(idx);
	u64 v = pgprot_val(prot);

	if (idx >= __end_of_fixed_addresses)
		return;
	b1nix_kvm_fixmap_set(va, v ? phys : 0, v && !(v & PTE_RDONLY));
}

/* ── CPUs ─────────────────────────────────────────────────────────────── */

u64 cpu_logical_map(unsigned int cpu)
{
	return b1nix_kvm_cpu_mpidr((int)cpu);
}

/* ── cache maintenance by virtual address ─────────────────────────────── */

static unsigned long dline(void)
{
	return 4UL << cpuid_feature_extract_unsigned_field(read_cpuid_cachetype(),
							   CTR_EL0_DminLine_SHIFT);
}

static unsigned long iline(void)
{
	return 4UL << cpuid_feature_extract_unsigned_field(read_cpuid_cachetype(),
							   CTR_EL0_IminLine_SHIFT);
}

#define DC_RANGE(op, start, end)					\
	do {								\
		unsigned long __l = dline();				\
		for (unsigned long __a = (start) & ~(__l - 1); __a < (end); __a += __l) \
			asm volatile("dc " #op ", %0" : : "r"(__a) : "memory"); \
	} while (0)

void dcache_clean_inval_poc(unsigned long start, unsigned long end)
{
	DC_RANGE(civac, start, end);
	dsb(sy);
}

void dcache_clean_poc(unsigned long start, unsigned long end)
{
	DC_RANGE(cvac, start, end);
	dsb(sy);
}

void dcache_clean_pou(unsigned long start, unsigned long end)
{
	if (!cpus_have_cap(ARM64_HAS_CACHE_IDC))
		DC_RANGE(cvau, start, end);
	dsb(ish);
}

/* Invalidate; lines only partly inside the range are cleaned first, so
 * the data around it survives. */
void dcache_inval_poc(unsigned long start, unsigned long end)
{
	unsigned long l = dline();

	if (start & (l - 1))
		asm volatile("dc civac, %0" : : "r"(start & ~(l - 1)) : "memory");
	if (end & (l - 1))
		asm volatile("dc civac, %0" : : "r"(end & ~(l - 1)) : "memory");
	DC_RANGE(ivac, start, end);
	dsb(sy);
}

void icache_inval_pou(unsigned long start, unsigned long end)
{
	if (!cpus_have_cap(ARM64_HAS_CACHE_DIC)) {
		unsigned long l = iline();

		for (unsigned long a = start & ~(l - 1); a < end; a += l)
			asm volatile("ic ivau, %0" : : "r"(a) : "memory");
		dsb(ish);
	}
	isb();
}

void caches_clean_inval_pou(unsigned long start, unsigned long end)
{
	dcache_clean_pou(start, end);
	icache_inval_pou(start, end);
}

/* ── the generic timer ────────────────────────────────────────────────── */

static u64 arch_counter_read_cc(const struct cyclecounter *cc)
{
	(void)cc;
	return __arch_counter_get_cntvct();
}

static struct cyclecounter arch_cyclecounter = {
	.read = arch_counter_read_cc,
	.mask = GENMASK_ULL(55, 0),
};

/* Upstream's clocks_calc_mult_shift: the largest shift whose multiplier
 * converts maxsec seconds of counter ticks without overflowing 64 bits. */
static void calc_mult_shift(u32 *mult, u32 *shift, u32 from, u32 to, u32 maxsec)
{
	u64 tmp;
	u32 sft, sftacc = 32;

	tmp = ((u64)maxsec * from) >> 32;
	while (tmp) {
		tmp >>= 1;
		sftacc--;
	}
	for (sft = 32; sft > 0; sft--) {
		tmp = (u64)to << sft;
		tmp += from / 2;
		tmp /= from;
		if ((tmp >> sftacc) == 0)
			break;
	}
	*mult = (u32)tmp;
	*shift = sft;
}

static struct arch_timer_kvm_info timer_kvm_info;

/* The counter KVM measures a guest's timer deadlines on, and the host
 * interrupts of the EL1 virtual and physical timers, which the host itself
 * leaves alone at EL2 (it runs on the EL2 virtual timer). */
struct arch_timer_kvm_info *arch_timer_get_kvm_info(void)
{
	if (!timer_kvm_info.timecounter.cc) {
		u32 freq = (u32)read_sysreg(cntfrq_el0);

		calc_mult_shift(&arch_cyclecounter.mult, &arch_cyclecounter.shift,
				freq, NSEC_PER_SEC, 600);
		timer_kvm_info.timecounter.cc = &arch_cyclecounter;
		timer_kvm_info.timecounter.cycle_last = arch_counter_read_cc(&arch_cyclecounter);
		timer_kvm_info.timecounter.mask = (1ULL << arch_cyclecounter.shift) - 1;
		timer_kvm_info.virtual_irq = (int)b1nix_kvm_timer_irq(2);
		timer_kvm_info.physical_irq = (int)b1nix_kvm_timer_irq(1);
	}
	return &timer_kvm_info;
}

/* The wall clock and the counter a guest's PTP clock pairs it with. */
void ktime_get_snapshot(struct system_time_snapshot *s)
{
	s->cycles = __arch_counter_get_cntvct();
	s->real = (ktime_t)ktime_get_real_ns();
	s->boot = ktime_get_boottime();
	s->raw = ktime_get_raw();
	s->cs_id = CSID_ARM_ARCH_COUNTER;
	s->clock_was_set_seq = 0;
	s->cs_was_changed_seq = 0;
}

/* ── FP/SIMD ──────────────────────────────────────────────────────────── */

/*
 * Linux tracks which state the FP/SIMD registers hold (the task's, a vCPU's
 * bound with fpsimd_bind_state_to_cpu, or none) so they are saved to the
 * right place. b1nix's scheduler owns the task's half (the CPU's
 * fpu_foreign, see sched_fpu_flush_current); this keeps the vCPU's.
 */
static DEFINE_PER_CPU(struct cpu_fp_state *, fp_bound);

void fpsimd_bind_state_to_cpu(struct cpu_fp_state *fp_state)
{
	/* The registers hold the vCPU's state now (the guest ran on them). */
	*this_cpu_ptr(&fp_bound) = fp_state;
}

void fpsimd_save_and_flush_cpu_state(void)
{
	unsigned long flags;
	struct cpu_fp_state **bound;

	local_irq_save(flags);
	bound = this_cpu_ptr(&fp_bound);
	if (b1nix_kvm_fp_foreign()) {
		if (*bound) {
			b1nix_kvm_fp_save((*bound)->st);
			if ((*bound)->fp_type)
				*(*bound)->fp_type = FP_STATE_FPSIMD;
		}
	} else {
		/* The current task's: to its image, restored before user mode. */
		b1nix_kvm_fp_flush_task();
	}
	*bound = NULL;
	local_irq_restore(flags);
}

/* TIF_FOREIGN_FPSTATE: the registers are neither the current task's nor a
 * bound vCPU's. The other flags KVM asks about (SVE, SME) b1nix never sets. */
bool test_thread_flag(int flag)
{
	if (flag != TIF_FOREIGN_FPSTATE)
		return false;
	return b1nix_kvm_fp_foreign() && !*this_cpu_ptr(&fp_bound);
}

void set_thread_flag(int flag)
{
	if (flag == TIF_FOREIGN_FPSTATE)
		fpsimd_save_and_flush_cpu_state();
}

void clear_thread_flag(int flag)
{
	/* After fpsimd_bind_state_to_cpu: what the registers hold is bound. */
	(void)flag;
}
