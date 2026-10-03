/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ARM64_LINUX_CPU_PM_H
#define KVM_SHIM_ARM64_LINUX_CPU_PM_H
#include <linux/notifier.h>
/* CPU low-power notifications. b1nix's idle never loses a CPU's state (WFI
 * only); the one state loss, suspend, takes virtualization down and back up
 * through its own hooks (kvm_hook_cpus_down/up). So there is nothing to
 * notify, and registration succeeds with nothing ever sent. */
enum cpu_pm_event { CPU_PM_ENTER, CPU_PM_ENTER_FAILED, CPU_PM_EXIT,
		    CPU_CLUSTER_PM_ENTER, CPU_CLUSTER_PM_ENTER_FAILED, CPU_CLUSTER_PM_EXIT };
static inline int cpu_pm_register_notifier(struct notifier_block *nb) { (void)nb; return 0; }
static inline int cpu_pm_unregister_notifier(struct notifier_block *nb) { (void)nb; return 0; }
#endif
