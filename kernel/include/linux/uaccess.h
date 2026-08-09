/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_UACCESS_H
#define LKPI_LINUX_UACCESS_H

#include <linux/errno.h>
#include <lkpi/env.h>
#include <linux/types.h>

/*
 * Copying to and from userspace.
 *
 * These go through b1nix's own copyin/copyout, which validate the user pointer
 * against the calling process's address space. That validation is the entire
 * point: a driver ioctl is handed a pointer chosen by userspace, and a shim
 * that dereferenced it directly would turn every ioctl into an arbitrary
 * kernel read. Returns the number of bytes NOT copied, as Linux does — 0 means
 * complete success, and callers test it that way.
 */

static inline unsigned long copy_from_user(void *to, const void __user *from,
                                           unsigned long n)
{
	return lkpi_copy_from_user(to, (const void *)from, n) == 0 ? 0 : n;
}

static inline unsigned long copy_to_user(void __user *to, const void *from,
                                         unsigned long n)
{
	return lkpi_copy_to_user((void *)to, from, n) == 0 ? 0 : n;
}

/*
 * A coarse range check: the address must be in the lower half of the address
 * space and the range must not wrap. It is deliberately weaker than the real
 * validation, which happens inside copy_from_user/copy_to_user against the
 * calling process's actual mappings — so access_ok returning 1 is not
 * permission to dereference, only that the copy helpers are worth calling. No
 * imported code should be dereferencing a __user pointer directly anyway, and
 * this shim gives it nothing that would let it.
 */
#define LKPI_USER_VA_END 0x0000800000000000ULL

static inline int access_ok(const void __user *addr, unsigned long size)
{
	u64 base = (u64)(usize)addr;
	if (base + size < base)
		return 0;
	return (base + size) <= LKPI_USER_VA_END;
}

/* The uid/gid reported when a real one does not fit the 16-bit fields of an
 * old interface. b1nix never truncates, so it is the conventional value and
 * nothing produces it. */
#define overflowuid 65534
#define overflowgid 65534

#define get_user(x, ptr)                                            \
	({                                                              \
		__typeof__(*(ptr)) __v;                                     \
		int __e = copy_from_user(&__v, (ptr), sizeof(__v)) ? -EFAULT : 0; \
		(x) = __v;                                                  \
		__e;                                                        \
	})

#define put_user(x, ptr)                                            \
	({                                                              \
		__typeof__(*(ptr)) __v = (x);                               \
		copy_to_user((ptr), &__v, sizeof(__v)) ? -EFAULT : 0;       \
	})

#endif
