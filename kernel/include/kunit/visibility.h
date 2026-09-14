/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_KUNIT_VISIBILITY_H
#define LKPI_KUNIT_VISIBILITY_H
#include <linux/export.h>

/* KUnit is not built: symbols exported only for tests stay static. The marker
 * consumes the `;` that follows at file scope (see <linux/export.h>). */
#define VISIBLE_IF_KUNIT static
#define EXPORT_SYMBOL_IF_KUNIT(symbol) LKPI_EXPORT_MARKER

#endif
