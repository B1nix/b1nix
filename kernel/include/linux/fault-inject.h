/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_FAULT_INJECT_H
#define LKPI_LINUX_FAULT_INJECT_H
#include <linux/types.h>
/* Debug-only failure injection. Never enabled here, and should_fail therefore
 * always answers no — the honest answer, since nothing is injecting. */
struct fault_attr { int dummy; };
#define DECLARE_FAULT_ATTR(name) struct fault_attr name = { 0 }
static inline bool should_fail(struct fault_attr *attr, ssize_t size)
{ (void)attr; (void)size; return false; }
#endif
