/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * kprobes — a breakpoint over a kernel function's first instruction.
 *
 * HOW IT WORKS
 *
 * Arming writes 0xCC (int3) over the symbol's first byte and keeps the byte it
 * replaced. When the function is entered the CPU traps: the #BP handler sees
 * that RIP-1 is a probe's address, reports the hit to the tracepoint the probe
 * belongs to, puts the original byte back, rewinds RIP by one and sets TF so
 * the instruction that was displaced executes exactly as written. The #DB that
 * TF produces immediately afterwards writes the int3 back and clears TF. The
 * function therefore runs unmodified; the only cost is two traps per call of a
 * probed function, and nothing at all for an unprobed one.
 *
 * WHAT IT DOES NOT DO
 *
 * A return probe (`r:name symbol`) is refused. A kretprobe needs a trampoline
 * planted on the return address of each live invocation, one instance per
 * frame; nothing here keeps that book, and a probe that silently fired at entry
 * while claiming to be a return probe would report the wrong thing.
 *
 * The single-step window is per-CPU and the patched byte is not: while one CPU
 * steps the displaced instruction, another CPU entering the same function runs
 * it without trapping, so a hit can be missed under contention. Linux avoids
 * that with per-CPU instruction buffers (an out-of-line single step); the
 * simpler scheme here costs an occasional undercount and never a wrong answer.
 * The counter a probe reports is therefore a lower bound on a busy SMP machine,
 * which is why `perf stat` on a kprobe is honest about being a sample of the
 * calls rather than a tally of them.
 */

#include <b1nix/kprobe.h>

#include <b1nix/arch.h>
#include <b1nix/errno.h>
#include <b1nix/klog.h>
#include <b1nix/lapic.h>
#include <b1nix/spinlock.h>
#include <b1nix/tracepoint.h>

#include <string.h>

#define KPROBE_MAX 32
#define KPROBE_INT3 0xCCu

/* The trap mechanism here is x86's int3 and TF. aarch64 has BRK and a
 * single-step bit that would do the same job, but nothing here plants them yet,
 * so a probe on that architecture is refused rather than pretended. */
#if !defined(__x86_64__)
int kprobe_symbol_ok(const char *symbol) { (void)symbol; return 0; }
const char *kprobe_symbol_of(u16 id) { (void)id; return "?"; }
int kprobe_arm(const char *symbol, u16 id, int is_return) {
  (void)symbol; (void)id; (void)is_return;
  return -EOPNOTSUPP;
}
int kprobe_disarm(u16 id) { (void)id; return -ENOENT; }
int kprobe_handle_bp(struct interrupt_frame *frame) { (void)frame; return 0; }
int kprobe_handle_db(struct interrupt_frame *frame) { (void)frame; return 0; }
#else

struct kprobe {
  u64 addr;    /* the patched byte's address */
  u8 orig;     /* what was there */
  u8 patched;  /* is the int3 in place right now? */
  u16 id;      /* the tracepoint this probe is */
  char symbol[64];
  int used;
};

static struct kprobe g_kp[KPROBE_MAX];
static spinlock_t g_kp_lock = SPINLOCK_INIT;

/* The probe each CPU is in the middle of single-stepping, +1 so that zero
 * means "none". A CPU steps one instruction at a time, so one slot each. */
static volatile u32 g_stepping[MAX_CPUS];

static unsigned kprobe_this_cpu(void) {
  struct percpu *pc = get_percpu();

  return pc ? (unsigned)pc->cpu_id : 0;
}

/* The one place that changes the text. A byte store is atomic on x86 and the
 * instruction stream is coherent with it on the same core; another core may be
 * executing the old byte, which is what makes the int3 (one byte, never
 * straddling an instruction boundary) the only safe thing to write here. */
static void kprobe_poke(u64 addr, u8 value) {
  *(volatile u8 *)(usize)addr = value;
  __asm__ volatile("" ::: "memory");
}


int kprobe_symbol_ok(const char *symbol) {
  return symbol && *symbol && ksym_addr_of(symbol) != 0;
}

const char *kprobe_symbol_of(u16 id) {
  for (usize i = 0; i < KPROBE_MAX; i++)
    if (g_kp[i].used && g_kp[i].id == id)
      return g_kp[i].symbol;
  return "?";
}

int kprobe_arm(const char *symbol, u16 id, int is_return) {
  u64 flags;
  u64 addr;
  int slot = -1;

  if (is_return)
    return -EOPNOTSUPP; /* no return trampoline: see the file comment */
  addr = symbol ? ksym_addr_of(symbol) : 0;
  if (!addr)
    return -ENOENT;
  spin_lock_irqsave(&g_kp_lock, &flags);
  for (usize i = 0; i < KPROBE_MAX; i++) {
    if (g_kp[i].used && g_kp[i].id == id) {
      /* Already known: just put the byte back in. */
      if (!g_kp[i].patched) {
        g_kp[i].orig = *(volatile u8 *)(usize)g_kp[i].addr;
        kprobe_poke(g_kp[i].addr, KPROBE_INT3);
        g_kp[i].patched = 1;
      }
      spin_unlock_irqrestore(&g_kp_lock, flags);
      return 0;
    }
    if (g_kp[i].used && g_kp[i].addr == addr) {
      spin_unlock_irqrestore(&g_kp_lock, flags);
      return -EEXIST; /* two probes on one byte cannot both keep the original */
    }
    if (!g_kp[i].used && slot < 0)
      slot = (int)i;
  }
  if (slot < 0) {
    spin_unlock_irqrestore(&g_kp_lock, flags);
    return -ENOSPC;
  }
  g_kp[slot].addr = addr;
  g_kp[slot].orig = *(volatile u8 *)(usize)addr;
  g_kp[slot].id = id;
  strncpy(g_kp[slot].symbol, symbol, sizeof(g_kp[slot].symbol) - 1);
  g_kp[slot].symbol[sizeof(g_kp[slot].symbol) - 1] = '\0';
  g_kp[slot].used = 1;
  kprobe_poke(addr, KPROBE_INT3);
  g_kp[slot].patched = 1;
  spin_unlock_irqrestore(&g_kp_lock, flags);
  return 0;
}

int kprobe_disarm(u16 id) {
  u64 flags;
  int rc = -ENOENT;

  spin_lock_irqsave(&g_kp_lock, &flags);
  for (usize i = 0; i < KPROBE_MAX; i++) {
    if (!g_kp[i].used || g_kp[i].id != id)
      continue;
    if (g_kp[i].patched) {
      kprobe_poke(g_kp[i].addr, g_kp[i].orig);
      g_kp[i].patched = 0;
    }
    /* The row stays: a CPU may still be stepping this probe, and it needs the
     * address to find its way back. Freed by a later arm of the same id or
     * simply reused once nothing steps it. */
    g_kp[i].used = 1;
    rc = 0;
    break;
  }
  spin_unlock_irqrestore(&g_kp_lock, flags);
  return rc;
}

/* ── the traps ───────────────────────────────────────────────────────────── */

int kprobe_handle_bp(struct interrupt_frame *frame) {
  u64 hit;
  unsigned cpu;

  if (!frame || (frame->cs & 3) == 3)
    return 0; /* a user int3 is not ours */
  hit = frame->rip - 1;
  for (usize i = 0; i < KPROBE_MAX; i++) {
    if (!g_kp[i].used || !g_kp[i].patched || g_kp[i].addr != hit)
      continue;
    /* The hit itself. The site's three words are what a probe can know without
     * a per-function description: where it was, and the first two arguments in
     * the SysV registers. */
    TRACEPOINT_FIRE(g_kp[i].id, hit, frame->rdi, frame->rsi);
    /* Step the displaced instruction with the original byte in place. */
    kprobe_poke(g_kp[i].addr, g_kp[i].orig);
    g_kp[i].patched = 0;
    frame->rip = hit;
    frame->rflags |= 0x100ull; /* TF */
    cpu = kprobe_this_cpu();
    if (cpu < MAX_CPUS)
      g_stepping[cpu] = (u32)i + 1;
    return 1;
  }
  return 0;
}

int kprobe_handle_db(struct interrupt_frame *frame) {
  unsigned cpu;
  u32 slot;

  if (!frame)
    return 0;
  cpu = kprobe_this_cpu();
  if (cpu >= MAX_CPUS)
    return 0;
  slot = g_stepping[cpu];
  if (!slot)
    return 0;
  g_stepping[cpu] = 0;
  slot--;
  frame->rflags &= ~0x100ull; /* TF off; the step is done */
  /* Put the breakpoint back, unless the probe was turned off meanwhile. */
  {
    struct b1nix_tracepoint *tp = tracepoint_by_id(g_kp[slot].id);

    if (g_kp[slot].used && tp && tp->enabled && !g_kp[slot].patched) {
      kprobe_poke(g_kp[slot].addr, KPROBE_INT3);
      g_kp[slot].patched = 1;
    }
  }
  /* DR6 is ours to clear: a stale single-step bit makes the next #DB from
   * anywhere look like this one. */
  {
    u64 dr6;

    __asm__ volatile("mov %%dr6, %0" : "=r"(dr6));
    __asm__ volatile("mov %0, %%dr6" ::"r"(dr6 & ~0xfull));
  }
  return 1;
}
#endif /* __x86_64__ */
