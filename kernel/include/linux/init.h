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
#endif
