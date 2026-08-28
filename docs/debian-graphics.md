# A Debian desktop on the DRM path

The systemd image reaches `graphical.target` and nothing renders: it ships
`systemd systemd-sysv udev dbus procps libproc2-0 libncursesw6` and no display
server, so the target is a name and the scanout keeps whatever the firmware
left. This is the same image with Debian's own **Weston** on it, started by a
systemd unit, modesetting `/dev/dri/card1` with no compositor underneath.

```sh
PROFILE=graphics sh tools/images/mk-debian-image.sh   # once, or after any image change
sh tests/debian-graphics-smoke.sh                     # builds the ISO, boots, judges
```

The picture is [`images/m112-debian-weston.png`](images/m112-debian-weston.png)
— 1280x800, the shell's panel with its clock and launcher, and two Wayland
clients drawing — taken from the **host** with the QEMU monitor's `screendump`
on the virtio-gpu scanout. Nothing inside the guest takes part in producing it,
and the run it came from is 12 checks green with 1644 distinct colours in the
frame.

## What the harness asserts, and why each one

`tests/debian-graphics-smoke.sh` is written so that it cannot pass without a
compositor having drawn. Every check is on something the guest cannot forge:

1. **The frame.** `screendump` reads the framebuffer the guest programmed into
   the virtual GPU. A display that never drew dumps a solid rectangle — two or
   three distinct colours. The threshold is on the distinct-colour count
   (`tools/ppm-colours.py`), not on a marker.
2. **The device open.** `b1nix.trace-sysfs` prints every open under `/dev/dri`
   with the task that made it, from inside the kernel:
   `drm: open /dev/dri/card1 flags 802 -> 0 by /usr/bin/weston`.
3. **No compositor underneath.** Weston's own log names the backend it loaded
   and the connector it lit; the run must have taken `drm-backend.so` on bare
   QEMU.
4. **Debian's init.** `/proc/1/comm` is read in the guest and must say
   `systemd`, and the session is started by a unit rather than by a shell.

**`tests/debian-graphics-smoke.sh` does not rebuild
`build/x86_64/debian-graphics.ext4`.** A change to the image or to the in-guest
harness does not reach the guest until `PROFILE=graphics sh
tools/images/mk-debian-image.sh` runs again. The same trap cost the systemd
work four wrongly-reported checks.

## Why Weston, and why the pixman renderer

Weston's DRM backend takes the same route a desktop takes: find the card
through libudev, open it through a launcher, modeset it, scan out of it. What
it drags in is the cost of compositing, not of a desktop environment — 160
packages against KDE's 293, and it needs no Mesa driver at all, because
`--use-pixman` composites in software into a DRM dumb buffer. A GL renderer
would have added a second thing that can fail for reasons that have nothing to
do with whether the display works.

The launcher is the **direct** one: Weston tries logind first, and logind
refuses because the harness runs from a system unit and a system unit is not a
session (`cannot find systemd session for uid: 0`). That refusal is correct
Linux behaviour, not a defect here; the direct launcher is what `weston-launch`
used to provide and it opens the card itself as root. The logind route is still
worth having, and it is listed under Open below.

## The kernel defects it found

### 1. A task SIGKILLed inside `mmap` never gave the address-space lock back

The machine went quiet about thirty seconds into every graphical run. No panic,
no error — the serial log simply stopped, and the last thing on it was:

```
vma: mutator lock slot 8 held for a very long time by ? pid 143 state 0; waiter pid 162
```

`state 0` is `TASK_UNUSED` and the name is NULL: the owner's task slot had
already been freed and recycled. So the lock was held by a task that no longer
existed.

`mmap`, `munmap` and `mprotect` serialise on a per-address-space mutex, and the
slot is chosen by hashing the PML4 frame, so unrelated processes share slots.
It is a yielding lock released only by the code that took it —
`scheduler_exit_current` hands back whatever a dying task still holds, for
exactly this reason.

But a task killed by `SIGKILL`, or by a fatal default action, never runs
`scheduler_exit_current`. It is marked `TASK_DEAD` from inside the scheduler's
signal-delivery pass, with each of its resources released by hand there:
children reparented, futex waiters woken, ctid cleared, POSIX timers freed,
ptrace links dropped, the CPU lease cleared. The address-space mutex was not on
that list. And `exit_group` posts `SIGKILL` to every sibling — so any
multithreaded program exiting while one of its threads is in `mmap` could leak
a slot, which is every desktop program there is.

The consequence is not confined to the process that died: every other process
whose address space hashes to that slot then spins in `vma_mutator_lock` for
ever. On this image the casualty was the harness's own `sleep`, and after it
the machine.

Both scheduler death paths release the lock now. `MM-SMOKE: ok
mmap-after-sigkill` covers it: it kills 120 children at the moment they are
looping through `mmap`, then asks 48 fresh processes to map memory and reports
how many came back. The reaping is bounded on purpose — the failure being
tested for is a hang, and a blocking `waitpid` would take the suite down with
it instead of naming the number.

The warning that found this now prints the scheduler's task dump with it, so
the one line reporting a stall also carries the syscall every task is in.

### 2. The imported drivers' `ktime_get()` was not `CLOCK_MONOTONIC`

Weston said, every frame:

```
Warning: computed repaint delay is insane: -10736 msec
```

10736 ms is not a random number: it is how far into the boot Weston started.

`ktime_get()` is not only a duration source inside a driver. The DRM core
stamps it into every page-flip and vblank event it delivers, and the client
that reads one compares it against the `CLOCK_MONOTONIC` it gets from
`clock_gettime(2)`. `lkpi_monotonic_ns()` counted from **its own first call** —
a TSC base captured lazily, unrelated to the one `arch_tsc_monotonic_ns()` uses
for the system clock. Same nanoseconds, different origins, which is not the
same clock.

So Weston read every flip event as having completed about ten seconds before
the frame it belonged to, concluded the next repaint was already overdue, and
scheduled it immediately — every frame, for ever. `lkpi_monotonic_ns()` now
returns `arch_tsc_monotonic_ns()`, and the warning is replaced by Weston's
ordinary `Output repaint window is 7 ms maximum`.

The first version of that fix kept the old lazily-based TSC path as a fallback
for the window before `arch_tsc_clock_init` declares the counter usable — and
so reintroduced the very bug it was fixing, inside a single boot: both epochs
appear, and anything holding a timestamp across the changeover sees time jump
forward by the whole boot so far. The in-guest watchdog does exactly that, and
it killed the machine at eleven seconds of uptime for "no console output for
60s", taking 261 checks of the main suite with it. The fallback is the tick
now, which is measured from boot like the TSC clock is, so the changeover moves
the resolution and not the origin.

### 3. A fault mapped its neighbours with the wrong page's protection

A read fault on a file-backed page maps the pages around it as well, straight
out of the page cache and without a fault of their own. Those frames are shared
with every other mapper of the file, so the protection they are installed with
decides whether one process's stores are served to the next.

The neighbours were installed with the flags computed for the **faulting**
page. The copy-on-write downgrade that keeps a `MAP_PRIVATE` writable mapping
off a shared cache frame fires on whether the *faulting* page's frame came from
the cache — and that is false whenever the page could not be added to it: a
duplicate insert lost to another CPU, or an allocation refused under pressure.
The flags then still carry `VMM_WRITABLE`, and every neighbour — which always
comes from the cache — was mapped writable into a private mapping.

That is the `libpam.so.2` corruption the code above it already documents,
arriving by a different road: `ld.so`'s relocation stores land in the page cache
instead of in a private copy, and the next process to map the library executes
them. The neighbour install now computes its own protection.

The same loop never marked a writable **shared** neighbour dirty, so stores
through it were invisible to writeback and dropped on eviction. It does now.

`MM-SMOKE: ok file-map-privacy` covers the invariant from userspace: a store
into a page the process never faulted itself must not reach the file through a
private mapping, and must reach it through a shared one.

### 4. Eviction could hand out a page-cache frame other processes were running

The page-fault handler registers a page with the swap evictor guarded by
`!vma_shared`, under a comment that says something else entirely:

> Shared page-cache frames are owned by the cache and mapped in several address
> spaces — leave them out of the per-task swap set.

`!vma_shared` excludes a `MAP_SHARED` mapping. A `MAP_PRIVATE` read-only
mapping of a shared library is not one, and its frame comes straight out of the
page cache. Registered, it could be chosen by the evictor — which writes it to
swap, marks **this** task's entry swapped, and returns the frame for immediate
reuse without asking who else holds it. The cache's entry and every other
mapper's page table still point at it.

The guard now tests what the comment says.

### 5. A SIGKILLed task never gave its `rseq(2)` registration back

Weston's own clients died at start-up with one line:

```
Fatal glibc error: rseq registration failed
```

glibc registers an rseq area for **every thread it creates**, and a refusal on
a thread is fatal — it kills the process rather than running without rseq. So a
kernel that loses track of a registration does not slow programs down, it stops
them.

The registrations live in a table keyed by the task, and the release lived in
`scheduler_exit_current` — the same place, and the same omission, as the
address-space lock in defect 1. A task killed by `SIGKILL` leaked its entry for
ever, and once its task slot was reused the next thread's first registration
looked like a conflicting re-registration of a different area and was refused.
The table also held 64 entries, which is fewer threads than a desktop has alive
at once even with nothing leaking; it is now one per task the scheduler can
hold.

`MM-SMOKE: ok rseq-after-sigkill` kills 200 registered children and then asks a
fresh process to register.

Both of those were real and both are fixed, and neither was what killed
`weston-terminal`: it still dies of the same message. What remains is a
**thread's** registration being refused after the process's first one
succeeded, which is the case glibc treats as fatal, and it is listed under Open.
`weston-simple-shm` and `weston-flower` — the two clients in the photograph —
are unaffected.

## What was ruled out

Weston, and an unrelated `journalctl`, died of `SIGILL` on four CPUs. The
obvious reading — a page mapped into the process that is not the page its file
holds — is **wrong**, and the fault report now says so itself: the sixteen bytes
at the faulting instruction are dumped on `#UD`, and they were
`85 c0 0f 88 5b 03 00 00 74 79 …`, exactly what `objdump` gives for
`libwayland-server.so.0.21.0+0xb820`. `b1nix.frame-alias` also found no frame
reachable from two mappings. The memory was right; the CPU fetched something
else, which is a stale translation rather than a corrupted page.

While reading that report a second defect in it turned up: the block that
prints `addr=`, `pte=` and a `not-present, read, kernel` decoding was reached
for **any** `SIGSEGV`, including one raised by a `#GP`. `#GP` does not write
CR2, so the address printed was a leftover from some earlier fault, and its
page-table entry and error-code decoding were about nothing at all. It is
gated on the page-fault vector now, and other vectors print the vector and the
raw error code instead.

## Open

- ~~**On more than one CPU a user process is intermittently killed by
  `SIGILL`.**~~ **FIXED** — see M116 in the roadmap. `CR4.PGE` was set on the
  APs and not the BSP, and bit 8 of a leaf PTE (the GLOBAL bit) was in use as
  the software flag `VMM_SHARED`, so shared translations on an AP survived the
  address space that owned them. 0 ring-3 faults in 10 four-CPU runs, against
  4 in 5. The original account follows, because what it ruled out is what made
  the answer findable:

  **On more than one CPU a user process is intermittently killed by `SIGILL`,
  and the instruction is valid.** Weston died of `SIGILL` in three runs, and in
  a fourth both it and an unrelated `journalctl` died of a `#GP`. The bytes at
  the faulting instruction, dumped by the kernel at the fault, are exactly what
  `objdump` gives for the library at that offset — `85 c0` (`test %eax,%eax`)
  once, `48 85 ff` (`test %rdi,%rdi`) another time — so the memory the kernel
  can read is right and the CPU fetched something else. `b1nix.frame-alias`
  finds no frame reachable from two mappings. That combination is what a stale
  instruction-TLB entry looks like: a translation the kernel has since replaced
  and some CPU is still using. No swap device is attached to these runs, so the
  swap evictor cannot be the mechanism. The harness runs on one CPU by default
  (see `tools/run-debian-graphics.sh`), and this is not fixed. Defects 3 and 4
  above were found while chasing it; neither removed it, and neither is claimed
  to have.

- **`weston-terminal` still dies of "Fatal glibc error: rseq registration
  failed".** The two defects behind the message that were found — the leak on
  the SIGKILL path and a 64-entry table — are fixed and did not remove it, so
  what is left is a thread's registration being refused where the process's
  first one succeeded. `rseq_register` refuses on three grounds
  (`len < 32`, a misaligned area, and a conflicting re-registration), and which
  one it is has not been established: the refusal is silent. The other two
  Wayland clients are unaffected and both draw.

- **The logind launcher is not exercised.** Weston falls back to the direct
  launcher because nothing gives the compositor a logind session.
  `pam_systemd.so` is staged and wired into `/etc/pam.d/common-session`, so the
  remaining piece is starting the session the way a display manager does — an
  autologin getty on tty1, or `systemd-run --property=PAMName=login`. logind
  itself already answers: `CanGraphical=yes` on seat0, and the DRM card carries
  udev's `seat` tag.
- **`agetty: cannot connect on UNIX socket`** on the console getty, twice per
  boot, with no visible consequence.
- **`linux-abi: unmapped syscall … nr=434`** from `systemd-udevd`
  (`pidfd_open`).
