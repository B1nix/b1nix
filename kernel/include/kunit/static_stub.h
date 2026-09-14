/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_KUNIT_STATIC_STUB_H
#define LKPI_KUNIT_STATIC_STUB_H

/* KUnit is not built, so a static stub never redirects: the macro is the
 * upstream !CONFIG_KUNIT form, a statement that does nothing. */
#define KUNIT_STATIC_STUB_REDIRECT(real_fn_name, args...) do {} while (0)

#endif
