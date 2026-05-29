# M24b — Big Kernel Lock & userspace on Application Processors

Closes the last M24b item: ordinary userspace processes now run on Application
Processors (APs), not just self-contained kernel workers. Kernel-mode execution
is serialised across cores by a **Big Kernel Lock (BKL)** while userspace runs
lock-free in parallel.

## The Big Kernel Lock

`kernel/sched/bkl.c` — a single recursive lock keyed on `cpu_id`. A CPU holds it
whenever it runs kernel code on behalf of a task; it is released when the CPU
enters ring 3 or goes idle. The owning CPU re-enters recursively (e.g. an
interrupt nested in its own syscall just bumps the depth).

Boundaries:

- **Acquire (userspace → kernel):** the `syscall_entry.S` SYSCALL path and the
  IRQ/exception handlers (`x86_irq_handler` / `x86_exception_handler` wrappers).
- **Release (kernel → userspace):** `syscall_entry.S` sysret path, `user_jump.S`
  (first entry into ring 3), the fork-child trampoline, and the idle loops.

`syscall_entry.S` calls `bkl_lock` only after the full register frame is built,
then reloads the syscall arguments from that saved frame (bkl_lock clobbers the
caller-saved registers that hold them).

### The depth-1 invariant

Every `arch_context_switch` happens with the current CPU holding the BKL at depth
exactly 1, and the lock is **handed off** across the switch (same CPU, owner
unchanged) — so the resumed task inherits depth 1. This is what lets the
cooperative scheduler stay completely unchanged:

- Internal kernel callers of syscalls (kernel threads) never call `bkl_lock` —
  they already hold depth 1, inherited from the switch that scheduled them.
- A task that exits or forks hands the depth-1 lock to the next/child task across
  the switch, so the "missing" unlock is correct.

## Scheduling userspace on APs

- Non-stealable READY tasks live on a shared **global runqueue** (`g_global_rq`
  in `kernel/sched/scheduler.c`). Stealable CPU-bound workers keep their per-CPU
  runqueues for the existing M24b work-stealing path.
- Each AP runs a dedicated **idle task** (`scheduler_setup_ap_idle`, stored in
  `percpu.idle_task`) and the full cooperative scheduler — `ap_main` phase 2,
  entered once `g_ap_userspace_enabled` is set by `main.c` after the
  work-stealing self-test. Phase 1 (work-stealing only) is preserved verbatim.
- `pick_next_task` parks an AP back to its idle task when nothing else is
  runnable, so the AP idle loop can drop the BKL and back off.

### Only userspace ELF tasks migrate to APs

`struct task::ap_runnable` is set (via `kthread_create_user`, from `user_spawn`)
only for real ELF userspace processes — they enter ring 3 and so release the
BKL, allowing genuine parallelism. Kernel threads (daemons, builtins, the
boot/idle task) stay BSP-pinned: they run entirely in ring 0 and would hold the
BKL across their cooperative yields, monopolising it.

Relatedly, `net_task` was a perpetually-READY busy-yield daemon; left as-is it
hung the whole SMP system by hogging the BKL. It now `scheduler_sleep_ticks(1)`
between polls, so it is BLOCKED between ~100 Hz polls instead of always READY.

## Per-CPU arch state

APs arrive on the trampoline's minimal GDT with no IDT, no TSS and unset
SYSCALL/SSE MSRs, so they cannot take ring-3 traps. `x86_ap_arch_init`
(`kernel/arch/x86/arch.c`, called from `ap_main`) fixes that per CPU:

- Loads the kernel GDT and reloads the data segments + CS — but never `%gs`
  (reloading a GS selector resets the per-CPU GS base; b1nix uses no SWAPGS).
- Loads the shared kernel IDT (`x86_idt_load`).
- Builds and `ltr`s this CPU's **TSS**. `TSS.rsp0` is repointed at the running
  task's kernel stack on every context switch (`arch_set_kernel_stack` now
  indexes per `cpu_id`), so ring-3 interrupts/page-faults land on the right
  stack. The GDT (`boot.S`) reserves `MAX_CPUS` TSS descriptors (CPU *k* uses
  selector `0x28 + k*16`).
- Programs the SYSCALL MSRs (EFER.SCE/STAR/LSTAR/FMASK), SSE (CR0/CR4 for
  fxsave/fxrstor), and CR0.WP.

## Verification

Proof program `userspace/bin/m24b_smoke.c`: a CPU-bound process that samples its
core via `getcpu()` (new `SYS_GETCPU` = 105). The kernel records which cores ran
a ring-3 syscall from an ELF task in `sched_user_cpu_mask()`; `init_main` spawns
six instances concurrently and emits the marker.

- **`-smp 4`:** six userspace processes ran across all four cores —
  `M24B-BKL: user-cpu-mask=15`, `M24B-BKL: ok userspace-on-ap`. Work-stealing
  still passes (`M24B-SMP: ok work-stealing`). Full suite **180 ok / 0 fail**,
  `B1NIX-TEST: done`, no faults. Runner: `smoke_run/qrun-smp.sh 120 4`.
- **Single CPU:** unchanged — **178 ok / 0 fail**, `M24B-BKL: skip single-cpu`.
  Runner: `smoke_run/qrun.sh`.

## Gotchas for future work

- The Makefile has no auto header-deps (`-MMD`); changes to `sched.h` / `lapic.h`
  need a `make clean`.
- Any new perpetually-runnable kernel daemon must block/sleep when idle, never
  busy-yield — under the BKL a busy ring-0 yielder starves every other core.
- The BKL is coarse: a CPU-bound *kernel* thread still serialises the system.
  Only ring-3 work runs truly in parallel. Finer-grained locking would be the
  next step beyond M24b.
