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

#endif
