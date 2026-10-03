/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ASM_PERCPU_H
#define KVM_SHIM_ASM_PERCPU_H
/* KVM's per-CPU variables are not GS-relative here (see linux/percpu.h).
 * The entry code reads two of them with %gs-relative operands, and those two
 * are mirrored into b1nix's struct percpu, which GS names: in assembly their
 * names are offsets into it (<b1nix/percpu_kvm.h>). svm_data is placed so
 * that its save_area_pa member lands on the mirror. */
#ifdef __ASSEMBLY__
#include <b1nix/percpu_kvm.h>
#define PER_CPU_VAR(var) %gs:0+(var)
#define x86_spec_ctrl_current PERCPU_KVM_SPEC_CTRL
#define svm_data (PERCPU_KVM_SVM_HSAVE_PA - SD_save_area_pa)
#else
#include <linux/percpu.h>
#endif
#endif
