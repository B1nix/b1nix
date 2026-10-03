// SPDX-License-Identifier: GPL-2.0-only
/*
 * The arm64 CPU as KVM reads it (M131): the system-wide ("sanitised") ID
 * register values, the capability bits its code and its alternatives test,
 * the errata it works around, the Spectre states it reports to a guest, and
 * the handful of kernel globals Linux keeps about the CPU.
 *
 * The per-field tables the sanitised values are folded with are upstream's,
 * generated from cpufeature.c (tools/import/kvm/gen-arm64-ftr.py). The
 * capabilities are decided here from those values, field by field, the way
 * upstream's arm64_features[] decides them; a capability that describes
 * something the host kernel itself would have to switch on (pointer
 * authentication, MTE, SVE, CnP, pseudo-NMI, KPTI) stays clear, because
 * b1nix does not switch it on and KVM must not assume it did. Errata are
 * matched by MIDR on every CPU, from upstream's lists.
 */
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/bitmap.h>
#include <linux/percpu.h>
#include <linux/jump_label.h>
#include <linux/kvm_host.h>
#include <asm/cpufeature.h>
#include <linux/irqchip/arm-gic-v3.h>
#include <asm/cache.h>
#include <asm/cputype.h>
#include <asm/esr.h>
#include <asm/spectre.h>
#include <asm/sysreg.h>
#include <asm/traps.h>
#include <asm/vectors.h>
#include <asm/virt.h>
#include <b1nix/kvm_bridge.h>

DECLARE_BITMAP(system_cpucaps, ARM64_NCAPS);
DECLARE_BITMAP(boot_cpucaps, ARM64_NCAPS);

/* Kernel mappings are global: b1nix runs every address space on ASID 0. */
bool arm64_use_ng_mappings;
unsigned long prot_ns_shared;

/* The instruction cache's aliasing, from CTR_EL0.L1Ip. */
unsigned long __icache_flags;

/* No mismatched-AArch32 support: 32-bit EL0 is all CPUs' or none's. */
DEFINE_STATIC_KEY_FALSE(arm64_mismatched_32bit_el0);

/* The host's vector table per CPU, which KVM puts back after a world
 * switch, and the branch-predictor hardening slot (none applied). */
DEFINE_PER_CPU(const char *, this_cpu_vector);
DEFINE_PER_CPU(struct bp_hardening_data, bp_hardening_data);

/* The exception level every CPU entered the kernel at. */
u32 __boot_cpu_mode[2];

/* ── sanitised ID registers ───────────────────────────────────────────── */

unsigned int lkpi_arm64_ftr_count(void);
u32 lkpi_arm64_ftr_sys_id(unsigned int i);
void lkpi_arm64_ftr_init(u32 sys_id, u64 val);
void lkpi_arm64_ftr_update(u32 sys_id, u64 val);

#define FTR_MAX 64

struct ftr_snapshot {
	u64 val[FTR_MAX];
	bool valid[FTR_MAX];
	u32 midr;
	u64 ctr;
};

/* The registers this CPU can be asked for: the AArch32 view only with
 * AArch32 at EL0, MPAMIDR only with MPAM, GMID only with MTE2 (the others
 * are RAZ when absent, these trap). */
static bool ftr_readable(u32 sys_id, u64 pfr0, u64 pfr1)
{
	if (sys_id == SYS_MPAMIDR_EL1)
		return cpuid_feature_extract_unsigned_field(pfr0, ID_AA64PFR0_EL1_MPAM_SHIFT) != 0;
	if (sys_id == SYS_GMID_EL1)
		return cpuid_feature_extract_unsigned_field(pfr1, ID_AA64PFR1_EL1_MTE_SHIFT) >=
		       ID_AA64PFR1_EL1_MTE_MTE2;
	if (sys_reg_Op0(sys_id) == 3 && sys_reg_Op1(sys_id) == 0 &&
	    sys_reg_CRn(sys_id) == 0 && sys_reg_CRm(sys_id) >= 1 &&
	    sys_reg_CRm(sys_id) <= 3)
		return cpuid_feature_extract_unsigned_field(pfr0, ID_AA64PFR0_EL1_EL0_SHIFT) ==
		       ID_AA64PFR0_EL1_EL0_AARCH32;
	return true;
}

static u64 read_ftr(u32 sys_id)
{
	if (sys_id == SYS_MPAMIDR_EL1)
		return read_sysreg_s(SYS_MPAMIDR_EL1);
	if (sys_id == SYS_GMID_EL1)
		return read_sysreg_s(SYS_GMID_EL1);
	return __read_sysreg_by_encoding(sys_id);
}

static void ftr_snapshot_this_cpu(void *arg)
{
	struct ftr_snapshot *s = arg;
	u64 pfr0 = read_sysreg_s(SYS_ID_AA64PFR0_EL1);
	u64 pfr1 = read_sysreg_s(SYS_ID_AA64PFR1_EL1);

	for (unsigned int i = 0; i < lkpi_arm64_ftr_count() && i < FTR_MAX; i++) {
		u32 id = lkpi_arm64_ftr_sys_id(i);

		s->valid[i] = ftr_readable(id, pfr0, pfr1);
		s->val[i] = s->valid[i] ? read_ftr(id) : 0;
	}
	s->midr = read_cpuid_id();
	s->ctr = read_cpuid_cachetype();
}

/* ── errata, by MIDR ──────────────────────────────────────────────────── */

bool is_midr_in_range_list(struct midr_range const *ranges)
{
	u32 midr = read_cpuid_id();

	for (; ranges && ranges->model; ranges++)
		if (midr_is_cpu_model_range(midr, ranges->model, ranges->rv_min,
					    ranges->rv_max))
			return true;
	return false;
}

static bool midr_in(u32 midr, const struct midr_range *ranges)
{
	for (; ranges->model; ranges++)
		if (midr_is_cpu_model_range(midr, ranges->model, ranges->rv_min,
					    ranges->rv_max))
			return true;
	return false;
}

/* Errata 1165522, 1319367, 1530923: speculative AT with the wrong regime. */
static const struct midr_range speculative_at_cpus[] = {
	MIDR_RANGE(MIDR_CORTEX_A76, 0, 0, 2, 0),
	MIDR_ALL_VERSIONS(MIDR_CORTEX_A57),
	MIDR_ALL_VERSIONS(MIDR_CORTEX_A72),
	MIDR_RANGE(MIDR_CORTEX_A55, 0, 0, 2, 0),
	MIDR_REV(MIDR_QCOM_KRYO_4XX_SILVER, 0xd, 0xe),
	{}
};

/* Broadcast TLBI completion not guaranteed: Falkor 1009, erratum 1286807,
 * 2441007, 2441009, 4118414. */
static const struct midr_range repeat_tlbi_cpus[] = {
	MIDR_REV(MIDR_QCOM_FALKOR_V1, 0, 0),
	MIDR_RANGE(MIDR_CORTEX_A76, 0, 0, 3, 0),
	MIDR_RANGE(MIDR_QCOM_KRYO_4XX_GOLD, 0xc, 0xe, 0xf, 0xe),
	MIDR_ALL_VERSIONS(MIDR_CORTEX_A55),
	MIDR_RANGE(MIDR_CORTEX_A510, 0, 0, 1, 1),
	MIDR_ALL_VERSIONS(MIDR_C1_PREMIUM),
	MIDR_ALL_VERSIONS(MIDR_C1_ULTRA),
	MIDR_ALL_VERSIONS(MIDR_CORTEX_A76AE),
	MIDR_ALL_VERSIONS(MIDR_CORTEX_A77),
	MIDR_ALL_VERSIONS(MIDR_CORTEX_A78),
	MIDR_ALL_VERSIONS(MIDR_CORTEX_A78AE),
	MIDR_ALL_VERSIONS(MIDR_CORTEX_A78C),
	MIDR_ALL_VERSIONS(MIDR_CORTEX_A710),
	MIDR_ALL_VERSIONS(MIDR_CORTEX_X1),
	MIDR_ALL_VERSIONS(MIDR_CORTEX_X1C),
	MIDR_ALL_VERSIONS(MIDR_CORTEX_X2),
	MIDR_ALL_VERSIONS(MIDR_CORTEX_X3),
	MIDR_ALL_VERSIONS(MIDR_CORTEX_X4),
	MIDR_ALL_VERSIONS(MIDR_CORTEX_X925),
	MIDR_ALL_VERSIONS(MIDR_NEOVERSE_N1),
	MIDR_ALL_VERSIONS(MIDR_NEOVERSE_N2),
	MIDR_ALL_VERSIONS(MIDR_NEOVERSE_V1),
	MIDR_ALL_VERSIONS(MIDR_NEOVERSE_V2),
	MIDR_ALL_VERSIONS(MIDR_NEOVERSE_V3),
	MIDR_ALL_VERSIONS(MIDR_NEOVERSE_V3AE),
	MIDR_ALL_VERSIONS(MIDR_NVIDIA_OLYMPUS),
	MIDR_ALL_VERSIONS(MIDR_MICROSOFT_AZURE_COBALT_100),
	{}
};

static const struct midr_range a57_834220[] = { MIDR_RANGE(MIDR_CORTEX_A57, 0, 0, 1, 2), {} };
static const struct midr_range a77_1508412[] = { MIDR_RANGE(MIDR_CORTEX_A77, 0, 0, 1, 0), {} };
static const struct midr_range a510_2077057[] = { MIDR_REV_RANGE(MIDR_CORTEX_A510, 0, 0, 2), {} };
static const struct midr_range ampere_ac03[] = {
	MIDR_ALL_VERSIONS(MIDR_AMPERE1), MIDR_ALL_VERSIONS(MIDR_AMPERE1A), {}
};
static const struct midr_range oryon_cntvoff[] = { MIDR_ALL_VERSIONS(MIDR_QCOM_ORYON_X1), {} };
static const struct midr_range tx2_family[] = {
	MIDR_ALL_VERSIONS(MIDR_BRCM_VULCAN), MIDR_ALL_VERSIONS(MIDR_CAVIUM_THUNDERX2), {}
};

/* Falkor erratum 1009 also covers Kryo parts, matched on part-number bits. */
static bool is_kryo(u32 midr)
{
	u32 model = midr & (MIDR_IMPLEMENTOR_MASK | (0xf00 << MIDR_PARTNUM_SHIFT) |
			    MIDR_ARCHITECTURE_MASK);

	return model == MIDR_QCOM_KRYO;
}

/* ── capabilities ─────────────────────────────────────────────────────── */

struct field_cap {
	u16 cap;
	u32 sys_reg;
	u8 shift;
	u8 width;
	bool sign;
	u8 min;
};

#define FIELD_CAP(c, reg, field, min_value)				\
	{ .cap = c, .sys_reg = SYS_##reg, .shift = reg##_##field##_SHIFT,	\
	  .width = reg##_##field##_WIDTH, .sign = reg##_##field##_SIGNED,	\
	  .min = SYS_FIELD_VALUE(reg, field, min_value) }

/* A feature this CPU (and every CPU, the values being sanitised) has. */
static const struct field_cap field_caps[] = {
	FIELD_CAP(ARM64_HAS_ECV, ID_AA64MMFR0_EL1, ECV, IMP),
	FIELD_CAP(ARM64_HAS_ECV_CNTPOFF, ID_AA64MMFR0_EL1, ECV, CNTPOFF),
	FIELD_CAP(ARM64_HAS_PAN, ID_AA64MMFR1_EL1, PAN, IMP),
	FIELD_CAP(ARM64_HAS_EPAN, ID_AA64MMFR1_EL1, PAN, PAN3),
	FIELD_CAP(ARM64_HAS_LSE_ATOMICS, ID_AA64ISAR0_EL1, ATOMIC, IMP),
	FIELD_CAP(ARM64_HAS_32BIT_EL1, ID_AA64PFR0_EL1, EL1, AARCH32),
	FIELD_CAP(ARM64_HAS_HCX, ID_AA64MMFR1_EL1, HCX, IMP),
	FIELD_CAP(ARM64_HAS_FPSIMD, ID_AA64PFR0_EL1, FP, IMP),
	FIELD_CAP(ARM64_HAS_DCPOP, ID_AA64ISAR1_EL1, DPB, IMP),
	FIELD_CAP(ARM64_HAS_DCPODP, ID_AA64ISAR1_EL1, DPB, DPB2),
	FIELD_CAP(ARM64_HAS_RAS_EXTN, ID_AA64PFR0_EL1, RAS, IMP),
	FIELD_CAP(ARM64_HAS_STAGE2_FWB, ID_AA64MMFR2_EL1, FWB, IMP),
	FIELD_CAP(ARM64_HAS_ARMv8_4_TTL, ID_AA64MMFR2_EL1, TTL, IMP),
	FIELD_CAP(ARM64_HAS_TLB_RANGE, ID_AA64ISAR0_EL1, TLB, RANGE),
	FIELD_CAP(ARM64_HAFT, ID_AA64MMFR1_EL1, HAFDBS, HAFT),
	FIELD_CAP(ARM64_HAS_CRC32, ID_AA64ISAR0_EL1, CRC32, IMP),
	FIELD_CAP(ARM64_SSBS, ID_AA64PFR1_EL1, SSBS, IMP),
	FIELD_CAP(ARM64_HAS_SB, ID_AA64ISAR1_EL1, SB, IMP),
	FIELD_CAP(ARM64_HAS_E0PD, ID_AA64MMFR2_EL1, E0PD, IMP),
	FIELD_CAP(ARM64_HAS_RNG, ID_AA64ISAR0_EL1, RNDR, IMP),
	FIELD_CAP(ARM64_HAS_LDAPR, ID_AA64ISAR1_EL1, LRCPC, IMP),
	FIELD_CAP(ARM64_HAS_FGT, ID_AA64MMFR0_EL1, FGT, IMP),
	FIELD_CAP(ARM64_HAS_FGT2, ID_AA64MMFR0_EL1, FGT, FGT2),
	FIELD_CAP(ARM64_HAS_WFXT, ID_AA64ISAR2_EL1, WFxT, IMP),
	FIELD_CAP(ARM64_HAS_TIDCP1, ID_AA64MMFR1_EL1, TIDCP1, IMP),
	FIELD_CAP(ARM64_HAS_DIT, ID_AA64PFR0_EL1, DIT, IMP),
	FIELD_CAP(ARM64_HAS_MOPS, ID_AA64ISAR2_EL1, MOPS, IMP),
	FIELD_CAP(ARM64_HAS_TCR2, ID_AA64MMFR3_EL1, TCRX, IMP),
	FIELD_CAP(ARM64_HAS_S1PIE, ID_AA64MMFR3_EL1, S1PIE, IMP),
	FIELD_CAP(ARM64_HAS_EVT, ID_AA64MMFR2_EL1, EVT, IMP),
	FIELD_CAP(ARM64_HAS_FPMR, ID_AA64PFR2_EL1, FPMR, IMP),
	FIELD_CAP(ARM64_HAS_S1POE, ID_AA64MMFR3_EL1, S1POE, IMP),
	FIELD_CAP(ARM64_HAS_GCS, ID_AA64PFR1_EL1, GCS, IMP),
	FIELD_CAP(ARM64_HAS_SCTLR2, ID_AA64MMFR3_EL1, SCTLRX, IMP),
	FIELD_CAP(ARM64_HAS_GICV5_CPUIF, ID_AA64PFR2_EL1, GCIE, IMP),
};

static bool field_has(const struct field_cap *f)
{
	u64 val = read_sanitised_ftr_reg(f->sys_reg);
	s64 v = f->sign ? cpuid_feature_extract_signed_field_width(val, f->shift, f->width)
			: (s64)cpuid_feature_extract_unsigned_field_width(val, f->shift, f->width);

	return v >= (s64)f->min;
}

static void set_cap(unsigned int cap)
{
	__set_bit(cap, system_cpucaps);
	__set_bit(cap, boot_cpucaps);
}

static bool has_rasv1p1(void)
{
	u64 pfr0 = read_sanitised_ftr_reg(SYS_ID_AA64PFR0_EL1);
	u64 pfr1 = read_sanitised_ftr_reg(SYS_ID_AA64PFR1_EL1);
	unsigned int ras = cpuid_feature_extract_unsigned_field(pfr0, ID_AA64PFR0_EL1_RAS_SHIFT);
	unsigned int frac = cpuid_feature_extract_unsigned_field(pfr1, ID_AA64PFR1_EL1_RAS_frac_SHIFT);

	return ras >= ID_AA64PFR0_EL1_RAS_V1P1 ||
	       (ras == ID_AA64PFR0_EL1_RAS_IMP && frac >= ID_AA64PFR1_EL1_RAS_frac_RASv1p1);
}

static bool has_pmuv3(void)
{
	u64 dfr0 = read_sanitised_ftr_reg(SYS_ID_AA64DFR0_EL1);
	unsigned int pmuver = cpuid_feature_extract_unsigned_field(dfr0,
						ID_AA64DFR0_EL1_PMUVer_SHIFT);

	return pmuver != ID_AA64DFR0_EL1_PMUVer_IMP_DEF &&
	       pmuver >= ID_AA64DFR0_EL1_PMUVer_IMP;
}

static bool has_gicv3_cpuif(void)
{
	u64 pfr0 = read_sanitised_ftr_reg(SYS_ID_AA64PFR0_EL1);

	/* The system-register interface is there and b1nix's GIC driver
	 * enabled it (ICC_SRE_EL2.SRE, at boot). */
	return cpuid_feature_extract_unsigned_field(pfr0, ID_AA64PFR0_EL1_GIC_SHIFT) >=
	       ID_AA64PFR0_EL1_GIC_IMP && b1nix_kvm_gic_v3() &&
	       (read_sysreg_s(SYS_ICC_SRE_EL1) & ICC_SRE_EL1_SRE);
}

static bool has_nested_virt(void)
{
	u64 mmfr2 = read_sanitised_ftr_reg(SYS_ID_AA64MMFR2_EL1);

	return kvm_get_mode() == KVM_MODE_NV &&
	       cpuid_feature_extract_unsigned_field(mmfr2, ID_AA64MMFR2_EL1_NV_SHIFT) >=
	       ID_AA64MMFR2_EL1_NV_NV2;
}

static bool has_nv1(void)
{
	u64 mmfr4 = read_sanitised_ftr_reg(SYS_ID_AA64MMFR4_EL1);
	s64 e2h0 = cpuid_feature_extract_signed_field(mmfr4, ID_AA64MMFR4_EL1_E2H0_SHIFT);

	return test_bit(ARM64_HAS_NESTED_VIRT, system_cpucaps) &&
	       !(e2h0 <= (s64)ID_AA64MMFR4_EL1_E2H0_NI_NV1);
}

static struct ftr_snapshot cpu_snap[NR_CPUS];

/* Build the sanitised registers from every CPU, then the capabilities and
 * errata from them. Called once, before KVM's init, with every CPU up. */
int lkpi_arm64_cpu_init(void)
{
	int ncpu = b1nix_kvm_cpu_count();
	u64 ctr;
	unsigned int n = lkpi_arm64_ftr_count();

	if (n > FTR_MAX) {
		pr_err("kvm: %u feature registers, room for %u\n", n, FTR_MAX);
		return -ENOSPC;
	}
	for (int cpu = 0; cpu < ncpu && cpu < NR_CPUS; cpu++)
		if (b1nix_kvm_cpu_present(cpu))
			b1nix_kvm_call_on_cpu(cpu, ftr_snapshot_this_cpu, &cpu_snap[cpu], 1);
	for (unsigned int i = 0; i < n; i++) {
		u32 id = lkpi_arm64_ftr_sys_id(i);
		bool first = true;

		for (int cpu = 0; cpu < ncpu && cpu < NR_CPUS; cpu++) {
			if (!b1nix_kvm_cpu_present(cpu) || !cpu_snap[cpu].valid[i])
				continue;
			if (first)
				lkpi_arm64_ftr_init(id, cpu_snap[cpu].val[i]);
			else
				lkpi_arm64_ftr_update(id, cpu_snap[cpu].val[i]);
			first = false;
		}
	}

	/* The kernel runs at EL2 on every CPU or on none (boot.S). */
	__boot_cpu_mode[0] = __boot_cpu_mode[1] =
		b1nix_kvm_at_el2() ? BOOT_CPU_MODE_EL2 : BOOT_CPU_MODE_EL1;

	set_cap(ARM64_ALWAYS_BOOT);
	set_cap(ARM64_ALWAYS_SYSTEM);
	if (b1nix_kvm_at_el2())
		set_cap(ARM64_HAS_VIRT_HOST_EXTN);
	for (unsigned int i = 0; i < ARRAY_SIZE(field_caps); i++)
		if (field_has(&field_caps[i]))
			set_cap(field_caps[i].cap);
	if (has_gicv3_cpuif())
		set_cap(ARM64_HAS_GICV3_CPUIF);
	if (has_rasv1p1())
		set_cap(ARM64_HAS_RASV1P1_EXTN);
	if (has_pmuv3())
		set_cap(ARM64_HAS_PMUV3);
	ctr = read_sanitised_ftr_reg(SYS_CTR_EL0);
	if (ctr & BIT(CTR_EL0_IDC_SHIFT))
		set_cap(ARM64_HAS_CACHE_IDC);
	if (ctr & BIT(CTR_EL0_DIC_SHIFT))
		set_cap(ARM64_HAS_CACHE_DIC);
	if (SYS_FIELD_GET(CTR_EL0, L1Ip, ctr) == CTR_EL0_L1Ip_VIPT)
		set_bit(ICACHEF_ALIASING, &__icache_flags);
	if (has_nested_virt())
		set_cap(ARM64_HAS_NESTED_VIRT);
	if (has_nv1())
		set_cap(ARM64_HAS_HCR_NV1);

	for (int cpu = 0; cpu < ncpu && cpu < NR_CPUS; cpu++) {
		u32 midr = cpu_snap[cpu].midr;

		if (!b1nix_kvm_cpu_present(cpu))
			continue;
		if (midr_in(midr, speculative_at_cpus))
			set_cap(ARM64_WORKAROUND_SPECULATIVE_AT);
		if (midr_in(midr, repeat_tlbi_cpus) || is_kryo(midr))
			set_cap(ARM64_WORKAROUND_REPEAT_TLBI);
		if (midr_in(midr, a57_834220))
			set_cap(ARM64_WORKAROUND_834220);
		if (midr_in(midr, a77_1508412))
			set_cap(ARM64_WORKAROUND_1508412);
		if (midr_in(midr, a510_2077057))
			set_cap(ARM64_WORKAROUND_2077057);
		if (midr_in(midr, ampere_ac03))
			set_cap(ARM64_WORKAROUND_AMPERE_AC03_CPU_38);
		if (midr_in(midr, oryon_cntvoff))
			set_cap(ARM64_WORKAROUND_QCOM_ORYON_CNTVOFF);
		if (midr_in(midr, tx2_family) && b1nix_kvm_at_el2() &&
		    MPIDR_AFFINITY_LEVEL(b1nix_kvm_cpu_mpidr(cpu), 0) != 0)
			set_cap(ARM64_WORKAROUND_CAVIUM_TX2_219_TVM);
		if (cpu_snap[cpu].ctr != cpu_snap[0].ctr)
			set_cap(ARM64_MISMATCHED_CACHE_TYPE);
	}

	for (int cpu = 0; cpu < ncpu && cpu < NR_CPUS; cpu++)
		if (b1nix_kvm_cpu_present(cpu))
			per_cpu(this_cpu_vector, cpu) =
				(const char *)(uintptr_t)b1nix_kvm_host_vectors();
	return 0;
}

bool this_cpu_has_cap(unsigned int cap)
{
	return cap < ARM64_NCAPS && test_bit(cap, system_cpucaps);
}

/* ── Spectre and friends, as reported to a guest ──────────────────────── */

/* b1nix applies no speculation mitigations of its own, so a CPU the
 * architecture does not declare unaffected is reported vulnerable. */
enum mitigation_state arm64_get_spectre_v2_state(void)
{
	u64 pfr0 = read_sanitised_ftr_reg(SYS_ID_AA64PFR0_EL1);

	return cpuid_feature_extract_unsigned_field(pfr0, ID_AA64PFR0_EL1_CSV2_SHIFT) ?
		SPECTRE_UNAFFECTED : SPECTRE_VULNERABLE;
}

enum mitigation_state arm64_get_meltdown_state(void)
{
	u64 pfr0 = read_sanitised_ftr_reg(SYS_ID_AA64PFR0_EL1);

	return cpuid_feature_extract_unsigned_field(pfr0, ID_AA64PFR0_EL1_CSV3_SHIFT) ?
		SPECTRE_UNAFFECTED : SPECTRE_VULNERABLE;
}

enum mitigation_state arm64_get_spectre_v4_state(void)
{
	return SPECTRE_VULNERABLE;
}

enum mitigation_state arm64_get_spectre_bhb_state(void)
{
	u64 mmfr1 = read_sanitised_ftr_reg(SYS_ID_AA64MMFR1_EL1);

	return cpuid_feature_extract_unsigned_field(mmfr1, ID_AA64MMFR1_EL1_ECBHB_SHIFT) ?
		SPECTRE_UNAFFECTED : SPECTRE_VULNERABLE;
}

/* ── exception reporting ──────────────────────────────────────────────── */

/* An SError's severity, as upstream's traps.c grades it. */
bool arm64_is_fatal_ras_serror(struct pt_regs *regs, unsigned long esr)
{
	unsigned long aet = arm64_ras_serror_get_severity(esr);

	(void)regs;
	switch (aet) {
	case ESR_ELx_AET_CE:
	case ESR_ELx_AET_UEO:
		return false;
	case ESR_ELx_AET_UEU:
	case ESR_ELx_AET_UER:
		return true;
	default:
		panic("kvm: uncontainable SError, ESR 0x%lx\n", esr);
	}
}

const char *esr_get_class_string(unsigned long esr)
{
	switch (ESR_ELx_EC(esr)) {
	case ESR_ELx_EC_UNKNOWN:	return "Unknown/Uncategorized";
	case ESR_ELx_EC_WFx:		return "WFI/WFE";
	case ESR_ELx_EC_CP15_32:	return "CP15 MCR/MRC";
	case ESR_ELx_EC_CP15_64:	return "CP15 MCRR/MRRC";
	case ESR_ELx_EC_CP14_MR:	return "CP14 MCR/MRC";
	case ESR_ELx_EC_CP14_LS:	return "CP14 LDC/STC";
	case ESR_ELx_EC_FP_ASIMD:	return "ASIMD";
	case ESR_ELx_EC_CP10_ID:	return "CP10 MRC/VMRS";
	case ESR_ELx_EC_PAC:		return "PAC";
	case ESR_ELx_EC_CP14_64:	return "CP14 MCRR/MRRC";
	case ESR_ELx_EC_BTI:		return "BTI";
	case ESR_ELx_EC_ILL:		return "PSTATE.IL";
	case ESR_ELx_EC_SVC32:		return "SVC (AArch32)";
	case ESR_ELx_EC_HVC32:		return "HVC (AArch32)";
	case ESR_ELx_EC_SMC32:		return "SMC (AArch32)";
	case ESR_ELx_EC_SVC64:		return "SVC (AArch64)";
	case ESR_ELx_EC_HVC64:		return "HVC (AArch64)";
	case ESR_ELx_EC_SMC64:		return "SMC (AArch64)";
	case ESR_ELx_EC_SYS64:		return "MSR/MRS (AArch64)";
	case ESR_ELx_EC_SVE:		return "SVE";
	case ESR_ELx_EC_ERET:		return "ERET/ERETAA/ERETAB";
	case ESR_ELx_EC_FPAC:		return "FPAC";
	case ESR_ELx_EC_SME:		return "SME";
	case ESR_ELx_EC_IMP_DEF:	return "EL3 IMP DEF";
	case ESR_ELx_EC_IABT_LOW:	return "IABT (lower EL)";
	case ESR_ELx_EC_IABT_CUR:	return "IABT (current EL)";
	case ESR_ELx_EC_PC_ALIGN:	return "PC Alignment";
	case ESR_ELx_EC_DABT_LOW:	return "DABT (lower EL)";
	case ESR_ELx_EC_DABT_CUR:	return "DABT (current EL)";
	case ESR_ELx_EC_SP_ALIGN:	return "SP Alignment";
	case ESR_ELx_EC_MOPS:		return "MOPS";
	case ESR_ELx_EC_FP_EXC32:	return "FP (AArch32)";
	case ESR_ELx_EC_FP_EXC64:	return "FP (AArch64)";
	case ESR_ELx_EC_GCS:		return "Guarded Control Stack";
	case ESR_ELx_EC_SERROR:		return "SError";
	case ESR_ELx_EC_BREAKPT_LOW:	return "Breakpoint (lower EL)";
	case ESR_ELx_EC_BREAKPT_CUR:	return "Breakpoint (current EL)";
	case ESR_ELx_EC_SOFTSTP_LOW:	return "Software Step (lower EL)";
	case ESR_ELx_EC_SOFTSTP_CUR:	return "Software Step (current EL)";
	case ESR_ELx_EC_WATCHPT_LOW:	return "Watchpoint (lower EL)";
	case ESR_ELx_EC_WATCHPT_CUR:	return "Watchpoint (current EL)";
	case ESR_ELx_EC_BKPT32:		return "BKPT (AArch32)";
	case ESR_ELx_EC_VECTOR32:	return "Vector catch (AArch32)";
	case ESR_ELx_EC_BRK64:		return "BRK (AArch64)";
	default:			return "UNRECOGNIZED EC";
	}
}

/* The instructions around a kernel address, for a hypervisor panic report. */
void dump_kernel_instr(unsigned long kaddr)
{
	const u32 *p = (const u32 *)(kaddr & ~3UL);

	pr_emerg("Code: %08x %08x %08x %08x (%08x)\n", p[-4], p[-3], p[-2], p[-1], p[0]);
}
