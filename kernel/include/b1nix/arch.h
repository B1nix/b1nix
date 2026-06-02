#ifndef B1NIX_ARCH_H
#define B1NIX_ARCH_H

#include <b1nix/types.h>

#ifdef __aarch64__
// aarch64 headers
#elif defined(__x86_64__)
#include <b1nix/arch_x86_64.h>
#else
#include <b1nix/arch_x86.h>
#endif

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
#ifdef __x86_64__
  __asm__ volatile("pushfq; popq %0" : "=r"(rflags));
#else
  u32 rflags32;
  __asm__ volatile("pushfd; popl %0" : "=r"(rflags32));
  rflags = rflags32;
#endif
  return (rflags & 0x200) != 0;
#endif
}

static inline u64 interrupts_save(void) {
  u64 f;
#ifdef __aarch64__
  __asm__ volatile("mrs %0, daif" : "=r"(f));
  __asm__ volatile("msr daifset, #2" : : : "memory");
#elif defined(__x86_64__)
  __asm__ volatile("pushfq; popq %0; cli" : "=r"(f) : : "memory");
#else
  u32 f32;
  __asm__ volatile("pushfd; popl %0; cli" : "=r"(f32) : : "memory");
  f = f32;
#endif
  return f;
}

static inline void interrupts_restore(u64 f) {
#ifdef __aarch64__
  __asm__ volatile("msr daif, %0" : : "r"(f) : "memory");
#elif defined(__x86_64__)
  __asm__ volatile("pushq %0; popfq" : : "r"(f) : "memory", "cc");
#else
  u32 f32 = (u32)f;
  __asm__ volatile("pushl %0; popfd" : : "r"(f32) : "memory", "cc");
#endif
}

#endif
