# Memory, scheduling and SMP

Milestones M2–M3, M24b, M28–M29, M41, M71, M86, M88, M115–M117 and M122.

## Memory

The physical memory manager tracks frames with bitmaps and a buddy allocator.
Its early metadata starts at 1 MiB, because the SMP trampoline is copied to
0x8000 and once overwrote those bitmaps (M122). The kernel lives in the higher
half with a direct map of physical memory. The 64 GiB ceiling is gone, and
16 GiB has been verified (M41).

Each process has its own page tables. Fork is copy-on-write, anonymous and file
mappings are demand-paged, and pages can be swapped out and back in (M2). The
kernel heap has canaries that `kheap_validate()` checks. PROT_NONE guard pages
are honoured (M88), and PIE executables are loaded at randomised addresses when
`b1nix.aslr` is set (M71).

Shared mappings of one file page have a single owner. When two threads fault
the same page at once, the loser adopts the page the winner cached instead of
mapping a frame of its own; otherwise its writes were invisible to everyone
else (M122). Pages of imported filesystems are reclaimed by kswapd once they
are clean and unreferenced. Heap growth, page tables, `fork` and large
allocations fail with ENOMEM instead of panicking.

Kernel and syscall stacks are 256 KiB with guard pages, and their peak usage is
asserted in the tests (M115). A page-table entry has exactly one meaning:
shared mappings no longer borrow the GLOBAL bit, and application processors run
without `CR4.PGE`. That combination used to produce `SIGILL` on SMP (M116).

## Scheduling

Tasks are kernel threads or user processes, preempted by a timer that runs at
1 kHz. A process has its process group and session, zombies wait for their
parent, and threads are ordinary tasks that share an address space: `clone`,
futexes, TLS and pthreads (M3, M29). Each thread has CPU clocks that roll up
into its process, and `tkill`, `tgkill`, `exit` and `exit_group` behave as on
Linux (M86).

The scheduler uses stride scheduling weighted by nice on every CPU, and
application processors preempt ring-3 ticks (M117). Tasks pinned to different
CPUs cannot share one virtual time, so a candidate's pass is held within 200
nice-0 turns of the lowest pass its CPU can run. Without that, a task woken
among pinned CPU-bound threads never ran (M122).

## SMP

Application processors boot through LAPIC and INIT/SIPI on x86_64, and through
PSCI or a spin table on AArch64. Each CPU has its own runqueue, and idle CPUs
steal work (M24b). There is no big kernel lock: shared state is protected by
locks under a lockdep checker, TLB shootdowns wait for every CPU to
acknowledge, and reschedule requests travel as IPIs (M28).

A CPU claims a READY task by compare-and-swap on the task's kernel-stack lease.
The lease is published only once the outgoing context has been saved, so at
most one CPU can load a task's context at a time. A task that has died delivers
no more signals, so `waitpid` and the reaper can no longer both tear it down.
That double reap freed physical frames twice and is the likely source of an old
`#GP` on an overwritten kernel stack (M122).

## How corruption is proven gone

`tools/run/soak/fsverify.sh` runs writers on every CPU against fresh ext4 and
btrfs disks. The verdict comes from tools that are not this kernel:
`e2fsck -fn`, `btrfs check --check-data-csum`, and extraction on the host
compared file by file with the guest's manifest. At six CPUs the closing
campaign was clean 14 times out of 14, alongside 10/10 `soak all` and 26/26
`gfx` runs.

Some events were seen only with the host overcommitted about three times over
and never reproduced otherwise: a spinlock lockup on the TLB-shootdown lock
(its report now names the round in flight and every CPU that has not answered)
and a `BCACHE-TEST` writer stuck on a virtio-blk completion. A guest with
384 MiB still degrades under fsverify at scale 200 instead of finishing; that
belongs to memory accounting (M127).
