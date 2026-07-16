/* Generic libatomic runtime (the size-generic __atomic_* entry points).
 *
 * clang emits a call to the generic __atomic_compare_exchange for an atomic
 * whose alignment is smaller than its size (e.g. V8's 16-byte, 8-aligned
 * ExternalEntityTable entries) — it cannot use cmpxchg16b there. The b1nix
 * cross GCC inlines those differently and never needs a libcall, and no
 * libatomic is built for the target, so b1nix provides the standard lock-based
 * fallback here. This is the same contract as GCC's libatomic / compiler-rt's
 * atomic.c.
 *
 * ponytail: one global lock guards every generic op. The only caller today is
 * V8's entity tables (rare, uncontended). Swap to an address-hashed lock pool
 * if a future workload makes this a contention point.
 */
#include <stddef.h>

/* Spinlock built on the always-lock-free 4-byte atomic builtin, so it never
 * recurses back into the generic path implemented below. */
static volatile int g_atomic_lock;

static void lock(void)
{
	while (__atomic_exchange_n(&g_atomic_lock, 1, __ATOMIC_ACQUIRE))
		;
}

static void unlock(void)
{
	__atomic_store_n(&g_atomic_lock, 0, __ATOMIC_RELEASE);
}

static void copy(void *dst, const void *src, size_t n)
{
	unsigned char *d = dst;
	const unsigned char *s = src;
	while (n--)
		*d++ = *s++;
}

static int equal(const void *a, const void *b, size_t n)
{
	const unsigned char *x = a, *y = b;
	for (size_t i = 0; i < n; i++)
		if (x[i] != y[i])
			return 0;
	return 1;
}

/* Define under non-builtin C names and pin the emitted symbol with an asm
 * label — clang refuses to redeclare the __atomic_* builtins directly, but the
 * asm label renames the symbol without ever using the builtin identifier in C. */
_Bool b1nix_atomic_cas(size_t size, void *ptr, void *expected, void *desired,
		       int success, int failure) asm("__atomic_compare_exchange");
void b1nix_atomic_load(size_t size, const void *ptr, void *ret,
		       int memorder) asm("__atomic_load");
void b1nix_atomic_store(size_t size, void *ptr, void *val,
			int memorder) asm("__atomic_store");
void b1nix_atomic_exchange(size_t size, void *ptr, void *val, void *ret,
			   int memorder) asm("__atomic_exchange");

_Bool b1nix_atomic_cas(size_t size, void *ptr, void *expected, void *desired,
		       int success, int failure)
{
	(void)success;
	(void)failure;
	_Bool ok;
	lock();
	if (equal(ptr, expected, size)) {
		copy(ptr, desired, size);
		ok = 1;
	} else {
		copy(expected, ptr, size);
		ok = 0;
	}
	unlock();
	return ok;
}

void b1nix_atomic_load(size_t size, const void *ptr, void *ret, int memorder)
{
	(void)memorder;
	lock();
	copy(ret, ptr, size);
	unlock();
}

void b1nix_atomic_store(size_t size, void *ptr, void *val, int memorder)
{
	(void)memorder;
	lock();
	copy(ptr, val, size);
	unlock();
}

void b1nix_atomic_exchange(size_t size, void *ptr, void *val, void *ret,
			   int memorder)
{
	(void)memorder;
	lock();
	copy(ret, ptr, size);
	copy(ptr, val, size);
	unlock();
}
