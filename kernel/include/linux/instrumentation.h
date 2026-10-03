/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_INSTRUMENTATION_H
#define LKPI_LINUX_INSTRUMENTATION_H
/* objtool's noinstr bookkeeping: nothing checks it here. */
#define instrumentation_begin() do { } while (0)
#define instrumentation_end()   do { } while (0)
#endif
