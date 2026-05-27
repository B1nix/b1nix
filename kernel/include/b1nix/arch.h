#ifndef B1NIX_ARCH_H
#define B1NIX_ARCH_H

#include <b1nix/types.h>

void arch_init(void);
void arch_halt(void) __attribute__((noreturn));
void arch_set_kernel_stack(u64 stack_top);

static inline void interrupts_disable(void) {
#ifdef __aarch64__
  __asm__ volatile("msr daifset, #2" : : : "memory");
#else
  __asm__ volatile("cli" : : : "memory");
#endif
}

static inline void interrupts_enable(void) {
#ifdef __aarch64__
  __asm__ volatile("msr daifclr, #2" : : : "memory");
#else
  __asm__ volatile("sti" : : : "memory");
#endif
}

static inline int interrupts_enabled(void) {
#ifdef __aarch64__
  u64 daif;
  __asm__ volatile("mrs %0, daif" : "=r"(daif));
  return (daif & (1ULL << 7)) == 0;
#else
  u64 rflags;
  __asm__ volatile("pushfq; popq %0" : "=r"(rflags));
  return (rflags & 0x200) != 0;
#endif
}

#endif
