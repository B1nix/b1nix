/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_MODULE_H
#define LKPI_LINUX_MODULE_H
#include <linux/export.h>
#include <linux/init.h>
/* The DRM core is linked into the kernel image, so module metadata has nowhere
 * to go and module_init/exit are never called by a loader — the kernel calls
 * the init functions directly. The macros exist so imported source compiles. */
struct module;
/* Same reason as EXPORT_SYMBOL: written at file scope with a semicolon. */
#define MODULE_AUTHOR(x)          struct lkpi_modmeta_author_unused
#define MODULE_DESCRIPTION(x)     struct lkpi_modmeta_desc_unused
#define MODULE_LICENSE(x)         struct lkpi_modmeta_license_unused
#define MODULE_PARM_DESC(p, d)    struct lkpi_modmeta_parm_unused
#define MODULE_FIRMWARE(x)        struct lkpi_modmeta_fw_unused
#define MODULE_DEVICE_TABLE(t, n) struct lkpi_modmeta_devtbl_unused
#define THIS_MODULE ((struct module *)0)
/*
 * An initcall the kernel calls directly. The imported function is static, so
 * the macro emits a wrapper under a name derived from it — that is the only way
 * to reach a subsystem's init without editing the source it lives in, which is
 * the rule the whole import rests on.
 */
#define module_init(fn) int lkpi_initcall_##fn(void) { return fn(); }
#define module_exit(fn) void lkpi_exitcall_##fn(void) { fn(); }
#define module_param_named(name, var, type, perm) struct lkpi_mp_##name##_unused
#define module_param_unsafe(name, type, perm) struct lkpi_mpu_##name##_unused
#define module_param(name, type, perm) struct lkpi_mpp_##name##_unused
/* Loading a module by name. b1nix has loadable modules (M95/M96) but nothing
 * registers an i2c encoder as one, so the request finds nothing — which is the
 * honest answer, and the caller then reports no encoder rather than waiting for
 * one that will never appear. */
static inline int request_module(const char *fmt, ...) { (void)fmt; return -1; }

#define try_module_get(m) (1)
#define module_put(m) do { } while (0)
#endif
