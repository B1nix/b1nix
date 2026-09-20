# Memory, scheduling and SMP

Milestones M2–M3, M24b, M28–M29, M41, M71, M86, M88, M115–M117, M122 and
M127.

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

## Resource control

The unified cgroup v2 hierarchy is a directory tree in the VFS's own node
graph: a directory is a cgroup, `mkdir` makes one and `rmdir` destroys one, and
membership lives in a side table keyed by task id because `struct task` must not
grow. Four controllers are advertised, and a controller is advertised only when
it is enforced — accepting a limit that nothing applies is a lie told to the
process that set it, and systemd's entire resource model is these files (M127).

**memory.** `memory.current` is the resident memory of the member address
spaces, counted once per address space so threads are not multiplied: the same
page-table walk `/proc/<pid>/status` answers `VmRSS` with, taken on demand. It
does not include kernel memory or unmapped page cache, so `memory.stat` lists
only the counters that are really kept rather than a page of zeroes.
`memory.max` is enforced from the page-fault path. Each fault charged to a
limited cgroup advances a running estimate; when the estimate crosses the limit
the cgroup is measured exactly, and only that exact figure decides anything —
the kernel never kills on a drifting counter. Over the limit means reclaim,
measure again, and if it is still over, kill the worst task inside the cgroup.
The fault itself always succeeds: failing it turned a clean `SIGKILL` into an
unhandled `SIGSEGV` against a page that had in fact just been installed.

**The OOM killer** ranks by Linux's badness — resident pages plus
`oom_score_adj` as a thousandth of the machine, with −1000 meaning never — and
the machine-wide killer and the per-cgroup one share the walk, so they cannot
disagree. `oom_score_adj` lives in the scheduler's per-task table, applies to a
whole thread group and survives `fork`; it used to be a 128-entry table inside
procfs that nothing read. `memory.events` counts what happened: `max` each time
a limit was hit, `oom` each episode, `oom_kill` each process killed.

**cpu.** `cpu.weight` rides the stride scheduler that already implements nice.
A cgroup's effective weight is the product of the weights down to it, and each
of its tasks is given a stride of `base × tasks_in_cgroup × 100 / weight`, so
the group's share is its weight however many tasks split it. The anti-starvation
clamp that holds a candidate within 200 nice-0 turns of the lowest pass had to
be widened a hundredfold for weighted tasks: a lead earned by weight is the
weighting, and clamping it put a ceiling of about three to one on `cpu.weight`
however far apart the weights were set. `cpu.max` is a quota per period; the
tick charges each task's CPU time to its cgroups, and a cgroup that has spent
its quota has its tasks passed over until the period rolls over — except a task
inside a system call, which may hold a lock the rest of the machine is waiting
behind.

**io** counts the bytes and device commands that really reach a device on a
member's behalf, charged where the block layer serialises them, per device as
`io.stat` prints them. `io.max` is a ceiling on those rates, enforced by making
the next command wait.

What the memory controller does not do: reclaim inside a cgroup. Page eviction
here is machine-wide and has no notion of whose pages it writes out, and
freeing another cgroup's memory to keep this one inside its limit is not what
`memory.max` means — so a cgroup over its limit is measured and then killed,
with no reclaim in between, and `memory.swap.current` reads 0 because nothing
attributes a swapped page back to its owner. `memory.high` is the throttle that
is left: one tick of sleep per crossing, rate-limited. Compressed swap (zram,
zswap) is not started, and belongs with the reclaim that would make these two
real.

**PSI** is measured machine-wide and published under `/proc/pressure/{cpu,
memory,io}` in the format Linux prints. A task is stalled on a resource for
exactly as long as it is inside a stall region, and the regions are where the
waiting really happens: the block layer's device call for io, page reclaim and
swap-in for memory. CPU pressure is counted from the runnable-but-not-running
tasks the tick sees, because a task waiting for a CPU cannot bracket its own
wait. `some` is charged while at least one task is stalled; `full` while one is
and no CPU is running anything else, which reads low on a machine spinning
inside reclaim. There are no per-cgroup pressure files: they would have to be
copies of the global one.

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


## Compressed swap, and reclaim that belongs to a cgroup

Three things arrived together, because none of them is much use alone: a place
to put pages that is not a disk, a reclaim that knows whose pages it is taking,
and an account of where they went.

**zram** (`kernel/dev/zram.c`) is a block device whose blocks live in RAM,
LZ4-compressed. `swapon /dev/zram0` is the whole of how a machine with no disk
gets swap. It is configured the way Linux's is, because that is what userspace
drives: a size is written to `/sys/block/zram0/disksize`, the device appears
with that many sectors, `mm_stat` reports the eight numbers `zramctl` parses,
and `reset` releases it. A page of zeroes is stored as nothing at all, which is
most of a fresh swap area; an incompressible page is stored raw, because the
compressed form of one is larger than the page.

**zswap** (`kernel/mm/swap.c`) is the other shape of the same idea and it
predates zram here: a bounded RAM pool in FRONT of a real device, which a page
falls through when it does not compress or the pool is full. A machine can run
either or both.

Both use the kernel's own LZ4 (`kernel/lib/lz4.c`), and both obey one rule that
is easy to state and was expensive to learn: **nothing allocates while the
compressor's lock is held.** The kernel heap's lock is taken first, growing the
heap maps pages, mapping pages can reclaim, and reclaim writes a page to swap —
so a `kmalloc` under a swap lock closes a cycle with a `kmalloc` that is already
inside the heap lock. The same reasoning covers freeing: the address-space
teardown walks the page tables with the paging lock held and frees every
swapped page's slot as it goes, so a compressed blob freed there goes onto a
pending list (threaded through its own first eight bytes) and is handed back
from a context that holds nothing.

**Reclaim inside a cgroup** is what makes `memory.max` a limit rather than a
death sentence. A cgroup over its limit now has its own cold pages written out
before the OOM killer is asked for a victim, and only its own: the eviction
ring is indexed by task (`kernel/mm/eviction.c`), so the scan costs what the
cgroup has rather than what the machine has. Two details are load-bearing:

* The page the faulting instruction is about to touch is protected for the
  duration of the charge. Without that, the fault installs a page, the charge
  reclaims it (its accessed bit is clear — nothing has touched it yet), the
  instruction faults again, and the machine makes no progress while saying
  nothing.
* Eviction does not re-enter itself on a CPU. Writing a page out allocates, an
  allocation can reclaim, and that wheel has no bottom.

**The account.** A swapped page carries the id of the cgroup that owned it, two
bytes beside its slot, the same way Linux's `swap_cgroup` array does — the task
that faulted it may be asleep, moved or dead before the page comes back.
`memory.swap.current` is that count; `memory.swap.max` is checked before a page
is written out, and a refusal is counted in `memory.swap.events`. A cgroup
removed while its pages are still out hands its id to its parent, so a charge
can neither vanish nor be attributed to something that no longer exists.
