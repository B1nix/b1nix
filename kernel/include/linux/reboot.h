/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_REBOOT_H
#define LKPI_LINUX_REBOOT_H
#include <linux/notifier.h>
#define SYS_DOWN      0x0001
#define SYS_RESTART   SYS_DOWN
#define SYS_HALT      0x0002
#define SYS_POWER_OFF 0x0003
/* Whether a reboot is under way: imported code that must not start new work
 * while the machine goes down reads it. Never set while b1nix is running. */
extern int system_state_rebooting;
int register_reboot_notifier(struct notifier_block *nb);
int unregister_reboot_notifier(struct notifier_block *nb);
#endif
