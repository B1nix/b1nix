/* SPDX-License-Identifier: GPL-2.0-only */
/* The configuration Linux's KVM is built with here (M131): the x86 core,
 * VMX and SVM (without SEV), an in-kernel irqchip, and none of the optional personalities (Hyper-V,
 * Xen, SMM, TDX, SEV, guest_memfd) or the pieces that need subsystems this
 * kernel does not have (VFIO, IRQ bypass, the dirty ring). Upstream's Kconfig
 * selects some of these unconditionally; each one left out here is left out
 * because its code is behind its own #ifdef, not because the source was
 * changed. */
#ifndef B1NIX_KVM_KCONFIG_H
#define B1NIX_KVM_KCONFIG_H

#define CONFIG_KVM 1
#define CONFIG_KVM_X86 1
#define CONFIG_KVM_INTEL 1
#define CONFIG_KVM_AMD 1
#define CONFIG_KVM_COMMON 1
#define CONFIG_KVM_GENERIC_MMU_NOTIFIER 1
#define CONFIG_KVM_ELIDE_TLB_FLUSH_IF_YOUNG 1
#define CONFIG_KVM_MMU_LOCKLESS_AGING 1
#define CONFIG_HAVE_KVM_IRQCHIP 1
#define CONFIG_HAVE_KVM_PFNCACHE 1
#define CONFIG_HAVE_KVM_IRQ_ROUTING 1
#define CONFIG_HAVE_KVM_READONLY_MEM 1
#define CONFIG_KVM_ASYNC_PF 1
#define CONFIG_KVM_MMIO 1
#define CONFIG_HAVE_KVM_MSI 1
#define CONFIG_HAVE_KVM_CPU_RELAX_INTERCEPT 1
#define CONFIG_HAVE_KVM_NO_POLL 1
#define CONFIG_HAVE_KVM_DIRTY_RING 1
#define CONFIG_HAVE_KVM_DIRTY_RING_TSO 1
#define CONFIG_HAVE_KVM_DIRTY_RING_ACQ_REL 1
#define CONFIG_VIRT_XFER_TO_GUEST_WORK 1
#define CONFIG_KVM_GENERIC_DIRTYLOG_READ_PROTECT 1
#define CONFIG_KVM_GENERIC_HARDWARE_ENABLING 1
#define CONFIG_KVM_GENERIC_PRE_FAULT_MEMORY 1
#define CONFIG_KVM_IOAPIC 1
#define CONFIG_KVM_MAX_NR_VCPUS 1024
#define CONFIG_X86_LOCAL_APIC 1
#define CONFIG_X86_IO_APIC 1
#define CONFIG_X86_64 1
#define CONFIG_X86 1
#define CONFIG_64BIT 1
#define CONFIG_SMP 1
#define CONFIG_NR_CPUS 64
#define CONFIG_MMU_NOTIFIER 1
#define CONFIG_PREEMPT_NOTIFIERS 1
#define CONFIG_USER_RETURN_NOTIFIER 1
#define CONFIG_HAVE_KVM_IRQFD 1
#define CONFIG_HAVE_KVM_EVENTFD 1
#define CONFIG_EVENTFD 1
#define CONFIG_IA32_FEAT_CTL 1
#define CONFIG_CPU_SUP_INTEL 1
#define CONFIG_CPU_SUP_AMD 1
#define CONFIG_HAS_IOMEM 1
#ifndef __LITTLE_ENDIAN_BITFIELD
#define __LITTLE_ENDIAN_BITFIELD
#define __LITTLE_ENDIAN 1234
#endif

#endif
