/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ASM_REBOOT_H
#define KVM_SHIM_ASM_REBOOT_H
/* Called on each CPU as the machine goes down in an emergency (a panic, a
 * crash restart), so VMX can be switched off first: a CPU left in VMX
 * operation ignores INIT, and a restart through INIT would hang. */
typedef void (cpu_emergency_virt_cb)(void);
void cpu_emergency_register_virt_callback(cpu_emergency_virt_cb *callback);
void cpu_emergency_unregister_virt_callback(cpu_emergency_virt_cb *callback);
#endif
