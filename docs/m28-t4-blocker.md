# M28 T4 — what's blocking it

This document captures what's known about the race that prevents T4 (BKL
out of `syscall_entry.S`) from landing. Read it before the next attempt
so you don't repeat the four failed iterations from this branch's session
history.

## Final status: T4 reverted; race fix kept

T4 was tried, applied, characterized at ~60% pass on the full smoke
suite, and ultimately **reverted from this branch** in favour of
keeping the residual races' failure rate at the documented pre-existing
baseline (~1-in-3 SMP-4 flakes from unrelated paths, not the M14 race).

What's in tree right now (without T4):
- `struct task::stack_released` lease + `arch_context_switch` publish
  + `scheduler_waitpid` poll — kept as a strict improvement; even with
  the BKL in `syscall_entry.S`, this closes a real reap-vs-save race
  that could fire (rarely) when a child exits while concurrent
  cooperative paths are running.
- Lockdep Variant-A (`lockdep_*_global` for BKL + per-inode rw_lock).
- M28 #9 ctx-switch benchmark (single-CPU only, see
  `kernel/sched/m28_ctxbench.c`).

Two T4 stabilizers were attempted on top and reverted:

1. **BKL on fork/exec only (CPU-owned lock, partial coverage).**
   Failed immediately — 0 % pass — because the M24b BKL bequeath model
   breaks the moment one syscall takes BKL and another doesn't.
   Specifically: task A in `fork` holds BKL, yields, BKL bequeathed
   to whatever task next runs on its CPU, and `user_jump.S`
   unconditionally `bkl_unlock`s on first ring-3 entry → "released"
   while A still thinks it holds it. Result: scheduler panics with
   "dead task has nowhere to yield".

2. **Task-owned sleeping mutex on fork/exec.**
   Correct in theory (owner = `struct task *`, migrates with the
   task, only the holder can unlock). In practice slowed the kernel
   ~19× on single CPU (single-CPU smoke reached only M12 in 60 s vs
   M27 in 28 s) — root cause not isolated. Reverted.

The residual ~40 % failures under T4 don't have fork/exec shape
(kernel-heap RIPs, `memmove` crashes) and look like heap UAF / spurious
stack-lifetime corruption in unspecific paths. Closing them needs
targeted debugging (per-call audit of kheap allocs, per-task struct
canaries, deeper lockdep extensions) that didn't fit this session.

## Earlier: T4 landed with the primary race fixed

**T4 was applied** (`bkl_lock`/`bkl_unlock` removed from
`kernel/arch/x86/syscall_entry.S`), together with the per-task
`stack_released` lease that closes the M14 reap-vs-save race:

- `struct task::stack_released` (kernel/include/b1nix/sched.h)
- `arch_context_switch` takes a 3rd arg `volatile int *released_publish`
  and stores `1` into it AFTER the RSP swap
  (kernel/arch/x86/context_switch.S)
- `scheduler_exit_current`, the SIGKILL/SIGDFL-terminate paths in signal
  delivery, and `ap_worker_trampoline` clear `stack_released = 0` BEFORE
  publishing `state = TASK_DEAD` (x86 TSO orders the stores)
- `scheduler_waitpid` spins on `stack_released == 1` after winning the
  `DEAD → REAPING` CAS, before calling `kfree(T(i)->stack)`

The previously-deterministic M14 `iretq` GP fault (RIP `0x16accd`) **no
longer reproduces**. SMP-4 smoke under T4 reaches `M14-SMOKE: ok
block-cache → stress-loop → done` cleanly when the suite succeeds.

### Stability

T4 smoke characterization on this branch (5 consecutive `tests/smoke.sh
x86` runs with T4 applied):

| run | result      |
|-----|-------------|
| 1   | 250 / 0 ✅  |
| 2   | 250 / 0 ✅  |
| 3   | 249 / 1 ❌  kernel-heap RIP `0xffff800007374ffb` |
| 4   | 249 / 1 ❌  concurrent panics: `memmove+0x42` + kernel-heap `0xffffa00000300000` |
| 5   | 250 / 0 ✅  |

3 / 5 = 60 % pass. For comparison, the documented pre-existing
SMP-4-baseline flake rate (without T4) is ~1-in-3 hangs / faults from
unrelated paths (see project memory `project_m28_smp.md`); the rates
are in the same ballpark, so T4 doesn't materially degrade stability
vs the residual baseline races.

The failures left after T4 land are NOT the M14 stack-lifetime race
this fix targeted. They have different shapes (kernel-heap-mapped RIPs,
concurrent panics in `memmove`) and almost certainly come from the
remaining unfixed races below (exec multi-step state mutation; fork
RBP-chain race for kthread paths; a stopped-child reap path that
lockdep-on additionally surfaces in M13-JC).

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

With `-DKERNEL_LOCKDEP=1` the SMP-4 smoke USED to trip two pre-existing
discipline issues:

- **Out-of-order BKL release**: the M24b bequeath model lets one CPU
  acquire the BKL and another release it (a task migrates between
  syscall acquire and syscall release). Lockdep's per-CPU stack model
  doesn't allow this. Real fix: track BKL globally (single ownership
  entry) or per-task instead of per-CPU.

- **Out-of-order INODE release on an empty stack**: same shape, for the
  per-inode sleeping rwlock when a yielding lock holder migrates across
  CPUs.

**Variant A fix landed:** `lockdep_acquire_global` /
`lockdep_release_global` (kernel/sched/lockdep.c) skip the per-CPU
acquisition stack for `LOCKDEP_LVL_BKL` and `LOCKDEP_LVL_INODE`, doing
only the inversion-check on acquire. No global atomic counter (a
shared cache-line bump on every syscall perturbs syscall density
enough to amplify the stack-corruption race in M14). Production build
(lockdep off) is unchanged.

Lockdep-on now boots cleanly to `B1NIX-TEST: done` on single CPU, and
both the bare BKL-bequeath and the INODE migrating-holder path no
longer false-positive.

**But: T4 attempted again with lockdep-on as detector still fails.**
SMP-4 smoke with `-DKERNEL_LOCKDEP=1` and `bkl_lock/bkl_unlock` removed
from `syscall_entry.S` crashes at M14 with `EXCEPTION: general
protection fault, rip: 0x16accd` — which is the `iretq` instruction in
the IRQ-return prologue. **No `LOCKDEP:` panic appears anywhere in the
log.** That's the verdict:

- Lockdep does NOT detect a lock-discipline mistake here.
- The crash is a corrupted IRET frame — shape #1 stack-lifetime
  corruption — invisible to any acquisition-order tracker.

So **lockdep-as-detector is not the right tool for T4's race**. The
race is in kernel-stack memory lifetime (fork copy / exit free / AP
ctx-switch save-side), not in lock ordering. The path forward remains
step 2 of "What the next attempt should do" below: per-CPU stack-lease
abstraction, not better lockdep coverage.

## What the next attempt should do

In rough order:

1. **Reproduce shape #2 on bare T8** (no T4). Confirm the race is
   pre-existing.  If yes, fix it under BKL first so smoke regression is
   a useful signal; then T4 should "just work."
2. ~~**Add a per-CPU "stack lease"** abstraction or equivalent: each
   syscall path takes a strong ref on its kernel stack at entry, drops
   it at exit. exit_current waits for refs == 0 before signalling the
   parent waitpid that the stack is free. This is what F7 *wanted* to
   be but for the more general case (not just reap).~~ **DONE** —
   landed as the per-task `stack_released` lease described above.
   Closes the deterministic M14 `iretq` reap-vs-save race. Different
   scope than the per-CPU stack-lease originally sketched (which would
   have covered every kernel-stack-using path) but sufficient for the
   actual race that was tripping shape #1 reliably.
3. ~~**Fix the lockdep model** (per-task or global-singleton for BKL) so
   lockdep-on is usable as a debugging aid for the next round.~~
   **DONE (Variant A landed).** Confirmed lockdep cannot detect this
   race — it's stack-lifetime, not lock-discipline. See above section.
4. **Re-attempt T4** with the smoke + benchmark in tree from the start
   — failure should be a single line of output, not a 9-second timeout
   with a corrupted backtrace.

T5 (BKL out of scheduler core) is largely a no-op once T4 lands: the
scheduler doesn't itself call `bkl_lock`/`unlock` anywhere.

## Files this work has already touched

Foundation that **stays in tree** regardless of T4 status:

- `kernel/sched/bkl.c` — owner-check no-op for non-owner unlock;
  outermost acquire/release now uses `LOCKDEP_*_GLOBAL` for BKL.
- `kernel/sched/scheduler.c` — atomic CAS state transitions (F1, F4, F5,
  F6) + the sleep/block re-entry recovery + BSP `idle_task = boot`.
- `kernel/arch/x86/lapic.c`, `main.c` — `bkl_unlock()` before `sti; hlt`
  in both idle loops (T1 + T2).
- `kernel/arch/x86/interrupts.c` — BKL bypass for vectors 64/65/66 (T3);
  `lapic_eoi()` before `scheduler_on_timer_tick` (T8).
- `kernel/include/b1nix/lockdep.h`, `kernel/sched/lockdep.c` — debug
  tracker (M28 #2) + Variant-A bequeath relaxation
  (`lockdep_acquire_global` / `lockdep_release_global`).
- `kernel/fs/vfs.c` — per-inode `rw_lock` now uses `LOCKDEP_*_GLOBAL`
  for the sleeping side that can migrate via `scheduler_block_on`.

Nothing else needs to be touched for T4 — `syscall_entry.S` is the only
file with `bkl_lock`/`bkl_unlock` left to remove.
