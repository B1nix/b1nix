# Memory, scheduling and SMP

Milestones M2–M3, M24b, M28–M29, M41, M71, M86, M88, M115–M117, M122, M127,
M128 and M129.

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

## Nodes, and memory that belongs to one (M128)

A machine can have more than one memory controller, and a page is faster for
the CPUs beside its controller than for the others. ACPI describes that in two
tables: SRAT says which physical ranges and which CPUs belong to which
proximity domain, SLIT how far apart the domains are. `kernel/mm/numa.c`
parses both and renumbers the domains into the dense node ids userspace
expects to see under `/sys/devices/system/node`.

The query on the hot path is "which node is this frame on", asked by the
allocator for every block it links or unlinks. It is a byte per 64 MiB chunk
of RAM — one shift and one load, 16 KiB of table for a terabyte — and on a
machine with one node the allocator skips it on a compare. That matters
because the buddy allocator's free lists are now per node
(`free_area[node][order]`): a block is linked onto the list of the node it
physically belongs to, an allocation starts at the node the running CPU sits
on and falls back through the others in SLIT's distance order, and a strict
allocation (see the policy below) refuses the fallback rather than quietly
answering from the wrong place. The per-node free counts are maintained where
a bitmap bit changes, because the bitmap is what "free" means here; they are
recomputed once when the topology becomes known, since `numa_init` needs ACPI
and the direct map and therefore runs long after `pmm_init`.

`kernel/mm/mempolicy.c` is what decides which node an anonymous page comes
from. A mapping's own policy (mbind) wins over the task's (set_mempolicy),
which is the order Linux resolves them in and the order `numactl` depends on.
MPOL_BIND is enforced strictly; MPOL_INTERLEAVE picks the node from the page's
own offset, so a page that is dropped and faulted again lands where it was;
MPOL_MF_MOVE migrates pages that are already on the wrong node, copying them
and repointing the leaf, and refuses to move a page more than one mapping
holds — the other holders would keep the old one and the copy would silently
stop being shared. `/sys/devices/system/node/nodeN/{cpulist,cpumap,meminfo,distance}`
is what `numactl --hardware` reads.

The proof is a QEMU guest with two nodes, one CPU each and a stated distance
between them (the posix lane), and the checks measure placement twice: by the
kernel's own answer for each page (`get_mempolicy(MPOL_F_NODE|MPOL_F_ADDR)`)
and by the bound node's free memory falling while the other node's does not.
Either one alone can be satisfied by a kernel that allocates anywhere.

## More memory than the direct map used to expect (M128)

A 72 GiB guest boots and runs the whole suite. The interesting part was not
the allocator — its metadata for that much RAM is tens of megabytes — but what
the memory map does to everything else: with that much RAM the firmware puts
the 64-bit PCI window **above** it, and this machine's NVMe controller answers
at 0x7000_00000000. Every driver that reached a BAR through
`vmm_direct_map_base() + bar` had been relying on the window sitting under
4 GiB, and each of them faulted in ring 0 on the first register read.
`pci_map_mmio()` hands back the direct-map address where it reaches and builds
a real mapping where it does not; three drivers use it.

Five-level paging is opt-in (`b1nix.la57`) and shaped so that nothing above
the page-table code has to know. boot.S decides — the CPU must report LA57 and
the command line must ask — and builds a PML5 whose entries 0 and 511 both
point at the PML4 four-level paging would have used, so the low identity
window and the kernel's high windows resolve through exactly the same entries
as before. What changes is which table CR3 names, which is why
`paging_cr3_to_pml4()` exists: the shootdown code and the fork reload compare
a live CR3 against an address space, and under LA57 those are different
numbers. Each address space's PML5 is built where the space is created and
never in the switch path — allocating there can yield under a lock, which is a
panic — and is found again through a hash of the PML4 frame.

Both are lanes rather than defaults: `SMOKE_BIGMEM=1` boots the large guest
(its memory is a sparse file, since nothing touches most of it), and
`SMOKE_LA57=1` runs the whole boot suite under TCG with `-cpu max,la57=on`,
because no machine here has the feature. The LA57 lane found something that
had nothing to do with paging: application processors were coming out of reset
with their caches disabled, because nothing cleared CR0.CD/NW in the
trampoline.

## Idling, and the tick that stopped being fixed (M129)

Every place the kernel parks a CPU goes through one `cpuidle_enter()`: MWAIT
into the deepest C-state CPUID's leaf 5 reports, plain HLT or WFI where there
is none, with the entries and the time counted per CPU and published under
`/sys/devices/system/cpu/cpuN/cpuidle/stateN`. Having one path is what makes
the rest measurable — and `/proc/interrupts` grew the `LOC` row on both
architectures so the measurement can be made from userspace.

The tick is no longer fixed. An idle CPU programs its one-shot for the next
deadline anybody actually has, capped at ten ticks; a CPU with work keeps its
kilohertz, because a stretched tick there is a ten-millisecond scheduling
quantum by accident rather than tickless. The half that is easy to forget is
the other one: a CPU that has already parked does not re-read the deadline
list, so a deadline armed against it afterwards has to reach it as an
interrupt (`arch_kick_idle_before`, driven from `sched_note_deadline` and
kicking only the CPUs whose armed tick is later than the new one). Without
that kick every wait is served up to a whole idle interval late, every time
round its caller's loop, and the machine reads as wedged rather than slow.

Two pollers had to go first, and they are worth naming because neither looked
like a power problem: `lkpi_sleep_ms` slept one tick at a time in a loop, and
parking an imported kernel thread cost ten one-tick sleeps. With them fixed
and the tick capped, an idle second on x86_64 costs 620 timer interrupts
instead of 1998. On aarch64 the same machinery saves little — that port ticks
at 100 Hz, so nearly every wakeup already asks for about a tick — and the test
says so rather than claiming the win. `/proc/b1nix-tick` reports the tick rate
and the cap, so a check knows what to expect instead of reading the command
line and guessing.
## Suspend, and the freezer under it (M129)

`/sys/power/state` lists one state and accepts one: `freeze`, which Linux
calls suspend-to-idle. Userspace is stopped, every CPU parks in the deepest
idle state `cpuidle_enter` found for it, and the machine comes back when a
wake source raises an interrupt. Nothing is powered off and no firmware is
called, which is why it works on hardware whose ACPI sleep path this kernel
cannot drive. `mem` (ACPI S3) is absent rather than present and broken: it
needs SLP_TYP from the DSDT written into the FADT's PM1 control ports and a
resume through a real-mode trampoline with every device re-initialised, and a
state that sleeps and cannot be woken is worse than a state that is not there.

**The freezer** (`sched_freeze_userspace`) is not SIGSTOP. A job-control stop
is reported to the parent, wakes `waitpid(WUNTRACED)`, sets a stop signal a
debugger reads and is undone by any `SIGCONT`; none of that is true of a
freeze, which userspace is not meant to see at all. It has its own bit, one
per task slot, and the only thing that bit does is make the picker pass the
task over. Each task's own state is left as it was found — one blocked in
`read()` is still BLOCKED — so a thaw is a store of zero and a reschedule IPI.

Two properties make it safe. Every candidate is marked at once, running or
not, and stays outstanding until it is really off every CPU: the mark does
not stop a task mid-instruction, it only takes it out of the picker, so the
task leaves its CPU at its next scheduling point — in ring 3 by preemption,
in the kernel by blocking or returning — and cannot be chosen again. Marking
only the tasks that happened to be off a CPU could not converge against a
task that spins: it is the only runnable thing on its CPU, the picker keeps
choosing it, it is never caught off-CPU, and the freeze failed on a machine
that was working perfectly. The safety argument survives because this kernel
never schedules away from a task holding a spinlock, so a task the picker has
dropped is not holding one when it stops. If one will not leave its CPU
before the deadline the freeze fails, which means thawing everything already
marked: a half-frozen machine is the one outcome worse than not suspending.
Kernel threads are deliberately never frozen — they are what services the
wake interrupt.

**The wake source.** A suspend with nothing able to end it is a hang, so it is
refused (`ENODEV`) unless a registered source says it is armed at that moment.
The RTC alarm is the one this kernel has, which is what makes `rtcwake`
behaviour possible: arm the alarm, write `freeze`, be woken by it. On x86_64
the CMOS alarm registers had been writable since M107 with nothing listening —
IRQ 8 was never routed — and two details decide whether claiming it works.
The line is ISA, edge-triggered and active high, so it is routed with
`irq_unmask_isa` rather than the PCI INTx defaults `irq_unmask` assumes;
programmed as active-low level it reads as permanently asserted and the
machine takes that interrupt for ever. And the MC146818 raises exactly one
interrupt until status register C is read, so the handler reads it — under the
same lock the ioctls take, because the CMOS is an index/data port pair and a
handler that interrupts an ioctl between the two reads the wrong register. On
aarch64 the PL031's match register already had an interrupt; what it did not
have was the right value in it. `RTC_RD_TIME` answers from the wall clock
while the match register is compared against the raw counter, so the alarm is
now resolved as a delta in one clock and applied in the other — built from the
counter and compared with a time of day read from the wall clock, it was a
whole day out the moment anything set the clock.

There is a ceiling on the sleep as well. An armed source can still fail to
fire, and the difference between a bug and a dead machine is whether anybody
is left to report it: after ten seconds the machine resumes anyway and says
that is what happened. That ceiling is paid on every `echo freeze` that arms
no alarm, because a machine with a console counts its input as an armed
source and nobody is typing at a test guest's serial line.
`/proc/interrupts` grew an `RTC` row, so whether the alarm interrupt was
really taken is a thing userspace can read.

The proof is `m129_suspend_smoke`, graded under `M129-SUSPEND:`. A child
process does nothing but spin, comparing consecutive `CLOCK_MONOTONIC`
readings and keeping the longest gap it ever sees; the parent arms the alarm
three seconds out, writes `freeze`, and when the write returns checks that
about that much wall time passed, that the child's own timeline has a hole of
the same length, that the RTC row moved, and that a file round-trip and a fork
still work. The child measures the hole itself because the parent cannot:
the thaw happens inside the write, and on a machine with a spare CPU the child
is running again before the write returns.

The other half of the proof is the distribution's own tools. On the Debian
lane (`tests/debian-smoke.sh`, stage 17) util-linux's `rtcwake -m freeze -s 3`
arms `/dev/rtc0` through its ioctls and writes `/sys/power/state` from a glibc
binary: the machine sleeps about three seconds, the alarm wakes it, and the
guest writes a file and reads `/proc` afterwards — graded as
`suspend-rtcwake` and `suspend-alive`. Nothing on that path is a b1nix
interface, which is the point of running it.

## Transparent huge pages for anonymous memory (M128)

A 2 MiB page in the anonymous fault path is the easy half of this. The hard
half is that every page-table walker in the kernel was written on the
assumption that a leaf is a 4 KiB page, and a walker that meets a 2 MiB entry
and quietly does the wrong thing is silent memory corruption rather than a slow
machine. The rule that makes the feature tractable is therefore a restriction:

> **The fault path is the only thing that creates a block. Everything else
> either breaks one up first or handles all 512 pages of it.**

Breaking one up — `paging_thp_split_range` — allocates, so it is always called
with no page-table lock held: at the top of `paging_unmap_range_from_space`,
`vmm_unmap_range_collect`, `paging_mprotect_range` and `paging_move_range`,
and over the whole address space before `fork` clones it. The unmap paths ask
only for the blocks their range cuts in half; one a range covers whole is
released as a block, which is what makes address-space teardown — the busiest
caller — allocate nothing. The four walkers that handle a block themselves are
the unmap of a whole one, `free_table` at teardown (which used to SKIP a huge
entry at a non-leaf level, and would have leaked the whole 512-frame block),
`paging_user_resident`, so a process using blocks does not read as having given
its memory back, and `paging_user_writable`, so a userfaultfd write-protect
check does not refuse a write to a page that is perfectly writable.

**Nothing in this path ever frees a page table.** That is the other half of why
it is safe. `vmm_set_lazy` and the `/proc` walkers descend the tables without
the page-table lock, so a frame retired from under them could be read by a
walker still holding a pointer into it. So a block is only installed where the
directory entry is ABSENT — and to make that reachable, an anonymous `mmap`
leaves its whole 2 MiB-aligned blocks unmarked when the feature is on, instead
of writing a lazy marker into every one of their 512 leaves. The markers were
only ever an optimisation: a fault with no leaf inside an anonymous mapping is
zero-filled by the same handler, reading the protection off the mapping rather
than off the marker.

The consequence, and it is a real one: an address range that has already been
faulted at 4 KiB granularity keeps its page table when the mapping goes, and
the next mapping at that address is served 4 KiB at a time. A long-lived arena
mapped once gets blocks; a program that churns mappings at recycled addresses
gets them the first time round.

A block is a buddy order-9 allocation, so the 2 MiB alignment the hardware
requires comes for free, and each of its 512 frames carries its own refcount —
which is what lets a split hand the block to the ordinary per-page paths and
the ordinary per-page free. The allocation is `pmm_alloc_block_node`, which
takes the node the mapping's memory policy names (strictly, under MPOL_BIND —
a block that ignored the policy would put 512 pages on the wrong node in one
go) and which never runs reclaim: an opportunistic 2 MiB request must not push
a machine into eviction on behalf of a mapping that is perfectly correct as
512 pages. For the same reason a block is refused while free memory is under
16 MiB, since taking one commits all 512 pages for a mapping that may touch
one.

**A block is not reclaimable while it is a block.** The eviction ring holds
4 KiB pages and there is nothing in it to take one page of a block, so a
split registers the 512 leaves and puts them back within reclaim's reach —
but nothing makes reclaim split a block in the first place. Having it do so
was tried and reverted: reclaim is reached from inside the allocator,
splitting allocates and takes the page-table lock, and the two orders do not
agree — the machine wedged with the console silent. The consequence is the
reason `always` is not a mode to run a machine in yet: a cgroup at its
`memory.max` whose memory is all blocks has nothing to reclaim and is killed
instead. `madvise` is the mode the suite runs green, and there the program has
asked for the trade.

`fork` breaks the parent's blocks apart rather than sharing them. A huge
copy-on-write entry would need a second representation and a fault path to
break it, and the per-page copy-on-write that already exists is correct; the
child re-forms its own blocks on its own faults.

**Off by default**, on x86_64 only. `b1nix.thp` selects madvise mode,
`b1nix.thp=always` every eligible mapping, and
`/sys/kernel/mm/transparent_hugepage/enabled` reads and writes the same state
in Linux's `always [madvise] never` format beside `hpage_pmd_size` and a
`stats` file counting blocks installed, refused and split. Per mapping it is
`madvise(MADV_HUGEPAGE)` / `MADV_NOHUGEPAGE`, which are no longer no-ops:
turning the advice off takes existing blocks back apart. aarch64 has block
descriptors that would serve, but none of its walkers has been audited, so the
knob will not move there and nothing installs one.

What a mapping actually holds is readable rather than assumed:
`/proc/<pid>/smaps` is new, and its `AnonHugePages` is computed by walking the
directory entries themselves. That is what `m128_thp_smoke` grades — that a
4 MiB `MADV_HUGEPAGE` mapping reports it, that the bytes survive a fork and a
copy-on-write write on both sides, an `mprotect` of half the range, a `munmap`
of half a block and a `MADV_NOHUGEPAGE`, and that eight rounds of mapping,
filling and releasing 8 MiB leave the machine's free memory where it started.
