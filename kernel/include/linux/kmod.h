/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_KMOD_H
#define LKPI_LINUX_KMOD_H

/*
 * On-demand module loading. request_module() itself lives in <linux/module.h>
 * here; the quota code asks for this header by name, and every format it could
 * ask to load is built in and registers itself at init, so the request fails
 * and the caller reports an unknown format rather than hanging.
 */
#include <linux/module.h>

#endif /* LKPI_LINUX_KMOD_H */
