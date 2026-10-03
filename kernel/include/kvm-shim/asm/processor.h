/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ASM_PROCESSOR_H
#define KVM_SHIM_ASM_PROCESSOR_H
#ifndef __ASSEMBLY__
/* struct cpuinfo_x86 as KVM reads it (M131): the vendor, family/model, the
 * address widths and the capability words, filled once from CPUID. */
#include <linux/types.h>
#include <asm/cpufeatures.h>
#include <asm/processor-flags.h>
#include <asm/cpuid/api.h>
#include <asm/special_insns.h>
#include <asm/msr.h>

#define X86_VENDOR_INTEL     0
#define X86_VENDOR_CYRIX     1
#define X86_VENDOR_AMD       2
#define X86_VENDOR_UMC       3
#define X86_VENDOR_CENTAUR   5
#define X86_VENDOR_TRANSMETA 7
#define X86_VENDOR_NSC       8
#define X86_VENDOR_HYGON     9
#define X86_VENDOR_ZHAOXIN   10
#define X86_VENDOR_VORTEX    11
#define X86_VENDOR_NUM       12
#define X86_VENDOR_UNKNOWN   0xff

/* KVM's view of the boot CPU is its own object: lkpi's boot_cpu_data (used
 * by i915) has a smaller layout under the same name. */
#define boot_cpu_data kvm_boot_cpu_data

struct cpuinfo_x86 {
	union {
		struct {
			u8 x86_model;
			u8 x86;          /* family */
			u8 x86_vendor;
			u8 x86_reserved;
		};
		u32 x86_vfm;
	};
	u8 x86_stepping;
	int x86_tlbsize;
	u8 x86_virt_bits;
	u8 x86_phys_bits;
	u8 x86_cache_bits;
	u32 extended_cpuid_level;
	int cpuid_level;
	u32 x86_capability[NCAPINTS + NBUGINTS];
	char x86_vendor_id[16];
	char x86_model_id[64];
	unsigned int x86_cache_size;
	int x86_cache_alignment;
	int x86_power;
	unsigned long loops_per_jiffy;
	u64 ppin;
	u16 x86_clflush_size;
	u16 booted_cores;
	u16 logical_proc_id;
	u16 cpu_core_id;
	u16 cpu_die_id;
	u16 cpu_index;
	u32 microcode;
	u8 x86_cache_max_rmid;
	u8 x86_cache_occ_scale;
	u8 x86_cache_mbm_width_offset;
};

extern struct cpuinfo_x86 boot_cpu_data;
#define cpu_data(cpu) boot_cpu_data

/* Fill boot_cpu_data from CPUID; idempotent. Called by kvm-shim's module init
 * before any capability is read. */
void lkpi_x86_cpu_init(void);


#define TASK_SIZE_MAX  ((1UL << 47) - PAGE_SIZE)

/* The top of the current task's kernel stack is never asked for by KVM on
 * this path; defined for the prototypes that mention it. */
struct x86_hw_tss;

/* L1TF: which mitigation the kernel applies. b1nix applies none. */
enum l1tf_mitigations {
	L1TF_MITIGATION_OFF,
	L1TF_MITIGATION_AUTO,
	L1TF_MITIGATION_FLUSH_NOWARN,
	L1TF_MITIGATION_FLUSH,
	L1TF_MITIGATION_FLUSH_NOSMT,
	L1TF_MITIGATION_FULL,
	L1TF_MITIGATION_FULL_FORCE
};
extern enum l1tf_mitigations l1tf_mitigation;

/* The ITLB-multihit mitigation for huge pages KVM would apply by itself; off,
 * like the rest of b1nix's mitigations. */
extern bool itlb_multihit_kvm_mitigation;

/* The hardware TSS, whose address KVM puts in each VMCS's host state. */
struct x86_hw_tss {
	u32 reserved1;
	u64 sp0, sp1, sp2;
	u64 reserved2;
	u64 ist[7];
	u32 reserved3, reserved4;
	u16 reserved5;
	u16 io_bitmap_base;
} __attribute__((packed));
struct cpu_entry_area {
	struct {
		struct x86_hw_tss x86_tss;
	} tss;
};
/* b1nix's per-CPU TSS, seen as the area Linux keeps it in. */
#define get_cpu_entry_area(cpu) ((struct cpu_entry_area *)lkpi_x86_tss_base(cpu))
unsigned long lkpi_x86_tss_base(int cpu);
#endif
#endif
