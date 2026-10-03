// SPDX-License-Identifier: GPL-2.0-only
/*
 * The separate (nVHE) hypervisor's data, as arm64 KVM's host code names it
 * (M131). b1nix runs KVM as a VHE host only and builds no nVHE hypervisor
 * image, but the host code that would set one up is compiled in with the
 * rest of arm.c and mmu.c, behind is_kernel_in_hyp_mode(), and writes these
 * before it would copy them over: they have to exist, with the types the
 * headers give them. The code symbols are in kvm_arm64_nvhe.S.
 */
#include <linux/kvm_host.h>
#include <asm/kvm_asm.h>
#include <asm/kvm_hyp.h>
#include <asm/kvm_mmu.h>
#include <asm/kvm_pkvm.h>

struct fgt_masks kvm_nvhe_sym(hfgrtr_masks);
struct fgt_masks kvm_nvhe_sym(hfgwtr_masks);
struct fgt_masks kvm_nvhe_sym(hfgitr_masks);
struct fgt_masks kvm_nvhe_sym(hdfgrtr_masks);
struct fgt_masks kvm_nvhe_sym(hdfgwtr_masks);
struct fgt_masks kvm_nvhe_sym(hafgrtr_masks);
struct fgt_masks kvm_nvhe_sym(hfgrtr2_masks);
struct fgt_masks kvm_nvhe_sym(hfgwtr2_masks);
struct fgt_masks kvm_nvhe_sym(hfgitr2_masks);
struct fgt_masks kvm_nvhe_sym(hdfgrtr2_masks);
struct fgt_masks kvm_nvhe_sym(hdfgwtr2_masks);

u64 kvm_nvhe_sym(id_aa64pfr0_el1_sys_val);
u64 kvm_nvhe_sym(id_aa64pfr1_el1_sys_val);
u64 kvm_nvhe_sym(id_aa64isar0_el1_sys_val);
u64 kvm_nvhe_sym(id_aa64isar1_el1_sys_val);
u64 kvm_nvhe_sym(id_aa64isar2_el1_sys_val);
u64 kvm_nvhe_sym(id_aa64mmfr0_el1_sys_val);
u64 kvm_nvhe_sym(id_aa64mmfr1_el1_sys_val);
u64 kvm_nvhe_sym(id_aa64mmfr2_el1_sys_val);
u64 kvm_nvhe_sym(id_aa64smfr0_el1_sys_val);

u64 kvm_nvhe_sym(hyp_cpu_logical_map)[NR_CPUS];
unsigned long kvm_nvhe_sym(kvm_arm_hyp_percpu_base)[NR_CPUS];
struct memblock_region kvm_nvhe_sym(hyp_memory)[HYP_MEMBLOCK_REGIONS];
unsigned int kvm_nvhe_sym(hyp_memblock_nr);
s64 kvm_nvhe_sym(hyp_physvirt_offset);
unsigned int kvm_nvhe_sym(hyp_gicv3_nr_lr);
unsigned int kvm_nvhe_sym(kvm_arm_vmid_bits);
unsigned long kvm_nvhe_sym(__icache_flags);
/* <asm/kvm_hyp.h> maps the name onto the nVHE symbol, struct tag and all. */
#undef kvm_host_psci_config
struct kvm_host_psci_config kvm_nvhe_sym(kvm_host_psci_config);
