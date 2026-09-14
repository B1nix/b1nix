/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_KUNIT_TEST_BUG_H
#define LKPI_KUNIT_TEST_BUG_H

/* KUnit is not built: no test is ever running. Upstream's !CONFIG_KUNIT form. */
struct kunit;
static inline struct kunit *kunit_get_current_test(void) { return (struct kunit *)0; }
#define kunit_fail_current_test(fmt, ...) do {} while (0)

#endif
