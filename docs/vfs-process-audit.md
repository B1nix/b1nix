# M46 — VFS Integrity & POSIX Process Conformance Audit

Date: 2026-06-11. Scope: a corruption-focused audit of the VFS layer
(`kernel/fs/`, `kernel/mm/page_cache.c`, fd-table code in
`kernel/sched/scheduler.c`) and a conformance audit of the POSIX
process-management surface (`kernel/sched/scheduler.c`,
`kernel/syscall/syscall.c`, `kernel/user/process.c`, `userspace/libc/`).

Status legend: **fixed** (in this milestone), **planned** (tracked in the M46
roadmap entry, not yet implemented), **wontfix** (intentional deviation,
documented).

---

## Part 1 — VFS: data-corruption / UAF findings

### V1. ext4/ext2 block & inode allocators had zero synchronization — CRITICAL — fixed

`ext4_alloc_block_tx` / `ext4_alloc_inode_tx` / `ext4_free_*_tx`
(`kernel/fs/ext4.c`) and the `ext2_alloc_*` / `ext2_free_*` family
(`kernel/fs/ext2.c`) performed unsynchronized read-modify-write cycles on the
on-disk bitmaps and superblock counters. Two CPUs allocating concurrently
(parallel writes/creates on different inodes — the per-inode rwlock does not
serialize them) could both observe the same bitmap bit clear and hand the
same physical block (or inode number) to two different files: silent
cross-file data corruption plus torn `s_free_*_count` accounting. This is the
most likely identity of the long-open "ext4 VFS-lookup race under
`-smp 4 -j4`".

**Fix:** per-fs sleeping mutex `alloc_lock` (`vfs_meta_lock_acquire/release`
in `vfs.c`, same publish-BLOCKED-then-retry pattern as the inode rwlock; it
must not be a spinlock because the holder sleeps on block I/O) held across
the bitmap+superblock RMW in all eight allocator entry points.

### V2. O_APPEND offset sampled before the inode lock — HIGH — fixed

`node_write` (`kernel/fs/vfs.c`) computed `h->offset = inode->size` *before*
acquiring the exclusive inode lock. Writer A samples EOF, blocks on the lock
while writer B appends, then A writes at the stale offset — overwriting B's
data instead of appending (lost writes in shared log files / `>>`).
**Fix:** the O_APPEND size sample moved under `vfs_inode_lock`.

### V3. Truncate never invalidated the page cache — HIGH — fixed

Neither `vfs_ftruncate` nor the `O_TRUNC` path dropped cached pages. After a
shrink-then-grow, reads beyond the shrink point returned the **pre-truncate
contents** of the stale cached pages instead of POSIX-required zeros (data
corruption / information disclosure); dirty pages beyond the new EOF lingered
forever. **Fix:** new `page_cache_truncate_inode(inode, new_size)`
(`kernel/mm/page_cache.c`) drops whole pages at/after the new EOF and zeroes
the tail of the partial page; called from `vfs_ftruncate` (before the fs
callback, bounded by `min(old,new)` so a grow also scrubs the stale tail) and
from the `O_TRUNC` open path. `O_TRUNC` now also calls `truncate_cb` when
present so the filesystem actually frees the data blocks (it previously only
zeroed the size field, leaking the extents).

### V4. Thread-group leader exit freed the shared fd table under live threads — HIGH — fixed

`scheduler_exit_current` unconditionally `vfs_close()`d every fd and
`kfree`d `fd_table`/`fd_flags`, even when CLONE_FILES siblings still used the
shared table — every surviving thread's next fd op dereferenced freed memory.
**Fix:** new `fdtable_release(task)` mirrors `mm_release_user`: under
`g_mm_release_lock` the releaser clears its own pointer and counts the
remaining holders; exactly one (the last) gets the table back and closes +
frees it. All three teardown sites converted (leader exit, the thread reaper
`scheduler_reap_dead_threads` — which previously never freed a shared table
at all, leaking it when the leader had detached — and the waitpid reaper).
Closing a handle without an fd table goes through the new
`vfs_close_handle(h, pid)` (`kernel/fs/vfs.c`).

Known limitation: POSIX file locks owned by the leader's pid are not released
when the leader exits while threads survive; they are only swept when the
inode's last handle is closed by the final table owner. Real `exit_group`
semantics (leader exit terminates all threads) remain **planned**.

### V5. `fdtable_other_refs` unlocked scan — MEDIUM — fixed

The old free/no-free decision walked all task fd-table pointers with no lock
(same class as the pre-436a992 mm double-free). Superseded by the
`g_mm_release_lock`-serialized `fdtable_release` above; the unlocked scan is
deleted.

### V6. `vfs_close` peek-then-clear race — MEDIUM — fixed

`vfs_close` fetched the handle (`scheduler_fd_get`), ran the close path, and
only then cleared the slot — two threads closing the same fd of a shared
table both retrieved the same handle and both ran release (double release /
touch-after-free of the recycled handle, double page-cache flush).
**Fix:** new `scheduler_fd_take(fd)` atomically fetch-and-clears the slot
under the fd lock; only the winner proceeds.

### V6b. CLONE_FILES siblings used private fd_lock copies — MEDIUM — fixed (found while fixing V4/V6)

Each CLONE_FILES thread got `child->fd_lock = 0` — a *private* spinlock copy
guarding the *shared* table, i.e. no mutual exclusion at all between
siblings (two threads could claim the same free slot in `fd_alloc`).
**Fix:** `g_task_fdlock_owner[]` side-table (slot-indexed, M29 pattern)
points every thread at the group leader's lock; `fd_lock_acquire/release`
resolve through it.

### V6c. fd-table growth dangled sibling pointers — HIGH — fixed (found while fixing V4/V6)

`scheduler_fd_alloc` / `scheduler_fd_set` grew the table by allocating a new
array and `kfree`ing the old one, updating **only** `current_task` — every
CLONE_FILES sibling kept pointing at the freed allocation (UAF on their next
fd op once a multithreaded process exceeded its initial fd capacity).
**Fix:** `fdtable_publish_grown` swaps the pointer for every task aliasing
the old table; the swap runs under `g_mm_release_lock` so an exiting
sibling's `fdtable_release` snapshot cannot race the `kfree`.

### V7. Directory rename: stale `..` and broken link counts — MEDIUM — fixed

Two distinct bugs in `ext4_vfs_rename`:
1. **Every** rename leaked a link: `ext4_vfs_unlink_tx` decrements the moved
   inode's `i_links_count` for the removed entry, but `ext4_add_dir_entry_tx`
   does not increment for the added one — a renamed regular file ended up
   with `i_links_count == 0` on disk (fsck-fatal orphan; a subsequent
   unlink+close frees blocks while a hard link may still reference them).
   Fixed by restoring the count after the unlink.
2. Cross-parent directory moves never rewrote the moved directory's `..`
   entry nor moved the back-link between the parents' `i_links_count`s.
   Fixed for both ext4 and ext2 (`ext2_vfs_rename` had bug 2 only — its
   `ext2_remove_dir_entry` is a pure entry removal).

### V8. `vfs_mount` claimed slots with no lock — MEDIUM — fixed

The free-slot scan and pre-registration ran without `vfs_mount_lock` (which
`vfs_umount` does take): two concurrent mounts could claim one slot, and
lookups could observe half-initialized entries. **Fix:** slot claim + field
initialization and the post-mount finalization (including the `next_fs_id++`
for V11) now run under `vfs_mount_lock`; the sleeping `fs->mount()` callback
stays outside it.

### V9. `vfs_link` error path refcount underflow — LOW — fixed

On `link_cb` failure the fresh node (refcount 0) was `vfs_node_put`, going to
-1 and never matching the `refcount==0 && deleted` free condition — a slab
leak per failed link. Fixed by marking the node deleted and handing it the
reference being dropped.

### V10. `vfs_umount` busy-check plain read — LOW — fixed

`root_node->refcount` (updated with atomics everywhere else) was read plainly;
now an acquire load.

### V11. `next_fs_id++` non-atomic — LOW — fixed

Moved under `vfs_mount_lock` (with V8). Duplicate fs_ids would have aliased
inodes across filesystems in the icache.

### Audited and sound (for the record)

- Lock-order invariant (block cache → inode): enforced by panic checks;
  no violating path found.
- `vfs_node`/`vfs_inode`/`vfs_handle` refcounts: atomic, balanced on
  lookup/open/error paths (modulo V9).
- Symlink-loop depth limit, `..` clamping at root, iterative resolution.
- `pipe.c` slot claim and lost-wakeup-safe blocking.
- VFS tree mutations under `vfs_tree_write_acquire`; readers under read lock.
- Orphan-inode deferred delete (unlinked-but-open file reclaimed at last
  close).

---

## Part 2 — POSIX process management: gaps and bugs

### P1. Exit status 128–158 misreported as signal death — HIGH — fixed

The kernel stored one `int exit_code` for both normal exits and signal
deaths (`128+sig`), and waitpid disambiguated purely by numeric range, so
`_exit(139)` (e.g. libc `assert()`) was reported as `WIFSIGNALED`/`SIGSEGV`.
**Fix:** signal deaths are now tagged with `TASK_EXIT_SIGNALED` (bit 16 of
`exit_code` — no `struct task` growth) at all four kill sites (both arch
exception handlers, SIGKILL, default-action terminate) and waitpid encodes
from the flag, not the range.

### P2. `kill(0, sig)` / `kill(-1, sig)` wrong targets — HIGH — fixed

`kill(0)` ("caller's process group") fell through to a pid-0 task lookup →
always EPERM; `kill(-1)` ("all permitted processes") was routed to process
group 1. **Fix:** SYS_KILL dispatch special-cases 0 (caller's pgrp) and -1
(broadcast to all userspace tasks except pid 1, kernel tasks, and the
caller).

### P3. `waitpid(pid < -1)` / `waitpid(0)` group waits — MED-HIGH — fixed

Negative pids below -1 (wait for pgroup |pid|) matched nothing → ECHILD;
pid 0 was treated as "any child" instead of "any child in my process group".
**Fix:** proper pgid matching in the waitpid scan. (Kernel-internal callers
that meant "any child" were audited to pass -1.)

### P4. Bare `-1` returns → wrong errno (EPERM) — MEDIUM — fixed

`scheduler_kill` (nonexistent pid), `scheduler_setpgrp` (not found),
`scheduler_set/get_priority` returned `-1`, which libc maps to EPERM; POSIX
wants ESRCH (and EINVAL for bad nice values). Explicit `-ESRCH`/`-EINVAL`
returned now.

### P5. `setpgid` missing POSIX rules — MEDIUM — fixed

Only the cross-session EPERM check existed. Added: ESRCH for unknown pid,
"self or child only" EPERM, EPERM if the target is a session leader, EPERM
when the destination pgrp does not exist in the caller's session
(unless pgid==pid), EACCES when the target child has already exec'd
(tracked in the `g_task_execed[]` side-table), EINVAL for negative pgid.

### P6. fork cleared the child's signal mask — LOW — fixed

POSIX: the child inherits the parent's blocked-signal mask (pending set
empty). `scheduler_fork_current` zeroed both; it now copies
`blocked_signals`.

### P7. Missing surface — fixed in M46

- `getpgid(pid)` — new `SYS_GETPGID` + libc wrapper.
- `nice()`, `getpriority()`, `setpriority()` — libc wrappers +
  `<sys/resource.h>` prototypes for the already-existing kernel syscalls.
  Wiring them up exposed a latent kernel bug (these syscalls had never been
  called): `scheduler_set_priority` wrote `10 - nice` into `task->priority`,
  but `pick_next_task` is a strict highest-priority scan with a `-1` floor
  and a default priority of 1 — any task with nice > 9 became permanently
  unschedulable once preempted (its negative priority never beats the floor),
  and any nice < 9 would starve every default task. The nice value now lives
  in a `g_task_nice[]` side-table (POSIX round-trip, inherited across fork);
  actually biasing the cooperative scheduler with it is **planned**.
- `setreuid()` / `setregid()` — new syscalls + wrappers on top of the
  existing saved-set machinery in `uidgid.c`.
- `#!` interpreter scripts in the kernel exec path (`user_execve_current`):
  previously only worked because userspace retried via `/bin/sh`; now a
  direct `execve("/script")` works (one interpreter level, optional single
  argument, `ENOEXEC` fallback preserved).

### P8. Follow-up process semantics — fixed

- `exit_group` terminates the full thread group.
- Sessions track their controlling terminal; `setsid()` detaches it and
  session-leader exit hangs up the foreground process group.
- Newly orphaned stopped process groups receive SIGHUP followed by SIGCONT.
- Timer ticks feed per-task user/system CPU accounting for `times()` and
  `getrusage()`.
- `setresuid`, `setresgid`, and `waitid` are implemented.
- Nice values bias otherwise-equal runnable tasks with stride accounting while
  preserving the scheduler's existing strict-priority ordering.

Remaining intentional limitations:

- **waitpid not interruptible by SIGCHLD** — *intentional* (wontfix for now):
  a caught SIGCHLD does not EINTR a parent blocked in waitpid; interrupting
  it broke dropbear/wget child reaping. Conformance gap documented in
  `wait_interrupted_by_signal`.
- **vfork = fork**: legal per POSIX (no address-space sharing guarantee).

### Audited and sound (for the record)

- Syscall number tables kernel ↔ userspace: identical.
- FD_CLOEXEC honored on exec; signal dispositions reset to SIG_DFL (SIG_IGN
  preserved); SUID/SGID applied; pending signals and blocked mask correctly
  carried across exec.
- fork: cwd/umask/pgrp/session/env inheritance; only the calling thread is
  duplicated in a multithreaded process.
- Zombie reaping, orphan reparenting, WUNTRACED/WCONTINUED reporting,
  SIGCHLD posted on exit *and* stop/continue (with SA_NOCLDSTOP honored).
- setuid/seteuid/setgid/setegid saved-set rules; getgroups/setgroups;
  kill permission probe (signal 0); setsid leader-EPERM rule.

---

# Part 3 — Second-round audit (subsystems beyond VFS/process core)

A follow-up sweep of the subsystems flagged as "likely to hide bugs" in the
codebase-quality review: file locks, xattr, the inode/page caches, swap,
loop devices, the block cache, the journal, IPC (mqueue/shm/futex/aio), and
UNIX sockets — plus an audit of the M46 process-semantics commit (9dce201)
itself. Findings below were produced by fan-out audit agents; the ones marked
**[verified]** were independently re-checked by an adversarial verifier that
traced the exact failing interleaving in the current tree. **None of these are
fixed yet** — this section is the to-do list (see roadmap M46 "open hardening").

## Confirmed — memory safety / corruption (verified)

- **FL-1 — CRITICAL — POSIX file locks have no lock at all.** **[FIXED]** `[verified]`
  `kernel/fs/filelock.c` — `file_locks[]` is a bare global; `filelock_set_lock`
  does a check-then-grant with nothing atomic between the conflict scan and the
  install, `alloc_lock()` is a non-atomic test-and-set, and its NULL return is
  dereferenced unchecked. Two CPUs grant conflicting `F_WRLCK`s; concurrent
  callers double-claim a slot or NULL-deref. Fix: one global spinlock around all
  table access; on `F_SETLKW` drop it before `scheduler_wait_commit`.

- **F1-unix — HIGH — UNIX-socket peer back-pointer UAF.** **[FIXED]** `[verified]`
  `kernel/net/unix.c:43` (`unix_free_state`) — closing one end frees its
  `unix_socket_data` but never clears the surviving peer's `->peer`; the peer's
  next `send`/`recv` writes through the dangling pointer. Fix: clear
  `peer->peer` (and mark it disconnected) under the peer lock on teardown, or
  refcount `unix_socket_data`.

- **F1-journal — HIGH — concurrent journal transactions corrupt the heap and
  the on-disk journal.** **[FIXED]** `[verified]` `kernel/fs/journal.c` — no per-`journal_dev`
  lock; `journal_start_transaction` claims `handles[]` non-atomically and
  `journal_commit_transaction` advances shared `s_start`/`next_seq` unlocked.
  ext4 only holds the *parent inode* lock, so two CPUs creating files in
  different directories commit on the same `jdev` → double-free of
  `handle->data_blocks` / interleaved journal blocks. Fix: per-`journal_dev`
  sleeping mutex held start→commit; atomic-CAS handle-slot claim.

- **F2-blk — HIGH — block cache keeps two valid entries for one (dev,lba).** **[FIXED]**
  `[verified]` `kernel/dev/blk.c` — after `bcache_evict` drops the lock for
  write-back, the read/write miss paths do **not** re-run `bcache_find` before
  claiming and hash-inserting their slot. Two CPUs missing the same key create
  two VALID entries; a later write updates one, reads via the other return
  stale data, and eviction of the dirty stale copy clobbers newer data. Fix:
  re-`bcache_find` after `bcache_evict` returns; insert into the hash before
  dropping the lock for the fill.

- **FL-3 — HIGH — file locks owned by an exited CLONE_FILES thread never
  release.** **[FIXED]** `[verified]` `filelock.c` + `scheduler.c` — ownership is keyed by
  task id, but the shared fd table is closed only by the *last* table user with
  *that* task's id, which never matches the original owner. Stale lock persists
  → other processes' `F_SETLK` get EAGAIN forever, the 64-slot table fills. Fix:
  key lock ownership by the thread-group/leader id.

- **X-1 — HIGH — xattr list mutated/traversed with no inode lock.** **[FIXED]** `[verified]`
  `kernel/fs/vfs.c` setxattr/getxattr/removexattr/listxattr — `removexattr`
  frees a node a concurrent `getxattr` is walking (UAF); two `setxattr` race the
  tail append (list corruption + leak). Fix: take the inode rwlock (write for
  set/remove, read for get/list).

- **F4-loop — MEDIUM — loop device stores the backing node with no reference.** **[FIXED]**
  `[verified]` `kernel/dev/loop.c` — `LOOP_SET_FD` does `lo->backing_node =
  h->node` with no `vfs_node_get`; after the setup process exits and the file is
  unlinked the node is freed, and `loop_read_blocks` dereferences it (UAF). Fix:
  `vfs_node_get` on SET_FD, `vfs_node_put` on CLR_FD; invalidate the block cache
  on SET/CLR; reject non-regular backing files (recursion DoS).

- **PC-1 — MEDIUM-HIGH — `page_cache_flush_inode` races `page_cache_truncate_inode`.** **[FIXED]**
  `kernel/mm/page_cache.c` + `kernel/fs/vfs.c` — `vfs_fsync`/`vfs_close_handle`
  flush without holding the inode lock; the M46 truncate path's refcount≠0 branch
  does `memset(frame,0,PAGE_SIZE)` on a frame a lockless writeback may be mid-DMA
  reading → torn/zeroed data on disk. (Directly adjacent to the M46 truncate
  work.) Fix: hold `vfs_inode_lock` in fsync/close around the flush.

- **IC-1 — MEDIUM (latent) — icache stores raw `vfs_inode*` with no reference.** **[FIXED]**
  `kernel/fs/vfs.c` — `icache_insert`/`icache_get` never `vfs_inode_get`, and
  `vfs_inode_put` frees the inode without removing it from the cache. Harmless
  *today* only because every `icache_get` result is used as a presence boolean
  and never dereferenced — it becomes a critical UAF the moment anyone reads
  `icache_get(...)->field`. Fix: refcount cached inodes; remove from the icache
  in `vfs_inode_put` before free.

## Confirmed — process-semantics commit 9dce201 (this milestone's own code)

- **M46-1 — HIGH — `terminate_group_siblings` can resurrect a DEAD/REAPING
  sibling.** **[FIXED]** `kernel/sched/scheduler.c:2100-2110` — it wakes siblings with a
  plain check-then-act `sibling->state = TASK_READY` instead of the
  compare-exchange every other wakeup in the file uses. Under CLONE_THREAD with
  siblings on APs, a sibling transitioning to DEAD/REAPING between the read and
  the store is overwritten back to READY and re-queued onto a task whose
  stack/slot is being freed → double-run / UAF / pmm poisoning. Fix: per-state
  `__atomic_compare_exchange_n(&sibling->state, &expected, TASK_READY, …)`,
  skipping DEAD/REAPING/UNUSED — mirror `wake_sleepers`.

- **M46-2 — MEDIUM — orphaned-pgrp false-negative with ≥2 children in one pgrp.** **[FIXED]**
  `scheduler.c:2229-2249` — the exit path reparents one child then immediately
  tests `is_pgrp_orphaned`, while not-yet-reparented siblings still point at the
  (still-RUNNING) exiting parent → the group reads as "not orphaned" and the
  stopped members miss SIGHUP+SIGCONT. Fix: reparent ALL children first, then
  compute orphan status once per distinct pgrp.

- **M46-3 — LOW — signal-death exit doesn't reparent children / run orphan
  handling.** The two signal-death sites call `terminate_group_siblings` but,
  unlike `scheduler_exit_current`, skip child reparenting and orphan-pgrp
  signalling. Fix: factor the reparent+orphan block into a helper, call it from
  both paths.

- **M46-4 — LOW — possibly spurious SIGHUP to the console fg pgrp.** ctty type
  defaults to 1 (console) and is inherited, so a session leader that never owned
  the console can still HUP `console.fg_pgrp` on exit. Low impact (real leaders
  setsid first). Fix: default ctty to "none" unless a TIOCSCTTY-equivalent set it.

- **M46-5 — LOW (cosmetic) — `frame->cs == 0x1B || 0x23` in `scheduler_charge_tick`
  callers** is the OR of both arches' user-CS; accidentally-correct but should be
  the per-arch constant.

## Traced but not adversarially re-verified (high-confidence, pending check)

These came from the first round's deep readers and were not run through the
verifier; treat as strong leads to confirm before fixing:

- **mqueue (`kernel/ipc/mqueue.c`)** — no locking at all; circular-buffer
  head/tail/count raced; duplicate-create on the same name; blocking is a bare
  `scheduler_yield` busy-spin (no wakeup).
- **shm (`kernel/ipc/shm.c`)** — `shm_detach_all` has no caller, so
  `shm_nattch` is never decremented on exit (segment + slot leak, IPC_RMID can
  never free); no table lock; attach table keyed by recyclable pid.
- **futex (`kernel/sched/futex.c`)** — no waiter cleanup on task exit (stale
  entry → wake-after-reuse spuriously wakes an unrelated task); FUTEX_WAIT is
  uninterruptible (no signal check); PROCESS_SHARED across separate mmaps never
  wakes (keyed by pml4+uaddr, not physical frame). *(Note: the lost-wakeup claim
  against the WAIT/WAKE handshake was a FALSE POSITIVE — the waiter is enqueued
  under the bucket lock before the block, so that window is closed.)*
- **aio (`kernel/fs/aio.c`)** — `aio_ctx_find_by_task` returns a `ctx` pointer
  after dropping the list lock with no refcount; a worker can use it after
  `aio_task_cleanup` frees it (UAF).
- **unix sockets (`kernel/net/unix.c`)** — `accept`/`recv` use bare
  `scheduler_block_on` after dropping the lock (lost-wakeup vs the TCP path which
  uses wait_prepare/recheck/commit); `connect` leaves itself in the listener
  backlog on signal/close (stale/UAF entry); blocking `send` on a full buffer
  returns EAGAIN instead of blocking.
- **journal crash-atomicity (`journal.c`/`ext4.c`)** — write-ahead ordering not
  enforced (no flush/barrier between data, commit, checkpoint); `ext4_journal_write_tx`
  writes the fs block pre-commit; `ext4_write_superblock` bypasses the journal
  mid-transaction; recovery replay has an unbounded tag walk and unvalidated
  target block numbers (crafted/torn-journal hazard); >32 logged blocks per op
  silently half-journal.
- **block flush (`blk.c`)** — `blk_flush_buffer` runs fully unlocked (lost DIRTY
  bit, stuck-BUSY wedge, torn flush); single-pass `blk_sync_all` can leave
  partition-parent blocks dirty after sync returns.

## Verifier correction

- **F3-futex — FALSE POSITIVE.** The earlier claim that FUTEX_WAIT loses wakeups
  was wrong: the waiter is linked into the bucket *under* `b->lock` and only then
  is the lock dropped and `scheduler_block_on` called, so any waker observes a
  published waiter. The futex *exit-cleanup* and *PROCESS_SHARED* gaps above are
  separate and real.
