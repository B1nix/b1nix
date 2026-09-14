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
/*
 * The symbol is not pasted into the name: 6.x wraps some exports in macros
 * (lzo's LZO_SAFE(sym)), and pasting an unexpanded `)` is ill-formed. A
 * repeated extern declaration of one object is valid C at any count.
 */
#define LKPI_EXPORT_MARKER            extern int lkpi_export_marker_unused
#define EXPORT_SYMBOL(sym)            LKPI_EXPORT_MARKER
#define EXPORT_SYMBOL_GPL(sym)        LKPI_EXPORT_MARKER
#define EXPORT_SYMBOL_NS(sym, ns)     LKPI_EXPORT_MARKER
#define EXPORT_SYMBOL_NS_GPL(sym, ns) LKPI_EXPORT_MARKER
#define EXPORT_SYMBOL_FOR_MODULES(sym, mods) LKPI_EXPORT_MARKER
#define MODULE_IMPORT_NS(ns)          LKPI_EXPORT_MARKER

#endif
