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

`/sys/power/state` lists what the machine really has. `freeze` — Linux's
suspend-to-idle — everywhere: userspace is stopped, every CPU parks in the
deepest idle state `cpuidle_enter` found for it, and the machine comes back when
a wake source raises an interrupt. Nothing is powered off and no firmware is
called, which is why it works on hardware whose ACPI sleep path this kernel
cannot drive. And `mem` — ACPI S3 — on a machine whose firmware declares
`\_S3` and whose FADT names the registers to enter it through, which on this
architecture means every PC: see below.

**A wake source has a class.** A byte on a serial console ends an idle suspend
and cannot end an S3: once the processor is powered off there is nothing left to
take the interrupt, and there is no software ceiling either — the ten-second
backstop that saves a `freeze` from a wake that never comes does not exist when
the kernel is not running. So a source says whether it can wake a powered-off
machine (`SUSPEND_WAKE_DEEP`; the RTC alarm can, the console cannot), and an S3
with none of them armed is refused. A sleep nothing can end is not a suspend.

## ACPI S3, and what it takes to come back (M129)

S3 is the firmware's sleep: the OS writes the address of real-mode code into the
FACS waking vector, writes the sleep type the firmware declared for `\_S3` into
PM1_CNT, and the platform removes power from everything but memory. On wake the
firmware hands back a processor at its power-on state — real mode, no paging, no
long mode, caches disabled, one CPU — and jumps to that address. Everything the
kernel was is in memory and nothing of it is in the processor.

The path, in the order it runs:

1. **The wake source is checked and armed.** A deep source has to be armed (see
   above), and the RTC's alarm is armed as a WAKE event as well as an interrupt:
   PM1_EN's RTC bit is what makes the chipset bring the machine back, and the
   alarm alone — which is enough for an idle suspend — fires into a processor
   that is not there. ACPI mode itself is enabled first (SCI_EN through the
   FADT's SMI command port) on a platform the firmware left in legacy mode.
2. **The other CPUs are parked** (`sched_park_secondary_cpus`). Their state does
   not survive, and one INIT'd while it holds a lock takes the machine with it,
   so they park from their idle loop — the one place an AP holds nothing — and an
   S3 whose secondary CPU will not get there in time is refused.
3. **The processor is saved** (`kernel/arch/x86_64/s3.c`): the callee-saved
   registers and the stack pointer are pushed by `x86_s3_sleep_and_wake`, and the
   descriptor tables, the control registers, the segment and syscall MSRs, the
   PAT and XCR0 are recorded beside them.
4. **The blob is placed and the vector armed.** `s3_wakeup.S` is a flat binary
   linked at 0x9000 (0x8000 belongs to the AP trampoline; everything below 1 MiB
   is outside the frame allocator), patched with the CR3 to load, the stack to
   resume on, the GDTR to reload and the address to jump to. It prints one
   character per stage to COM1 — `W`, `3`, `6` — which is the only debugging
   surface a resume has before it has a kernel again, and it is what found two of
   the defects below.
5. **The sleep**: `wbinvd`, then SLP_TYP|SLP_EN into PM1a_CNT (and PM1b where the
   block is split). A platform that returns from that write has refused the
   state, and that is reported as a refusal rather than as a very short sleep.
6. **The wake**: real mode → protected mode → CR3, CR4 (PAE, LA57 where the
   kernel runs five-level) → EFER (LME, NXE) → CR0 with paging on and caches
   enabled → 64-bit code → the kernel's GDT and the saved stack → C.
7. **The processor is rebuilt** (`x86_s3_restore_cpu`), then the interrupt
   hardware, then the clocks, then the secondary CPUs, then the devices, then
   userspace is thawed.

Five things had to be right, and each was wrong first:

**The clocks.** The time-stamp counter comes back at zero. Everything built on it
— this kernel's monotonic clock and the one userspace reads out of the vDSO —
then computes a subtraction that underflowed, which reads as a clock 160 years
ahead; the scheduler's stall watchdog shot the machine for it. The fix is to move
the counter's ANCHOR rather than the consumers: `arch_tsc_reanchor` makes the
counter read what it read on the way down plus however long the machine was away,
measured by the hardware clock, which is the only clock that runs through an S3.
Anchoring rather than offsetting matters because userspace's CLOCK_MONOTONIC *is*
the counter — a base fixed in the kernel alone would leave every program
measuring an interval that ended before it began.

**The TSS descriptor is still busy.** The processor sets that bit in memory when
a TSS is loaded, and memory survived; `ltr` on a busy descriptor is a
general-protection fault, which with no IDT yet is a triple fault and a machine
that reboots instead of resuming. It is cleared before the load.

**The code selector.** The trampoline arrives on its own GDT's selector and then
loads the kernel's, where that selector is something else. Nothing faults until
the first operation that reloads CS — an interrupt return, a syscall — and then
it faults with the selector in the error code. CS is reloaded with a far return
the moment the kernel's GDT is in place, exactly as an AP does.

**The PCI configuration space is reset.** BARs unassigned, bus mastering off,
memory and I/O decoding disabled. A driver that resumes on top of that writes
into a device that is not listening and reports success, which is the worst shape
a resume can take. So the header is snapshotted during the PCI scan and restored
FIRST, before any driver's resume runs (`pci-config` in the resume order).

**`lapic_init()` is not a resume.** It also re-measures the processor's clock and
re-anchors the counter, throwing away the anchor the suspend had just computed:
the kernel's clock then read a constant, and a hundred-millisecond wait for the
other processor spun for two minutes. `lapic_resume()` does the three things a
resume needs — the enable bit in the APIC base MSR, the per-CPU LAPIC state, the
periodic timer at the rate already in force — and nothing else. For the same
class of reason the secondary CPU is started with a SIPI first and an INIT only
as a fallback: the wake already left it waiting for a start-up message, and the
INIT that was sent anyway was never accepted while the delivery-status spin held
the machine for it.

**The devices come back through a registry.** A driver with device state to
rebuild registers a resume callback (`suspend_register_device`), and the resume
calls them in registration order with interrupts on and the scheduler running:
the PCI header first, then virtio-blk, AHCI, NVMe, virtio-9p, virtio-net, e1000,
virtio-gpu, xHCI and the AC'97 codec. Each re-states the agreement rather than probing again — the ring, the
queue, the command list and the buffers all still exist, and a resume that
allocated them again would leak a set per sleep. A virtio device's ring is zeroed
and re-published on both sides, because a device that comes back at reset
believes it has consumed nothing and would otherwise replay every request the
driver ever posted.

What the firmware is asked for, and when: `_PTS(3)` before the sleep, `_WAK(3)`
after it — and `_WAK` is called with interrupts back ON, because it is a program
and QEMU's own spends time in `Sleep()`; running it with no timer cost the resume
two minutes of its own.

**A bus has state of its own.** xHCI comes back halted and reset, and the
keyboard's slot and address belonged to the controller rather than to the driver:
re-programming the rings leaves a keyboard that is plugged in and reports
nothing. So the resume re-programs the controller over the rings it already has
and then ENUMERATES THE PORTS AGAIN, which re-addresses the device and re-arms
its interrupt endpoint. The driver's own enumeration markers appear a second
time in the log, after the sleep, which is what the lane grades.

**A display and a codec are device state too.** virtio-gpu's scanout is an
object the HOST owns: it went with the power, and nothing in the driver's memory
says so. A resume that only re-states the transport leaves every later command
naming a resource the device has never heard of — a machine that answers a
modesetting query and then fails the scanout, which is a display that is there
and never updates. So the resume re-creates the resource over the same backing
pages, at the same geometry, and the frame that was on screen comes back. The
AC'97 codec is the same shape of problem with a simpler answer: it returns muted
and in reset with the descriptor list forgotten, so a resumed machine is silent
while every write still succeeds.

**Proof.** The suspend lane arms the RTC alarm, writes `mem`, and grades the
kernel's own S3 counter — which counts only the path that wrote SLP_TYP and came
back through the trampoline, so an idle suspend cannot pass it — the length of
the sleep as userspace measures it, and that a file round trip, a fork and a
syscall all work afterwards — and that the DEVICES came back with it, which is
the half of a suspend that fails quietly: an evdev node answers, the display
takes a whole modeset (a dumb buffer, a framebuffer, a CRTC — the last of which
sends the scanout command to the device), and the sound card plays. That last
one is graded on what the emulator captured: the test writes an 880 Hz tone
after the resume, nothing else in the lane plays that frequency, and the
capture is searched for it. The lane also opens a QMP socket and a small waker
(`tests/support/acpi/s3-waker.py`): QEMU's RTC does drive the wake path, so the
guest wakes itself, and the waker is the insurance that says so rather than
leaving a machine asleep for ever if it ever stops doing that.

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

## Power management: what is not done (M135)

The machine sleeps, wakes and scales. It does not manage its own power: nothing
here reacts to a closed lid, lowers the clock because the machine is idle, or
acts on a temperature. This is the list, kept here rather than in the roadmap
because it is long and each line names the thing that is missing rather than a
theme.

**Events from the platform.** There is no SCI handler and no GPE dispatch, and
`Notify` is not implemented, so nothing the firmware raises reaches the kernel:
the power button, the lid switch, the adapter going in or out, a battery
changing state, a thermal trip. Two consequences follow. A desktop cannot ask
this kernel to suspend on a lid close, because the kernel never hears the lid.
And the battery and thermal files are only ever evaluated when somebody reads
them — a change is not an event, it is something a poller notices later.

**Powering off.** `reboot(RB_POWER_OFF)` writes three hard-coded ports
(`0x604`, `0xB004`, `0x4004` — QEMU and Bochs) and, on a machine that is
neither, prints "poweroff unsupported, halting". The honest path is the one S3
already uses: `\_S5` for the sleep type and the FADT's PM1 control register,
which the kernel now reads anyway.

**The resume remainder.** Ten drivers have a resume callback (see the S3
section); these do not: Intel HDA, virtio-input, virtio-console, the PS/2
controller, and the IOMMUs (VT-d/AMD-Vi domains and the interrupt-remapping
table; SMMUv3 on aarch64). The PCI snapshot restores the BARs, the command
register, the cache line, the latency timer and the interrupt line — and NOT
the MSI or MSI-X capability, so a device driven over message-signalled
interrupts comes back with no vectors. There is no quiesce (suspend) callback
at all: the queues are not drained before the power goes, and what saves the
machine today is that userspace is frozen rather than that the devices are
idle. The resume list is flat — no ordering, no dependencies, and a driver that
fails gets a line in the log and nothing else.

**States that are absent.** Hibernate (S4) does not exist: no image, no
`/sys/power/disk`. aarch64 has no deep sleep — PSCI `SYSTEM_SUSPEND` is not
implemented, so `/sys/power/state` there is `freeze` alone. The `/sys/power`
surface is one file: no `wakeup_count`, `mem_sleep`, `wakeup_sources` or
`pm_wakeup_irq`. Only the RTC alarm counts as a wake source deep enough for
S3; a power button, a lid, wake-on-LAN and USB wake do not exist as sources.
A `freeze` with nothing armed still sleeps to its ten-second ceiling.

**Frequency.** Two governors, and they are the two ends of the range:
`performance` pins the top state and `powersave` the bottom. Nothing moves
between them by itself, which is what `ondemand` and `schedutil` are for, so an
idle machine runs at the frequency it was left at. `_PPC` — the ceiling the
firmware asks for, which is how a laptop says "you are on battery" — is not
read. `_PSS` is taken from one processor container and applied to the machine:
there is no per-policy or per-core control, and no `policy*` directory layout,
which is the shape `cpupower` and every tuning daemon expect. No boost or turbo
knob, no energy-performance preference beyond the two presets, and no feedback
from temperature or a power cap (no RAPL).

**Idle.** The C-states come from CPUID leaf 5 — the MWAIT hints the processor
advertises — and ACPI's `_CST`, which is where a platform declares the states
it really has, is not read. There is no idle governor: `cpuidle_enter` takes
the deepest state this CPU has, every time, rather than choosing from the
predicted length of the idle period and the state's exit latency. On aarch64
there is one state, WFI; PSCI `CPU_SUSPEND` is not used. And there is no
runtime power management anywhere: an idle PCI device stays at full power
until the whole machine sleeps, because nothing puts it in D3 and nothing gates
a clock or a power domain.

**Heat and charge, read but not acted on.** `/sys/class/thermal/thermal_zoneN`
publishes a type and a temperature and nothing else: no trip points (`_PSV`,
`_AC0`, `_CRT`), no cooling devices, no passive throttling, and no
critical-temperature shutdown — a machine that overheats keeps going. The
battery publishes `_BIF`/`_BST` and not `_BIX`, has no charge thresholds, and
raises no event, so a desktop's indicator polls. Hotkeys and the idle-power
comparison the M129 notes mention are untouched.

**What the tests do not cover.** S3 is exercised in one lane and on x86_64
only: one sleep per run, with the machine quiet. There is no repeated-cycle
test and none with I/O in flight across the sleep, which is exactly where the
missing quiesce callback would show. The five-level-paging lane
(`SMOKE_LA57=1`) is flaky under TCG — it can trip the guest's own
twenty-second console-silence watchdog on a slow test — so a wedge there is
worth a second run before it is read as a regression.

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
and over the whole address space before `fork` clones it (the aarch64 port
splits at the same places, plus `vmm_unmap_range_nosync`, which reports one
frame per page and has nowhere to put a block). The unmap paths ask only for the
blocks their range cuts in half; one a range covers whole is released as a
block, which is what makes address-space teardown — the busiest caller —
allocate nothing. The four walkers that handle a block themselves are the unmap
of a whole one, `free_table` at teardown (which used to SKIP a huge entry at a
non-leaf level, and would have leaked the whole 512-frame block),
`paging_user_resident`, so a process using blocks does not read as having given
its memory back, and `paging_user_writable`, so a userfaultfd write-protect
check does not refuse a write to a page that is perfectly writable.

A range that covers a block WHOLE is a different case from one that cuts it in
half, and `mprotect` now treats it as one: the protection goes into the 2 MiB
entry and the block stays a block. `ld.so` mprotects every segment it has just
mapped, so without that a dynamically linked program lost its blocks on the way
up. Half a block still has to be split — half an entry cannot carry two
protections.

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

**...or where the table that is there is empty.** A range that has already been
faulted at 4 KiB keeps its page table when the mapping goes: the leaves are
cleared, the table stays, and a directory entry naming a table is one the fault
path may not take. Every long-lived allocator recycles addresses, so that meant
blocks the first time round and 4 KiB pages ever after. An empty table is
therefore taken out of the tree and ORPHANED rather than freed
(`thp_orphan_table`): the frame stays claimed as a page table on a list
belonging to its address space, and goes back when that space is torn down —
when nothing can be walking it. A walker mid-descent reads the same zeroes it
read before, and the two words the list threads through the frame are
page-aligned physical addresses, which every walker reads as an absent entry.
The cost is one frame per recycled 2 MiB range for the life of the process.
What can be lost is at most a lazy marker another CPU installed between the
emptiness check and the swap, and a lost marker is harmless for the reason
above; nothing else writes a leaf without the lock, so no swap slot and no
copy-on-write leaf can go missing.

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
4 KiB pages and there is nothing in it to take one page of a block, so a split
registers the 512 leaves and puts them back within reclaim's reach. Having
reclaim do the splitting was tried twice and reverted both times — reclaim is
reached from inside the allocator, splitting allocates and takes the page-table
lock, and the two orders do not agree: the machine wedged with the console
silent. The split therefore happens one level out, where the charge against
`memory.max` is decided (`cgroup_mem_charge_pages` → `cg_reclaim`), in task
context with no lock held and before the allocation that would fail: when a
batch of reclaim finds nothing to take and the feature is on, as many of the
cgroup's blocks as that batch needs are broken up
(`paging_thp_split_for_reclaim`, which registers the leaves against the task
they belong to rather than the one running) and the batch is tried again.

The other half is a refusal. A block that would take the cgroup past its
`memory.max` is not installed at all (`cgroup_mem_would_exceed`): the fault
falls back to a 4 KiB page, which reclaim can take. That is the same decision
Linux makes when it charges a huge page before installing it, and between the
two halves `always` is a mode a machine can be run in — a cgroup with a 12 MiB
limit fills 36 MiB with the knob at `always`, loses the pages to swap and stays
alive, which is what `cgroup-reclaim-thp` grades in the compressed-swap suite.

**`fork` shares a block copy-on-write**, as it shares a 4 KiB page: both sides
keep the 2 MiB entry, read-only, and each of the block's 512 frames gains a
reference. The first write from either side is a protection fault, and the
answer to a refused access to a block is always the same — break it into its
512 leaves, which inherit the read-only flag and the copy-on-write mark, and
let the ordinary per-page copy-on-write resolve the page that was written. So
there is no second representation and no huge-page copy: a block that is only
read stays one entry in both processes, and one that is written costs a page
table and a copy of the one page that was touched. Breaking every block in the
parent at fork time, which is what this did first, charged that price for a
child that may never write at all.

Two things had to be true before that worked. A **directory entry must not
carry the leaves' restrictions**: the CPU takes the AND of write permission
down the walk, so a split that copied the block's read-only flag into the new
page table's own entry left every leaf under it read-only — the copy-on-write
handler granted the write on the leaf, the retry faulted again, and the process
died of a protection fault on a page its own tables said was writable
(`pt_directory_flags`). And on aarch64 the fault handler has to do the split
itself, under the lock it already holds, because that port services faults with
the page-table lock held (`thp_split_here`).

**khugepaged.** The fault path can only take a block where nothing describes
the address yet, so everything a program touched before huge pages were asked
for stays 4 KiB for ever — which is most of the memory of most long-lived
programs. A kernel thread closes that: one pass every
`khugepaged/scan_sleep_millisecs` (200 ms by default), at most eight blocks a
pass, over the mappings of every task the scheduler knows. A range it can
collapse is 512 present, private, anonymous user pages with a refcount of one
and identical permissions — anything else is something one entry cannot
describe — and the 512 pages are COPIED into a freshly allocated block, because
the hardware needs 2 MiB of contiguous aligned memory and these frames are from
wherever they were allocated. The copy runs under the page-table lock, which is
what makes it safe: a write landing halfway through would otherwise be lost.
The page table the block replaces is orphaned, as at any other block install,
and the old frames go back to the allocator afterwards. What it has done is at
`khugepaged/pages_collapsed` and in the `thp_collapse_alloc` counter.

**What a block does not need**: a block is never in the eviction ring, so
nothing samples the access flag of one — `paging_test_and_clear_accessed` and
`paging_test_and_clear_dirty` answer about 4 KiB leaves and are not asked about
a 2 MiB entry. Reclaim reaches a block's pages by splitting it first, which is
the path above, and the dirty bit is only read for file mappings, which are
never blocks.

**Off by default**, on both arches. `b1nix.thp` selects madvise mode,
`b1nix.thp=always` every eligible mapping, and
`/sys/kernel/mm/transparent_hugepage/enabled` reads and writes the same state
in Linux's `always [madvise] never` format beside `hpage_pmd_size` and a
`stats` file counting blocks installed, refused, split and collapsed, and a
`khugepaged/` directory with the thread's own counter and its sleep. Per mapping it is
`madvise(MADV_HUGEPAGE)` / `MADV_NOHUGEPAGE`, which are no longer no-ops:
turning the advice off takes existing blocks back apart.

**aarch64** uses a level-2 block descriptor and the same rule, with three things
of its own. Break-before-make is observed for every block installed or split —
the architecture permits no other transition between a block and a table
describing the same addresses — where `ensure_child`'s long-standing exception
stands only for the kernel's own 1 GiB block, which cannot be unmapped while it
is running the code doing the unmapping. A software bit (`SW_THP`) separates the
blocks this path made from the identity map's own, whose frames are not
anybody's to release. And the walkers that meet one now answer for it rather
than about it: `paging_user_frame`, `paging_user_pte` and `paging_leaf_pte`
report the page inside the block that the caller asked about, `paging_user_writable`
reads the permission off the block, and `free_user_subtree` releases its 512
frames instead of skipping a block at a non-leaf level and leaking them.

Bringing it up found something older and larger: nothing on aarch64 had ever
called `pmm_switch_to_direct_map`, which is where the buddy tree is seeded. It
is x86_64's "the direct map now exists" hook, and this port's RAM is
identity-mapped from boot, so the call had no obvious home and never got one.
Every allocation had been falling back to a bitmap scan, and
`pmm_alloc_block_node` refuses outright while the tree is unseeded — so no
contiguous block could be allocated at all, on a machine with a free gigabyte.
It is now called at the end of `vmm_init`.

What a mapping actually holds is readable rather than assumed:
`/proc/<pid>/smaps` is new, and its `AnonHugePages` is computed by walking the
directory entries themselves. That is what `m128_thp_smoke` grades — that a
4 MiB `MADV_HUGEPAGE` mapping reports it, that the bytes survive a fork and a
copy-on-write write on both sides, an `mprotect` of half the range, a `munmap`
of half a block and a `MADV_NOHUGEPAGE`, that a whole-block `mprotect` leaves
the block alone (`mprotect-keeps-block`), that a `fork` shares it rather than
splitting it (`fork-shares-block`), that an address already faulted at 4 KiB is
block-backed the second time round (`recycled-range`), that khugepaged
collapses a range that was faulted at 4 KiB on purpose — graded on the kernel's
own collapse counter and on every byte of the 8 MiB reading back
(`khugepaged-collapse`) — and that eight
rounds of mapping, filling and releasing 8 MiB leave the machine's free memory
where it started. It prints the kernel's own counters beside the verdict, so a
mapping that is not block-backed says which of the two things happened: no block
was installed, or one was and something split it again.
