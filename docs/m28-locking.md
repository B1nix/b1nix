# M28 — Kernel lock order

This is the load-bearing reference for every concurrent access in b1nix. If
you add a lock, add it here too. If you find a real or potential lock
ordering violation, the existing comment in the offending site is the WHY —
write down the fix here, not just in the diff.

The **Big Kernel Lock (BKL) has been removed** (M28 #7, done — see the "BKL
teardown" section below). There is no longer any global kernel lock: every
kernel entry (syscall, exception, IRQ) runs concurrently across CPUs and all
serialisation is via the per-subsystem locks documented here. This file is now
the *sole* authority on lock order — there is no BKL umbrella papering over a
mistake, so a violation against the DAG below is a real SMP bug.

## The DAG (top-down acquisition order)

```
        ┌─────────────────┬─────────────────┐
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
        │                              heap_lock
        │                                   │
        │                              pmm_lock
        │                                   │
        └────── (independent subtree) ──────┘
        page_cache_lock, dcache_lock, icache_lock,
        aio_*_lock, pipe->lock, net_*_lock, etc.
```

Read top-down: a thread that already holds a lock at level N may acquire a
lock at level N+1, never the other way around. Lock-acquisition graphs
that close into a cycle deadlock under SMP.

## Per-lock rules

### BKL — REMOVED (M28 #7)
The Big Kernel Lock no longer exists. Kernel entry runs concurrently across
CPUs; there is no outermost umbrella lock. The locks below are all load-bearing.

### `g_tasks_lock` (`kernel/sched/scheduler.c`)
- **Type:** `spinlock_t`, irq-save.
- **Protects:** the `tasks[]` slot lifecycle — allocation/free of slots,
  `g_task_hwm`. NOT task fields after the slot is published.
- **Order:** below BKL, above the per-CPU runqueue lock and any subsystem
  the allocated task immediately touches.
- **Why NOT rwlock:** the walker contract (skip TASK_UNUSED + read `state`
  word-atomically + leave allocated slots alone) is already lock-free
  correct. Making it an rwlock would require taking the read lock in
  every walker, which is decorative overhead with no race actually
  closed. The real SMP gap on tasks[] is **per-task field tearing**
  — see below.

### Per-task fields beyond `state` (`struct task`) — M28 #3 CLOSED BY ANALYSIS
Audited every cross-task field read/write after BKL removal. There is **no
memory-corrupting race left**; a per-task `state_lock` would be decorative
(forbidden), so none was added. Classification:

- **The one real bug — `pending_signals` — is FIXED.** It was a multi-bit RMW
  (`|=`/`&=~`) raced across CPUs (killer vs. the task consuming its own
  signals): a genuine lost-update that dropped whole signals. Now atomic
  fetch_or/fetch_and/load everywhere (see the "Pre-BKL-teardown gates" section).
- **Reaper fields** (`exit_code`, `user_image`, `pml4_phys`, `vma_list`,
  `fd_table`/`fd_flags`/`fd_capacity`, `name`, `stack`): read/freed by
  `scheduler_waitpid` ONLY after winning the atomic `DEAD→REAPING` CAS *and*
  spinning on `stack_released==1`. That makes the reaper the single exclusive
  accessor of a fully-stopped task → safe, no lock needed.
- **`parent_id`, `cwd[64]`, `env[][]`, `sigactions[]`**: written once at
  fork/creation (parent is `current_task`, IRQs off during the copy) and
  thereafter read only by the task itself. No concurrent cross-task writer.
- **Single aligned words** (`state`, `priority`, `process_group_id`,
  `session_id`): x86 TSO gives atomic aligned load/store — no tearing. No RMW
  race on them (unlike `pending_signals`). The job-control trio in
  `scheduler_kill` writes `last_stop_signal → stop_report_pending → state`
  with `state` LAST; `scheduler_waitpid` reads `state==STOPPED` FIRST, so TSO
  store-order guarantees the reader that sees STOPPED also sees the trio.
- **Known benign LOGICAL race (not memory-unsafe):** `stop_report_pending` /
  `continued_report_pending` set by a killer on one CPU while `waitpid` clears
  them on another can lose/duplicate ONE job-control report under concurrent
  SIGSTOP/SIGCONT + waitpid on the same child from different cores. Single-word
  (no corruption); atomics would NOT fix it (it is a set-vs-clear ordering
  race, not a torn read) — only a waitpid/kill-path lock or CAS protocol would,
  which is not worth it for a cosmetic, pathological-timing reporting glitch.
  Documented, deliberately left.

### Per-CPU runqueue lock (`kernel/sched/runqueue.c`, `rq->lock`)
- **Type:** `spinlock_t`, **plain** (no irqsave) — every caller is already
  running with interrupts off (BKL holders + idle loops).
- **Protects:** the per-CPU ready ring buffer (`head`, `tail`, `nr`).
- **Order:** terminal leaf — innermost lock in the DAG (LOCKDEP=1000). Has
  to be deeper than every other lock because sleeping-lock release paths
  (`vfs_inode_unlock_*` → `scheduler_wake_all` → `rq_enqueue`, plus every
  yield-on-contention atomic-test-and-set lock) all eventually take it.
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

### `heap_lock` (`kernel/mm/kheap.c`)
- **Type:** `spinlock_t`, irq-save.
- **Protects:** the general-heap bump region, segregated free lists, the
  `last_block` invariant used by M26 coalescing.
- **Order:** above `pmm_lock`. `kmalloc` holds `heap_lock` and when the
  heap needs to grow it calls `pmm_alloc_frame` which takes `pmm_lock` —
  so HEAP is the OUTER lock and PMM is the inner.
- **Special:** `klarge_alloc` (M26) reserves vaddr span under `heap_lock`,
  then maps frames with the lock released so `swap_evict_page` can run
  with interrupts enabled.

### `pmm_lock` (`kernel/mm/pmm.c`)
- **Type:** `spinlock_t`, irq-save.
- **Protects:** physical-frame free list + accounting.
- **Order:** terminal — innermost MM lock. Acquired under `heap_lock`
  during heap_grow; never the reverse.
- **Will become:** atomic accounting counters in M28 #4 follow-up —
  `pmm_total_usable_memory()` / `pmm_total_free_frames()` are simple
  counter reads that don't need locking; the free-list head still does.

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

1. **There is no BKL.** It has been removed entirely (M28 #7). Every kernel
   entry runs concurrently across CPUs; the locks below are the only
   serialisation and each rule here is load-bearing, not decorative.
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

- Acquiring an inode rw_lock while holding `bcache_lock`. Panics by design.
- Acquiring `vfs_tree_lock` (write) on an outer scope and then calling
  `vfs_inode_lock_write` (which may yield). The tree lock is IRQs-off; a
  yield with IRQs off is an undefined-behaviour SMP deadlock. Always:
  inode lock first, tree lock briefly over the mutation, tree lock out.
- Using `scheduler_block_on` while holding a plain spin lock. The block_on
  path itself disables interrupts and yields — interactions with held
  spin locks are race-prone. The only legal place to call block_on is
  from above a yieldable lock or with no lock at all.

## TLB-shootdown invariant (added after the smp>=2 build-panic fix)

The cross-CPU TLB-shootdown protocol (`kernel/arch/x86/tlb.c`) assumes every
other CPU is *either* IRQs-on (so it takes the shootdown IPI) *or* spinning in
a loop that calls `tlb_shootdown_poll()`. There is a third, easy-to-miss case:
a CPU running a **long IRQs-off section that does WORK rather than spin-wait**.
It will not take the IPI (IRQs masked) and will not poll (no spin loop), so the
initiator — which waits on `g_tlb_pending` with IRQs off too — stalls and trips
the runaway-guard panic `tlb: shootdown stalled, pending=1 op=1`.

This is exactly what bit the recursive page-table walkers
(`clone_table`/`free_table`/`swap_in_recursive` in `paging.c`): fork holds
`vmm_write_lock` (IRQs-off) across the whole clone, and the exit reaper frees an
address space under `interrupts_disable()`. They now `tlb_shootdown_poll()` once
per visited table. **Rule going forward:** any new IRQs-off section that loops
over a non-trivial amount of data must poll for shootdowns, not just the
spin-wait loops in spinlock.h/rwlock.h/bkl.c/page_cache.c. Do NOT paper over a
recurrence by raising the `1ULL << 28` spin guard in tlb.c.

## Pre-BKL-teardown gates (M28 #3)

Two SMP correctness gates were closed in this branch so the remaining BKL
teardown step (IRQ/exception handler) does not expose them:

- **`task->pending_signals` lost updates — CLOSED.** The u64 signal bitmask
  was updated with plain `|=` / `&=~` RMWs from a killer on one CPU and the
  target on another; two RMWs to the same word lose a bit (a dropped signal).
  All raises are now `__atomic_fetch_or` (RELEASE), all consumes
  `__atomic_fetch_and` (RELAXED), all set-reads ACQUIRE loads, across
  scheduler.c / arch/x86/signal.c / syscall.c. `blocked_signals` is task-local
  and left plain. Single-word task fields (state, process_group_id, session_id,
  job-control trio) do NOT physically tear on x86 TSO and are left as-is —
  atomics there would be decorative.
- **`vfs` sibling-list walks under bare cli/sti — CLOSED.** `vfs_list` and the
  fallback `readdir` walked first_child/next_sibling under cli/sti, which only
  blocks same-CPU preemption; a concurrent unlink on another core frees a node
  mid-walk. Both now take `vfs_tree_read_acquire`/`release` — the same rwlock
  unlink holds for write over its splice, matching `find_child`.

## Residual SMP-4 stack-lifetime race — CLOSED

The shape-#2 stack-corruption fault (invalid-opcode at a kernel-heap RIP under
SMP-4, reproducible by hammering SYS_GETPID+SYS_YIELD) that
`docs/m28-t4-blocker.md` left open is now fixed. Root cause: the per-task
`stack_released` lease was claimed (set 0 before publishing the new state) on
the RUNNING→READY yield arm and the exit/DEAD arm, but NOT on the voluntary
block/sleep arms (`scheduler_block_current`, `scheduler_block_on`,
`scheduler_sleep_ticks` set BLOCKED/SLEEPING then yield). A waker on another
CPU (`scheduler_wake_all` / `wake_sleepers` CAS to READY + enqueue) could then
observe a stale `stack_released==1` and resume the task on its kernel stack
while the owning CPU was still mid-`arch_context_switch` save. Fix: clear the
lease in all three block/sleep paths before the state store, and skip the lease
wait in `pick_next_task` when the chosen task is `current_task` (a cross-CPU
wake can enqueue us before we reach pick; waiting on our own
not-yet-published lease would self-deadlock). Verified 8/8 clean SMP-4 boots
(baseline was ~60%) plus 271/0 smoke. NOT the hypothesised exec race — fork's
copy was already IRQ-off-safe; it was a general block/wake lease asymmetry.

## BKL teardown (M28 #7) — kernel entry is now BKL-free

All three kernel-entry paths run WITHOUT the BKL:
- **syscall entry** — T4, landed earlier (`syscall_entry.S`).
- **exception handler** (`x86_exception_handler`) — done here. Prerequisite was
  making `vmm_handle_page_fault` self-locking: it took NO locks while mutating
  page tables / PMM / page cache / swap, so the BKL was the only thing
  serialising concurrent faults. `vmm_lock` is IRQs-off + non-recursive and the
  file/swap paths block, so a plain wrap self-deadlocks; instead the handler now
  uses prepare-then-commit — allocate + blocking I/O outside the lock, then take
  `vmm_lock`, re-read the live leaf PTE via the non-allocating `pf_leaf_pte_ptr`,
  and install only if the precondition still holds (else discard the spare
  frame). `vmm_map_page_locked` is the lock-held core split out so the commit
  doesn't re-enter the non-recursive lock.
- **device IRQ handler** (`x86_irq_handler`, vectors 32/33/44/net) — done here.
  PS/2 keyboard ring made a release/acquire SPSC; mouse event flag made an
  atomic exchange (packet assembly is single-producer; mouse_state is a cosmetic
  cursor coord, left unlocked); net already had its own locks. Hot vectors
  (64/65/66/255) already bypassed BKL.

**The BKL is now fully deleted** (`kernel/sched/bkl.c`, `include/b1nix/bkl.h`,
the `task.bkl_depth` field, and the Makefile entry are all gone). Once kernel
entry was BKL-free, `scheduler_yield` no longer touched the lock and nothing
read its state except the idle loops themselves, so it was vestigial: the idle
loops in `main.c`/`lapic.c` now just `scheduler_yield()` + park, the
scheduler-init seed is removed, and the `bkl_unlock`-on-exit in
`syscall_entry.S`/`user_jump.S` is gone. There is no global kernel lock anymore;
all SMP serialisation is via the per-subsystem locks in the DAG above. Verified:
smoke 271/0 (UP + SMP-4) after each step, plus standalone SMP-4 stress boots per
step (10/10 per entry cut, 15/15 after the full deletion).

## Known open issues (not yet fixed)

- **Job-control report flags** (`stop_report_pending` / `continued_report_pending`):
  benign logical set-vs-clear race between a killer and a concurrent `waitpid`
  on the same child from two cores — at worst one missed/duplicated
  WUNTRACED/WCONTINUED report, no memory corruption. See the "Per-task fields"
  section for why atomics don't fix it and a lock isn't worth it.
- **mouse_state torn read** (`ps2_mouse.c`): cursor x/y/buttons read by the
  worker/`ps2_mouse_get_state` while the IRQ updates them — cosmetic only (a
  stale cursor pixel), no memory-safety impact. Left unlocked deliberately.
- **In-guest `make -j4` repro not re-run end-to-end on this checkout**: the
  native toolchain (`tools/build-native-toolchain.sh`) is not built on the
  fresh Fedora tree, so `tools/inguest/run-build.py` falls back to the
  smoke-test ISO instead of a real build. The shootdown fix is verified by the
  smoke suite and a clean smp=4 KVM boot with shootdowns active, but the
  original fork/exec/exit build load itself still needs a confirming run once
  the native toolchain is rebuilt.

## Roadmap

- M28 #2 — Lockdep-light: **DONE.** Per-CPU acquisition stack, debug-only,
  panics on inversion / out-of-order release against the DAG above. Enable with
  `make LOCKDEP=1` (off by default — macros are no-ops in production). Verified
  clean over the full suite at -smp 4. The retired BKL level (100) is gone; the
  per-inode sleeping rwlock is the sole user of the bequeath (GLOBAL) path.
- M28 #3 — per-task field tearing: **CLOSED by analysis** (no
  memory-corrupting race remains; see "Per-task fields beyond state").
  `g_tasks_lock` stays a spinlock by design — making it an rwlock would be
  decorative.
- M28 #4 — heap/pmm granularity: **largely done.** PMM already has a per-CPU
  cache; the heap now has a per-CPU small-allocation magazine + an O(1)
  track_free fast path (see git log: per-CPU kmalloc magazine). Measured heap
  cross-core contention dropped 2.58x→1.78x; the dominant per-op cost was a
  fixed O(1024) track_free scan, now gated by size. Further sharding has
  diminishing returns until a benchmark shows a new hot lock.
- M28 #7 — BKL teardown: **COMPLETE.** idle loop ✓, syscall entry ✓ (T4),
  exception handler ✓, device IRQ handler ✓, and the lock object itself
  deleted ✓ (no global kernel lock exists anymore — see "BKL teardown").
