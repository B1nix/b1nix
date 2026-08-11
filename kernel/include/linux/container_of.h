/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_CONTAINER_OF_H
#define LKPI_LINUX_CONTAINER_OF_H

#include <linux/stddef.h>

/*
 * Recover a containing struct from a member's address.
 *
 * Kept in its own header, separate from <linux/kernel.h>, so <linux/list.h> can
 * have it without including kernel.h — which includes list.h. Linux splits it
 * for the same reason.
 */
#define container_of(ptr, type, member)                        \
	({                                                         \
		const __typeof__(((type *)0)->member) *__mptr = (ptr); \
		(type *)((char *)__mptr - offsetof(type, member));     \
	})

#define container_of_const(ptr, type, member) container_of(ptr, type, member)


/* The type of a struct member, without an instance of the struct. Used where a
 * macro has to declare a temporary of the same type as the field it moves. */
#define typeof_member(T, m) __typeof__(((T *)0)->m)

#endif
