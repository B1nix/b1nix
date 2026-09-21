# Platforms and userspace

Milestones M0–M1, M37, M94, M97, M104, M108, M111–M113, M119, M121 and M134,
plus AArch64 as a second target of the same kernel.

## What b1nix is

b1nix is a monolithic Unix-like kernel written in C11 and assembly. It boots
through Limine (a hybrid ISO that works from CD, USB and PXE; M97), prints to a
serial line, a framebuffer and, when asked, a virtio console, and on a fatal
error gives a symbolised backtrace. It does not ship its own userspace any more
(M121): the programs that run on it are unmodified distribution packages. The
repository holds the kernel, its tests, the scripts that build images and run
them, and the `b1cc` compiler.

## The x86_64 target

The primary target runs in QEMU with KVM and on real machines. On real
hardware it has driven e1000/e1000e and r8169 network cards, xHCI keyboards and
mice, AHCI and NVMe disks, and ACPI with MADT and the IOAPIC (M37) — and, since M134, the bytecode
inside the DSDT as well as the tables around it. A laptop
with a UHD 620 is booted over PXE (`tools/run/pxe-serve.sh`) and, having no
serial port, reports through netconsole. The kernel asks the processor what it
is rather than guessing: the CPU name, `RNDR`/`RDRAND`, the physical address
width and the TSC frequency come from CPUID, and `/proc/cpuinfo` shows the real
flags (M119).

## The firmware's own bytecode (M134)

ACPI is two things wearing one name. The tables `kernel/dev/acpi.c` reads —
RSDP, RSDT/XSDT, MADT, MCFG, SRAT, FADT — are structs: fields at fixed offsets,
and a parser for them is a hundred lines. The DSDT is not that. It is a
compiled program, and the SSDTs beside it are more of the same. A battery's
remaining charge, a thermal zone's temperature, the sleep type this machine
wants for soft-off, the frequencies a processor will accept: all of them are
methods or packages inside that program, and none of them can be read by
anything but an interpreter. That is what `kernel/dev/aml.c` is.

**Loading.** The DSDT is found through the FADT (the 32-bit pointer at offset
40, or `X_DSDT` at 140 where the table is long enough to have it), and every
SSDT the root table lists is loaded after it; `acpi_table_at()` enumerates the
root table for that, because `acpi_find_table()` only ever returns the first
match for a signature and a machine has as many SSDTs as its firmware felt
like emitting. The load pass walks the term list building the namespace —
scopes, devices, processors, thermal zones, power resources, names, methods,
packages, buffers, operation regions and their fields — and executes nothing.
It can walk past a construct it has never seen because every declaration in
AML carries a `PkgLength`, so skipping one is arithmetic rather than
understanding; an `If` at declaration level is followed when its predicate can
be decided, because firmware does hide declarations behind one. Whatever is
skipped is counted and reported, so "the parser did not understand this
machine" is a number rather than a silence.

**Evaluating.** Methods are run on demand, never at load. The evaluator
covers the arithmetic, bitwise and logical opcodes,
`If`/`Else`/`While`/`Return`/`Break`/`Continue`, `Store` and `CopyObject` with
ACPI's conversion rules, `Index`, `DerefOf`, `RefOf`, `SizeOf`, `Match`,
`Mid`/`Concat`/`ToString` and the other conversions, the `CreateXField` buffer
fields, method invocation with arguments and locals, and field access through
`SystemMemory` and `SystemIO` regions — index fields, the five access widths
and the three update rules included. A revision-1 table gets 32-bit
arithmetic, because that is what its author tested against. `\_OSI` answers
the Windows strings and nothing else, for the same reason Linux does: firmware
branches on the answer, and a kernel that claims nothing takes a path nobody
has ever run.

Two things bound it. Everything runs under one lock and nothing sleeps:
`Sleep` and `Stall` are bounded busy waits, so a firmware asking for a long nap
costs a bounded delay rather than a stalled CPU. And every evaluation spends
from a step budget, so a `While` that never falls out ends as an error instead
of as a hung machine.

**What it refuses.** PCI configuration space, the embedded controller, SMBus
and CMOS are not implemented, and an access to one returns an error that
propagates out of the whole evaluation. It is never answered with a zero. A
battery reading 0% because the interpreter invented the number is worse than a
battery that is absent, and the refusals are counted in `/proc/b1nix-acpi` so
that a machine needing one says which. On QEMU this is visible: the PIIX link
devices keep their routing in PCI config space, so `\_SB_.LNKA._STA` is
refused, and the smoke suite checks that it is refused rather than answered.

**What it is for.** `/sys/class/power_supply/BAT0/{type,present,status,capacity,energy_now,energy_full}`
comes from `_STA`, `_BIF` and `_BST`; `AC0/online` from `_PSR`;
`/sys/class/thermal/thermal_zoneN/{type,temp}` from a zone's `_TMP`. Each file
re-evaluates its method on every read, because that is the only way a charge
is ever current, and each is published only for a device the firmware really
declares. No machine here declares one — a QEMU guest has no ACPI battery —
so `/sys/class/power_supply` is empty, and the test insists on that rather
than papering over it.

**Reading it.** `/proc/b1nix-acpi` lists the tables loaded, the object counts
by type, the terms the loader could not decode, the address spaces it refused,
and the namespace itself, one absolute path per line. `/proc/b1nix-acpi-eval`
is root-only and evaluates one object on demand: write a path and its integer
arguments, read the answer back. Writing to it makes the kernel execute
firmware bytecode, which may touch any I/O port or physical address the DSDT
names, which is why it is mode 0600.

On QEMU's `pc` machine that namespace is 354 objects — 105 methods, 53
devices, 7 regions, 20 fields — with no term the loader could not decode.
The boards on the other architecture have a device tree and no RSDP, so
nothing is loaded there and the namespace is the handful of roots the
specification says an operating system creates; `/proc/b1nix-acpi` says so,
and the same test checks it.

## The AArch64 target

AArch64 is not a milestone of its own. It is the same kernel built for another
architecture, and any gap belongs to the milestone that owns the mechanism.
It runs on QEMU `virt` (the smoke suite), on the Raspberry Pi 4 (QEMU
`raspi4b`, `make run-rpi4`, `SMOKE_RASPI_LANE=1`) and on a Sony Xperia 5
(`make bahamut`, tools in `tools/board/sony-xperia-5/`). On `virt` the suite
runs the same lanes as on x86_64, with PCIe devices reached through ECAM; the
`smp` lane adds GICv3 with an ITS and an SMMUv3.

What differs is mostly below the syscall layer. Page faults are taken with
interrupts masked, so the handler never blocks: when a page has to be read from
a file or from swap it returns to a path that does the reading outside the
page-table lock. MMIO lives in a 32 GiB window at 416 GiB that is never
reclaimed, and cacheability is chosen through MAIR slots instead of the PAT.
Every new kernel virtual address has to stay out of the boot identity map and
under 512 GiB. Secondaries start with PSCI or a spin table, and TLB shootdowns
need no IPI because the hardware broadcasts `tlbi …is`. Interrupts come from a
GICv2 or GICv3, PCI INTx lines are routed from the device tree's
`interrupt-map`, block and network devices use virtio-mmio, and the console is
a PL011 with a PL031 clock.

At the system-call boundary AArch64 uses the asm-generic numbers, and some
`open` flags have different values (`O_DIRECTORY`, `O_NOFOLLOW`, `O_DIRECT`,
`O_LARGEFILE`). `struct epoll_event` is packed only on x86_64. Signal frames
carry a real `fpsimd_context`, and ptrace reports FP state as
`user_fpsimd_state`.

Userspace runs on the secondary CPUs, as on x86_64. It was kept off for a
long time because a secondary was once caught running with its stack pointer
inside another task's kernel stack; that no longer reproduces, and
`b1nix.no-ap-userspace` keeps the secondaries on kernel workers if it comes
back. One thing is still off: the kernel heap does not return tail pages,
because live kernel stacks were found in returned ranges. Chromium has no AArch64 entry in
the package lock, and i915 is x86-only.

When porting a mechanism to AArch64: read the x86_64 implementation first,
check the syscall number and flag tables before the code, keep FP and SIMD out
of kernel C (vector code lives in `.S` files), register new files in the
Makefile, and run x86_64 again whenever a shared file changed. An `#else` after
`#ifdef __x86_64__` used to mean 32-bit x86. That port is gone, so such a
branch is now the AArch64 branch.

## Userspace from distributions

The Linux ABI is the only system-call interface (M40, M121). Binaries are
dynamically linked against musl by default (M92), and the root image is
assembled from pinned Alpine packages (`tools/image/alpine/alpine-ports.map`, with
hashes in `alpine.lock`; M104). Optional groups keep large stacks out of the
ordinary image: `B1NIX_BROWSER=1` for Chromium, `B1NIX_GPU_DRV=1` for Mesa's
hardware drivers, `B1NIX_KDE=1` for Plasma.

On the Alpine image BusyBox init is PID 1 and starts OpenRC; `su`, `passwd` and
`login` are BusyBox's own, and dropbear authenticates through PAM (M105, M108).
`init=` accepts any program, and a stock Alpine minirootfs boots unchanged
(M94).

The Debian lane boots bookworm unmodified from a disk image built without root
privileges (`tools/image/mk-debian-image.sh`), with glibc as the libc (M111).
The kernel log is levelled and timestamped the way Debian's tools expect. The
systemd profile of the same image runs systemd 252 as PID 1 up to
`graphical.target`, with cgroup v2, mount propagation, devtmpfs and Weston on
DRM (M112). `tests/debian-smoke.sh` runs Debian's own tools against the kernel
and grades them by their results.

KDE Plasma runs on the `kde` image: `kwin_wayland` and `plasmashell` on atomic
DRM through elogind and eudev, with a painted desktop about seven seconds after
boot (M113, `tests/kde-smoke.sh`).
