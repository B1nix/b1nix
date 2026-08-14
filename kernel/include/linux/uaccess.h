/* SPDX-License-Identifier: GPL-2.0-only */
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


/* The unchecked spellings. They are not unchecked here: both go through the
 * same validation, because b1nix's copy helpers validate against the calling
 * process's mappings and there is no cheaper path that skips it. A caller gets
 * more safety than it asked for, never less. */
#define __copy_from_user(to, from, n) copy_from_user(to, from, n)
#define __copy_to_user(to, from, n)   copy_to_user(to, from, n)


/* The unchecked accessor spellings. As with __copy_from_user, they are not
 * unchecked here — the validation is the same, and a caller gets more than it
 * asked for. */
#define __get_user(x, ptr) get_user(x, ptr)
#define __put_user(x, ptr) put_user(x, ptr)


/*
 * The unsafe_ accessors, used inside a user_access_begin/end region where
 * upstream drops the per-access checks and jumps to a label on fault.
 *
 * There is no such region here: every access is checked, and a fault returns an
 * error rather than jumping. So the label is honoured by branching to it on
 * failure, which preserves the caller's control flow exactly — what changes is
 * that the checks are still paid for.
 */
#define unsafe_put_user(x, ptr, label) \
	do { if (put_user(x, ptr)) goto label; } while (0)
#define unsafe_get_user(x, ptr, label) \
	do { if (get_user(x, ptr)) goto label; } while (0)
#define user_access_begin(ptr, len) access_ok(ptr, len)
#define user_access_end() do { } while (0)


/* The atomic-context copy. There is no faster unchecked path here, so it is the
 * same copy — and it can still fail, which is what its callers already handle. */
#define __copy_to_user_inatomic(to, from, n)   copy_to_user(to, from, n)
#define __copy_from_user_inatomic(to, from, n) copy_from_user(to, from, n)


/* Disabling page faults around an access that must not sleep. b1nix's copy
 * helpers never fault into the pager — a bad address fails immediately — so
 * there is nothing to disable, and the property these ask for already holds. */
static inline void pagefault_disable(void) {}
static inline void pagefault_enable(void) {}
static inline bool pagefault_disabled(void) { return false; }


/*
 * The batched user-access window: upstream opens SMAP once, does several
 * unsafe_put_user() stores, then closes it. b1nix checks and copies per access
 * through its own copy_to_user, so there is no window to open — these are the
 * begin/end of a region that is already implicit, and the stores inside go
 * through the checked path.
 */
static inline int user_write_access_begin(void __user *ptr, usize len)
{ (void)ptr; (void)len; return 1; }
static inline void user_write_access_end(void) { }
static inline int user_read_access_begin(const void __user *ptr, usize len)
{ (void)ptr; (void)len; return 1; }
static inline void user_read_access_end(void) { }

/* Copy from user without faulting and without polluting the cache. b1nix has
 * no non-temporal copy helper and no pagefault-disabled copy, so this is the
 * ordinary checked copy: same bytes, same failure reporting, without the
 * cache-bypass the name promises. */
static inline unsigned long
__copy_from_user_inatomic_nocache(void *to, const void __user *from, unsigned long n)
{ return copy_from_user(to, from, n); }

int kstrtoint_from_user(const char __user *s, usize count, unsigned int base, int *res);
int kstrtouint_from_user(const char __user *s, usize count, unsigned int base, unsigned int *res);
int kstrtoull_from_user(const char __user *s, usize count, unsigned int base, unsigned long long *res);
int kstrtobool_from_user(const char __user *s, usize count, bool *res);

#endif
