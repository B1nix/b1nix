#include <b1nix/panic.h>
#include <b1nix/console.h>
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


/* Stack-protector runtime.
 *
 * The kernel is built -fno-stack-protector, so nothing should reference these
 * -- but "should" is a property of every translation unit's command line, and
 * the build has more than one path that compiles kernel sources (ports, module
 * rules, the analyzer pass). A single object built with the protector on turns
 * into `ld.lld: undefined symbol: __stack_chk_guard` at the very end of a
 * multi-minute build, which is a bad way to find out. Provide the runtime
 * instead: it costs two symbols, and if a guard ever does fire it reports a
 * smashed stack by name rather than failing to link.
 *
 * The canary is a fixed value: this kernel has no entropy source at the point
 * the first guarded frame could run, and a predictable canary still catches the
 * accidental overflows this is here for. */
unsigned long __stack_chk_guard = 0xC0FFEE5713579BDFUL;

void __stack_chk_fail(void);
void __stack_chk_fail(void) {
  /* Name the frame that smashed. The panic backtrace starts here, so without
   * the return address the report says only that *something* overflowed. */
  console_write("stack smashing detected in the frame that returned to 0x");
  console_write_hex64((u64)(usize)__builtin_return_address(0));
  console_write("\n");
  panic("stack smashing detected");
}
