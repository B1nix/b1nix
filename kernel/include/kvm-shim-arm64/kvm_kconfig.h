/* SPDX-License-Identifier: GPL-2.0-only */
/* The configuration arm64 KVM is built with here (M131): a VHE host (the
 * kernel runs at EL2 with HCR_EL2.E2H, see boot.S), the in-kernel vgic and
 * arch timer, and none of the pieces that need subsystems this kernel does
 * not have (VFIO, IRQ bypass, guest_memfd, the guest PMU) or CPU features
 * the kernel does not use itself (SVE, pointer authentication, MTE). Each
 * one left out is behind its own #ifdef upstream. */
#ifndef B1NIX_KVM_KCONFIG_H
#define B1NIX_KVM_KCONFIG_H

#define CONFIG_KVM 1
#define CONFIG_KVM_COMMON 1
#define CONFIG_KVM_GENERIC_HARDWARE_ENABLING 1
#define CONFIG_KVM_GENERIC_MMU_NOTIFIER 1
#define CONFIG_HAVE_KVM_CPU_RELAX_INTERCEPT 1
#define CONFIG_KVM_MMIO 1
#define CONFIG_KVM_GENERIC_DIRTYLOG_READ_PROTECT 1
#define CONFIG_VIRT_XFER_TO_GUEST_WORK 1
#define CONFIG_HAVE_KVM_DIRTY_RING 1
#define CONFIG_HAVE_KVM_DIRTY_RING_ACQ_REL 1
#define CONFIG_NEED_KVM_DIRTY_RING_WITH_BITMAP 1
#define CONFIG_HAVE_KVM_MSI 1
#define CONFIG_HAVE_KVM_IRQCHIP 1
#define CONFIG_HAVE_KVM_IRQ_ROUTING 1
#define CONFIG_HAVE_KVM_READONLY_MEM 1
#define CONFIG_HAVE_KVM_VCPU_RUN_PID_CHANGE 1
#define CONFIG_HAVE_KVM_IRQFD 1
#define CONFIG_HAVE_KVM_EVENTFD 1
#define CONFIG_EVENTFD 1
#define CONFIG_KVM_MAX_NR_VCPUS 512
#define CONFIG_ARM64 1
#define CONFIG_64BIT 1
#define CONFIG_SMP 1
#define CONFIG_NR_CPUS 64
#define CONFIG_MMU_NOTIFIER 1
#define CONFIG_PREEMPT_NOTIFIERS 1
#define CONFIG_HAS_IOMEM 1
#define CONFIG_ARM64_4K_PAGES 1
#define CONFIG_ARM64_PAGE_SHIFT 12
#define CONFIG_ARM64_VA_BITS 48
#define CONFIG_ARM64_CONT_PTE_SHIFT 4
#define CONFIG_ARM64_CONT_PMD_SHIFT 4
#define CONFIG_FUNCTION_ALIGNMENT 4
#define CONFIG_ARM64_PA_BITS 48
#define CONFIG_PGTABLE_LEVELS 4
#define CONFIG_ARM_GIC_V3 1
#define CONFIG_ARM_ARCH_TIMER 1
/* Upstream force-includes <linux/kconfig.h> ahead of everything; arm64's
 * capability headers use IS_ENABLED before anything else would define it.
 * The same tokens as <linux/kernel.h>, so the two definitions agree. */
#define __ARG_PLACEHOLDER_1 0,
#define __take_second_arg(__ignored, val, ...) val
#define ____is_defined(arg1_or_junk) __take_second_arg(arg1_or_junk 1, 0)
#define ___is_defined(val) ____is_defined(__ARG_PLACEHOLDER_##val)
#define __is_defined(x) ___is_defined(x)
#define IS_ENABLED(cfg)  __is_defined(cfg)
/* Upstream's compile-time assertion is an error attribute that fires only if
 * a call survives optimisation; this build has no equivalent, so the check is
 * not made. */
#ifndef compiletime_assert
#define compiletime_assert(condition, msg) do { } while (0)
#endif
/* The arch-level bit test arm64's cpufeature.h names: the generic one. */
#define arch_test_bit(nr, addr) test_bit(nr, addr)
#ifndef __LITTLE_ENDIAN_BITFIELD
#define __LITTLE_ENDIAN_BITFIELD
#define __LITTLE_ENDIAN 1234
#endif

#endif
