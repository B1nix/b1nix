/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_MODULEPARAM_H
#define LKPI_LINUX_MODULEPARAM_H
#include <linux/module.h>

/* A parameter with its own get/set. Nothing parses a command line into these
 * here — b1nix has its own cmdline handling — so a registered parameter keeps
 * its compiled-in default, and the ops exist because the definition names
 * them. */
struct kernel_param;
struct kernel_param_ops {
	unsigned int flags;
	int (*set)(const char *val, const struct kernel_param *kp);
	int (*get)(char *buffer, const struct kernel_param *kp);
	void (*free)(void *arg);
};

struct kernel_param {
	const char *name;
	const struct kernel_param_ops *ops;
	void *arg;
};


/*
 * Module parameters.
 *
 * b1nix has no module-parameter interface: i915 is built into the kernel and
 * its parameters keep their compiled-in defaults. Each of these declares an
 * unused struct tag so the statement at file scope is well-formed and the
 * parameter's variable is untouched — which is what leaves the default in
 * place. The cost is that none of these is settable at boot or through sysfs.
 */
#define module_param_named_unsafe(name, var, type, perm) struct lkpi_mpnu_##name##_unused
#define module_param_cb(name, ops, arg, perm)            struct lkpi_mpcb_##name##_unused
#define module_param_cb_unsafe(name, ops, arg, perm)     struct lkpi_mpcbu_##name##_unused
#define module_param_array(name, type, nump, perm)       struct lkpi_mpa_##name##_unused
#define module_param_string(name, str, len, perm)        struct lkpi_mps_##name##_unused
#define MODULE_PARM_DESC(name, desc)
#define kernel_param_lock(mod)   do { (void)(mod); } while (0)
#define kernel_param_unlock(mod) do { (void)(mod); } while (0)

/* The stock ops for the common types, named by parameters that do not supply
 * their own. Nothing calls them — see above. */
extern const struct kernel_param_ops param_ops_bool;
extern const struct kernel_param_ops param_ops_int;
extern const struct kernel_param_ops param_ops_uint;

#endif
