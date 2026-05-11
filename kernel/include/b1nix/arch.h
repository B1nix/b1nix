#ifndef B1NIX_ARCH_H
#define B1NIX_ARCH_H

void arch_init(void);
void arch_halt(void) __attribute__((noreturn));

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
