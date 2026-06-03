#include <stdlib.h>
#include <string.h>
#include <b1nix/syscall.h>
#include <b1nix/mm.h>
#include <b1nix/arch.h>

void abort(void)
{
	/* Print message then exit */
	const char *msg = "abort() called\n";
	syscall_dispatch(SYS_WRITE, (u64)(usize)msg, strlen(msg), 0, 0, 0, 0);
	syscall_dispatch(SYS_EXIT, 1, 0, 0, 0, 0, 0);
	while (1);
}

void exit(int status)
{
	syscall_dispatch(SYS_EXIT, (u64)status, 0, 0, 0, 0, 0);
	while (1);
}

void *malloc(size_t size)
{
	/* Use kernel heap allocation through mmap-like interface */
	return kmalloc(size);
}

void free(void *ptr)
{
	kfree(ptr);
}

int atoi(const char *s)
{
	int result = 0;
	int sign = 1;
	
	if (*s == '-') {
		sign = -1;
		s++;
	} else if (*s == '+') {
		s++;
	}
	
	while (*s >= '0' && *s <= '9') {
		result = result * 10 + (*s - '0');
		s++;
	}
	
	return result * sign;
}

void lib_atomic_load(size_t size, const void *ptr, void *ret, int memorder) __asm__("__atomic_load");
void lib_atomic_load(size_t size, const void *ptr, void *ret, int memorder) {
    (void)memorder;
    u64 flags = interrupts_save();
    memcpy(ret, ptr, size);
    interrupts_restore(flags);
}

int lib_atomic_compare_exchange(size_t size, void *ptr, void *expected, const void *desired, int success, int failure) __asm__("__atomic_compare_exchange");
int lib_atomic_compare_exchange(size_t size, void *ptr, void *expected, const void *desired, int success, int failure) {
    (void)success; (void)failure;
    int obj_equal = 0;
    u64 flags = interrupts_save();
    if (memcmp(ptr, expected, size) == 0) {
        memcpy(ptr, desired, size);
        obj_equal = 1;
    } else {
        memcpy(expected, ptr, size);
    }
    interrupts_restore(flags);
    return obj_equal;
}

u64 __udivmoddi4(u64 num, u64 den, u64 *rem) {
    if (den == 0) { if (rem) *rem = 0; return 0; }
    u64 quot = 0, qbit = 1, accum = den;
    while ((accum & (1ULL << 63)) == 0 && accum < num) { accum <<= 1; qbit <<= 1; }
    while (qbit > 0) {
        if (num >= accum) { num -= accum; quot |= qbit; }
        accum >>= 1; qbit >>= 1;
    }
    if (rem) *rem = num;
    return quot;
}

u64 __udivdi3(u64 num, u64 den) { return __udivmoddi4(num, den, NULL); }
u64 __umoddi3(u64 num, u64 den) { u64 rem; __udivmoddi4(num, den, &rem); return rem; }
