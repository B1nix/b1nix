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

---

# Round 3 — Full-system bug sweep (2026-06-12)

Scope: a fresh corruption/hang/security sweep across **all** subsystems
(`kernel/fs/`, `kernel/net/`, `kernel/mm/`, `kernel/sched/`, `kernel/arch/x86/`,
`kernel/syscall/`, `kernel/ipc/`) via four parallel deep readers, with the
highest-severity items re-read and confirmed by hand. None of these are fixed
yet — all are **open**. Status tags: **CONFIRMED** = source re-read by hand this
round; **LEAD** = agent-reported with cited lines, not yet hand-verified.

## R3 — Critical (confirmed by hand)

- **R3-1 — CRITICAL — CONFIRMED — VFS double-decref → UAF when the fd table is
  full.** `vfs_open_flags` (`kernel/fs/vfs.c:2331-2345`). When
  `scheduler_fd_alloc` fails (ordinary `EMFILE`), the EMFILE branch calls
  `vfs_handle_release(h)` → `node_file_ops` has no `.release` (vfs.c:2562) so it
  falls to `vfs_node_put(h->node)` (decref #1); then the `out:` label runs
  `vfs_node_put(node)` on the same pointer (decref #2). Two puts for one ref →
  premature free of the vfs_node/inode slab object → UAF. Trivially reachable:
  exhaust the fd table, then `open()` any file. Fix: `node = NULL;` after
  `vfs_handle_release(h)` in the EMFILE branch so `out:` does not re-put.

- **R3-2 — CRITICAL — CONFIRMED — IPv6 `::1` loopback delivered synchronously →
  TCP state-machine re-entrancy.** `ipv6_send` (`kernel/net/ipv6.c:310-314`)
  calls `ipv6_receive()` inline for loopback — exactly the bug already fixed for
  IPv4 (`ipv4.c:121` uses `net_loopback_enqueue`). The deferral infra already
  takes an `is_v6` flag (`net.h`). Recursion `ipv6_send → ipv6_receive →
  tcp6_receive → … → ipv6_send` corrupts/deadlocks any TCP-over-`::1` exchange
  (and UDP6/ICMPv6 loopback). Fix: `net_loopback_enqueue(buffer, total, 1);`.

- **R3-3 — CRITICAL (security) — CONFIRMED — mqueue handle is a raw kernel
  pointer → arbitrary kernel read/write from any process.**
  `SYS_MQ_OPEN` returns `(u64)(usize)mq` — the kernel VA of the queue
  (`kernel/syscall/syscall.c:2443`). `SYS_MQ_SEND/RECEIVE/CLOSE` cast `arg0`
  straight back to `struct mqueue *` with zero validation (syscall.c:2454,
  2459, 2469). Any process: `mq_send(arbitrary_kaddr, buf, len)` →
  `memcpy(&mq->msgs[...], data, len)` writes up to 256 bytes at an
  attacker-chosen kernel address (receive gives an arbitrary read back).
  The *user* buffer is copyin/copyout'd correctly — the hole is the handle.
  Fix: return the array index from `mqueue_create`; validate
  `0 <= mqd < MQ_MAX_QUEUES && queues[mqd].used` and resolve the pointer in
  kernel. (Supersedes the "no locking" mqueue lead above as the priority item.)

- **R3-4 — CRITICAL — CONFIRMED — ext4 uninitialized-extent high bit not masked
  → cross-file read/write.** `ext4_extent_lookup` (`kernel/fs/ext4.c:271, 287`)
  and the merge in `ext4_add_extent` (`:307`) compare against the raw `ee_len`.
  Per the ext4 format `ee_len > 0x8000` marks an *uninitialized* extent of true
  length `ee_len - 0x8000`. The truncate path masks this (`ext4.c:439`); lookup
  and merge do not. Mounting any real ext4 image with a `fallocate`/delayed-alloc
  extent makes the offset math land outside the file's allocation → another
  file's blocks. Fix: one `ext4_extent_len()` mask accessor used in all three
  sites.

- **R3-5 — CRITICAL — CONFIRMED — ext4 `ext4_add_extent` zeroes `i_block[]` of a
  block-mapped inode → data loss + orphaned blocks.** `ext4_set_block` routes to
  `ext4_add_extent` for any `block_idx >= EXT2_NDIR_BLOCKS` even when the inode
  has no `EXT4_EXTENTS_FL`; the function then `memset(inode->i_block, 0, ...)`
  (`kernel/fs/ext4.c:312`), destroying all 12 direct pointers of a block-mapped
  inode (these exist on real ext4 images). Fix: take the extent path only when
  `EXTENTS_FL` is set; otherwise implement the indirect set-block or return
  `-EOPNOTSUPP`.

## R3 — High / Medium (agent leads, cited, pending hand-verification)

- **R3-6 — HIGH — LEAD — shared `vma_list` of CLONE_VM threads is completely
  unlocked → UAF.** Threads share `vma_list` (`scheduler.c:1465`); every reader
  and writer — `mmap` insert (`syscall.c:1702`), `munmap`→`vma_delete_range`+
  `kfree` (`scheduler.c:3419`), `mprotect`/`vma_split` (`syscall.c:1766`), `brk`
  (`syscall.c:1834`), and the **page-fault handler walk** (`paging.c:401`) —
  touches it with no lock (`vmm_lock` guards page tables, not this list).
  Concurrent mmap/munmap (or a fault racing a munmap) corrupts the intrusive
  list / frees a VMA another CPU is dereferencing. Reachable by any multithreaded
  process, no swap needed. Fix: per-address-space `vma_lock` (shared like
  `vma_list`), `irqsave`, taken around every traversal/mutation incl. the fault
  handler (snapshot fields under the lock, release before blocking I/O).

- **R3-7 — HIGH — LEAD — TLB shootdown counts not-yet-LAPIC-ready APs →
  boot-time hang/panic.** `online_cpu_count()` (`tlb.c:28-34`) counts
  `g_max_cpus` without checking `cpu_online`. `g_max_cpus` is bumped the instant
  the AP trampoline sets `ready_flag`, *before* the AP enables its LAPIC. The
  first shootdown after `tlb_shootdown_set_enabled(1)` (main.c:524) waits for an
  ACK from a CPU that can't yet answer → spin then `panic("tlb_shootdown
  timeout")`. Survives today on timing luck. Fix: count only
  `get_percpu_n(i) != NULL`; set the AP's shootdown-ready flag at the end of
  `lapic_init_local`, not in the trampoline.

- **R3-8 — HIGH — LEAD — shm `IPC_RMID` frees frames still mapped in forked
  children (cross-process UAF) + no owner/permission checks.**
  `kernel/ipc/shm.c:289-301`. `shmat` maps `VMM_SHARED` and refs frames, but
  `shm_nattch` is not bumped on fork, so a child holds a live writable mapping
  the segment doesn't know about; `IPC_RMID` sees `nattch==0`, `pmm_free_frame`s
  every page and `memset`s the segment → PMM recycles frames still mapped into
  the child. No owner check on `IPC_RMID`/`IPC_SET`; `SHM_RDONLY` ignored
  (always `VMM_WRITABLE`). Extends the existing shm lead — the fork-frame-free is
  the dangerous part. Fix: account shm VMAs on fork; mark-pending + free only at
  last teardown; enforce creds.

- **R3-9 — HIGH — LEAD — untrusted on-disk/packet parsing crash class.** Several
  parsers trust length/index fields from a crafted image or packet:
  - `fat32.c` — self-referential FAT entry → unbounded chain loop (in-kernel
    hang); `cluster_to_sector` (`:67`) has no `cluster >= 2 && < total` check;
    `total_clusters = total_sectors / sectors_per_cluster` (`:347`) is a
    divide-by-zero panic when SPC==0.
  - ext dir walkers — `ext2.c:711`, `ext3.c:457`, `ext4.c:591`, `ext1.c:320` —
    advance by `rec_len` and index `buf+off` with no `rec_len >= 8 && off+rec_len
    <= block_size && 8+name_len <= rec_len` check → heap overrun at mount.
  - `exfat.c:130/244` — `secondary_count == 0` file entry → 32-byte OOB read past
    the cluster buffer.
  - `ntfs.c:276/316` — resident `$DATA` `voff+vlen` and index `name_len`
    unbounded → OOB heap read disclosed as file data.
  - `dns.c:186-207` — three OOB reads on a malformed/spoofed reply (name-skip
    deref past end; unbounded `while (*ptr) ptr += *ptr+1`; TYPE/rdlen read past
    a stale bound).
  Fix: bound every length/index against the buffer/end before deref.

- **R3-10 — HIGH — LEAD — ext2 directory-grow corrupts the indirect slot +
  hole-read overruns the caller buffer.** `ext2_add_dir_entry`
  (`ext2.c:739`) writes `i_block[blocks]` directly; past 12 blocks this clobbers
  the single-indirect pointer slot with a data block number → later reads treat
  data as indirect pointers (arbitrary-block I/O). `ext2.c:481` hole-read does
  `memset(buffer + bytes_read, 0, block_size - block_offset)` without clamping to
  `to_read - bytes_read` → up to a full block of zeros past the requested `size`.
  Fix: set-block helper for the dir-grow; clamp the memset length.

- **R3-11 — MEDIUM — LEAD — aio context UAF/double-free on task exit with
  in-flight AIO.** (Already noted as an aio lead in round 2; re-confirmed by the
  MM reader.) `aio_task_cleanup` frees `ctx` while the worker may still
  `aio_ctx_find_by_task` + write `ctx->completed[...]` after `kfree`. Fix: drain
  `inflight==0` (or mark dying, last ref frees) under the ctx-list lock.

- **R3-12 — MEDIUM — LEAD — non-atomic signal-wake state set →
  runqueue/UAF race.** `scheduler_kill` (`scheduler.c:3176`) and
  `scheduler_kill_process_group` (`:3232`) do a plain check-then-set
  `T(i)->state = TASK_READY` + enqueue under local-IRQ-disable only (no global
  lock / CAS) — the pattern `terminate_group_siblings` already avoids with a CAS.
  A slot recycled between the load and the store gets a stale READY/enqueue. Fix:
  per-state CAS (BLOCKED→READY, STOPPED→READY).

- **R3-13 — MEDIUM — LEAD — blk.c drops dirty buffers on write-back failure.**
  `bcache_evict`/`blk_flush_buffer` clear `BLK_CACHE_DIRTY` regardless of the
  `write_blocks` return → a failed device write silently loses data and
  `blk_sync_all`/umount still report success. Fix: clear DIRTY only on success;
  propagate the error. (Overlaps the existing blk-flush lead.)

- **R3-14 — MEDIUM — LEAD — 32-bit fork COWs `VMM_SHARED` pages → SysV shm breaks
  across fork.** `paging.c:598-601` (32-bit) marks every writable PTE COW
  unconditionally; the x86_64 path guards `!(entry & VMM_SHARED)`
  (`x86_64/paging.c:841`). After fork on i686, a shmat'd writable segment becomes
  private on first write — shared writes stop being visible (silent IPC break).
  Fix: mirror the `!(pte & VMM_SHARED)` guard.

- **R3-15 — LOW/MEDIUM — LEAD — VFS create/mkdir/symlink error paths put a
  refcount==0 node → underflow.** `vfs.c:2715, 2835, 3327` — the same bug already
  fixed for `vfs_link` (comment at vfs.c:3225-3230), not applied to these three.
  Fix: set `node->deleted = 1` + refcount to 1 before the put, per the `vfs_link`
  precedent.

- **R3-16 — LOW — LEAD — demand-paging fault runs blocking VFS `read_cb` with
  IRQs disabled.** `paging.c:408-433`; safe today only because the read polls.
  Fix: `sti` for user faults after latching CR2, before the read.

---

# Round 4 — Drivers / ELF loader / boot-parsing sweep (2026-06-12)

Second sweep covering the subsystems round 3 did not reach: device drivers
(`kernel/dev/`, `kernel/arch/x86/{rtc,serial}.c`), the ELF loaders + exec path
(`kernel/user/process.c`), and boot/firmware-table parsing (`kernel/bootinfo/`,
`kernel/mm/pmm.c`, ACPI, procfs/sysfs). Same tags: **CONFIRMED** = re-read by
hand; **LEAD** = agent-cited, pending hand-verification. All **open**.

## R4 — Critical / High (confirmed by hand)

- **R4-1 — HIGH — CONFIRMED — RTC `is_updating()` spin has no timeout → boot
  hangs forever on bare metal.** `rtc_init` (`kernel/arch/x86/rtc.c:22-23`,
  identical in `x86_64/rtc.c`): `while (is_updating()) ;` on the CMOS UIP bit,
  early at boot before the test `done` marker. A dead/absent RTC or a wedged
  southbridge that leaves UIP stuck loops forever — the exact 120s-policy
  violation that `ps2_kbd.c`/`serial.c` were already converted away from, but RTC
  was missed. QEMU clears UIP promptly so it never reproduces in CI. Fix:
  `for (int i = 0; i < 1000000 && is_updating(); i++) ;` and proceed regardless.

- **R4-2 — HIGH — CONFIRMED — multiboot2 mmap walk: `entry_size == 0` → infinite
  loop / OOB.** `parse_mmap_tag` (`kernel/bootinfo/multiboot2.c:95`):
  `for (cursor = ...; cursor < entries_end; cursor += tag->entry_size)` trusts the
  bootloader-supplied `entry_size` with no validation. `entry_size == 0` →
  `cursor` never advances → hang before serial init can report; `entry_size <
  sizeof(entry)` reads misaligned/overlapping; the last entry can straddle past
  `entries_end`. Fix: reject `entry_size < sizeof(struct multiboot2_mmap_entry)`
  and use `cursor + tag->entry_size <= entries_end` as the guard (mirror
  `acpi.c:113`).

- **R4-3 — HIGH — CONFIRMED — multiboot2 tag walk: `tag->size == 0` → infinite
  loop.** `multiboot2.c:118-155`: `cursor = align_up(cursor + tag->size, 8)` — a
  non-END tag with `size == 0` (or `< 8`) leaves `cursor` unchanged → hang. No
  check that the tag header/body fits in `[cursor, end)` before the typed
  dereferences (e.g. framebuffer tag). Fix: after reading the header,
  `if (tag->size < sizeof(*tag) || cursor + tag->size > end) break;`.

- **R4-4 — HIGH — CONFIRMED — ELF loaders trust `e_phentsize` / `e_phoff` →
  OOB heap read + wild pointer.** `user_load_elf64` (`kernel/user/process.c:615`)
  and `user_load_elf32` (`:917`) only check
  `e_phoff + e_phentsize*e_phnum > file_size`. (a) `e_phentsize` is never checked
  to equal `sizeof(struct elf64_phdr)` (56) / elf32 (32) — the shared-object
  loader at `:230` validates this, the two primary entry points do not. With
  `e_phentsize` small/zero the phdr-walk casts read 56/32 bytes per slot past the
  `kmalloc(file_size)` buffer; the garbage `p_offset`/`p_filesz`/`p_vaddr` then
  drive memcpy/mapping. (b) `e_phoff` is a raw u64; `e_phoff + product` can wrap
  below `file_size` so the check passes and `file_data + e_phoff + j*e_phentsize`
  is a wild kernel pointer. Untrusted ELFs are loaded from VFS/TCC. Fix: reject
  `e_phentsize != sizeof(phdr)`; use subtraction-form bounds
  (`e_phoff > file_size || product > file_size - e_phoff`).

## R4 — Medium / Low (agent leads, cited)

- **R4-5 — HIGH — LEAD — virtio-net used-ring drains trust device `used->idx`
  with no per-poll cap → livelock holding the ring lock.** `virtio_net.c:190`
  (RX) and `:172` (TX): `while (used->idx != last_used_idx) {…}`. The inner
  `id`/`len` ARE bounds-checked (no OOB), but a device that keeps `used->idx`
  perpetually unequal spins `vnet_poll` forever under `net_rx_lock`/`net_tx_lock`.
  Fix: bound each drain to `queue_size` iterations per poll.

- **R4-6 — MEDIUM-HIGH — LEAD — ELF PT_INTERP offset check lacks the wrap
  guard.** `process.c:654` (elf64) and `:946` (elf32): `if (p_offset + p_filesz >
  file_size) continue;` — unlike the PT_LOAD path (`:677`) and `parse_dynamic`
  (`:184`), no `p_offset + p_filesz < p_offset` guard. Huge `p_offset` + small
  `p_filesz` wraps below `file_size`, then `memcpy(interp, file_data + p_offset,
  ilen)` is a wild read. Same class at elf32 PT_DYNAMIC (`:1008`, u32 wrap). Fix:
  add the wrap guard to all three.

- **R4-7 — MEDIUM — LEAD — PMM accepts a wrapped/overlapping memory region.**
  `pmm.c:257, 296, 342` compute `base + length` from the (now trusted) mmap
  entries with no `base + length < base` overflow/empty guard; an AVAILABLE
  region that overlaps low memory/MMIO is bulk-freed via `mark_frames_free_range`
  (the kernel/bitmap/ramdisk are re-reserved afterward, but other false regions
  are not). Fix: `if (length == 0 || base + length < base) continue;` at the top
  of each region loop. Related LOW: multiboot2 module tag (`:132`) can underflow
  `ramdisk_size` if `mod_end < mod_start` — add a guard.

- **R4-8 — MEDIUM — LEAD — ELF strtab / symbol-name reads are length-unbounded
  (PIE only).** `process.c` DT_NEEDED (`:732`) and symbol compares in
  `elf64_resolve_symbol` (`:297`) / `elf64_apply_rela_table` (`:340`) do
  `strlen`/`strcmp` from a vaddr that's only guaranteed 1 byte inside a segment;
  a string with no NUL before the segment's heap allocation ends → OOB scan. Fix:
  bounded string accessor that stops at `seg->vaddr + seg->memsz`.

- **R4-9 — LOW/MEDIUM — LEAD — NVMe `count == 0` underflows NLB → 4 GiB DMA
  overrun.** `nvme.c:258` `cdw12 = count - 1`; `count == 0` → `0xFFFFFFFF`. Latent
  (cache always issues `count == 1`) but unguarded. Plus PRP-list overflow for
  transfers > 513 pages (`nvme.c:244`) and virtio_blk `count * 512` 32-bit
  overflow (`virtio_blk.c:117`) — all latent behind the one-request/`count==1`
  invariant. Fix: guard `count == 0`; clamp PRP pages; `(u64)count * 512`.

- **R4-10 — LOW/MEDIUM — LEAD — ELF `p_memsz` unbounded → OOM-panic DoS.**
  `process.c:700` `kzalloc(p_memsz)` capped only by the ~127 TiB vaddr ceiling; a
  multi-GiB `p_memsz` makes `kmalloc` panic (project rule: OOM = panic) → a
  loadable file panics the kernel. Fix: reject `p_memsz` above a sane image
  ceiling before allocating.

- **R4-11 — LOW — LEAD — PS/2 mouse `packet_index` OOB write under IRQ
  re-entrancy/SMP.** `ps2_mouse.c:127` `packet[packet_index++]` into
  `static u8 packet[3]`; the i8042 decode state is non-atomic with two IRQ entry
  points + a timer-tick poll. IRQs are BSP-routed today so effectively
  single-CPU, but nothing pins them. Fix: defensive `if (packet_index >= 3)
  packet_index = 0;` + pin i8042 IRQs/poll to the BSP.

## R4 — Design note (not a bug)

- **AHCI/NVMe completion waits are deliberately unbounded** (`ahci.c:62`,
  `nvme.c:102/192`) with in-code comments: a timeout-return would let the device
  DMA into a buffer the caller then frees. The *init/reset* loops ARE bounded
  (`timeout > 0`). Conscious tradeoff, but a genuinely wedged device still hangs
  past 120s; strict compliance would need bound-then-abort-the-queue (mark device
  dead, don't free the buffer), not a plain timeout.

## R4 — Verified clean (no action)

PCI/virtio enumeration loops are fixed-bound (no capability-pointer walk —
legacy port-I/O transport). ACPI MADT/RSDT/XSDT parsing is well-hardened (every
table length/entry-count guarded; zero-length subtables handled; `map_sdt`
enforces `length >= sizeof(header)`). procfs.c / sysfs.c builders clamp every
buffer (`PROCFS_BUF`/`sb_addf`) and clamp read offset to the generated length —
no OOB. execve argv/envp is bounded (`MAX_EXEC_ARGS`→`USER_MAX_ARGS=32`,
per-string space checks). PT_LOAD bounds, relocation target writes (via
`elf64_stage_ptr`), `vsnprintf_impl`, keyboard/serial SPSC rings, RNG copyout —
all correct.
