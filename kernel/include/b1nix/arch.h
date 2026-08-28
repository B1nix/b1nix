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


/* ── FPU / XSAVE state management (M80) ──────────────────────────────────────
 * The kernel saves userspace FPU state on every context switch. With XSAVE
 * available it saves x87+SSE+AVX into a per-task area of arch_xsave_area_size()
 * bytes; without it, the 512-byte FXSAVE area in struct task. ARCH_XSAVE_MAX_SIZE
 * bounds what the scheduler is willing to reserve per task — a CPU that would
 * need more simply stays on FXSAVE rather than silently losing state. */
#define ARCH_XSAVE_MAX_SIZE 1088

void arch_fpu_save(void *area);
void arch_fpu_restore(void *area);
void arch_fpu_capture_clean(void *area);
void arch_xsave(void *area, u64 mask);
void arch_xrstor(void *area, u64 mask);
void arch_xsave_capture_clean(void *area, u64 mask);
/* Processor clock, measured against the PIT during LAPIC calibration. 0 means
 * it was never measured (nothing is reported rather than guessed). */
void arch_set_cpu_khz(u32 khz);
u32 arch_cpu_khz(void);

/* The cycle-counter clock behind clock_gettime's monotonic family. Available
 * only on an invariant TSC with a calibrated frequency; callers must fall back
 * to the 100 Hz tick when arch_tsc_clock_ready() is false. */
void arch_tsc_clock_init(void);
int  arch_tsc_clock_ready(void);
u64  arch_tsc_monotonic_ns(void);
u32 arch_cpu_max_khz(void);
/* Exact TSC rate in kHz from CPUID leaf 15h, or 0 when the CPU does not
 * publish one and it has to be measured instead. */
u32 arch_tsc_khz_from_cpuid(void);

/* Boot-stack accounting (x86_64: kernel/arch/x86_64/arch.c).
 *
 * boot_stack_paint() fills the unused part of the boot CPU's stack with a
 * marker word and must be called at the top of kernel_main, while the stack is
 * still shallow. boot_stack_peak_bytes() then reports how deep the boot path
 * ever went. The boot stack overflowed silently for months because nobody
 * could see this number. */
void boot_stack_paint(void);
u64 boot_stack_peak_bytes(void);
u64 boot_stack_size_bytes(void);
int boot_stack_is_guard_addr(u64 addr);

int arch_xsave_enabled(void);
u64 arch_xsave_mask(void);
usize arch_xsave_area_size(void);

#endif
