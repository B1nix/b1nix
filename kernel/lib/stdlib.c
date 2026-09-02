#include <b1nix/panic.h>
#include <b1nix/console.h>
#include <b1nix/sched.h>
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
  /* Everything needed to tell the three candidates apart, because the panic
   * header alone cannot: (a) the frame really overflowed its own array,
   * (b) something else wrote over a live frame, (c) __stack_chk_guard itself
   * was clobbered, which would make every subsequent check fail and send the
   * hunt after innocent functions. Printing the guard settles (c) on the spot;
   * SP against the task's own stack range settles whether we are even on the
   * right stack. */
  u64 sp;
#ifdef __aarch64__
  __asm__ volatile("mov %0, sp" : "=r"(sp));
#elif defined(__x86_64__)
  __asm__ volatile("movq %%rsp, %0" : "=r"(sp));
#else
  sp = 0;
#endif
  console_write("stack smashing detected in the frame that returned to 0x");
  console_write_hex64((u64)(usize)__builtin_return_address(0));
  console_write("\n  sp=0x");
  console_write_hex64(sp);
  console_write(" guard=0x");
  console_write_hex64(__stack_chk_guard);
  console_write(" (expected 0xC0FFEE5713579BDF)\n");
  {
    struct task *t = current_task;
    console_write("  task=");
    console_write(t && t->name ? t->name : "(none)");
    console_write("/");
    console_write_dec(t ? (u64)t->id : 0);
    console_write(" stack=0x");
    console_write_hex64(t ? (u64)(usize)t->stack : 0);
    console_write("..0x");
    console_write_hex64(t ? t->kernel_stack_ptr : 0);
    console_write("\n");
  }
  /* The words around the smashed frame. The canary sits between the locals and
   * the saved x29/x30, so whatever overwrote it is very likely still visible
   * here -- and its VALUE is what names the writer. */
  console_write("  frame:");
  {
    const u64 *w = (const u64 *)(usize)(sp & ~7ULL);
    for (int i = 0; i < 20; i++) {
      console_write(" 0x");
      console_write_hex64(w[i]);
    }
  }
  console_write("\n");
  panic("stack smashing detected");
}
