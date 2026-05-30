# M28 T4 — what's blocking it

This document captures what's known about the race that prevents T4 (BKL
out of `syscall_entry.S`) from landing. Read it before the next attempt
so you don't repeat the four failed iterations from this branch's session
history.

## What T4 actually changes

Two lines in `kernel/arch/x86/syscall_entry.S`:

```diff
-    call bkl_lock
+    /* no BKL */
     ...
-    call bkl_unlock
+    /* no BKL */
```

Effective behaviour: every ring-3 syscall runs on its CPU **without**
serialising against other CPUs at the BKL. Per-subsystem locks
(`vfs_tree_lock`, per-inode rw_lock, `bcache_lock`, `heap_lock`,
`pmm_lock`, `vmm_lock`, `g_tasks_lock`, `runqueue`, `fd_lock`) plus the
F-tier atomic state CAS coverage in the scheduler are supposed to be
enough. They are not, yet.

## The failure mode

Single CPU smoke always passes — there are no concurrent kernel paths to
race against. **SMP-4** smoke fails reproducibly in one of two shapes:

1. **GP fault on a non-canonical RIP** (e.g. `0xff01ffffffffffef`) in the
   M13-JC `wcontinued` step, just after the parent reaps a stopped-then-
   continued child. The RIP is a corrupted stack qword being treated as a
   return address.

2. **Invalid-opcode at a kernel-heap address** (e.g. `0xffffc000000783b2`)
   during M14 `block-cache` or M13-SMOKE syscall-heavy phases. The CPU
   resumed execution at a freed-and-reused kernel page.

Both shapes are *stack corruption*: a kernel stack page was either
freed-and-reused while another CPU was still executing on it, or
overwritten by a path that didn't expect concurrent access.

## Failed attempts on this branch (do not repeat as-is)

### Attempt 1 — bare T4

Removed BKL from `syscall_entry.S`. Single CPU passed. SMP-4 hit shape #1.
The diagnostic ctx-switch trace (which is no longer in tree — it was
diagnostic-only) showed the parent waitpid CAS'ing `DEAD → REAPING` and
freeing the child's `kernel_stack` while the AP was still in the child's
`exit_current → scheduler_yield` save side, *on the child's kernel stack*.
The AP's next IRET / RET picked up a freed-and-overwritten qword as RIP.

### Attempt 2 — F7 (departing-task tracking) + T4

Added a per-CPU `g_departing[]` slot set in `scheduler_yield`'s save side
and cleared by the continuation, plus a `released_from_cpu` bit in
`struct task`. `scheduler_waitpid`'s reap path was supposed to poll
`released_from_cpu` after winning the `DEAD → REAPING` CAS, so it only
freed once the AP had actually moved off the child's kernel stack.

Single CPU still passed. SMP-4 still failed — now shape #2, slightly
earlier (M14 block-cache instead of M13-JC). So F7 closed the *waitpid*
reap race but there is at least one **second** stack-lifetime race in a
different syscall path. Reverted.

### Attempt 3 — M28 #9 micro-benchmark (under reverted T4, with T8)

The benchmark hammers SYS_GETPID + SYS_YIELD 1000 times each from the
init kthread. With T8's preemptive timer ISR, that's enough to trigger
the *same* shape-#2 invalid-opcode fault under SMP-4 *with BKL still in
syscall_entry*. So the underlying race is not introduced by T4 — T4 just
makes it ~constantly reachable instead of "only at extremely high syscall
density". Benchmark also reverted.

## Where the second race lives — hypothesis, not verified

Two places are highly suspect, in priority order:

1. **`scheduler_fork_current`'s kernel-stack copy.** It memcpys the parent
   kernel stack into the child *and then relocates the saved RBP chain
   in-place*. If the parent is preempted (T8) mid-copy and the timer ISR
   yields, the child's saved RIPs may end up dangling. Look at the
   `child->kernel_stack_ptr` arithmetic at fork's `is_user ? ... : ...`
   branches.

2. **`scheduler_set_user_image` / `paging_create_address_space`** under
   exec. The pml4 swap is one CR3 write but the user_image swap, page
   table teardown, and runqueue placement happen across multiple lines
   without any explicit lock. Two concurrent execs (or an exec + a fork
   in another task that touches the same global pml4 free list) can
   race.

Neither is provably the bug; both are *the kind of multi-step state
mutation* that BKL was protecting.

## What lockdep-on shows (and why it doesn't help yet)

With `-DKERNEL_LOCKDEP=1` the SMP-4 smoke trips two pre-existing
discipline issues:

- **Out-of-order BKL release**: the M24b bequeath model lets one CPU
  acquire the BKL and another release it (a task migrates between
  syscall acquire and syscall release). Lockdep's per-CPU stack model
  doesn't allow this. Real fix: track BKL globally (single ownership
  entry) or per-task instead of per-CPU.

- **Out-of-order INODE release on an empty stack**: same shape, for the
  per-inode sleeping rwlock when a yielding lock holder migrates across
  CPUs.

Both issues are *debug-only* — they don't affect production behaviour
— but they mean lockdep-on cannot be used to find T4's stack-corruption
race. Fix lockdep's per-task model first if you want to use it as a
T4 debugging aid.

## What the next attempt should do

In rough order:

1. **Reproduce shape #2 on bare T8** (no T4). Confirm the race is
   pre-existing.  If yes, fix it under BKL first so smoke regression is
   a useful signal; then T4 should "just work."
2. **Add a per-CPU "stack lease"** abstraction or equivalent: each
   syscall path takes a strong ref on its kernel stack at entry, drops
   it at exit. exit_current waits for refs == 0 before signalling the
   parent waitpid that the stack is free. This is what F7 *wanted* to
   be but for the more general case (not just reap).
3. **Fix the lockdep model** (per-task or global-singleton for BKL) so
   lockdep-on is usable as a debugging aid for the next round.
4. **Re-attempt T4** with the smoke + benchmark in tree from the start
   — failure should be a single line of output, not a 9-second timeout
   with a corrupted backtrace.

T5 (BKL out of scheduler core) is largely a no-op once T4 lands: the
scheduler doesn't itself call `bkl_lock`/`unlock` anywhere.

## Files this work has already touched

Foundation that **stays in tree** regardless of T4 status:

- `kernel/sched/bkl.c` — owner-check no-op for non-owner unlock.
- `kernel/sched/scheduler.c` — atomic CAS state transitions (F1, F4, F5,
  F6) + the sleep/block re-entry recovery + BSP `idle_task = boot`.
- `kernel/arch/x86/lapic.c`, `main.c` — `bkl_unlock()` before `sti; hlt`
  in both idle loops (T1 + T2).
- `kernel/arch/x86/interrupts.c` — BKL bypass for vectors 64/65/66 (T3);
  `lapic_eoi()` before `scheduler_on_timer_tick` (T8).
- `kernel/include/b1nix/lockdep.h` etc. — debug tracker (M28 #2).

Nothing else needs to be touched for T4 — `syscall_entry.S` is the only
file with `bkl_lock`/`bkl_unlock` left to remove.
