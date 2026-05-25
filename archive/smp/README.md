# SMP / Runqueue Archive

This directory contains SMP support code that is NOT part of the default build.

## Contents

- `runqueue.c` — Per-CPU runqueue operations (rq_enqueue, rq_dequeue, 
  sched_rq_enqueue_current, sched_steal_task)
- `runqueue.h` — Header with struct runqueue and function declarations

## Why Archived

Adding runqueue.c to the kernel build increases .text by ~220 bytes, which
causes the TCC compilation test (M25) to hang. Root cause: TCC (Tiny C Compiler)
is sensitive to the kernel ELF .text layout — even benign code of the same size
in a separate .o file triggers the hang.

## How to Re-enable

1. Copy `runqueue.h` → `kernel/include/b1nix/runqueue.h`
2. Copy `runqueue.c` → `kernel/sched/runqueue.c`
3. Add `kernel/sched/runqueue.c \` to `KERNEL_SOURCES` in Makefile
4. Optionally use `rq_dequeue()`/`sched_steal_task()` in `ap_main()` (lapic.c)
   instead of direct struct access

Note: M25 TCC test will not pass when runqueue is linked.

## SMP Infrastructure (always built)

The following SMP code is always active and tested (210/210):

- `kernel/arch/x86/lapic.c` — LAPIC driver, per-CPU init, smp_boot_aps(), ap_main
- `kernel/arch/x86/ap_trampoline.S` — AP boot trampoline (flat binary at 0x8000)
- `kernel/include/b1nix/lapic.h` — LAPIC registers, percpu struct, runqueue struct
- `kernel/include/b1nix/spinlock.h` — SMP spinlock primitives
- `kernel/sched/scheduler.c` — `struct task` has `next_run` field (SMP runqueue linkage)
