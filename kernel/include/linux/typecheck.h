/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_TYPECHECK_H
#define LKPI_LINUX_TYPECHECK_H
/* Assert that `x` has the given type, yielding 1. Upstream uses it to catch a
 * macro handed the wrong integer width; the comparison is never executed, it
 * only has to typecheck — which is the whole point, and why it is written as a
 * pointer comparison the compiler rejects on a mismatch. */
#define typecheck(type, x) \
	({ type __dummy; __typeof__(x) __dummy2; (void)(&__dummy == &__dummy2); 1; })
#define typecheck_fn(type, function) \
	({ __typeof__(type) __tmp = function; (void)__tmp; })
#endif
