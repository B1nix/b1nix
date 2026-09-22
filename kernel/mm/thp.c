/* SPDX-License-Identifier: GPL-2.0-only */
/* Transparent huge pages: the policy, the counters, and nothing else.
 *
 * Why this is opt-in. A 2 MiB entry in the fault path is the easy half; the
 * hard half is that every walker which meets one has to either understand it
 * or break it up first, and a walker that quietly does neither is silent
 * memory corruption rather than a slow machine. The feature therefore starts
 * off, and the whole smoke suite is run with it both off and on. Both ports
 * have it: a 2 MiB directory entry on x86_64, a level-2 block descriptor on
 * aarch64, and the same rule keeps each of them tractable — the fault path is
 * the only thing that creates one.
 */
#include <b1nix/thp.h>
#include <b1nix/bootinfo.h>
#include <b1nix/mm.h>
#include <b1nix/sched.h>
#include <b1nix/spinlock.h>
#include <string.h>

static int g_thp_mode = THP_MODE_NEVER;
static u64 g_thp_alloc;
static u64 g_thp_fallback;
static u64 g_thp_split;
static u64 g_thp_collapse;

void thp_init(void) {
  char value[16];

  /* bootinfo_get_kv reports PRESENCE, and a value only comes back with it:
   * non-zero means the command line carried `b1nix.thp=<something>`. A bare
   * `b1nix.thp` is not a key/value pair and is found by the flag test below.
   * Getting this the wrong way round read an uninitialised buffer and made
   * `=always` mean madvise. */
  value[0] = '\0';
  if (bootinfo_get_kv("b1nix.thp", value, sizeof(value)) && value[0]) {
    if (strcmp(value, "always") == 0)
      g_thp_mode = THP_MODE_ALWAYS;
    else if (strcmp(value, "never") == 0 || strcmp(value, "0") == 0)
      g_thp_mode = THP_MODE_NEVER;
    else
      g_thp_mode = THP_MODE_MADVISE;
    return;
  }
  if (bootinfo_has_flag("b1nix.thp"))
    g_thp_mode = THP_MODE_MADVISE;
}

int thp_mode(void) { return g_thp_mode; }

void thp_set_mode(int mode) {
  if (mode >= THP_MODE_NEVER && mode <= THP_MODE_ALWAYS)
    g_thp_mode = mode;
  /* A machine that boots with the feature off and has it turned on through
   * sysfs gets khugepaged then, rather than only on the next boot. */
  if (g_thp_mode != THP_MODE_NEVER)
    thp_khugepaged_maybe_start();
}

int thp_vma_eligible(const struct vm_area *vma) {
  if (g_thp_mode == THP_MODE_NEVER || !vma)
    return 0;
  if (vma->thp < 0)          /* MADV_NOHUGEPAGE always wins */
    return 0;
  if (g_thp_mode == THP_MODE_MADVISE && vma->thp <= 0)
    return 0;
  /* Anonymous private memory only. A file mapping's pages come from the page
   * cache one at a time, and a shared anonymous one is a memfd underneath —
   * both are 4 KiB objects with their own refcounts, and neither can be
   * handed a contiguous block without teaching the page cache about it. */
  if (vma->node)
    return 0;
  if (vma->flags & MAP_SHARED)
    return 0;
  if (vma->special)
    return 0;
  if (vma->prot == PROT_NONE)
    return 0;
  /* A block has to fit whole, or the mapping's edges would gain pages it
   * never asked for. */
  if (vma->end - vma->start < THP_SIZE)
    return 0;
  return 1;
}

void thp_count_alloc(void) { __atomic_add_fetch(&g_thp_alloc, 1, __ATOMIC_RELAXED); }
void thp_count_fallback(void) { __atomic_add_fetch(&g_thp_fallback, 1, __ATOMIC_RELAXED); }
void thp_count_split(void) { __atomic_add_fetch(&g_thp_split, 1, __ATOMIC_RELAXED); }
void thp_count_collapse(void) {
  __atomic_add_fetch(&g_thp_collapse, 1, __ATOMIC_RELAXED);
}
u64 thp_stat_alloc(void) { return __atomic_load_n(&g_thp_alloc, __ATOMIC_RELAXED); }
u64 thp_stat_fallback(void) { return __atomic_load_n(&g_thp_fallback, __ATOMIC_RELAXED); }
u64 thp_stat_split(void) { return __atomic_load_n(&g_thp_split, __ATOMIC_RELAXED); }
u64 thp_stat_collapse(void) {
  return __atomic_load_n(&g_thp_collapse, __ATOMIC_RELAXED);
}

/* ── page tables a block replaced ─────────────────────────────────────────
 *
 * Why they are kept rather than freed is in <b1nix/thp.h>. The list is
 * threaded through the frames themselves — word 0 is the next frame, word 1
 * the address space that owned it — so it costs no memory of its own, and
 * both words are page-aligned physical addresses, which every architecture's
 * walkers read as an absent entry.
 */
static spinlock_t thp_orphan_lock = SPINLOCK_INIT;
static u64 thp_orphan_head;

void thp_orphan_table(u64 frame, u64 owner_space) {
  u64 *words = frame ? paging_table_map(frame) : 0;
  u64 flags;

  if (!words || !owner_space)
    return;
  spin_lock_irqsave(&thp_orphan_lock, &flags);
  words[0] = thp_orphan_head;
  words[1] = owner_space;
  thp_orphan_head = frame;
  spin_unlock_irqrestore(&thp_orphan_lock, flags);
}

void thp_release_orphan_tables(u64 owner_space) {
  u64 flags;
  u64 taken = 0;

  if (!owner_space)
    return;
  spin_lock_irqsave(&thp_orphan_lock, &flags);
  {
    u64 *prev_word = 0;
    u64 frame = thp_orphan_head;

    while (frame) {
      u64 *words = paging_table_map(frame);
      u64 next = words ? words[0] : 0;

      if (words && words[1] == owner_space) {
        if (prev_word)
          *prev_word = next;
        else
          thp_orphan_head = next;
        words[0] = taken; /* re-linked onto the local list, freed below */
        taken = frame;
      } else if (words) {
        prev_word = &words[0];
      }
      frame = next;
    }
  }
  spin_unlock_irqrestore(&thp_orphan_lock, flags);

  while (taken) {
    u64 *words = paging_table_map(taken);
    u64 next = words[0];

    words[0] = 0;
    words[1] = 0;
    pmm_note_page_table(taken, 0);
    pmm_free_frame(taken);
    taken = next;
  }
}

/* ── khugepaged ───────────────────────────────────────────────────────────
 *
 * Why it has to exist: the fault path installs a block only where nothing
 * describes the address yet, so a program gets blocks for the memory it maps
 * AFTER the feature is on and 4 KiB pages for everything it had already
 * touched. A shell that has been running since boot, or any program whose
 * allocator faulted its arena in early, would never hold one. This thread is
 * the other direction — it finds a 2 MiB range that is already 512 ordinary
 * pages and has the architecture replace it with one block.
 *
 * Deliberately slow and deliberately dumb: one pass every
 * scan_sleep_millisecs, at most THP_SCAN_BUDGET blocks per pass, and every
 * decision about whether a range can be collapsed belongs to
 * paging_thp_collapse, which holds the page-table lock across the copy. The
 * cost of being wrong here is silent corruption, so the thread does no page
 * table work of its own at all.
 */
#define THP_SCAN_BUDGET 8
#define THP_SCAN_SLEEP_DEFAULT_MS 200

static u64 g_thp_scan_sleep_ms = THP_SCAN_SLEEP_DEFAULT_MS;
static int g_khugepaged_started;

u64 thp_scan_sleep_ms(void) {
  return __atomic_load_n(&g_thp_scan_sleep_ms, __ATOMIC_RELAXED);
}

void thp_set_scan_sleep_ms(u64 ms) {
  /* A floor, not a veto: zero would make the thread a busy loop on a machine
   * whose administrator meant "as often as possible". */
  if (ms < 10)
    ms = 10;
  if (ms > 60000)
    ms = 60000;
  __atomic_store_n(&g_thp_scan_sleep_ms, ms, __ATOMIC_RELAXED);
}

/* One pass over one task: collapse what can be collapsed, up to `budget`
 * blocks. Returns how many it took. The mapping list is walked inside the
 * counted-walker bracket, because it belongs to another task and a concurrent
 * munmap may unlink from it. */
static int khugepaged_scan_task(struct task *t, int budget) {
  int took = 0;

  if (!t || !t->pml4_phys || !t->vma_list)
    return 0;
  vma_walker_enter();
  for (struct vm_area *v = t->vma_list; v && took < budget; v = v->next) {
    if (!thp_vma_eligible(v))
      continue;
    for (u64 base = (v->start + THP_SIZE - 1) & ~(THP_SIZE - 1);
         base + THP_SIZE <= v->end && took < budget; base += THP_SIZE) {
      if (paging_thp_collapse(t, v, base) > 0)
        took++;
    }
  }
  vma_walker_exit();
  return took;
}

static void khugepaged_thread(void *arg) {
  (void)arg;
  for (;;) {
    scheduler_sleep_ticks(SCHED_MS_TO_TICKS(thp_scan_sleep_ms()));
    if (thp_mode() == THP_MODE_NEVER)
      continue;

    usize slots = scheduler_task_slots();
    int budget = THP_SCAN_BUDGET;

    for (usize i = 0; i < slots && budget > 0; i++) {
      struct task *t = scheduler_task_slot(i);

      if (!t || !t->pml4_phys)
        continue;
      budget -= khugepaged_scan_task(t, budget);
    }
  }
}

void thp_khugepaged_maybe_start(void) {
  if (g_thp_mode == THP_MODE_NEVER || g_khugepaged_started)
    return;
  if (!scheduler_can_block())
    return; /* too early: the next caller starts it */
  g_khugepaged_started = 1;
  if (kthread_create("khugepaged", khugepaged_thread, 0) < 0)
    g_khugepaged_started = 0;
}
