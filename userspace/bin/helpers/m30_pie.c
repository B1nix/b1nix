/* M30 PIE smoke. Compiled with -fPIC -pie so the linker emits ET_DYN with
 * R_X86_64_RELATIVE relocations. The in-kernel loader picks a load base
 * (`PIE_LOAD_BASE` in process.c), offsets every PT_LOAD vaddr by that
 * base, and walks PT_DYNAMIC's DT_RELA to add the base to every
 * RELATIVE relocation's stored value. If any of those steps is wrong
 * the strings below would print as garbage (or the binary would
 * page-fault on dereferencing a still-zero relocated pointer).
 *
 * Deliberately self-contained: raw syscall inline asm, no libc, no
 * stdio buffering. Smoke just greps the marker. */

#include "syscall.h"

#ifdef __linux__
#undef SYS_WRITE
#undef SYS_EXIT
#define SYS_WRITE 1
#define SYS_EXIT 60
#endif

/* Hand-rolled syscall (no libc): RAX=number, args in RDI/RSI/RDX, return
 * in RAX. Matches the b1nix x86_64 ABI documented in docs/abi.md. */
#ifdef __x86_64__
static long raw_syscall(long n, long a, long b, long c) {
  long ret;
  register long rdi __asm__("rdi") = a;
  register long rsi __asm__("rsi") = b;
  register long rdx __asm__("rdx") = c;
  register long rax __asm__("rax") = n;
  __asm__ volatile("syscall"
                   : "=a"(ret)
                   : "r"(rax), "r"(rdi), "r"(rsi), "r"(rdx)
                   : "rcx", "r11", "memory");
  return ret;
}
#else
static long raw_syscall(long n, long a, long b, long c) {
  long ret;
  register long ebx __asm__("ebx") = a;
  register long ecx __asm__("ecx") = b;
  register long edx __asm__("edx") = c;
  register long eax __asm__("eax") = n;
  __asm__ volatile("int $0x80"
                   : "=a"(ret)
                   : "r"(eax), "r"(ebx), "r"(ecx), "r"(edx)
                   : "memory");
  return ret;
}
#endif

/* Mutable globals in .data force the linker to emit R_X86_64_RELATIVE
 * relocations against the pointers below. With static `const` the
 * compiler can prove the values are link-time constants and constant-
 * fold them away, leaving no RELATIVE relocs to test. */
static char marker_text[] = "M30-DYN: ok pie-binary\n";
static char relocs_text[] = "M30-DYN: ok pie-relocs\n";
static char done_text[]   = "M30-DYN: done\n";

/* Pointer table in .data — each entry needs a RELATIVE relocation.
 * Marked `volatile` to be sure the compiler doesn't fold the array
 * indexing in `_start` into a pre-computed offset. */
static char *volatile messages[] = {
    marker_text,
    relocs_text,
    done_text,
};

static int my_strlen(const char *s) {
  int n = 0;
  while (s[n]) n++;
  return n;
}

void _start(void) {
  for (int i = 0; i < 3; i++) {
    char *p = messages[i];
    raw_syscall(SYS_WRITE, 1, (long)p, my_strlen(p));
  }
  raw_syscall(SYS_EXIT, 0, 0, 0);
  for (;;) {}
}
