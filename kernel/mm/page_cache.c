#include <b1nix/blk.h>
#include <b1nix/arch.h>
#include <b1nix/page_cache.h>
#include <b1nix/vfs.h>
#include <b1nix/sched.h>
#include <b1nix/mm.h>
#include <b1nix/errno.h>
#include <b1nix/console.h>
#include <string.h>
#include <b1nix/bootinfo.h>
#include <b1nix/klog.h>
#include <b1nix/lapic.h>
#include <b1nix/arch.h>

/* Hash width, derived at init rather than compiled in.
 *
 * The page cache has no capacity ceiling of its own — eviction is target-
 * driven, not size-capped — so the number of resident pages is bounded only by
 * memory. A fixed 1024 buckets meant that on anything but a small guest the
 * chains grew without limit: a machine holding 200k cached pages ran chains
 * roughly two hundred long, and every lookup, insert and evict walked one.
 *
 * Linux sizes its page-cache and dentry hashes the same way, from the memory
 * present at boot (alloc_large_system_hash). One bucket per 16 pages of RAM
 * keeps the average chain short whatever the machine.
 *
 * FLOOR   1024 buckets — exactly the previous fixed size, so a 256 MiB guest
 *         allocates what it always had and nothing about it changes.
 * CEILING 262144 buckets — 2 MiB of pointers plus 1 MiB of bucket locks, i.e.
 *         0.04% of the 8 GiB machine that first reaches it. Beyond that the
 *         table costs more than the chain walk it saves.
 * `b1nix.pagecache-buckets=N` overrides; N is rounded up to a power of two and
 * still clamped to the range above. */
#define PC_HASH_MIN 1024u
#define PC_HASH_MAX 262144u

static struct page_cache_entry **hash_table;
static u32 pc_hash_size = PC_HASH_MIN; /* always a power of two */
static u32 pc_hash_mask = PC_HASH_MIN - 1;
/* Variant E — two LRU lists. lru_head/lru_tail is the INACTIVE list (pages seen
 * once); active_head/active_tail is the ACTIVE list (the protected working set:
 * pages referenced again or refaulted). New pages land on inactive; a second
 * reference (page_cache_get_page hit) promotes to active. Eviction drains
 * inactive first and only demotes active->inactive when inactive is empty, so a
 * one-shot scan (a source file read once) is reclaimed before clang's text that
 * every TU touches. */
static struct page_cache_entry *lru_head;       /* inactive (oldest at head) */
static struct page_cache_entry *lru_tail;
static struct page_cache_entry *active_head;     /* active (oldest at head) */
static struct page_cache_entry *active_tail;
/* A queue, not a scramble — the same change the console lock needed.
 *
 * This is the busiest global lock in the kernel: every read, every write and
 * every eviction passes through it. As a test-and-set lock it has no order, so
 * a CPU can lose the exchange indefinitely while others keep re-taking it. The
 * console lock proved that is not theoretical here — it starved until the
 * kernel's own lockup detector fired on a lock whose value was zero. `next` is
 * the ticket drawn on arrival, `owner` is whose turn it is. */
static volatile u32 pc_lock_next;
static volatile u32 pc_lock_owner;

/* Refault ring: keys (ino,offset) of pages evicted recently. If a page is
 * re-added soon after eviction it was wrongly evicted (working set bigger than
 * the inactive list), so seed it straight onto the active list. A small ring is
 * enough to catch the hot set churning under pressure. */
#define PC_REFAULT_N 256
static struct { u64 ino; u64 offset; } pc_refault[PC_REFAULT_N];
static u32 pc_refault_w;

static void pc_refault_record(u64 ino, u64 offset) {
  pc_refault[pc_refault_w].ino = ino;
  pc_refault[pc_refault_w].offset = offset;
  pc_refault_w = (pc_refault_w + 1) % PC_REFAULT_N;
}
static int pc_refault_hit(u64 ino, u64 offset) {
  for (u32 i = 0; i < PC_REFAULT_N; i++)
    if (pc_refault[i].ino == ino && pc_refault[i].offset == offset)
      return 1;
  return 0;
}

static struct page_cache_entry *to_free_list = 0;

static int is_power_of_two_u64(u64 value) {
  return value && ((value & (value - 1)) == 0);
}

static void m26_diag_task(void) {
  if (current_task && current_task->name) {
    console_write(" task=");
    console_write(current_task->name);
    console_write(" pid=");
    console_write_dec(current_task->id);
  }
}

/* Per-bucket locks, in front of the one cache-wide lock.
 *
 * Everything used to serialise on pc_lock, including the lookup — the hottest
 * path in the kernel, taken by every mapped page of every executable, by every
 * read(), and by the fault handler's read-ahead. Two CPUs faulting on
 * unrelated files queued behind each other for no reason: their entries live
 * in different hash chains and share nothing.
 *
 * The rule that keeps this deadlock-free is one-directional: a reader takes
 * only its bucket, never pc_lock. A mutator takes pc_lock first and the bucket
 * second, and holds the bucket only across the chain edit itself — never
 * across writeback, which drops pc_lock and blocks on I/O.
 */
static volatile int *pc_bucket;

/* Interrupts off while a bucket is held, for the same reason the cache-wide
 * lock masks them.
 *
 * The lookup path takes a bucket with interrupts enabled, so its holder could
 * be preempted mid-chain — and a CPU that holds pc_lock (masked) then waits on
 * that same bucket forever, while every other CPU waits on pc_lock behind it.
 * Nothing can run the preempted holder, because nothing is left to run it on.
 * The section is a few pointer reads; masking costs nothing and removes the
 * only way it can be interrupted. */
static u64 lock_bucket(u32 h) {
  extern void tlb_shootdown_poll(void);

  /* Preemption off, interrupts on.
   *
   * The holder must not be descheduled — a CPU inside the cache-wide lock can
   * be waiting for this bucket, and nothing would be left to run the holder.
   * Masking interrupts would do it too, but this section sits on the lookup
   * path of every read in the system, and the machine still needs its timer. */
  scheduler_preempt_disable();
  while (__sync_lock_test_and_set(&pc_bucket[h], 1)) {
    while (pc_bucket[h]) {
      cpu_relax();
      tlb_shootdown_poll();
    }
  }
  return 0;
}

static void unlock_bucket(u32 h, u64 flags) {
  (void)flags;
  __sync_lock_release(&pc_bucket[h]);
  scheduler_preempt_enable();
}

/* Who holds pc_lock, so a wedge can name its owner instead of being guessed at.
 *
 * Three runs of a browser under a compositor ended with every CPU spinning
 * here and the console silent — a hard wedge that looks, from outside, exactly
 * like a slow machine. Nothing in the dump said which call had taken the lock
 * and not given it back, so the search was a reading of the source rather than
 * a measurement. These three words cost one store per acquire and turn the
 * next occurrence into an address. */
static volatile u64 pc_lock_owner_ra;
static volatile u64 pc_lock_owner_task;
static volatile int pc_lock_owner_cpu = -1;
/* Set by a waiter that has spun past the threshold, read by the task dump. */
static volatile u64 pc_lock_stuck_waiter_ra;
static volatile u32 pc_lock_stuck_count;

/* Spins before a waiter says so. At a few nanoseconds an iteration this is
 * some seconds of real contention — far longer than any legitimate hold of
 * this lock, which never blocks and never does I/O. */
#define PC_LOCK_STUCK_SPINS 400000000ull

/* Deliberately silent.
 *
 * The first version of this printed the holder from inside the spin loop, and
 * that made the wedge worse rather than visible: console_write takes the
 * console lock with interrupts masked, so a waiter that started reporting
 * could no longer be preempted, and it joined a second queue while still in
 * the first. The record below is enough — pc_lock_owner_ra and
 * pc_lock_owner_task are ordinary variables, readable from a debugger or a
 * monitor on a machine that has stopped, and that is exactly when they are
 * wanted. The task dump prints them from a context that may safely print. */
static void pc_lock_report_stuck(const char *what, u64 waiter_ra) {
  (void)what;
  pc_lock_stuck_waiter_ra = waiter_ra;
  __atomic_add_fetch(&pc_lock_stuck_count, 1u, __ATOMIC_RELAXED);
}

/* What the page-cache lock is doing, for the task dump.
 *
 * A machine whose CPUs are all spinning here says nothing about why. These
 * three numbers do: whether the lock is held at all, which call took it, and
 * which task that was. Printed from the watchdog's dump, never from the spin
 * loop. */
void page_cache_dump_lock(void) {
  u32 next = __atomic_load_n(&pc_lock_next, __ATOMIC_ACQUIRE);
  u32 owner = __atomic_load_n(&pc_lock_owner, __ATOMIC_ACQUIRE);

  console_write("pc_lock: ");
  if (next == owner) {
    console_write("free\n");
    return;
  }
  console_write("held, ");
  console_write_dec((u64)(next - owner - 1));
  console_write(" waiting, taken at 0x");
  console_write_hex64(pc_lock_owner_ra);
  ksym_print(pc_lock_owner_ra);
  console_write(" by task ");
  console_write_dec(pc_lock_owner_task);
  console_write(" on cpu ");
  console_write_dec((u64)(unsigned)pc_lock_owner_cpu);
  if (pc_lock_stuck_count) {
    console_write(", a waiter gave up at 0x");
    console_write_hex64(pc_lock_stuck_waiter_ra);
    ksym_print(pc_lock_stuck_waiter_ra);
  }
  console_write("\n");
}

static void pc_lock_took(u64 ra) {
  pc_lock_owner_ra = ra;
  pc_lock_owner_cpu = (int)percpu_read(cpu_id);
  pc_lock_owner_task = current_task ? current_task->id : 0;
}

static void lock_pc(void) {
  extern void tlb_shootdown_poll(void);
  u64 spins = 0;
  u64 ra = (u64)(usize)__builtin_return_address(0);
  /* Preemption off BEFORE the ticket is drawn, and kept off until the release.
   *
   * A ticket lock hands the turn to one particular waiter. If that waiter can
   * be descheduled while it spins, its turn comes up while it is off-CPU and
   * everybody behind it waits for a task no CPU is running — which is how
   * three runs died with "pc_lock: held, 7 waiting" repeated every thirty
   * seconds and the holder's record already cleared.
   *
   * Preemption, not interrupts. Masking interrupts also stops the handoff, and
   * it was the first thing tried, but the sections under this lock include
   * walks of the whole cache — a hundred thousand entries on a large guest —
   * and running those with the timer off starves everything the machine does
   * on a clock. The browser went from printing its document in ninety seconds
   * to not printing it in seven hundred. Nothing here sleeps, so keeping the
   * task on its CPU is all that is required. */
  scheduler_preempt_disable();
  u32 me = __atomic_fetch_add(&pc_lock_next, 1u, __ATOMIC_SEQ_CST);

  while (__atomic_load_n(&pc_lock_owner, __ATOMIC_ACQUIRE) != me) {
    /* Drain TLB shootdowns while spinning — a waiter with interrupts masked
     * cannot ACK the initiator's IPI, and the initiator waits masked too. */
    /* cpu_relax(), not a bare `pause`: that mnemonic exists only on x86 and
     * this file is built for both arches again. */
    cpu_relax();
    tlb_shootdown_poll();
    if (++spins == PC_LOCK_STUCK_SPINS)
      pc_lock_report_stuck("lock_pc", ra);
  }
  pc_lock_took(ra);
}

static void unlock_pc(void) {
  pc_lock_owner_ra = 0;
  pc_lock_owner_cpu = -1;
  pc_lock_owner_task = 0;
  __atomic_add_fetch(&pc_lock_owner, 1u, __ATOMIC_RELEASE);
  scheduler_preempt_enable();
}

/* Pages this cache is holding, so /proc/meminfo can say Cached.
 *
 * Without it "where did the memory go" has no answer at all: MemFree falling
 * to zero says nothing about whether the kernel is caching file data it would
 * give back under pressure, or whether something is genuinely leaked. Counted
 * where the entries are created and destroyed, which is the only place that
 * sees every one of them. */
static volatile u64 g_pc_resident_pages;

static void page_cache_process_deferred_free(void) {
  lock_pc();
  struct page_cache_entry *curr = to_free_list;
  to_free_list = 0;
  unlock_pc();
  
  while (curr) {
    struct page_cache_entry *next = curr->hash_next;
    __atomic_sub_fetch(&g_pc_resident_pages, 1, __ATOMIC_RELAXED);
    kfree(curr);
    curr = next;
  }
}

/* Hash on (fs_id, ino), NOT the inode pointer: the inode slab pool reuses
 * addresses, and pointer-keyed entries of a destroyed inode would collide with
 * a later inode landing at the same address (observed: ld.so mapping libOSMesa
 * got another file's cached page for offset 0). ino alone is not enough either
 * — it is only unique within one filesystem, so with initramfs and the ext4
 * root both mounted, two unrelated files sharing an ino number served each
 * other's pages (observed: read() of libpam.so.2 returning another mapping's
 * live pointers instead of its .plt bytes). */
static u32 pc_hash(struct vfs_inode *inode, u64 offset) {
  u64 val = (inode ? inode->ino : 0) ^ ((u64)(inode ? inode->fs_id : 0) << 32) ^
            (offset >> 12);
  val ^= val >> 16;
  return (u32)val & pc_hash_mask;
}

/* Full cache-key comparison: (fs_id, ino, offset). See the key_fsid comment in
 * page_cache.h for why the filesystem id is part of the identity. */
static int pc_key_eq(const struct page_cache_entry *e, struct vfs_inode *inode,
                     u64 offset) {
  return e->key_ino == inode->ino && e->key_fsid == inode->fs_id &&
         e->offset == offset;
}

/* Unlink from whichever list (active or inactive) the page is currently on —
 * the PAGE_CACHE_ACTIVE flag selects the head/tail pair. */
static void lru_remove(struct page_cache_entry *page) {
  struct page_cache_entry **head = (page->flags & PAGE_CACHE_ACTIVE)
                                       ? &active_head : &lru_head;
  struct page_cache_entry **tail = (page->flags & PAGE_CACHE_ACTIVE)
                                       ? &active_tail : &lru_tail;
  if (page->lru_prev) page->lru_prev->lru_next = page->lru_next;
  else if (*head == page) *head = page->lru_next;

  if (page->lru_next) page->lru_next->lru_prev = page->lru_prev;
  else if (*tail == page) *tail = page->lru_prev;

  page->lru_next = page->lru_prev = 0;
}

/* Append to the INACTIVE list tail (most-recently-added cold page). */
static void lru_append(struct page_cache_entry *page) {
  page->flags &= ~PAGE_CACHE_ACTIVE;
  page->lru_next = 0;
  page->lru_prev = lru_tail;
  if (lru_tail) lru_tail->lru_next = page;
  else lru_head = page;
  lru_tail = page;
}

/* Append to the ACTIVE list tail (working set, MRU end). */
static void active_append(struct page_cache_entry *page) {
  page->flags |= PAGE_CACHE_ACTIVE;
  page->lru_next = 0;
  page->lru_prev = active_tail;
  if (active_tail) active_tail->lru_next = page;
  else active_head = page;
  active_tail = page;
}

/* Demote up to `n` oldest active pages to the inactive list so eviction can
 * reclaim them once the inactive list is drained. Returns how many moved. */
static usize demote_active(usize n) {
  usize moved = 0;
  while (moved < n && active_head) {
    struct page_cache_entry *p = active_head;
    lru_remove(p);       /* unlinks from active (flag still set) */
    lru_append(p);       /* clears ACTIVE, appends to inactive */
    moved++;
  }
  return moved;
}

static void ra_init(void); /* readahead sizing, defined with the RA machinery */

/* Round up to a power of two, saturating at `max` (itself a power of two). */
static u32 pc_pow2_ceil(u32 v, u32 max) {
  u32 p = 1;

  while (p < v && p < max)
    p <<= 1;
  return p;
}

void page_cache_init(void) {
  u64 ram_mb = pmm_total_usable_memory() / (1024ULL * 1024ULL);
  /* One bucket per 16 pages of RAM: ram_mb * (256 pages/MiB) / 16. */
  u32 want = bootinfo_get_u32("b1nix.pagecache-buckets",
                            (u32)(ram_mb > 0x10000ULL ? 0x10000ULL : ram_mb) * 16u);

  if (want < PC_HASH_MIN)
    want = PC_HASH_MIN;
  if (want > PC_HASH_MAX)
    want = PC_HASH_MAX;
  pc_hash_size = pc_pow2_ceil(want, PC_HASH_MAX);

  hash_table = kzalloc((usize)pc_hash_size * sizeof(*hash_table));
  pc_bucket = kzalloc((usize)pc_hash_size * sizeof(*pc_bucket));
  if (!hash_table || !pc_bucket) {
    /* Fall back to the floor rather than run without a hash at all. */
    if (hash_table)
      kfree(hash_table);
    if (pc_bucket)
      kfree((void *)pc_bucket);
    pc_hash_size = PC_HASH_MIN;
    hash_table = kzalloc((usize)pc_hash_size * sizeof(*hash_table));
    pc_bucket = kzalloc((usize)pc_hash_size * sizeof(*pc_bucket));
  }
  pc_hash_mask = pc_hash_size - 1;

  ra_init();

  lru_head = lru_tail = 0;
  active_head = active_tail = 0;
  pc_refault_w = 0;
  for (u32 i = 0; i < PC_REFAULT_N; i++) { pc_refault[i].ino = 0; pc_refault[i].offset = 0; }
  pc_lock_next = 0;
  pc_lock_owner = 0;
}

/* How many cached pages are dirty right now. See page_cache_flush_inode. */
static volatile u64 g_pc_dirty_pages;

u64 page_cache_dirty_pages(void) { return g_pc_dirty_pages; }

u64 page_cache_resident_pages(void) {
  return __atomic_load_n(&g_pc_resident_pages, __ATOMIC_RELAXED);
}

/* ── File-level sequential readahead ────────────────────────────────────────
 * A small per-inode cursor table detects a sequential access pattern in
 * page_cache_get_page. On a sequential cache MISS it prefetches the next
 * earned window of pages (see ra_init) so a file read sequentially — the
 * reading sources/headers, or a sequential mmap scan) stops paying one
 * blocking block read per page. Best-effort by design: a cold table, a
 * non-file inode, no read_cb, or memory pressure simply skips the burst. */
/* Stream table width, derived at init.
 *
 * The table is direct-mapped with no chaining, so a collision does not merely
 * slow a lookup — the two files reset each other's cursor (see the fsid/ino
 * mismatch path below) and NEITHER ever earns a window. 256 slots is fewer hot
 * files than a build or a browser keeps open, so the detector quietly stopped
 * working on exactly the workloads it was written for.
 *
 * FLOOR   256 entries (8 KiB) — the previous fixed size; a 256 MiB guest is
 *         unchanged.
 * CEILING 16384 entries (512 KiB), reached at 16 GiB. Past that the table is
 *         larger than the set of files anything realistically keeps hot.
 * `b1nix.ra-streams=N` overrides, rounded up to a power of two and clamped. */
#define RA_STREAMS_MIN 256u
#define RA_STREAMS_MAX 16384u

/* Read-ahead window ceiling, in pages, derived at init.
 *
 * This is the largest burst a stream can earn; `win` still starts at RA_MIN and
 * doubles, so a random reader never pays it. It is a memory decision as much as
 * an I/O one — read-ahead that gets evicted before it is used was read twice —
 * so it scales with RAM in coarse steps, exactly as the block layer's
 * blk_readahead_ceiling() does.
 *
 * FLOOR   RA_MIN (4 pages, 16 KiB) — a window can never be smaller than the
 *         smallest burst.
 * CEILING RA_WIN_MAX_PAGES (64 pages, 256 KiB), which is also the hard clamp on
 *         the kmalloc inside page_cache_read_cluster. It matches the block
 *         layer's 256 KiB default read-ahead, so one cluster is one device
 *         command rather than four.
 * A machine of 256 MiB or less keeps the previous 16 pages exactly.
 * `b1nix.readahead-pages=N` overrides, clamped to [RA_MIN, RA_WIN_MAX_PAGES]. */
#define RA_WIN_MAX_PAGES 64u

/* Staging buffer for page_cache_read_cluster, in pages. Deliberately a
 * constant and deliberately small: it is a contiguous kmalloc taken inside the
 * page-fault handler, so it must not grow with the window. See the comment at
 * the allocation itself. */
#define PC_CLUSTER_CHUNK_PAGES 16u

#define RA_WARMUP      1  /* sequential misses seen before the first burst */

struct ra_stream {
  u64 ino;
  u32 fsid;
  u64 next; /* next file page expected for this stream (in PAGE_SIZE units) */
  u32 seq;  /* consecutive sequential misses observed */
  /* Current burst size in pages. Starts at RA_MIN, doubles on each further
   * sequential miss up to ra_win_max, and collapses to nothing the moment the
   * stream jumps. Linux's ondemand read-ahead has the same shape: a window that
   * earns its size from the access pattern instead of always reading the
   * maximum, so a random reader pays for the pages it asked for and a long
   * sequential reader still reaches the full window within a few faults. */
  u32 win;
};

static struct ra_stream *ra_streams;
static u32 ra_streams_n = RA_STREAMS_MIN; /* always a power of two */
static u32 ra_streams_mask = RA_STREAMS_MIN - 1;
static u32 ra_win_max = 16; /* pages; the pre-scaling value, see ra_init */
/* Counted for the profile: see page_cache_read_stats. */
static u64 g_pc_readahead_pages;
static void pc_top_note(const struct vfs_inode *inode, u64 pages);
/*
 * "Am *I* inside a prefetch?", not "is anyone".
 *
 * The guard was a single global, which is right only while the prefetch runs
 * on the thread that scheduled it: read one CPU deep, it also silences
 * read-ahead for every other CPU that happens to fault while a prefetch is in
 * progress. That is what made the prefetch worker slower than no worker at
 * all. A small table of the tasks currently prefetching answers the question
 * the recursion check actually asks, and survives the task moving CPU.
 */
#define RA_PREFETCHERS 8u

static void *ra_prefetchers[RA_PREFETCHERS];

static int ra_prefetch_active(void) {
  void *me = (void *)current_task;
  unsigned i;

  for (i = 0; i < RA_PREFETCHERS; i++)
    if (__atomic_load_n(&ra_prefetchers[i], __ATOMIC_ACQUIRE) == me)
      return 1;
  return 0;
}

/* 1 when this thread now owns a slot; the caller must release it. A full
 * table means "too many prefetches at once": the caller skips its burst,
 * which is what the old guard did on re-entry anyway. */
static int ra_prefetch_enter(void) {
  void *me = (void *)current_task;
  unsigned i;

  if (!me || ra_prefetch_active())
    return 0;
  for (i = 0; i < RA_PREFETCHERS; i++) {
    void *expect = 0;

    if (__atomic_compare_exchange_n(&ra_prefetchers[i], &expect, me, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
      return 1;
  }
  return 0;
}

static void ra_prefetch_leave(void) {
  void *me = (void *)current_task;
  unsigned i;

  for (i = 0; i < RA_PREFETCHERS; i++) {
    void *expect = me;

    if (__atomic_compare_exchange_n(&ra_prefetchers[i], &expect, 0, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
      return;
  }
}

static u32 ra_hash(const struct vfs_inode *inode) {
  u32 h = (u32)(inode->ino ^ (inode->ino >> 32));
  h ^= (u32)inode->fs_id;
  h ^= h >> 10;
  return h & ra_streams_mask;
}

/* Smallest burst, in pages. Linux starts its ondemand window at 16 KiB too. */
#define RA_MIN 4

/* What a fault that missed reads when it knows nothing about the stream.
 *
 * Measured on a KDE start-up, varying only this: 64 pages read 885 MB and had
 * the desktop up in 13.1 s, 16 pages read 430 MB and 12.3 s, 8 pages read
 * 334 MB and 12.4 s. Reading a quarter of a megabyte to use four kilobytes of
 * it is what the ceiling did on every scattered mapping -- and a desktop's
 * mappings are scattered. Sixty-four kilobytes is the floor; a stream that
 * proves itself sequential still climbs to the ceiling, one doubling per
 * sequential miss, in page_cache_get_page. */
#define RA_FAULT_MIN 16u

/* The largest cluster any caller may ask page_cache_read_cluster for, in pages.
 * It is the same number as the read-ahead ceiling because the cluster reader
 * kmallocs `pages * PAGE_SIZE` in one go: an unclamped `pages` would turn a
 * tunable into an arbitrary contiguous allocation. */
unsigned page_cache_cluster_pages(void) { return ra_win_max; }

/*
 * How much to read on a fault that missed, for THIS stream.
 *
 * The page-fault handler used to ask for the ceiling every time -- 256 KiB on
 * a 4 GiB guest -- so a scattered mapping read a quarter of a megabyte to use
 * four kilobytes of it. A desktop start-up takes twenty-one thousand
 * file-backed faults and that was 112 us apiece, the largest single cost left
 * in it.
 *
 * The window a stream has earned is the right size: a random reader gets the
 * minimum, and a sequential one still reaches the ceiling within a few faults
 * because page_cache_get_page doubles it on every sequential miss. This only
 * reads that state; the growing is where it always was.
 */
unsigned page_cache_fault_cluster(const struct vfs_inode *inode, u64 offset) {
  const struct ra_stream *r;
  u32 win;

  if (!inode || !ra_streams)
    return RA_FAULT_MIN;
  r = &ra_streams[ra_hash(inode)];
  if (r->ino != inode->ino || r->fsid != inode->fs_id)
    return RA_FAULT_MIN;
  /* One page past the cursor is still this stream: the fault that missed is
   * the one the cursor was waiting for. */
  if (offset / PAGE_SIZE + 1 < r->next || offset / PAGE_SIZE > r->next)
    return RA_FAULT_MIN;
  win = r->win ? r->win : RA_FAULT_MIN;
  if (win < RA_FAULT_MIN)
    win = RA_FAULT_MIN;
  if (win > ra_win_max)
    win = ra_win_max;
  return win;
}

static void ra_init(void) {
  u64 ram_mb = pmm_total_usable_memory() / (1024ULL * 1024ULL);

  /* One stream slot per 4 MiB of RAM: enough that the hot set of a build or a
   * browser fits without the direct-mapped table thrashing. */
  u32 want = bootinfo_get_u32("b1nix.ra-streams",
                            (u32)(ram_mb > 0x100000ULL ? 0x100000ULL : ram_mb) / 4u);

  if (want < RA_STREAMS_MIN)
    want = RA_STREAMS_MIN;
  if (want > RA_STREAMS_MAX)
    want = RA_STREAMS_MAX;
  ra_streams_n = pc_pow2_ceil(want, RA_STREAMS_MAX);
  ra_streams = kzalloc((usize)ra_streams_n * sizeof(*ra_streams));
  if (!ra_streams) {
    ra_streams_n = RA_STREAMS_MIN;
    ra_streams = kzalloc((usize)ra_streams_n * sizeof(*ra_streams));
  }
  ra_streams_mask = ra_streams_n - 1;

  /* Coarse steps, because this picks an I/O size, not an allocation. A small
   * machine keeps the 16 pages it always had. */
  u32 win = 16;

  if (ram_mb >= 2048)
    win = RA_WIN_MAX_PAGES; /* 256 KiB */
  else if (ram_mb >= 1024)
    win = 32; /* 128 KiB */
  win = bootinfo_get_u32("b1nix.readahead-pages", win);
  if (win < RA_MIN)
    win = RA_MIN;
  if (win > RA_WIN_MAX_PAGES)
    win = RA_WIN_MAX_PAGES;
  ra_win_max = win;
}

/*
 * Read-ahead stays on the faulting thread. Twice measured, twice reverted.
 *
 * The idea is sound on its face: a start-up is 77% idle, so the machine has a
 * spare CPU while a process waits for a window of pages it did not ask for.
 * The first attempt was much slower because the re-entrancy guard was a
 * single global and a worker silenced read-ahead for every other CPU. With
 * the guard made per-thread (see ra_prefetch_enter) the worker behaved, and
 * it was still slower: 12.9 s to a desktop against 12.4 s, and the average
 * disk wait rose from 90 us to 102 us. The reason is in the same profile --
 * the device is found busy 33 times in 15741 requests, so it answers one
 * request at a time, and every page the worker prefetches is a request the
 * faulting thread's own read now queues behind.
 *
 * A worker pays only once the driver can keep several requests in flight.
 * Until then the prefetch belongs on the thread that will want the pages.
 */

/* Prefetch `pages` pages starting at (offset+1). Must be called with
 * pc_lock RELEASED: it allocates frames and issues blocking read_cb I/O. It
 * re-enters page_cache_get_page/page_cache_add_page; those re-entries observe
 * ra_in_prefetch and never schedule a nested burst. */
static void pc_readahead(struct vfs_inode *inode, u64 offset, u32 pages) {
  if (!inode->read_cb || pages == 0 || !ra_prefetch_enter())
    return;
  __atomic_fetch_add(&g_pc_readahead_pages, pages, __ATOMIC_RELAXED);
  pc_top_note(inode, pages);
  struct vfs_node dummy;
  memset(&dummy, 0, sizeof(dummy));
  dummy.inode = inode;
  /* offset is a byte offset; the burst walks PAGE NUMBERS.
   *
   * Multiplying the byte offset by PAGE_SIZE again aimed every prefetch at a
   * position PAGE_SIZE times too far into the file: reading page 1 of a binary
   * fetched somewhere past 16 MB. Each page came back under its own correct
   * key, so nothing was corrupted — it simply never prefetched the pages the
   * reader was about to want, and spent a disk command per page to fill the
   * cache with parts of the file nobody asked for. */
  u64 base = offset / PAGE_SIZE;

  /* One filesystem read for the whole burst, not one per page: the same
   * economy the page-fault path gets from page_cache_read_cluster. A burst of
   * separate 4 KiB reads is one disk round trip per page; this is one. */
  ra_prefetch_leave(); /* the cluster reader takes the slot itself */
  page_cache_read_cluster(inode, (base + 1) * PAGE_SIZE, pages);
  if (!ra_prefetch_enter())
    return;
  for (u64 po = base + 1; po <= base + pages; po++) {
    u64 poff = po * PAGE_SIZE;
    if (poff >= inode->size)
      break; /* don't read past EOF */
    struct page_cache_entry *pe = page_cache_get_page(inode, poff);
    if (pe) {
      page_cache_put_page(pe); /* already resident — drop the pin */
      continue;
    }
    u64 frame = pmm_alloc_frame();
    if (!frame)
      break; /* memory pressure — drop the rest of the burst */
    void *virt = (void *)(usize)(frame + vmm_direct_map_base());
    memset(virt, 0, PAGE_SIZE);
    if (inode->read_cb(&dummy, poff, virt, PAGE_SIZE, 0) < 0) {
      pmm_free_frame(frame);
      break;
    }
    if (page_cache_add_page(inode, poff, frame) < 0)
      pmm_free_frame(frame); /* raced with another reader — keep their page */
  }
  ra_prefetch_leave();
}

/* Read a run of pages in ONE call to the filesystem.
 *
 * Demand-paging a quarter-gigabyte executable one page at a time costs a disk
 * round trip per 4 KiB: measured at 2.6 ms a fault, seventeen thousand faults
 * in ninety seconds of start-up, ten seconds of pure waiting. A cluster asks
 * for the whole window at once — up to a quarter of a megabyte, filling sixty-
 * four cache entries — so the fault that follows finds its page already there.
 * That is what the fault-around in the page-fault handler was built to exploit,
 * and until now there was rarely anything cached for it to map.
 *
 * `pages` is clamped to page_cache_cluster_pages() below, so callers ask for a
 * window rather than naming a constant of their own.
 *
 * Best effort throughout: no buffer, no read_cb, a short read or memory
 * pressure simply stops the burst. Must be called with pc_lock RELEASED — it
 * both allocates and does blocking I/O. */
/* How much this run actually read from disk, and in how many requests.
 *
 * "Twenty thousand file faults costing seven G cycles" does not say whether
 * the disk moved three hundred megabytes or five gigabytes, and the two want
 * opposite fixes -- a wider window, or a narrower one. */
static u64 g_pc_cluster_calls, g_pc_cluster_pages;

/* Which files the run reads, by inode.
 *
 * Half a gigabyte read to put a desktop on the screen is either what KDE
 * needs or a file this kernel reads more than once; the total cannot tell
 * them apart, and a per-inode tally can -- the numbers resolve to paths with
 * `debugfs -R "ncheck <ino>"` against the image on the host. A fixed table:
 * the busiest files are few, and a full one simply stops recording. */
#define PC_TOP_SLOTS 64u

struct pc_top {
  u64 ino;
  u32 fsid;
  u64 pages;
};

static struct pc_top g_pc_top[PC_TOP_SLOTS];
static spinlock_t pc_top_lock = SPINLOCK_INIT;

static void pc_top_note(const struct vfs_inode *inode, u64 pages) {
  u64 flags;
  unsigned i;

  if (!inode)
    return;
  spin_lock_irqsave(&pc_top_lock, &flags);
  for (i = 0; i < PC_TOP_SLOTS; i++) {
    if (g_pc_top[i].pages && (g_pc_top[i].ino != inode->ino ||
                              g_pc_top[i].fsid != inode->fs_id))
      continue;
    g_pc_top[i].ino = inode->ino;
    g_pc_top[i].fsid = inode->fs_id;
    g_pc_top[i].pages += pages;
    break;
  }
  spin_unlock_irqrestore(&pc_top_lock, flags);
}

void page_cache_read_top(unsigned n, u64 *ino_out, u64 *pages_out) {
  u64 flags;
  unsigned want, i, j;

  spin_lock_irqsave(&pc_top_lock, &flags);
  for (want = 0; want < n; want++) {
    unsigned best = PC_TOP_SLOTS;

    for (i = 0; i < PC_TOP_SLOTS; i++) {
      if (!g_pc_top[i].pages)
        continue;
      for (j = 0; j < want; j++)
        if (ino_out[j] == g_pc_top[i].ino)
          break;
      if (j < want)
        continue;
      if (best == PC_TOP_SLOTS || g_pc_top[i].pages > g_pc_top[best].pages)
        best = i;
    }
    if (best == PC_TOP_SLOTS)
      break;
    ino_out[want] = g_pc_top[best].ino;
    pages_out[want] = g_pc_top[best].pages;
  }
  spin_unlock_irqrestore(&pc_top_lock, flags);
  for (i = want; i < n; i++) {
    ino_out[i] = 0;
    pages_out[i] = 0;
  }
}

void page_cache_read_stats(u64 *cluster_calls, u64 *cluster_pages,
                           u64 *readahead_pages) {
  if (cluster_calls)
    *cluster_calls = __atomic_load_n(&g_pc_cluster_calls, __ATOMIC_RELAXED);
  if (cluster_pages)
    *cluster_pages = __atomic_load_n(&g_pc_cluster_pages, __ATOMIC_RELAXED);
  if (readahead_pages)
    *readahead_pages = __atomic_load_n(&g_pc_readahead_pages, __ATOMIC_RELAXED);
}

void page_cache_read_cluster(struct vfs_inode *inode, u64 offset,
                             unsigned pages) {
  if (!inode || !inode->read_cb || pages < 2 || ra_prefetch_active())
    return;
  __atomic_fetch_add(&g_pc_cluster_calls, 1, __ATOMIC_RELAXED);
  __atomic_fetch_add(&g_pc_cluster_pages, pages, __ATOMIC_RELAXED);
  pc_top_note(inode, pages);
  /* Hard clamp: `pages` reaches the kmalloc below unchanged, so an unbounded
   * caller would turn a tuning knob into an arbitrary contiguous allocation.
   * The ceiling is the configured window — nothing may ask for more in one
   * cluster than the machine was sized to read ahead. */
  if (pages > ra_win_max)
    pages = ra_win_max;
  u64 base = offset & ~(u64)(PAGE_SIZE - 1);

  if (base >= inode->size)
    return;
  u64 want = (u64)pages * PAGE_SIZE;

  if (base + want > inode->size)
    want = inode->size - base;
  unsigned n = (unsigned)((want + PAGE_SIZE - 1) / PAGE_SIZE);

  if (n < 2)
    return;

  /* The staging buffer is a CONSTANT 64 KiB, not the whole window.
   *
   * This runs in the page-fault handler. Sizing the allocation from the window
   * would put a growing contiguous kmalloc on that path — a quarter of a
   * megabyte on a large machine — and a big contiguous allocation taken during
   * a fault is exactly the wrong thing to depend on: it is the most
   * fragmentation-sensitive request in the kernel, made at the least
   * convenient moment. A window larger than the buffer is read in successive
   * chunks instead. That costs nothing at the device: the filesystems coalesce
   * adjacent blocks into one command (see blk_run_blocks), so a 64 KiB read of
   * a contiguous file is a single request either way. */
  char *buf = kmalloc((usize)PC_CLUSTER_CHUNK_PAGES * PAGE_SIZE);

  if (!buf)
    return;
  if (!ra_prefetch_enter()) {
    kfree(buf);
    return;
  }
  struct vfs_node dummy;

  memset(&dummy, 0, sizeof(dummy));
  dummy.inode = inode;

  for (unsigned done = 0; done < n;) {
    unsigned chunk = n - done;

    if (chunk > PC_CLUSTER_CHUNK_PAGES)
      chunk = PC_CLUSTER_CHUNK_PAGES;

    u64 cbase = base + (u64)done * PAGE_SIZE;
    isize got = inode->read_cb(&dummy, cbase, buf, (usize)chunk * PAGE_SIZE, 0);

    if (got <= 0)
      break;

    unsigned full = (unsigned)(got / PAGE_SIZE);

    for (unsigned i = 0; i < full; i++) {
      u64 poff = cbase + (u64)i * PAGE_SIZE;
      struct page_cache_entry *pe = page_cache_get_page(inode, poff);

      if (pe) {
        page_cache_put_page(pe); /* already resident */
        continue;
      }
      u64 frame = pmm_alloc_frame();

      if (!frame) {
        full = 0; /* memory pressure — stop, the rest faults in on its own */
        break;
      }
      memcpy((void *)(usize)(frame + vmm_direct_map_base()),
             buf + (usize)i * PAGE_SIZE, PAGE_SIZE);
      if (page_cache_add_page(inode, poff, frame) < 0)
        pmm_free_frame(frame); /* another reader won the race */
    }
    if (full < chunk)
      break; /* short read, EOF or pressure — nothing more to stage */
    done += chunk;
  }
  ra_prefetch_leave();
  kfree(buf);
}

struct page_cache_entry *page_cache_get_page(struct vfs_inode *inode, u64 offset) {
  if (to_free_list) {
    page_cache_process_deferred_free();
  }
  u32 h = pc_hash(inode, offset);

  /* The lookup needs its chain, nothing else: the hit path no longer touches
   * the LRU lists (it sets PAGE_CACHE_REFERENCED instead), so the cache-wide
   * lock is not involved at all. */
  u64 bflags_h = lock_bucket(h);
  struct page_cache_entry *curr = hash_table[h];
  while (curr) {
    /* Identity by (fs_id, ino), not the inode pointer — the inode slab pool
     * reuses freed addresses, so a stale entry whose owning inode was already
     * destroyed can still carry a pointer that numerically matches a brand-new,
     * unrelated inode landing at the same address (the exact "ld.so mapping
     * libOSMesa got another file's cached page" class of bug this struct's
     * key_ino field exists to prevent — see page_cache_invalidate_inode). */
    if (pc_key_eq(curr, inode, offset)) {
      /* Atomic, because this is the one refcount update made under the bucket
       * lock while every other one is made under pc_lock: the two do not
       * exclude each other, so a plain ++ here loses updates against a
       * concurrent put. A lost increment is the same use-after-free as an
       * unchecked eviction; a lost decrement leaks the entry. */
      __atomic_add_fetch(&curr->refcount, 1, __ATOMIC_ACQ_REL);
      /* Mark it touched and leave the lists alone.
       *
       * Promoting on every hit meant unlinking and re-linking the entry — four
       * pointer writes under the cache's one lock, on the path every mapped
       * page of every executable takes. The bit says the same thing to
       * eviction, which is the only code that needs to know, and it costs one
       * store. Entries still reach the active list: eviction promotes the ones
       * it finds referenced instead of taking them. */
      __atomic_fetch_or(&curr->flags, PAGE_CACHE_REFERENCED, __ATOMIC_RELAXED);
      /* Hit: a sequential reader landing on an already-prefetched page advances
       * the cursor (so the next burst fires at the right place) without
       * scheduling — the earlier burst already filled this window. */
      if (!ra_prefetch_active() && inode->read_cb && inode->type == VFS_FILE) {
        struct ra_stream *r = &ra_streams[ra_hash(inode)];
        if (r->ino == inode->ino && r->fsid == inode->fs_id &&
            offset / PAGE_SIZE == r->next)
          r->next = offset / PAGE_SIZE + 1;
      }
      unlock_bucket(h, bflags_h);
      return curr;
    }
    curr = curr->hash_next;
  }

  /* Miss: sequential-stream bookkeeping under the bucket lock; the actual
   * prefetch (blocking I/O) runs after it is released. The read-ahead cursors
   * are a heuristic — a rare race between two buckets costs one mispredicted
   * burst, never correctness. */
  u32 ra_pages = 0;
  if (!ra_prefetch_active() && inode->read_cb && inode->type == VFS_FILE) {
    struct ra_stream *r = &ra_streams[ra_hash(inode)];
    if (r->ino == inode->ino && r->fsid == inode->fs_id &&
        offset / PAGE_SIZE == r->next) {
      r->next = offset / PAGE_SIZE + 1;
      if (++r->seq >= RA_WARMUP) {
        /* Grow the window one doubling per sequential miss, never past the
         * ceiling. The first burst after warm-up is deliberately small: a file
         * read once, briefly, in order pays for four pages, not for the whole
         * maximum window it will never touch. */
        u32 next_win = r->win ? r->win * 2u : RA_MIN;
        if (next_win > ra_win_max)
          next_win = ra_win_max;
        r->win = next_win;
        ra_pages = next_win;
      }
    } else {
      /* New stream or a jump — restart the cursor, no burst yet (warm-up), and
       * throw away whatever window the previous pattern had earned. */
      r->ino = inode->ino;
      r->fsid = inode->fs_id;
      r->next = offset / PAGE_SIZE + 1;
      r->seq = 0;
      r->win = 0;
    }
  }
  unlock_bucket(h, bflags_h);

  if (ra_pages)
    pc_readahead(inode, offset, ra_pages);
  return 0;
}

/* Re-entrancy guard for proactive eviction: page_cache_evict() writes dirty
 * pages back through inode->write_cb (ext4), and that write path can itself read
 * + cache blocks, re-entering page_cache_add_page. Without the guard the proactive
 * evict below would recurse. Best-effort across CPUs — a race only costs one extra
 * evict pass (page_cache_evict is internally locked), never corruption. */
static int pc_proactive_evicting;

int page_cache_add_page(struct vfs_inode *inode, u64 offset, u64 frame) {
  if (to_free_list) {
    page_cache_process_deferred_free();
  }

  /* Proactive cap: the page cache holds reclaimable pmm frames, but reactive
   * reclaim only fires on an allocation miss and tops free back up to just
   * total/512 (~1 MB at 512 MB RAM). That razor-thin headroom let the cache grow
   * to the brink, so a burst allocation — clang's staging during the in-guest
   * self-host — outran reclaim and OOM'd at free=0. When free drops below
   * total/16 we trim a small batch of the oldest CLEAN pages before adding a new
   * one, keeping real headroom. Crucially this is clean-ONLY
   * (page_cache_evict_clean): it must never force synchronous dirty .o writeback
   * here. A demand-paged clang faults its text through page_cache_add_page on
   * every new page, and a dirty-writeback in that hot path would drive the slow
   * polled AHCI on each fault and thrash (143 reclaim cycles, OOM). Expensive
   * dirty writeback is deferred to reactive reclaim in pmm_alloc_frames, which
   * only runs on a true allocation miss. On a roomy guest free stays far above
   * the watermark so this never fires. */
  if (!pc_proactive_evicting) {
    usize total_frames = (usize)(pmm_total_usable_memory() / PAGE_SIZE);
    usize watermark = total_frames / 16;
    if (watermark < 512)
      watermark = 512; /* ~2 MB floor so tiny guests still get headroom */
    usize free = pmm_free_frame_count();
    if (free < watermark) {
      usize deficit = watermark - free;
      usize batch = deficit < 32 ? deficit : 32;
      pc_proactive_evicting = 1;
      page_cache_evict_clean(batch);
      pc_proactive_evicting = 0;
    }
  }

  u32 h = pc_hash(inode, offset);

  struct page_cache_entry *new_entry = kmalloc(sizeof(struct page_cache_entry));
  if (new_entry)
    __atomic_add_fetch(&g_pc_resident_pages, 1, __ATOMIC_RELAXED);
  if (!new_entry) return -ENOMEM;

  new_entry->inode = inode;
  new_entry->key_ino = inode->ino;
  new_entry->key_fsid = inode->fs_id;
  new_entry->offset = offset;
  new_entry->frame = frame;
  new_entry->flags = PAGE_CACHE_UPTODATE;
  new_entry->refcount = 0; // Starts at 0, incremented by get_page if needed
  
  /* Mutator order: the cache-wide lock first (the LRU lists below need it),
   * the bucket second, and the bucket only across the chain edit. */
  lock_pc();
  u64 bflags_h = lock_bucket(h);
  // Check if it was added concurrently
  struct page_cache_entry *curr = hash_table[h];
  while (curr) {
    /* (fs_id, ino), not the inode pointer — see the matching comment in
     * page_cache_get_page. */
    if (pc_key_eq(curr, inode, offset)) {
      unlock_bucket(h, bflags_h);
      unlock_pc();
      __atomic_sub_fetch(&g_pc_resident_pages, 1, __ATOMIC_RELAXED);
      kfree(new_entry);
      return -EEXIST;
    }
    curr = curr->hash_next;
  }
  
  pmm_ref_frame(new_entry->frame);
  new_entry->hash_next = hash_table[h];
  hash_table[h] = new_entry;
  if (inode)
    __atomic_add_fetch(&inode->cached_pages, 1, __ATOMIC_RELEASE);
  unlock_bucket(h, bflags_h);

  /* Refault: this page was evicted recently and is already back — the working
   * set is bigger than the inactive list, so seed it straight onto the active
   * list instead of making it climb through inactive again (where it would just
   * be re-evicted). Otherwise it starts cold on the inactive list. */
  if (pc_refault_hit(inode->ino, offset))
    active_append(new_entry);
  else
    lru_append(new_entry);
  unlock_pc();

  return 0;
}

/* Inodes with dirty pages, for the writeback thread.
 *
 * Dirty pages used to reach the disk only when the file was closed: close(2)
 * held the inode lock and wrote every dirty page out before it returned, and
 * sync(2) did not look at the page cache at all. A desktop start-up writes
 * a hundred megabytes of caches that way, each close waiting for its file's
 * pages to hit the disk on the program's own critical path -- 1.6 s of
 * close(2) while Plasma started -- and every one of those flushes walks the
 * whole LRU per dirty page. Now a file that dirties a page joins this list,
 * the writeback thread drains the list twice a second, and sync, syncfs and
 * umount drain it first. Memory-backed inodes (tmpfs, memfd) never join:
 * nothing of theirs is ever written anywhere. */
static struct vfs_inode *g_dirty_inodes;

void page_cache_mark_dirty(struct page_cache_entry *page) {
  lock_pc();
  if (!(page->flags & PAGE_CACHE_DIRTY)) {
    __atomic_add_fetch(&g_pc_dirty_pages, 1, __ATOMIC_RELEASE);
    if (page->inode)
      __atomic_add_fetch(&page->inode->dirty_pages, 1, __ATOMIC_RELEASE);
  }
  page->flags |= PAGE_CACHE_DIRTY;
  struct vfs_inode *inode = page->inode;
  if (inode && !inode->on_dirty_list && !(inode->flags & VFS_NODE_MEMORY_BACKED)) {
    vfs_inode_get(inode);
    inode->on_dirty_list = 1;
    inode->dirty_next = g_dirty_inodes;
    g_dirty_inodes = inode;
  }
  unlock_pc();
}

struct vfs_inode *page_cache_take_dirty_inodes(void) {
  lock_pc();
  struct vfs_inode *list = g_dirty_inodes;
  g_dirty_inodes = 0;
  for (struct vfs_inode *in = list; in; in = in->dirty_next)
    in->on_dirty_list = 0; /* a page dirtied from here on re-links it */
  unlock_pc();
  return list;
}

static void writeback_page_locked(struct page_cache_entry *page) {
  if ((page->flags & PAGE_CACHE_DIRTY) && page->inode && page->inode->write_cb) {
    struct vfs_node dummy;
    memset(&dummy, 0, sizeof(dummy));
    dummy.inode = page->inode;

    usize size = PAGE_SIZE;
    if (page->offset + PAGE_SIZE > page->inode->size) {
      if (page->offset >= page->inode->size) {
        size = 0;
      } else {
        size = page->inode->size - page->offset;
      }
    }

    if (size > 0) {
      /* Pin the entry across the unlocked write_cb: a concurrent
       * page_cache_invalidate_inode must orphan it (refcount != 0), not free
       * it out from under us. */
      /* Atomic: lookups pin pages under their bucket lock alone, so a plain
       * increment here raced theirs, lost counts, and freed an entry that
       * was still on the LRU. */
      __atomic_add_fetch(&page->refcount, 1, __ATOMIC_ACQ_REL);
      unlock_pc();
      void *virt_addr = (void *)(usize)(page->frame + vmm_direct_map_base());
      /* Say whose blocks these are before the filesystem turns the page into
       * block writes, so a later fsync of this file can find them.
       *
       * vfs_write already stamps the owner, but only on the branch that calls
       * write_cb directly. A write that lands in this cache is finished HERE,
       * from a flush, and left every block it produced unowned -- so the
       * per-inode filter in blk_flush_matching matched nothing it was meant to
       * exclude, and, because an unowned block is written rather than risked,
       * every fsync drained the whole dirty cache. Measured on the aarch64 sys
       * lane: 234 blocks written per fsync, all 234 of them unowned, whichever
       * file was being synced -- 47 s across 19 calls. */
      blk_set_dirty_owner(page->inode->fs_id, page->inode->ino);
      page->inode->write_cb(&dummy, page->offset, virt_addr, size, 0);
      blk_clear_dirty_owner();
      lock_pc();
      if (__atomic_sub_fetch(&page->refcount, 1, __ATOMIC_ACQ_REL) == 0 &&
          (page->flags & PAGE_CACHE_ORPHAN)) {
        /* Invalidated while we were writing: finish its teardown here. */
        pmm_free_frame(page->frame);
        if (page->inode && page->inode->cached_pages)
          __atomic_sub_fetch(&page->inode->cached_pages, 1, __ATOMIC_RELEASE);
        page->hash_next = to_free_list;
        to_free_list = page;
        return;
      }
    }

    if (page->flags & PAGE_CACHE_DIRTY) {
      __atomic_sub_fetch(&g_pc_dirty_pages, 1, __ATOMIC_RELEASE);
      if (page->inode && page->inode->dirty_pages)
        __atomic_sub_fetch(&page->inode->dirty_pages, 1, __ATOMIC_RELEASE);
    }
    page->flags &= ~PAGE_CACHE_DIRTY;
  }
}

/* How many cached pages are dirty right now.
 *
 * Flushing one inode means walking both LRU lists, which hold every cached
 * page in the machine — a hundred thousand of them on a 4 GiB guest. close(2)
 * does that walk, and at 1900 closes in a browser start-up it was 132 ms a
 * call for, usually, nothing: the file being closed had no dirty page at all.
 * The counter turns the common case into a load and a branch. */
int page_cache_flush_inode(struct vfs_inode *inode) {
  if (!inode)
    return -1;
  if (__atomic_load_n(&g_pc_dirty_pages, __ATOMIC_ACQUIRE) == 0)
    return 0; /* nothing anywhere is dirty — the walk cannot find anything */
  if (__atomic_load_n(&inode->cached_pages, __ATOMIC_ACQUIRE) == 0)
    return 0;
  if (__atomic_load_n(&inode->dirty_pages, __ATOMIC_ACQUIRE) == 0)
    return 0; /* this file has nothing dirty, whatever the rest of the cache
                 is doing — and the walk is over every cached page */

  /* Walk the inode's cached pages, not its offsets. Probing offset by offset
   * asked the cache for pages that were never resident, and every miss
   * advanced the sequential cursor and armed read-ahead — so closing a large
   * file read most of it back from disk in order to write a few dirty pages
   * out. page_cache_invalidate_inode already walks the LRU lists this way. */
  /* Stop once this inode's dirty pages have been written.
   *
   * The walk is over every cached page in the machine, and the cache is sized
   * from RAM — a 4 GiB guest holds over a hundred thousand entries, so an
   * fsync of a two-page file paid for all of them, with the inode's own lock
   * held throughout. A run doing that per file stalled long enough for the
   * test harness to call the instance hung. The counter says how many there
   * are to find; past that there is nothing left to write. */
  u64 want = __atomic_load_n(&inode->dirty_pages, __ATOMIC_ACQUIRE);
  u64 written = 0;

  /* Restart the walk after every write, exactly as eviction does.
   *
   * writeback_page_locked gives pc_lock back while the filesystem writes, and
   * in that window another CPU may evict the very entry this walk is standing
   * on — so the `curr->lru_next` read after it returned followed a pointer out
   * of a freed structure. Picking one page per pass and finding it from the
   * head again is the same fix the eviction loop already carries, and it costs
   * a list walk per dirty page of one inode, not per page in the machine.
   * Bounded: writeback clears DIRTY, so no page is chosen twice. */
  lock_pc();
  while (written < want) {
    struct page_cache_entry *flush = 0;
    struct page_cache_entry *heads[2] = { lru_head, active_head };

    for (int li = 0; li < 2 && !flush; li++) {
      for (struct page_cache_entry *curr = heads[li]; curr;
           curr = curr->lru_next) {
        if (curr->inode == inode && (curr->flags & PAGE_CACHE_DIRTY) &&
            !(inode->flags & VFS_NODE_MEMORY_BACKED)) {
          flush = curr;
          break;
        }
      }
    }
    if (!flush)
      break;
    writeback_page_locked(flush);
    written++;
  }
  unlock_pc();
  return 0;
}

/* Drop the pages matching `match`: one inode's, or (with inode NULL) every page
 * under (fs_id, ino) that belongs to an inode other than `keep`. */
static void pc_invalidate(struct vfs_inode *inode, u32 fs_id, u64 ino,
                          struct vfs_inode *keep) {
  int invalidated = 0;
  lock_pc();
  /* Walk BOTH LRU lists (inactive then active) — pages of this inode can sit on
   * either after Variant E's promotion. */
  struct page_cache_entry *heads[2] = { lru_head, active_head };
  for (int li = 0; li < 2; li++) {
    struct page_cache_entry *curr = heads[li];
    while (curr) {
      struct page_cache_entry *next = curr->lru_next;
      if (inode ? curr->inode == inode
                : (curr->inode && curr->inode != keep &&
                   curr->key_ino == ino && curr->key_fsid == fs_id)) {
        u32 h = pc_hash(curr->inode, curr->offset);
        u64 bflags_h = lock_bucket(h);
        struct page_cache_entry **prev = &hash_table[h];
        struct page_cache_entry *hcurr = *prev;
        while (hcurr) {
          if (hcurr == curr) {
            *prev = hcurr->hash_next;
            break;
          }
          prev = &hcurr->hash_next;
          hcurr = hcurr->hash_next;
        }
        unlock_bucket(h, bflags_h);
        lru_remove(curr);
        if (curr->inode && curr->inode->cached_pages)
          __atomic_sub_fetch(&curr->inode->cached_pages, 1, __ATOMIC_RELEASE);
        curr->inode = 0;
        if (curr->refcount == 0) {
          pmm_free_frame(curr->frame);
          if (curr->inode && curr->inode->cached_pages)
            __atomic_sub_fetch(&curr->inode->cached_pages, 1, __ATOMIC_RELEASE);
          curr->hash_next = to_free_list;
          to_free_list = curr;
        } else {
          /* A reader still holds a reference (fault path / writeback). The
           * entry is now unreachable (off hash + LRU); mark it ORPHAN so the
           * last put frees the frame and defers the entry. Leaving it in the
           * hash keyed by a soon-to-be-recycled inode is what used to serve
           * another file's page to a new inode at the same address. */
          curr->flags |= PAGE_CACHE_ORPHAN;
        }
        invalidated++;
      }
      curr = next;
    }
  }
  unlock_pc();

  if (bootinfo_has_flag("b1nix.debug.heap") && invalidated > 0) {
    console_write("[M26DIAG] pc_invalidate inode=0x");
    console_write_hex64((u64)(usize)(inode ? inode : keep));
    console_write(" pages=");
    console_write_dec(invalidated);
    m26_diag_task();
    console_write("\n");
  }
}

void page_cache_invalidate_inode(struct vfs_inode *inode) {
  if (!inode)
    return;
  if (__atomic_load_n(&inode->cached_pages, __ATOMIC_ACQUIRE) == 0)
    return;
  pc_invalidate(inode, 0, 0, 0);
}

/* A filesystem that reuses inode numbers hands a new file the number of one
 * that was deleted, and the cache is keyed by that number: pages the deleted
 * file left cached then answered for the new one, and its writes went into
 * them. Called when `inode` is born under (fs_id, ino). */
void page_cache_invalidate_stale(struct vfs_inode *inode) {
  if (!inode || !inode->ino)
    return;
  if (__atomic_load_n(&g_pc_resident_pages, __ATOMIC_ACQUIRE) == 0)
    return;
  pc_invalidate(0, inode->fs_id, inode->ino, inode);
}

/*
 * Take a victim off its hash chain, or refuse because a reader holds it.
 *
 * A reader takes its reference in page_cache_get_page under the entry's BUCKET
 * lock and nothing else -- it never touches pc_lock. So an evictor holding
 * pc_lock has no exclusion against that increment at all, and testing
 * refcount during the LRU scan proves nothing by the time the entry is
 * unlinked: between the two, a reader can find the entry, pin it, and return
 * the pointer, after which this code frees the frame and defers the entry to
 * kfree while that reader is still dereferencing it. That is a real
 * use-after-free -- it was observed as a #GP in node_read_impl's memcpy with a
 * source address that was not an address at all but the reused bytes of the
 * freed entry.
 *
 * The test has to happen under the same lock the increment does. Once this
 * holds the bucket, either the reader's increment is already visible (refuse,
 * and let the caller move on) or it cannot happen: a reader that has not yet
 * walked the chain will not find the entry after this unlinks it.
 *
 * Returns 1 with the entry off the chain and owned by the caller, 0 to leave
 * it alone. pc_lock must be held.
 */
static int pc_unlink_unreferenced(struct page_cache_entry *victim) {
  u32 h = pc_hash(victim->inode, victim->offset);
  u64 bflags_h = lock_bucket(h);

  if (__atomic_load_n(&victim->refcount, __ATOMIC_ACQUIRE) != 0) {
    unlock_bucket(h, bflags_h);
    return 0;
  }
  struct page_cache_entry **prev = &hash_table[h];
  struct page_cache_entry *hcurr = *prev;
  while (hcurr) {
    if (hcurr == victim) {
      *prev = hcurr->hash_next;
      break;
    }
    prev = &hcurr->hash_next;
    hcurr = hcurr->hash_next;
  }
  unlock_bucket(h, bflags_h);
  return 1;
}

void page_cache_truncate_inode(struct vfs_inode *inode, u64 new_size) {
  /* Nothing of this file is cached, so there is nothing for the walk over
   * every cached page in the machine to find. A browser sizing its shared
   * memory regions hits this hundreds of times per start-up. */
  if (inode && __atomic_load_n(&inode->cached_pages, __ATOMIC_ACQUIRE) == 0)
    return;
  if (!inode)
    return;

  lock_pc();
  /* Both LRU lists — a truncated inode's tail pages may be active. */
  struct page_cache_entry *heads[2] = { lru_head, active_head };
  for (int li = 0; li < 2; li++) {
   struct page_cache_entry *curr = heads[li];
   while (curr) {
    struct page_cache_entry *next = curr->lru_next;
    if (curr->inode == inode && curr->offset + PAGE_SIZE > new_size) {
      void *virt = (void *)(usize)(curr->frame + vmm_direct_map_base());
      if (curr->offset >= new_size) {
        /* Page lies fully beyond the new EOF. Drop it so a later re-grow
         * reads zeros instead of resurrecting pre-truncate contents. If a
         * reader still holds a reference, neutralize in place instead. */
        /* The claim decides it, not a refcount read taken before the bucket
         * lock: a reader can pin the page in between, and dropping it then is
         * the same use-after-free the evictors had. A refusal falls through to
         * neutralising the page in place, which is what a pinned page needs
         * anyway. */
        if (pc_unlink_unreferenced(curr)) {
          lru_remove(curr);
          pmm_free_frame(curr->frame);
          /* Before clearing the owner, not after: the old order read
           * curr->inode when it had just been set to 0, so the inode's page
           * count was never decremented and the cache looked permanently
           * populated to every fast path that checks it. */
          if (curr->inode && curr->inode->cached_pages)
            __atomic_sub_fetch(&curr->inode->cached_pages, 1, __ATOMIC_RELEASE);
          curr->inode = 0;
          curr->hash_next = to_free_list;
          to_free_list = curr;
        } else {
          memset(virt, 0, PAGE_SIZE);
          if (curr->flags & PAGE_CACHE_DIRTY) {
            __atomic_sub_fetch(&g_pc_dirty_pages, 1, __ATOMIC_RELEASE);
            if (curr->inode && curr->inode->dirty_pages)
              __atomic_sub_fetch(&curr->inode->dirty_pages, 1, __ATOMIC_RELEASE);
          }
          curr->flags &= ~PAGE_CACHE_DIRTY;
        }
      } else {
        /* Partial tail page: zero the bytes beyond the new EOF. */
        usize keep = (usize)(new_size - curr->offset);
        memset((u8 *)virt + keep, 0, PAGE_SIZE - keep);
      }
    }
    curr = next;
   }
  }
  unlock_pc();
}

void page_cache_put_page(struct page_cache_entry *page) {
  lock_pc();
  if (__atomic_load_n(&page->refcount, __ATOMIC_ACQUIRE) > 0)
    __atomic_sub_fetch(&page->refcount, 1, __ATOMIC_ACQ_REL);
  if (__atomic_load_n(&page->refcount, __ATOMIC_ACQUIRE) == 0 &&
      (page->flags & PAGE_CACHE_ORPHAN)) {
    /* Inode was destroyed while we held the reference; the entry is already
     * off the hash and LRU — finish its teardown now. */
    pmm_free_frame(page->frame);
    if (page->inode && page->inode->cached_pages)
      __atomic_sub_fetch(&page->inode->cached_pages, 1, __ATOMIC_RELEASE);
    page->hash_next = to_free_list;
    to_free_list = page;
  }
  unlock_pc();
}

/* Evict up to target_pages CLEAN, unreferenced pages from the oldest end of the
 * LRU — never writing anything back. This is the cheap reclaim used in hot paths
 * (the proactive cap in page_cache_add_page, fired on every demand-fault that
 * caches a page): it must NOT trigger synchronous dirty .o writeback over the
 * slow polled AHCI, which otherwise thrashes once a demand-paged clang faults
 * its text against a cache full of dirty build output. Expensive dirty
 * writeback is left to reactive reclaim (page_cache_evict from
 * pmm_alloc_frames), which only runs on a genuine allocation miss. Returns the
 * number of pages actually freed (may be < target if the tail is all dirty or
 * referenced). */
usize page_cache_evict_clean(usize target_pages) {
  if (target_pages == 0)
    target_pages = 1;
  lock_pc();
  usize evicted = 0;
  while (evicted < target_pages) {
    struct page_cache_entry *victim = 0;
    /* Inactive list only — the cold pages.
     *
     * The page is claimed here, inside the walk, rather than after it: the
     * claim can fail (a reader pinned the entry in the window between the scan
     * and the bucket lock) and the walk has to continue from where it was
     * instead of restarting and choosing the same page again. */
    for (struct page_cache_entry *curr = lru_head; curr;) {
      struct page_cache_entry *next = curr->lru_next;

      if (curr->refcount != 0 || (curr->flags & PAGE_CACHE_DIRTY)) {
        curr = next; /* clean-only: never write back here */
        continue;
      }
      if (pc_unlink_unreferenced(curr)) {
        victim = curr;
        break;
      }
      curr = next;
    }
    if (!victim) {
      /* Inactive is exhausted of clean victims: demote a batch of the oldest
       * active (working-set) pages to inactive and try again. This is how the
       * working set is eventually reclaimable under sustained pressure without
       * being the FIRST thing evicted. If nothing can be demoted, give up. */
      if (demote_active(target_pages - evicted) == 0)
        break;
      continue;
    }
    /* Already off the hash chain: pc_unlink_unreferenced did that as part of
     * claiming it. */
    lru_remove(victim);
    pc_refault_record(victim->key_ino, victim->offset);
    pmm_free_frame(victim->frame);
    if (victim->inode && victim->inode->cached_pages)
      __atomic_sub_fetch(&victim->inode->cached_pages, 1, __ATOMIC_RELEASE);
    victim->hash_next = to_free_list;
    to_free_list = victim;
    evicted++;
  }
  unlock_pc();
  return evicted;
}

usize page_cache_evict(usize target_pages) {
  static u64 evict_calls;
  if (target_pages == 0)
    target_pages = 1;
  evict_calls++;
  lock_pc();

  usize evicted = 0;
  usize dirty_skipped = 0;
  usize dirty_written = 0;

  /* Each pass walks the LRU from the oldest end and either evicts one clean,
   * unreferenced page or writes back one dirty page so it becomes evictable.
   * The pre-read-ahead behaviour skipped dirty pages outright, so a workload
   * that dirties most of the cache (e.g. the in-guest self-host writing .o
   * files to the ram0 module, on a machine with no swap device) could reclaim
   * NOTHING under memory pressure and OOM/triple-fault — which is why the
   * self-host needed ~16 GiB. Writing dirty pages back caps the cache at the
   * real working set. writeback_page_locked transiently drops pc_lock, so a
   * cursor saved across it could dangle; restart the walk from lru_head each
   * pass instead. Bounded: every dirty page is flushed at most once (writeback
   * clears DIRTY) and every clean page evicted at most once. */
  while (evicted < target_pages) {
    struct page_cache_entry *victim = 0;
    struct page_cache_entry *flush = 0;
    dirty_skipped = 0;
    for (struct page_cache_entry *curr = lru_head; curr;) {
      struct page_cache_entry *next = curr->lru_next;

      if (curr->refcount != 0) {
        curr = next;
        continue;
      }
      /* Touched since the last sweep: clear the bit and promote it instead of
       * taking it. This is where a hit's single store becomes the working-set
       * decision that the hit path used to make by relinking the entry. */
      if (curr->flags & PAGE_CACHE_REFERENCED) {
        curr->flags &= ~PAGE_CACHE_REFERENCED;
        lru_remove(curr);
        active_append(curr);
        curr = next;
        continue;
      }
      if (curr->flags & PAGE_CACHE_DIRTY) {
        /* An in-memory file has nowhere to write back to: this cache is where
         * its bytes live, and write_cb would copy the page into the inode's
         * heap buffer so the same bytes are held twice -- asked for under
         * memory pressure, which is the worst moment for a second copy. Such a
         * page is unflushable in the same sense as one with no write_cb at
         * all; it is reclaimable only by swapping.
         *
         * Skipped at SELECTION, not inside the writeback. Both loops here take
         * "writeback clears DIRTY" as their termination argument, so a
         * writeback that returns without clearing it hands the same page back
         * on the next pass forever -- an endless reclaim loop holding pc_lock,
         * which is what the first version of this did. */
        if (curr->inode && curr->inode->write_cb &&
            !(curr->inode->flags & VFS_NODE_MEMORY_BACKED)) {
          flush = curr; /* oldest flushable dirty page */
          break;
        }
        dirty_skipped++; /* unflushable — cannot reclaim, leave it in place */
        curr = next;
        continue;
      }
      /* Claimed here, for the same reason evict_clean claims inside its walk:
       * a reader can pin the entry between this scan and the bucket lock, and
       * the walk must then move past it rather than choose it again. */
      if (pc_unlink_unreferenced(curr)) {
        victim = curr; /* oldest clean, untouched page */
        break;
      }
      curr = next;
    }

    if (flush) {
      writeback_page_locked(flush); /* clears DIRTY (drops+retakes pc_lock) */
      dirty_written++;
      continue; /* rescan: the page is now clean and evictable */
    }
    if (!victim) {
      /* Inactive list has nothing reclaimable (all referenced / unflushable):
       * demote a batch of the oldest active working-set pages to inactive and
       * retry, so the working set is reclaimable last rather than never. */
      if (demote_active(target_pages - evicted) == 0)
        break;
      continue;
    }

    /* Evict the clean victim: it is already off the hash chain (claimed in the
     * walk above), so unlink it from the LRU, free its frame, and defer the
     * entry's kfree. */
    lru_remove(victim);
    pc_refault_record(victim->key_ino, victim->offset);
    pmm_free_frame(victim->frame);
    if (victim->inode && victim->inode->cached_pages)
      __atomic_sub_fetch(&victim->inode->cached_pages, 1, __ATOMIC_RELEASE);
    victim->hash_next = to_free_list;
    to_free_list = victim;
    evicted++;
  }

  unlock_pc();
  if (bootinfo_has_flag("b1nix.debug.heap") && (evict_calls <= 16 || is_power_of_two_u64(evict_calls) ||
      dirty_skipped > 0 || evicted < target_pages)) {
    console_write("[M26DIAG] pc_evict call=");
    console_write_dec(evict_calls);
    console_write(" target=");
    console_write_dec(target_pages);
    console_write(" evicted=");
    console_write_dec(evicted);
    console_write(" dirty_skipped=");
    console_write_dec(dirty_skipped);
    console_write(" dirty_written=");
    console_write_dec(dirty_written);
    console_write(" free_frames=");
    console_write_dec(pmm_free_frame_count());
    m26_diag_task();
    console_write("\n");
  }
  return evicted;
}
