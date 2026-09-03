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
 * Two things are recorded:
 *
 *  - The tick DISTRIBUTION (user / kernel / idle, per CPU). Three counters per
 *    CPU, always on: it costs one increment per tick and it is the honest
 *    measure of whether a change moved kernel time at all.
 *  - The kernel RIP HISTOGRAM, only under b1nix.sysprof. Open-addressed hash
 *    of 16-byte buckets, aggregated by symbol at dump time via kallsyms, so a
 *    run reports function names rather than addresses to be guessed at.
 *
 * The sample counter is printed unconditionally: a profile of zero must be
 * distinguishable from a probe that never ran.
 */
#include <b1nix/ktime.h>
#include <b1nix/sched.h>
#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/klog.h>
#include <b1nix/types.h>

#define KPROF_SLOTS 8192u
#define KPROF_GRAIN 16u

struct kprof_slot {
  u64 addr;
  u64 hits;
};

static struct kprof_slot g_kprof[KPROF_SLOTS];
static u64 g_kprof_samples; /* kernel-mode ticks offered to the histogram */
static u64 g_kprof_dropped; /* histogram full — samples lost */

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

  if (on < 0)
    on = bootinfo_has_flag("b1nix.sysprof") ? 1 : 0;
  return on;
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

void kprof_tick(u64 rip, int in_user, int in_idle, int cpu) {
  int mode = in_user ? 0 : (in_idle ? 2 : 1);
  u64 weight = kprof_weight(cpu);

  if (cpu >= 0 && cpu < KPROF_MAX_CPUS)
    __atomic_fetch_add(&g_tick_mode[cpu][mode], weight, __ATOMIC_RELAXED);
  __atomic_fetch_add(&g_tick_total, weight, __ATOMIC_RELAXED);

  if (waitprof_enabled() && cpu == 0)
    sched_waitprof_tick(mode == 2);

  if (mode != 1 || !kprof_enabled())
    return;

  __atomic_fetch_add(&g_kprof_samples, weight, __ATOMIC_RELAXED);

  u64 key = rip & ~(u64)(KPROF_GRAIN - 1);
  /* Fibonacci hash of the bucket address; linear probe over a short window so
   * a tick never walks the whole table with interrupts off. */
  u32 h = (u32)((key * 0x9e3779b97f4a7c15ULL) >> 51) & (KPROF_SLOTS - 1);

  for (u32 probe = 0; probe < 16; probe++) {
    u32 i = (h + probe) & (KPROF_SLOTS - 1);
    u64 cur = __atomic_load_n(&g_kprof[i].addr, __ATOMIC_RELAXED);

    if (cur == key) {
      __atomic_fetch_add(&g_kprof[i].hits, weight, __ATOMIC_RELAXED);
      return;
    }
    if (cur == 0) {
      u64 expect = 0;

      if (__atomic_compare_exchange_n(&g_kprof[i].addr, &expect, key, 0,
                                      __ATOMIC_RELAXED, __ATOMIC_RELAXED) ||
          expect == key) {
        __atomic_fetch_add(&g_kprof[i].hits, weight, __ATOMIC_RELAXED);
        return;
      }
    }
  }
  __atomic_fetch_add(&g_kprof_dropped, 1, __ATOMIC_RELAXED);
}

/* Aggregate the raw buckets by symbol and print the heaviest. The table is
 * small and this runs once per read, so an O(n^2) merge is cheaper than an
 * index nobody else needs. */
#define KPROF_TOP 30

static u64 g_sym_addr[KPROF_SLOTS];
static u64 g_sym_hits[KPROF_SLOTS];

static void kprof_dump_histogram(void) {
  u64 total = __atomic_load_n(&g_kprof_samples, __ATOMIC_RELAXED);

  console_write("kprof: kernel-rip samples=");
  console_write_dec(total);
  console_write(" dropped=");
  console_write_dec(__atomic_load_n(&g_kprof_dropped, __ATOMIC_RELAXED));
  console_write("\n");
  if (!total)
    return;

  /* Pass 1: fold every bucket onto the start address of its symbol. */
  usize nsym = 0;

  for (u32 i = 0; i < KPROF_SLOTS; i++) {
    u64 a = g_kprof[i].addr;
    u64 hits = g_kprof[i].hits;

    if (!a || !hits)
      continue;

    u64 off = 0;
    const char *name = ksym_lookup(a, &off);
    u64 base = name ? a - off : a;
    usize j;

    for (j = 0; j < nsym; j++) {
      if (g_sym_addr[j] == base) {
        g_sym_hits[j] += hits;
        break;
      }
    }
    if (j == nsym && nsym < KPROF_SLOTS) {
      g_sym_addr[nsym] = base;
      g_sym_hits[nsym] = hits;
      nsym++;
    }
  }

  /* Pass 2: selection sort of the top KPROF_TOP only. */
  for (usize k = 0; k < KPROF_TOP && k < nsym; k++) {
    usize best = k;

    for (usize j = k + 1; j < nsym; j++)
      if (g_sym_hits[j] > g_sym_hits[best])
        best = j;

    u64 ta = g_sym_addr[k], th = g_sym_hits[k];

    g_sym_addr[k] = g_sym_addr[best];
    g_sym_hits[k] = g_sym_hits[best];
    g_sym_addr[best] = ta;
    g_sym_hits[best] = th;

    console_write("  kprof ");
    console_write_dec(g_sym_hits[k] * 1000 / total);
    console_write(" permil ");
    console_write_dec(g_sym_hits[k]);
    console_write(" 0x");
    console_write_hex64(g_sym_addr[k]);
    ksym_print(g_sym_addr[k]);
    console_write("\n");
  }
}

/* The heaviest individual buckets, with their offset into the containing
 * symbol. A function of four thousand bytes can be hot for any of a dozen
 * reasons, and the symbol total does not say which; these addresses go
 * straight into llvm-addr2line -f -i and name the line. */
#define KPROF_TOP_RAW 15

static void kprof_dump_raw(void) {
  static u32 order[KPROF_TOP_RAW];
  usize n = 0;

  for (u32 i = 0; i < KPROF_SLOTS; i++) {
    if (!g_kprof[i].addr || !g_kprof[i].hits)
      continue;

    if (n < KPROF_TOP_RAW)
      order[n++] = i;
    else if (g_kprof[i].hits <= g_kprof[order[n - 1]].hits)
      continue;
    else
      order[n - 1] = i;

    /* Bubble the newcomer up into descending order. */
    for (usize p = n - 1; p > 0 && g_kprof[order[p]].hits > g_kprof[order[p - 1]].hits;
         p--) {
      u32 tmp = order[p];

      order[p] = order[p - 1];
      order[p - 1] = tmp;
    }
  }
  for (usize k = 0; k < n; k++) {
    console_write("  kprof-raw ");
    console_write_dec(g_kprof[order[k]].hits);
    console_write(" 0x");
    console_write_hex64(g_kprof[order[k]].addr);
    ksym_print(g_kprof[order[k]].addr);
    console_write("\n");
  }
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

  kprof_dump_histogram();
  kprof_dump_raw();
}
