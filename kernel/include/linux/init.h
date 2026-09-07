/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_INIT_H
#define LKPI_LINUX_INIT_H
#include <linux/compiler.h>
#define __initdata
#define __initconst
#define __exitdata
#define subsys_initcall(fn)   struct lkpi_initcall_subsys_unused
#define late_initcall(fn)     struct lkpi_initcall_late_unused
#define postcore_initcall(fn) struct lkpi_initcall_postcore_unused
/*
 * Initcalls.
 *
 * b1nix has no initcall sections and calls its subsystems from kernel/main.c in
 * an order it chooses. But an imported filesystem's entry point is `static` —
 * `late_initcall(init_btrfs_fs)` is the only thing that makes it reachable —
 * so expanding to nothing would leave the filesystem in the image and
 * unstartable.
 *
 * So the macro emits a wrapper with a name derived from the function's:
 * `late_initcall(init_btrfs_fs)` gives `lkpi_initcall_init_btrfs_fs()`. The
 * bridge calls that. It is the one thing that has to be reachable from outside,
 * and this is how it becomes so without editing the imported source.
 */
#ifndef fs_initcall
#define LKPI_INITCALL(fn) \
	int lkpi_initcall_##fn(void); \
	int lkpi_initcall_##fn(void) { return fn(); }

#define fs_initcall(fn)          LKPI_INITCALL(fn)
#define subsys_initcall(fn)      LKPI_INITCALL(fn)
#define device_initcall(fn)      LKPI_INITCALL(fn)
#define late_initcall(fn)        LKPI_INITCALL(fn)
#define core_initcall(fn)        LKPI_INITCALL(fn)
#define postcore_initcall(fn)    LKPI_INITCALL(fn)
#define arch_initcall(fn)        LKPI_INITCALL(fn)
#define module_init(fn)          LKPI_INITCALL(fn)
#define module_exit(fn)
#endif

#endif
