/* Interrupts-off accounting for the kernel profiler (b1nix.sysprof).
 *
 * The tick sampler charges a delayed tick to the instruction that re-enabled
 * interrupts, which names spin_unlock_irqrestore and never the code that held
 * them off. So the outermost section is timed here instead: every helper that
 * masks interrupts calls kprof_irqoff_begin() when they were on before, and
 * every helper that unmasks them calls kprof_irqoff_end(). A nested section
 * finds them already off and stays silent. The site is the return address of
 * the masking helper, so nm names the function that holds the lock.
 *
 * Off (the default) this is one load and a predicted branch per helper. */
#pragma once

extern int kprof_irqoff_on;
void kprof_irqoff_begin(void *site);
void kprof_irqoff_end(void);

/* The site is the helper's return address. The helpers are static inline
 * but the compiler emits out-of-line copies of them in most units, and a
 * label inside the helper then names the copy, not who called it; the return
 * address names the caller in that case, and the caller's caller where the
 * helper was inlined -- either one is a function nm can name. */

#if defined(__aarch64__)
/* DAIF: I set means masked. */
#define KPROF_IRQ_WAS_ON(f) (((f) & 0x80ULL) == 0)
#else
#define KPROF_IRQ_WAS_ON(f) (((f) & 0x200ULL) != 0)
#endif
