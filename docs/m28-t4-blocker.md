# M28 T4 — what's blocking it

This document captures what's known about the race that prevents T4 (BKL
out of `syscall_entry.S`) from landing. Read it before the next attempt
so you don't repeat the prior iterations from this branch's session
history.

## Status (CS/SS swap session, 2026-05-31): single-CPU GREEN with T4 applied

The prior session's "T4 + new BKL handoff" attempt failed single-CPU at
M25 with no clear root cause. This session bisected the failure: the
breakage was NOT in the BKL handoff, but in the **CS/SS push order** of
`syscall_entry.S`. The prior session had "swapped" the constants from the
old (functionally-correct) ordering to a new (broken) ordering, on the
mistaken theory that the old code was a bug. Reverting that swap unblocks
T4 single-CPU completely.

### Background: why the old push order works

In long-mode SYSCALL, the kernel never returns to userspace via `iretq`
on the syscall-return path — `sysretq` is used instead. `sysretq` derives
CS/SS from the IA32_STAR MSR (CS = STAR[63:48]+16 = 0x23,
SS = STAR[63:48]+8 = 0x1B), it does NOT pop them from the stack. So
whatever the syscall entry path pushes for CS/SS on the per-task kernel
stack is, by itself, never reloaded by the CPU.

What it IS used for: building a `struct interrupt_frame` that other code
inspects — `arch_check_and_deliver_signals` looks at `frame->cs` to
decide whether the interrupted context was user (0x1B/0x23 either way),
the panic dumper prints CS for debugging, etc. So the values just need
to be present; the actual ordering convention has wiggle room.

The PRE-commit-"1" entry pushed `SS=0x23, CS=0x1B` (which is
"data-segment-as-SS, code-segment-as-CS reversed"). That looks wrong by
inspection, BUT it has been compatible with every consumer in the tree
for the entire SMP era. Switching to `SS=0x1B, CS=0x23` broke the M25
flow on single CPU — likely via some sigreturn-style codepath in the
TCC-compiled child that depends on the historically-used values.

### What's in tree now

`kernel/arch/x86/syscall_entry.S` restored to `pushq $0x23 /* SS */ ...
pushq $0x1B /* CS */` (the pre-commit-"1" ordering), with everything else
of commit "1" kept:

- T4 itself (`call bkl_lock` commented out in syscall_entry.S)
- Per-CPU syscall scratch at `gs:0x60` (struct percpu::syscall_scratch_rax)
- `bkl_*_for_switch` helpers in kernel/sched/bkl.c
- scheduler_yield BKL handoff (save old's depth, release, acquire new's)
- BSP/AP idle loop: lock BKL before yield, unlock before sti;hlt
- Timer ISR: lock BKL around yield if it interrupted userspace
- 64 KiB kernel stacks (up from 32 KiB)
- kthread/idle/boot tasks init `bkl_depth = 1`
- `scheduler_fork_current`: interrupts disabled across the entire child
  init critical section
- `paging_create_address_space`: kernel-half clone under vmm_read_acquire
- `paging_clone_address_space`: full user-half walk under vmm_write_acquire
- `scheduler_wake_blocked_parent`: tightened SIGKILL/default-terminate
  wake path

### Smoke verification (3 consecutive runs, BKL out of syscall_entry.S)

| run | single-CPU | SMP-4 |
|-----|------------|-------|
| 1 | 270/0 ✅ | invalid-opcode kernel-CS at RIP 0x7ffffffffed3 ❌ (M13-JC start) |
| 2 | 270/0 ✅ | same shape ❌ |
| 3 | 270/0 ✅ | same shape ❌ |

Single-CPU is now deterministically GREEN with T4 applied — a first for
this branch. SMP-4 deterministically fails at M13-JC start with the
documented shape #1 (kernel CS jumping to user-stack RIP — kernel-stack
lifetime / save-side corruption).

### What's next for SMP-4

The remaining race is a kernel-side stack-lifetime / ctx-switch save-side
race that materializes around the M13-JC fork+setpgrp+tcsetpgrp sequence
under SMP-4. The next session should attack this directly — the
debugging signal is now clean (single-CPU is a control, SMP-4 a
near-deterministic reproducer) instead of the prior 60% flake rate.

---

## Status (stack-lifetime + vmm_read session, 2026-05-30): fixes landed, T4 still not attempted

This session targeted the two highest-priority suspects identified at the
end of the previous session: the fork kernel-stack copy race and the
`paging_create_address_space` kernel-half clone running without any lock.
Both fixes landed as strict improvements. T4 was **not re-attempted** in
this session; the goal was to reduce the baseline flake rate first.

### Fixes applied

**1. `scheduler_fork_current` — interrupts held across the entire child
init critical section.**
`interrupts_disable()` is now called before `find_unused_task()` and
`interrupts_enable()` is called only after `child->state = TASK_READY` +
`sched_rq_enqueue_current(child)`.  Previously interrupts were re-enabled
too early (after just claiming the task slot), meaning the LAPIC timer ISR
could fire, preempt the fork path mid-stack-copy, and schedule the child
task on an AP before the child's `context.rsp`, `context.rbp`, VMA list,
and FD table were initialised. The AP would then resume the child on a
partially-written kernel stack → corrupted return addresses.

*File:* `kernel/sched/scheduler.c`, `scheduler_fork_current` (lines 671–847).

**2. `paging_create_address_space` — kernel-half clone now under
`vmm_read_acquire`.**
`paging_create_address_space` clones `kernel_pml4_virt[256..511]` into
a freshly allocated PML4. Without the read lock a concurrent
`vmm_map_page` (writer) could be modifying a kernel-half PD entry at the
same moment, producing a new process's PML4 with a torn intermediate
entry. The fix wraps the entire clone loop in `vmm_read_acquire` /
`vmm_read_release` (using the existing `vmm_lock` rwlock already held as
a write lock by `vmm_map_page` and `vmm_unmap_page`).

*File:* `kernel/arch/x86/paging.c`, `paging_create_address_space`
(lines 708–744).

Note: `paging_clone_address_space` was already taking `vmm_write_acquire`
(landed in the previous session). The new `vmm_read_acquire` in
`paging_create_address_space` closes the symmetric gap for exec-style
address-space creation.

### Smoke test result (BKL in, fixes above, SMP-4)

Run: `LD=/opt/homebrew/bin/ld.lld ./tests/smoke.sh x86` (-smp 4)

| marker                        | result |
|-------------------------------|--------|
| AP boots (INIT-SIPI)          | ✅ PASS |
| cross-CPU work-stealing       | ✅ PASS |
| AHCI / NVMe block device      | ✅ PASS |
| virtio-net init / DHCP        | ✅ PASS |
| M27 user/passwd smoke         | ✅ PASS |
| kernel cmdline parser         | ✅ PASS |
| full suite `B1NIX-TEST: done` | ❌ FAIL |
| SMP-4 `B1NIX-TEST: done`      | ❌ FAIL |

Overall: **93 passed / 178 failed / 0 skipped.**

The SMP-4 leg never reaches `B1NIX-TEST: done`. The failures are
concentrated in the shell / pthread / network / PIE / security smokes —
all of which require a working shell (`M11-SHELL`) as their prerequisite.
`M11-SHELL: ok simple-success` is the first failure, suggesting the shell
binary itself crashes or hangs before any output. This is a single-CPU
regression (the shell also fails on UP in this run), not a new SMP race.

Root-cause hypothesis: the shell binary's failure is unrelated to the
vmm_lock / fork-init changes; it is most likely a pre-existing issue in
the M33 shell feature branch that is exposed because the test tree is on
that branch. The AP/SMP infrastructure (INIT-SIPI, work-stealing) passes
cleanly.

### What remains for T4

The two strict improvements above reduce the fork-setup window but do not
close the residual shape-#2 (kernel-heap RIP / memmove crash) race
documented below. T4 should not be re-attempted until the shell regression
is resolved and the BKL-in baseline returns to `B1NIX-TEST: done`.

## Status (m28-t4-closure session, 2026-05-30): T4 still does not land

This session found and fixed several real BKL-invariant and scheduler
publication bugs, but it was not enough to land T4.

`kernel/arch/x86/syscall_entry.S` used to save the incoming syscall
number (`RAX`) in one global `.bss` scratch slot before switching to the
task kernel stack and before acquiring the BKL. On SMP, two CPUs entering
syscall at the same time could overwrite each other's saved syscall
number before either CPU reached `bkl_lock`. That was a true T4-relevant
bug because it existed outside the BKL's protection window. The scratch
is now `struct percpu::syscall_scratch_rax` at GS offset `0x60`, pinned
by a static assert in `kernel/sched/scheduler.c`.

Two more BKL-invariant fixes landed after that initial scratch cleanup:

- BSP/AP idle loops now take the BKL before calling `scheduler_yield`
  and drop it before `hlt`. A runnable userspace task can be either a
  fresh `user_jump` entry or a resumed kernel-side syscall/exit
  continuation; the latter must not resume without the BKL.
- The LAPIC timer preemption path now takes the BKL before calling
  `scheduler_yield` when the tick interrupted userspace. Previously the
  timer ISR bypassed the BKL and could mutate scheduler state in
  parallel with AP syscall/idle handoff paths.

Signal/wait publication was also tightened:

- `SIGKILL`/default-terminate delivery now wakes a blocked parent via
  the same CAS + runqueue + IPI helper as normal `exit`, fixing the M12
  `kill`/`waitpid` hang seen during this session.
- Stop signals sent to another task publish `TASK_STOPPED` and the
  WUNTRACED report immediately, so `waitpid(WUNTRACED)` no longer
  depends on the stopped task entering the kernel again.
- Kernel stacks are now 64 KiB; 32 KiB left too little headroom for the
  SMP/T8 nested syscall/signal/scheduler paths hit by M12/M14/M25.

Validation with BKL kept in `syscall_entry.S` is now clean for the
current tree:

| variant | runs | pass | notes |
|---------|------|------|-------|
| BKL in + fixes above | 1 | 1 | full `LD=/opt/homebrew/bin/ld.lld ./tests/smoke.sh x86` passed `271 / 0 / 0`, including SMP-4 `B1NIX-TEST: done` |

T4 was then re-applied on top of the fixes above by disabling the
syscall-entry `bkl_lock` call. It still failed before the SMP leg:

| variant | runs | pass | typical fail shape |
|---------|------|------|--------------------|
| T4 + fixes above | 1 | 0 | single-CPU hang in M25 after `/tmp/hello` printed `M25-HELLO: hello from native tcc`, before `M25-SMOKE: ok run-hello` / suite completion |

Conclusion: the global scratch race and BKL-invariant holes are fixed as
strict improvements, and the BKL-in baseline is green again. T4 still
cannot honestly land: with syscall-entry BKL acquisition disabled, even
the single-CPU smoke can wedge in the native-toolchain path.

## Status (M29-M32 closeout session, 2026-05-30): T4 still does not land

A second attempt was made after the M29-M32 closeout work landed. The
hypothesis was that the SMP-spawn pmm-churn reduction and stack-lifetime
fixes from that closeout might have closed enough of the residual race
for T4 to be stable. They did not.

Empirical characterization on top of branch `m29-m32-closeout`:

| variant | runs | pass | typical fail shape |
|---------|------|------|--------------------|
| baseline (BKL in, no other changes) | 2 | 1 | M25 TCC-launch hang at SMP-4 |
| T4 (BKL out)                         | 4 | 1 | M12 fork PML4-U-bit corruption; M14/M15 stack corruption (GP fault, garbage RIP) |
| T4 + vmm_lock on clone               | 1 | 0 | stack corruption |
| BKL in + vmm_lock on clone           | 3 | 1 | M14 stack corruption + one M25 hang |

Sample sizes are small, but the trend matches the prior session's 60 %
characterization: T4 doesn't reliably regress smoke vs. the BKL-on
baseline (which itself has the documented SMP-4 flake), but it
*amplifies* the residual race enough that single-PR closure is not
honest. **BKL stays in `syscall_entry.S`.**

What landed as a strict improvement in this session:

- `paging_clone_address_space` now holds `vmm_lock` for write across the
  whole user-half walk. Without it, a fork that ran while another path
  was in `vmm_map_page` could observe a torn intermediate-level entry
  and produce a child PML4 with the user bit cleared in PML4[0] /
  PDPT[0] / PD[N] — the child page-faulted on its first ring-3
  instruction fetch (shape observed once in this session, gone after
  the lock). Closes a real fork-vs-mm race regardless of T4 status.

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

## Status (M11-SHELL trace session, 2026-05-30): regression isolated to M25

The previous session's report read "all failures cascade from M11-SHELL,
shell binary crashes/hangs before any output, single-CPU regression."
Re-tracing the smoke log with the experimental fixes applied shows the
real failure is one step earlier: **single-CPU hangs in M25**, after
`/tmp/hello` (the TCC-compiled binary) prints `M25-HELLO: hello from
native tcc` but before it reaches `SYS_EXIT`. The M11-SHELL "fail" is
just the smoke harness reporting every later marker as missing, because
boot never reaches `B1NIX-TEST: done`.

### What was reproduced this session (BKL kept in syscall_entry.S)

5 single-CPU smoke runs on the experimental tree:

| run | result | last marker reached |
|-----|--------|---------------------|
| with diagnostic prints (sched/syscall) | hang | `M25-HELLO: hello from native tcc` |
| after removing diagnostic prints       | hang | `M25-HELLO: hello from native tcc` |
| after reverting `bkl_depth` save/restore in `scheduler_yield` alone | hang single-CPU, **pass SMP-4** | `M25-HELLO: hello from native tcc` (single-CPU) |
| clean `m28-t4-closure` HEAD (none of the experimental fixes) | **270 / 1** | full suite, single-CPU green; pre-existing SMP-4 M25 hang flake |
| `b1nix.skip-m25` with experimentals applied | hang in M29 pthread futex | `syscall: 109 task=pthread` looping |

So: the regression that turns 271/0 into 93/178 is in the experimental
fix stack, not in the M33 shell branch. Reverting just the
`bkl_depth` save/restore block (lines 1159–1170 in the experimental
scheduler.c) is **not enough** — single-CPU still hangs at the same
M25-HELLO point. The race is somewhere else in the stack.

### What's ruled out

- **Not a kernel `.text` layout boundary issue** (the TCC-hang.md
  failure mode). `objdump -h build/x86/kernel.elf` shows `.text` at
  450 558 bytes inside the 512 KiB padded slot — 73 KiB of headroom.
- **Not the diagnostic `console_write` prints alone.** Removing every
  added `console_write` (`exit_current:`, `waitpid: blocking/woke`,
  `FORK-USER-DEBUG`, `paging_*: ...`, `syscall: <n> task=<name>`)
  did not change the hang or the marker the smoke reaches.
- **Not the CS/SS swap fix in `user_frame_is_valid` / `sys_sigreturn`.**
  Both call sites had inverted constants; the new constants match what
  `syscall_entry.S` actually pushes. Reverting that change would just
  re-introduce a different bug.
- **Not `paging_clone_address_space`'s new `vmm_write_acquire`.** It
  is held over the user-half walk only and does not change kernel-side
  page-table mutation timing.

### What stays suspect (for the next session to bisect)

The remaining experimental delta versus `m28-t4-closure` HEAD that
plausibly affects the user-mode exit path is:

1. `scheduler_yield` — explicit `bkl_unlock_for_switch` /
   `bkl_lock_for_switch(new_task->bkl_depth)` around
   `arch_context_switch`. Partial revert (only this block) was tested;
   single-CPU still hangs, so this alone isn't the root cause but it
   probably contributes.
2. `kernel/main.c` BSP idle loop — `bkl_lock()` before
   `scheduler_yield()`, `bkl_unlock()` immediately after, then
   `sti; hlt` if not switched. Differs from the prior commit that
   relied on yield not touching BKL.
3. `kernel/arch/x86/lapic.c` AP idle loop — same pattern as #2.
4. `scheduler_on_timer_tick` — takes `bkl_lock()` around the yield if
   the timer interrupted userspace, releases after.
5. `kernel/arch/x86/paging.c` `paging_create_address_space` — now holds
   `vmm_read_acquire` while cloning kernel-half PML4 entries. Affects
   `execve` timing for every user-image swap (including the
   `/tmp/hello` execve done from m25-smoke's child).
6. `scheduler_kill` SIGSTOP/SIGTSTP path — publishes `TASK_STOPPED` +
   wakes the blocked parent inline. Unrelated to M25 in theory
   (hello.c doesn't use stop signals).

Recommended next-session bisect: take `m28-t4-closure` HEAD, apply the
experimental hunks one at a time (in the order above), rebuild, run
`tests/smoke.sh x86`, and stop at the first hunk that wedges
single-CPU M25.

### What's in tree right now (working state, not committed)

The 14 experimentally modified files from the previous session are
preserved in the working tree (restored from a dropped stash). `git
status` lists them; `git diff` against `m28-t4-closure` HEAD shows the
full experimental delta to bisect. Nothing in this session was
committed — the experimental work is unchanged from where the previous
session left it.

`m28-t4-closure` HEAD = commit `66eb0e1` (docs + `vmm_lock` on clone),
baseline `270 / 1` on a single run (single SMP-4 flake — documented
pre-existing M25 TCC hang under `-smp 4`).
