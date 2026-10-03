// SPDX-License-Identifier: GPL-2.0-only
/*
 * arm64 alternatives (M131): the instructions KVM's code and the headers it
 * is written against leave for the kernel to patch once the CPU's features
 * are known -- a VHE-only sysreg access, a TLBI that needs repeating, a
 * capability test that is a single branch. Each is an entry in
 * .altinstructions (bounds in kernel/arch/aarch64/linker.ld): the original
 * instructions, a replacement or a callback that writes one, and the
 * capability that selects it.
 *
 * The patching is upstream's arch/arm64/kernel/alternative.c. It runs once,
 * from KVM's start-up, after kvm_arm64_cpu.c has decided the capabilities
 * and before any of the patched code has run -- all of it is KVM's -- so no
 * other CPU can be executing it, and the broadcast instruction-cache
 * invalidation at the end reaches them all.
 */
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/bitmap.h>
#include <asm/alternative.h>
#include <asm/cacheflush.h>
#include <asm/cpufeature.h>
#include <asm/insn.h>
#include <asm/sections.h>
#include <asm/spectre.h>

#define __ALT_PTR(a, f)		((void *)&(a)->f + (a)->f)
#define ALT_ORIG_PTR(a)		__ALT_PTR(a, orig_offset)
#define ALT_REPL_PTR(a)		__ALT_PTR(a, alt_offset)
#define ALT_CAP(a)		((a)->cpucap & ~ARM64_CB_BIT)
#define ALT_HAS_CB(a)		((a)->cpucap & ARM64_CB_BIT)

static bool branch_insn_requires_update(struct alt_instr *alt, unsigned long pc)
{
	unsigned long replptr = (unsigned long)ALT_REPL_PTR(alt);

	return !(pc >= replptr && pc <= (replptr + alt->alt_len));
}

#define align_down(x, a)	((unsigned long)(x) & ~(((unsigned long)(a)) - 1))

/* A replacement instruction as it must read at the original's address:
 * PC-relative branches and ADRPs that leave the replacement are retargeted. */
static u32 get_alt_insn(struct alt_instr *alt, __le32 *insnptr, __le32 *altinsnptr)
{
	u32 insn = le32_to_cpu(*altinsnptr);

	if (aarch64_insn_is_branch_imm(insn)) {
		s32 offset = aarch64_get_branch_offset(insn);
		unsigned long target = (unsigned long)altinsnptr + offset;

		if (branch_insn_requires_update(alt, target)) {
			offset = target - (unsigned long)insnptr;
			insn = aarch64_set_branch_offset(insn, offset);
		}
	} else if (aarch64_insn_is_adrp(insn)) {
		s32 orig_offset = aarch64_insn_adrp_get_offset(insn);
		unsigned long target = align_down(altinsnptr, SZ_4K) + orig_offset;
		s32 new_offset = target - align_down(insnptr, SZ_4K);

		insn = aarch64_insn_adrp_set_offset(insn, new_offset);
	} else if (aarch64_insn_uses_literal(insn)) {
		/* A PC-relative literal load cannot be moved. */
		BUG();
	}
	return insn;
}

static void patch_alternative(struct alt_instr *alt, __le32 *origptr,
			      __le32 *updptr, int nr_inst)
{
	__le32 *replptr = ALT_REPL_PTR(alt);

	for (int i = 0; i < nr_inst; i++)
		updptr[i] = cpu_to_le32(get_alt_insn(alt, origptr + i, replptr + i));
}

static void clean_dcache_range_nopatch(u64 start, u64 end)
{
	u64 ctr_el0 = read_sanitised_ftr_reg(SYS_CTR_EL0);
	u64 d_size = 4 << cpuid_feature_extract_unsigned_field(ctr_el0,
							       CTR_EL0_DminLine_SHIFT);
	u64 cur = start & ~(d_size - 1);

	do {
		/* Clean+invalidate to PoC, as upstream (Cortex-A53 errata). */
		asm volatile("dc civac, %0" : : "r" (cur) : "memory");
	} while (cur += d_size, cur < end);
}

/* Patch every alternative whose capability this system has. */
int lkpi_arm64_apply_alternatives(void)
{
	struct alt_instr *begin = (struct alt_instr *)__alt_instructions;
	struct alt_instr *end = (struct alt_instr *)__alt_instructions_end;
	unsigned int patched = 0;

	for (struct alt_instr *alt = begin; alt < end; alt++) {
		int cap = ALT_CAP(alt);
		int nr_inst;
		__le32 *origptr;
		alternative_cb_t alt_cb;

		if (!cpus_have_cap(cap))
			continue;
		if (ALT_HAS_CB(alt) ? alt->alt_len != 0 : alt->alt_len != alt->orig_len) {
			pr_err("kvm: malformed alternative at %p\n", ALT_ORIG_PTR(alt));
			return -EINVAL;
		}
		origptr = ALT_ORIG_PTR(alt);
		nr_inst = alt->orig_len / AARCH64_INSN_SIZE;
		alt_cb = ALT_HAS_CB(alt) ? (alternative_cb_t)ALT_REPL_PTR(alt) : patch_alternative;
		/* The kernel image is its own linear alias here. */
		alt_cb(alt, origptr, origptr, nr_inst);
		clean_dcache_range_nopatch((u64)origptr, (u64)(origptr + nr_inst));
		patched++;
	}
	dsb(ish);
	asm volatile("ic ialluis" ::: "memory");
	dsb(ish);
	isb();
	pr_info("kvm: %u of %lu alternatives applied\n", patched,
		(unsigned long)(end - begin));
	return 0;
}

/* The callback behind alternative_has_cap_likely/unlikely: the capability
 * is there, so the branch around it goes. */
void alt_cb_patch_nops(struct alt_instr *alt, __le32 *origptr,
		       __le32 *updptr, int nr_inst)
{
	(void)alt;
	(void)origptr;
	for (int i = 0; i < nr_inst; i++)
		updptr[i] = cpu_to_le32(aarch64_insn_gen_nop());
}

/* Spectre-BHB mitigation through firmware (SMCCC_ARCH_WORKAROUND_3): b1nix
 * applies none, and upstream leaves the instruction as it is then. */
void spectre_bhb_patch_wa3(struct alt_instr *alt, __le32 *origptr,
			   __le32 *updptr, int nr_inst)
{
	(void)alt;
	(void)origptr;
	(void)updptr;
	(void)nr_inst;
}
