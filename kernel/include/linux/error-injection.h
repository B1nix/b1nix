/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_ERROR_INJECTION_H
#define LKPI_LINUX_ERROR_INJECTION_H

/*
 * Error injection.
 *
 * ALLOW_ERROR_INJECTION marks a function as one a debugging harness may force
 * to fail. It emits a section entry; there is no harness here to read it, so it
 * emits nothing. The macro still has to exist, because it appears at file scope
 * after a function definition and its absence is a syntax error rather than a
 * missing symbol.
 */
#define ALLOW_ERROR_INJECTION(fname, _etype)
#define EI_ETYPE_NULL 0
#define EI_ETYPE_ERRNO 1
#define EI_ETYPE_ERRNO_NULL 2
#define EI_ETYPE_TRUE 3

#endif
