# M28 — Kernel lock order

This is the load-bearing reference for every concurrent access in b1nix. If
you add a lock, add it here too. If you find a real or potential lock
ordering violation, the existing comment in the offending site is the WHY —
write down the fix here, not just in the diff.

Today the **Big Kernel Lock (BKL)** still serialises the entire kernel for
userspace execution on APs (M28 item 2 pending). Everything below is the
shape the kernel will have **after** the BKL is removed; today most of the
inner locks are decorative. Documenting the order now is what lets the BKL
teardown happen one subsystem at a time.

## The DAG (top-down acquisition order)

```
                    BKL (recursive, per-CPU owner)
                          │
        ┌─────────────────┼─────────────────┐
        │                 │                 │
   per-task fd_lock   g_tasks_lock     vfs_mount_lock
        │                 │                 │
        │                 │                 │
        │            per-CPU rq.lock        │
        │                                   │
        │                              vfs_tree_lock (rwlock, M28-B)
        │                                   │
        │                              per-inode rw_lock (sleeping)
        │                                   │
        │                              bcache_lock
        │                                   │
        │                              pmm_lock
        │                                   │
        │                              heap_lock
        │                                   │
        └────── (independent subtree) ──────┘
        page_cache_lock, dcache_lock, icache_lock,
        aio_*_lock, pipe->lock, net_*_lock, etc.
```

Read top-down: a thread that already holds a lock at level N may acquire a
lock at level N+1, never the other way around. Lock-acquisition graphs
that close into a cycle deadlock under SMP.

## Per-lock rules

### BKL (`kernel/sched/bkl.c`, `g_bkl`)
- **Type:** recursive ticket-spin with per-CPU owner.
- **Scope:** every entry into the kernel from ring 3 (`syscall_entry.S`),
  every interrupt (`x86_irq_handler`), every AP cooperative loop.
- **Order:** **outermost**. Take *before* anything else and release *last*.
- **May yield while held:** YES — that is the whole point of cooperative
  scheduling under M24b BKL. The lock is handed off across a context
  switch at depth 1; recursion depth must be exactly 1 at yield time.
- **Going away in:** M28 item 2.

### `g_tasks_lock` (`kernel/sched/scheduler.c`)
- **Type:** `spinlock_t`, irq-save.
- **Protects:** the `tasks[]` slot lifecycle — allocation/free of slots,
  `g_task_hwm`. NOT task fields after the slot is published.
- **Order:** below BKL, above the per-CPU runqueue lock and any subsystem
  the allocated task immediately touches.
- **Will become:** `rwlock_t` in M28 #3 — readers (PID lookup, process-list
  walks) parallelise; allocator/exit remains a writer.

### Per-CPU runqueue lock (`kernel/sched/runqueue.c`, `rq->lock`)
- **Type:** `spinlock_t`, **plain** (no irqsave) — every caller is already
  running with interrupts off (BKL holders + idle loops).
- **Protects:** the per-CPU ready ring buffer (`head`, `tail`, `nr`).
- **Order:** below `g_tasks_lock`, above nothing else (terminal).
- **Does not yield.** Held for O(1) operations only.

### Per-task `fd_lock` (`struct task::fd_lock`)
- **Type:** `spinlock_t`, plain.
- **Protects:** `fd_table` / `fd_flags` / `fd_capacity`.
- **Order:** sibling of `g_tasks_lock` — never taken together. Held only
  by the task that owns the fd table (or a syscall acting on it).

### `vfs_tree_lock` (`kernel/fs/vfs.c`, M28-B)
- **Type:** `rwlock_t`, irq-save.
- **Protects:** every `vfs_node` parent/sibling chain walk + mutation. See
  `kernel/include/b1nix/rwlock.h` for semantics.
- **Order:** below per-inode rw_lock when both are held (writers take the
  inode lock first, then the tree lock briefly over the actual mutation).
- **Does NOT yield while held.** Critical sections are 3-line splices.

### Per-inode rw_lock (`struct vfs_inode::rw_lock`, in `vfs.c`)
- **Type:** custom sleeping reader-writer counter on `scheduler_block_on`.
- **Protects:** inode metadata fields (mode, size, times) AND the parent's
  sibling-list mutation (writers serialise across the parent's children).
- **Order:** above `vfs_tree_lock`; **below** `bcache_lock` — see
  `vfs_inode_lock_*` panic guard. The panic was paid for in the M14
  storage milestone; the ordering is sticky.
- **May yield while held** (it sleeps on its own channel).

### `bcache_lock` (`kernel/dev/blk.c`)
- **Type:** `spinlock_t`.
- **Protects:** the block cache hash + LRU.
- **Order:** the panic guard in `vfs_inode_lock_*` enforces **bcache →
  inode** — taking the inode lock while holding bcache is a kernel panic.
  Reverse is fine: holders of the inode lock that need a block may take
  bcache and release it before touching inode fields again.
- **Does not yield.**

### `pmm_lock` (`kernel/mm/pmm.c`)
- **Type:** `spinlock_t`, irq-save.
- **Protects:** physical-frame free list + accounting.
- **Order:** terminal. May be taken under any other lock that does not yield.
- **Will become:** `rwlock_t` in M28 #4 — `pmm_total_usable_memory()` /
  `pmm_total_free_frames()` and similar getters become readers.

### `heap_lock` (`kernel/mm/kheap.c`)
- **Type:** `spinlock_t`, irq-save.
- **Protects:** the general-heap bump region, segregated free lists, the
  `last_block` invariant used by M26 coalescing.
- **Order:** terminal. Calls to `kmalloc` / `kfree` under any other lock
  are fine as long as that lock does not yield (heap_lock never yields).
- **Special:** `klarge_alloc` (M26) reserves vaddr span under `heap_lock`,
  then maps frames with the lock released so `swap_evict_page` can run
  with interrupts enabled.

### `vfs_mount_lock` / `dcache_lock` / `icache_lock` (`kernel/fs/vfs.c`)
- **Type:** atomic test-and-set, yields to scheduler on contention.
- **Order:** above the per-inode rw_lock — these are taken first when
  walking the path; the inode lock is taken after the lookup finishes.
- **May yield while held** (every contention iteration yields).
- **Note:** find_child intentionally bypasses dcache because
  `dcache_acquire` yields on contention, which reintroduces the same race
  the cli/sti window closed. M28-B's `vfs_tree_lock` is the proper fix.

### Network and IPC locks
- `net_tx_lock`, `net_rx_lock` (`kernel/net/net.c`)
- `tcp_queue_lock` (`kernel/net/tcp.c`)
- `u->lock` (`kernel/net/unix.c`)
- `pipe->lock` (`kernel/fs/pipe.c`)
- `aio_ctx_list_lock`, `aio_pending_lock`, `ctx->lock` (`kernel/fs/aio.c`)

All are atomic test-and-set with `scheduler_yield()` on contention. Each
sits in its own subtree — they are not held together with each other or
with the VFS / MM / sched chain. Order rule: take **at most one** per
syscall path. If you need a second one across subsystems, document the
order here first.

### `page_cache_lock` (`kernel/mm/page_cache.c`, `pc_lock`)
- **Type:** GCC `__sync_lock_test_and_set` spin, no yield.
- **Order:** terminal, never held alongside the heap or pmm locks.
- **Does not yield.**

## Inter-subsystem rules

1. **BKL is outermost.** Always.
2. **bcache → inode.** Enforced by panic guard. Never reverse.
3. **inode → vfs_tree.** Writers take per-inode write first, then the tree
   lock over the splice.
4. **mount/dcache/icache → inode.** Path walkers take these first, release,
   then take the inode lock.
5. **Heap and pmm are terminal.** May be acquired under any non-yielding
   lock; never acquire them under a sleeping lock unless you can prove the
   sleeping lock owner already holds them transitively.
6. **Page cache is terminal.** Same as 5.
7. **Network/IPC locks are siblings of the VFS chain, not below it.**
   A pipe write must not hold pipe->lock while taking inode rw_lock; the
   write completes the IPC operation, then resolves the VFS side.

## Anti-patterns the audit should catch

- Acquiring `heap_lock` (or any irqsave lock) inside an ISR handler that
  itself ran from an IRQ entry, then yielding. The irqsave restores the
  pre-IRQ interrupt state, but the BKL hand-off path expects depth
  bookkeeping; yielding with the wrong depth corrupts the BKL.
- Acquiring an inode rw_lock while holding `bcache_lock`. Panics by design.
- Acquiring `vfs_tree_lock` (write) on an outer scope and then calling
  `vfs_inode_lock_write` (which may yield). The tree lock is IRQs-off; a
  yield with IRQs off is an undefined-behaviour SMP deadlock. Always:
  inode lock first, tree lock briefly over the mutation, tree lock out.
- Using `scheduler_block_on` while holding a plain spin lock. The block_on
  path itself disables interrupts and yields — interactions with held
  spin locks are race-prone. The only legal place to call block_on is
  from above a yieldable lock or with no lock at all.

## Roadmap

- M28 #2 — Lockdep-light: per-CPU acquisition stack, debug-only, panics on
  inversion against the DAG above.
- M28 #3 — `g_tasks_lock` → `rwlock_t`.
- M28 #4 — `pmm_lock` / `heap_lock` (and `vmm` page-table walks) → finer
  granularity where it pays off. PMM as a writer-only path with readers
  for accounting; heap as a per-arena lock; vmm as PML4-level rwlock.
- M28 #7 — BKL teardown, in pieces: first remove from the BSP idle loop,
  then from syscall entry, then from IRQ handler. The per-subsystem locks
  above MUST be load-bearing before each step.
