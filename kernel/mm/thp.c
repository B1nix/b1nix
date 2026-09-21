/* SPDX-License-Identifier: GPL-2.0-only */
/* Transparent huge pages: the policy, the counters, and nothing else.
 *
 * Why this is opt-in. A 2 MiB entry in the fault path is the easy half; the
 * hard half is that every walker which meets one has to either understand it
 * or break it up first, and a walker that quietly does neither is silent
 * memory corruption rather than a slow machine. The feature therefore starts
 * off, and the whole smoke suite is run with it both off and on.
 */
#include <b1nix/thp.h>
#include <b1nix/bootinfo.h>
#include <b1nix/mm.h>
#include <b1nix/sched.h>
#include <string.h>

static int g_thp_mode = THP_MODE_NEVER;
static u64 g_thp_alloc;
static u64 g_thp_fallback;
static u64 g_thp_split;

void thp_init(void) {
#if defined(__x86_64__)
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
#else
  /* aarch64 keeps its own page-table walkers, and none of them has been
   * audited for a block descriptor in a user mapping. The knob stays at
   * "never" there and the fault path never installs one. */
#endif
}

int thp_mode(void) { return g_thp_mode; }

void thp_set_mode(int mode) {
#if defined(__x86_64__)
  if (mode >= THP_MODE_NEVER && mode <= THP_MODE_ALWAYS)
    g_thp_mode = mode;
#else
  (void)mode;
#endif
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
u64 thp_stat_alloc(void) { return __atomic_load_n(&g_thp_alloc, __ATOMIC_RELAXED); }
u64 thp_stat_fallback(void) { return __atomic_load_n(&g_thp_fallback, __ATOMIC_RELAXED); }
u64 thp_stat_split(void) { return __atomic_load_n(&g_thp_split, __ATOMIC_RELAXED); }
