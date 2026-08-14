/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_EXPORT_H
#define LKPI_LINUX_EXPORT_H

/*
 * Symbol export markers.
 *
 * b1nix links the DRM core into the kernel image, so there is no module symbol
 * table for these to populate and no GPL-vs-non-GPL distinction to enforce at
 * link time. They exist so imported source compiles unmodified.
 */
/*
 * Each of these is written at file scope followed by a semicolon, so expanding
 * to nothing leaves a stray `;` that C11 rejects. Expanding to a harmless
 * declaration consumes it, which is what Linux's own no-module build does.
 */
#define EXPORT_SYMBOL(sym)            struct lkpi_export_##sym##_unused
#define EXPORT_SYMBOL_GPL(sym)        struct lkpi_export_gpl_##sym##_unused
#define EXPORT_SYMBOL_NS(sym, ns)     struct lkpi_export_ns_##sym##_unused
#define EXPORT_SYMBOL_NS_GPL(sym, ns) struct lkpi_export_nsgpl_##sym##_unused
#define MODULE_IMPORT_NS(ns)          struct lkpi_import_ns_unused

#endif
