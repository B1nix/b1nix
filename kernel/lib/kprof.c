/* SPDX-License-Identifier: GPL-2.0-only */
/* kprof — where the kernel's CPU time actually goes.
 *
 * The scheduler tick (LAPIC vector 64 on x86_64, the CNTV PPI on aarch64 — one
 * per CPU either way) already interrupts every core a hundred times a second. That is a ready-made sampling clock: record
 * the instruction pointer it interrupted (RIP or ELR_EL1) and, over a run, the histogram is the
 * kernel's own profile. Nothing else in the tree could answer "which function
 * burns the CPU-seconds a browser start-up spends in ring 0" — the system-call
 * profile times whole calls including the blocking, so it names a syscall,
 * never a line.
 *
 * What is recorded here is the tick DISTRIBUTION (user / kernel / idle, per
 * CPU): three counters per CPU, always on, one increment per tick, and the
 * honest measure of whether a change moved kernel time at all.
 *
 * THE RIP HISTOGRAM IS GONE, and perf is why (M126).
 *
 * This file used to keep its own open-addressed histogram of the kernel
 * instruction pointer under `b1nix.sysprof`, aggregated by symbol at dump
 * time. perf_event_open(2) now answers exactly that question and answers it
 * better: from userspace with no boot flag, per task or machine-wide, with
 * call chains, with user-mode addresses as well as kernel ones, and read
 * through a ring buffer instead of the console -- which is what made the old
 * one change the behaviour it was measuring.
 *
 *     perf record -F 200 -a -- sleep 5      (or the hand-driven equivalent in
 *     tests/programs/bin/smoke/m126_smoke.c)
 *
 * What stays here is what perf cannot answer in this kernel: the tick
 * distribution above, the interrupts-off sections, the wait sites and the
 * wake-up latencies. Those are not sampling questions.
 */
#include <b1nix/ktime.h>
#include <b1nix/sched.h>
#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/klog.h>
#include <b1nix/types.h>
#include <b1nix/kprof.h>
#include <b1nix/lapic.h>


/* Its own ceiling rather than the interrupt controller's MAX_CPUS: that
 * constant lives in the x86 LAPIC header, and this file is shared. The array
 * is indexed by cpu id and bounds-checked, so a wider machine loses samples
 * from the extra cores rather than corrupting anything. */
#define KPROF_MAX_CPUS 64

/* Tick distribution: [cpu][0]=user [cpu][1]=kernel [cpu][2]=idle. */
static u64 g_tick_mode[KPROF_MAX_CPUS][3];
static u64 g_tick_total;

static int kprof_enabled(void) {
  static int on = -1;

  if (on < 0) {
    on = bootinfo_has_flag("b1nix.sysprof") ? 1 : 0;
    __atomic_store_n(&kprof_irqoff_on, on, __ATOMIC_RELEASE);
  }
  return on;
}

/* ── Interrupts-off sections, by the site that began them ─────────────────
 * See kprof.h. Cycles from the free-running counter: a section cannot
 * migrate, so the same CPU reads both ends. A task switch inside a section
 * ends it on the switched-to side; the time is still real interrupts-off
 * time on that CPU and is charged to the site that began it. */
/* b1nix.sysprof-task=<name>: only the kernel time of tasks whose name contains
 * it, so one program's share is not lost in everybody's. 1 when unset. */
static int kprof_task_match(void) {
  static char want[32];
  static int have = -1;

  if (have < 0)
    have = bootinfo_get_kv("b1nix.sysprof-task", want, sizeof(want)) ? 1 : 0;
  if (!have)
    return 1;
  const char *nm = current_task && current_task->name ? current_task->name : "";

  for (usize i = 0; nm[i]; i++) {
    usize k = 0;

    while (want[k] && nm[i + k] == want[k])
      k++;
    if (!want[k])
      return 1;
  }
  return 0;
}

int kprof_irqoff_on;
#define IRQOFF_SLOTS 1024u
struct irqoff_slot {
  u64 site;
  u64 cycles;
  u64 count;
};
static struct irqoff_slot g_irqoff[IRQOFF_SLOTS];
static u64 g_irqoff_cycles, g_irqoff_sections, g_irqoff_dropped;

static inline u64 irqoff_now(void) {
#if defined(__aarch64__)
  u64 c;
  __asm__ volatile("mrs %0, cntvct_el0" : "=r"(c));
  return c;
#else
  return __builtin_ia32_rdtsc();
#endif
}

void kprof_irqoff_begin(void *site) {
  struct percpu *p = get_percpu();
  if (!p || p->irqoff_site)
    return;
  p->irqoff_site = site;
  p->irqoff_t0 = irqoff_now();
}

void kprof_irqoff_end(void) {
  struct percpu *p = get_percpu();
  if (!p || !p->irqoff_site)
    return;
  u64 site = (u64)(usize)p->irqoff_site;
  u64 d = irqoff_now() - p->irqoff_t0;
  p->irqoff_site = 0;
  if (!kprof_task_match())
    return;
  __atomic_fetch_add(&g_irqoff_cycles, d, __ATOMIC_RELAXED);
  __atomic_fetch_add(&g_irqoff_sections, 1, __ATOMIC_RELAXED);
  u32 h = (u32)((site * 0x9e3779b97f4a7c15ULL) >> 54) & (IRQOFF_SLOTS - 1);
  for (u32 probe = 0; probe < 16; probe++) {
    u32 i = (h + probe) & (IRQOFF_SLOTS - 1);
    u64 cur = __atomic_load_n(&g_irqoff[i].site, __ATOMIC_RELAXED);
    if (cur == 0) {
      u64 expect = 0;
      if (!__atomic_compare_exchange_n(&g_irqoff[i].site, &expect, site, 0,
                                       __ATOMIC_RELAXED, __ATOMIC_RELAXED) &&
          expect != site)
        continue;
      cur = site;
    }
    if (cur == site) {
      __atomic_fetch_add(&g_irqoff[i].cycles, d, __ATOMIC_RELAXED);
      __atomic_fetch_add(&g_irqoff[i].count, 1, __ATOMIC_RELAXED);
      return;
    }
  }
  __atomic_fetch_add(&g_irqoff_dropped, 1, __ATOMIC_RELAXED);
}

/* ── Waits, by the function that armed them ───────────────────────────────
 * Every scheduler_wait_prepare under b1nix.sysprof, counted against its
 * caller. The interrupts-off table charges a wait's context switch to
 * scheduler_wait_prepare itself, which says that the machine waits a great
 * deal and not who does. */
#define WAITSITE_SLOTS 512u
struct site_table {
  struct { u64 site; u64 count; } slot[WAITSITE_SLOTS];
  u64 dropped;
};
static struct site_table g_waitsite, g_pollwake, g_bcachesite, g_wakesite;

static void site_count(struct site_table *tb, void *site) {
  if (!kprof_enabled() || !site)
    return;
  u64 key = (u64)(usize)site;
  u32 h = (u32)((key * 0x9e3779b97f4a7c15ULL) >> 55) & (WAITSITE_SLOTS - 1);
  for (u32 probe = 0; probe < 16; probe++) {
    u32 i = (h + probe) & (WAITSITE_SLOTS - 1);
    u64 cur = __atomic_load_n(&tb->slot[i].site, __ATOMIC_RELAXED);
    if (cur == 0) {
      u64 expect = 0;
      if (!__atomic_compare_exchange_n(&tb->slot[i].site, &expect, key, 0,
                                       __ATOMIC_RELAXED, __ATOMIC_RELAXED) &&
          expect != key)
        continue;
      cur = key;
    }
    if (cur == key) {
      __atomic_fetch_add(&tb->slot[i].count, 1, __ATOMIC_RELAXED);
      return;
    }
  }
  __atomic_fetch_add(&tb->dropped, 1, __ATOMIC_RELAXED);
}

void kprof_wait_site(void *site) { site_count(&g_waitsite, site); }

/* Wakes of the shared poll channel, by the function that issued them: which
 * readiness event keeps every poll and epoll sleeper in the machine busy. */
void kprof_pollwake_site(void *site) { site_count(&g_pollwake, site); }

/* Block-cache lock acquisitions, by caller: which filesystem path takes the
 * cache's one lock hundreds of thousands of times. */
void kprof_bcache_site(void *site) { site_count(&g_bcachesite, site); }

/* Every scheduler_wake_all, by caller (the task filter applies). */
void kprof_wake_site(void *site) {
  if (kprof_task_match())
    site_count(&g_wakesite, site);
}

static void site_dump(struct site_table *tb, const char *title, const char *tag) {
  u64 floor = ~0ull;

  if (!kprof_enabled())
    return;
  console_write(title);
  console_write(" (dropped=");
  console_write_dec(tb->dropped);
  console_write(")\n");
  for (int n = 0; n < 16; n++) {
    u64 best = 0;
    usize bi = WAITSITE_SLOTS;
    for (usize i = 0; i < WAITSITE_SLOTS; i++) {
      u64 c = tb->slot[i].count;
      if (tb->slot[i].site && c < floor && c > best) {
        best = c;
        bi = i;
      }
    }
    if (bi == WAITSITE_SLOTS)
      break;
    floor = best;
    console_write(tag);
    console_write_dec(best);
    console_write(" 0x");
    console_write_hex64(tb->slot[bi].site);
    ksym_print(tb->slot[bi].site);
    console_write("\n");
  }
}

static void kprof_dump_waitsites(void) {
  site_dump(&g_waitsite, "kprof: waits by caller", "  kprof-wait ");
  site_dump(&g_pollwake, "kprof: poll-channel wakes by caller", "  kprof-pollwake ");
  site_dump(&g_bcachesite, "kprof: block-cache lock by caller", "  kprof-bcache ");
  site_dump(&g_wakesite, "kprof: wake_all by caller", "  kprof-wake ");
}

static void kprof_dump_irqoff(void) {
  if (!kprof_irqoff_on)
    return;
  console_write("kprof: irqoff cycles=");
  console_write_dec(g_irqoff_cycles);
  console_write(" sections=");
  console_write_dec(g_irqoff_sections);
  console_write(" dropped=");
  console_write_dec(g_irqoff_dropped);
  console_write("\n");
  /* Top 24 by cycles: pick the largest not yet printed, 24 times. */
  u64 floor = ~0ull;
  for (int n = 0; n < 24; n++) {
    u64 best = 0;
    usize bi = IRQOFF_SLOTS;
    for (usize i = 0; i < IRQOFF_SLOTS; i++) {
      u64 c = g_irqoff[i].cycles;
      if (g_irqoff[i].site && c < floor && c > best) {
        best = c;
        bi = i;
      }
    }
    if (bi == IRQOFF_SLOTS)
      break;
    floor = best;
    console_write("  kprof-irqoff ");
    console_write_dec(best);
    console_write(" ");
    console_write_dec(g_irqoff[bi].count);
    console_write(" 0x");
    console_write_hex64(g_irqoff[bi].site);
    /* Name it here, the way the histogram does. An address alone has to be
     * resolved against the exact kernel that produced it, and reading it back
     * later against a rebuilt one -- as happened while chasing this -- names
     * the wrong function with no sign that it did. */
    ksym_print(g_irqoff[bi].site);
    console_write("\n");
  }
}

/* Called from the vector-64 handler with interrupts off. Must be cheap: it
 * runs on every core at every tick. */
static int waitprof_enabled(void) {
  static int on = -1;

  if (on < 0)
    on = bootinfo_has_flag("b1nix.waitprof") ? 1 : 0;
  return on;
}

/* How much time this interrupt stands for, in nominal ticks.
 *
 * Counting interrupts and calling the result "ticks" is only right while the
 * timer is periodic. With one-shot ticks (b1nix.dynticks) an interrupt can
 * stand for one tick or for twenty, so counting them would report an idle
 * machine as busy: the long sleeps are exactly the intervals that get dropped
 * to one count each. Weigh every sample by the time it actually covers, read
 * from the monotonic clock, and the figures keep meaning what their names say
 * under either mode. */
static u64 kprof_weight(int cpu) {
  static u64 last_ns[KPROF_MAX_CPUS];
  u64 ns_per_tick = 1000000000ull / (sched_tick_hz() ? sched_tick_hz() : 100);
  u64 now = ktime_monotonic_ns();
  u64 w = 1;

  if (cpu < 0 || cpu >= KPROF_MAX_CPUS || !ns_per_tick)
    return 1;
  if (last_ns[cpu] && now > last_ns[cpu])
    w = (now - last_ns[cpu]) / ns_per_tick;
  last_ns[cpu] = now;
  return w ? w : 1;
}

/* Where the machine's time went, summed over every CPU. For a caller that
 * wants to say what a stall was made of: busy in userspace, busy in the
 * kernel, or idle waiting for something. */
void kprof_tick_totals(u64 *user, u64 *kernel, u64 *idle) {
  u64 u = 0, k = 0, i = 0;

  for (unsigned c = 0; c < KPROF_MAX_CPUS; c++) {
    u += __atomic_load_n(&g_tick_mode[c][0], __ATOMIC_RELAXED);
    k += __atomic_load_n(&g_tick_mode[c][1], __ATOMIC_RELAXED);
    i += __atomic_load_n(&g_tick_mode[c][2], __ATOMIC_RELAXED);
  }
  if (user)
    *user = u;
  if (kernel)
    *kernel = k;
  if (idle)
    *idle = i;
}

/* One CPU's share of the same distribution, in ticks: /proc/stat's per-CPU
 * rows. */
void kprof_tick_cpu(unsigned cpu, u64 *user, u64 *kernel, u64 *idle) {
  if (cpu >= KPROF_MAX_CPUS) {
    *user = *kernel = *idle = 0;
    return;
  }
  *user = __atomic_load_n(&g_tick_mode[cpu][0], __ATOMIC_RELAXED);
  *kernel = __atomic_load_n(&g_tick_mode[cpu][1], __ATOMIC_RELAXED);
  *idle = __atomic_load_n(&g_tick_mode[cpu][2], __ATOMIC_RELAXED);
}

void kprof_tick(u64 rip, int in_user, int in_idle, int cpu) {
  int mode = in_user ? 0 : (in_idle ? 2 : 1);
  u64 weight = kprof_weight(cpu);

  (void)rip; /* the instruction pointer is perf's business now: see the top */

  if (cpu >= 0 && cpu < KPROF_MAX_CPUS)
    __atomic_fetch_add(&g_tick_mode[cpu][mode], weight, __ATOMIC_RELAXED);
  __atomic_fetch_add(&g_tick_total, weight, __ATOMIC_RELAXED);

  if (waitprof_enabled() && cpu == 0)
    sched_waitprof_tick(mode == 2);
}

void kprof_dump(void) {
  u64 u = 0, k = 0, idl = 0;

  console_write("ticks (cpu user/kernel/idle):");
  for (int c = 0; c < KPROF_MAX_CPUS; c++) {
    u64 cu = __atomic_load_n(&g_tick_mode[c][0], __ATOMIC_RELAXED);
    u64 ck = __atomic_load_n(&g_tick_mode[c][1], __ATOMIC_RELAXED);
    u64 ci = __atomic_load_n(&g_tick_mode[c][2], __ATOMIC_RELAXED);

    if (!(cu | ck | ci))
      continue;
    u += cu;
    k += ck;
    idl += ci;
    console_write(" ");
    console_write_dec((u64)c);
    console_write(":");
    console_write_dec(cu);
    console_write("/");
    console_write_dec(ck);
    console_write("/");
    console_write_dec(ci);
  }
  console_write("\nticks total: user=");
  console_write_dec(u);
  console_write(" kernel=");
  console_write_dec(k);
  console_write(" idle=");
  console_write_dec(idl);
  console_write(" ticks_seen=");
  console_write_dec(__atomic_load_n(&g_tick_total, __ATOMIC_RELAXED));
  console_write("\n");

  /* Where the kernel's time went, by function, is perf's answer now. */
  console_write("kernel profile: use perf_event_open(2) sampling "
                "(PERF_SAMPLE_IP); this file no longer keeps a histogram\n");
  kprof_dump_irqoff();
  kprof_dump_waitsites();
  {
    extern void sched_wakelat_dump(void);

    sched_wakelat_dump();
  }
}
