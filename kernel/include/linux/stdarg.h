/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_STDARG_H
#define LKPI_LINUX_STDARG_H
/* The compiler's own, not the C library's: the kernel is freestanding and has
 * no <stdarg.h> of its own to reach for. */
typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_end(ap)         __builtin_va_end(ap)
#define va_arg(ap, type)   __builtin_va_arg(ap, type)
#define va_copy(d, s)      __builtin_va_copy(d, s)
#endif
