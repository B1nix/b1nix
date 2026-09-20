# Platforms and userspace

Milestones M0–M1, M37, M94, M97, M104, M108, M111–M113, M119 and M121, plus
AArch64 as a second target of the same kernel.

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
mice, AHCI and NVMe disks, and ACPI with MADT and the IOAPIC (M37). A laptop
with a UHD 620 is booted over PXE (`tools/run/pxe-serve.sh`) and, having no
serial port, reports through netconsole. The kernel asks the processor what it
is rather than guessing: the CPU name, `RNDR`/`RDRAND`, the physical address
width and the TSC frequency come from CPUID, and `/proc/cpuinfo` shows the real
flags (M119).

## The AArch64 target

AArch64 is not a milestone of its own. It is the same kernel built for another
architecture, and any gap belongs to the milestone that owns the mechanism.
It runs on QEMU `virt` (the smoke suite), on the Raspberry Pi 4 (QEMU
`raspi4b`, `make run-rpi4`, `SMOKE_RASPI_LANE=1`) and on a Sony Xperia 5
(`make bahamut`, tools in `tools/boards/sony-xperia-5/`). On `virt` the suite
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
assembled from pinned Alpine packages (`tools/packages/alpine-ports.map`, with
hashes in `alpine.lock`; M104). Optional groups keep large stacks out of the
ordinary image: `B1NIX_BROWSER=1` for Chromium, `B1NIX_GPU_DRV=1` for Mesa's
hardware drivers, `B1NIX_KDE=1` for Plasma.

On the Alpine image BusyBox init is PID 1 and starts OpenRC; `su`, `passwd` and
`login` are BusyBox's own, and dropbear authenticates through PAM (M105, M108).
`init=` accepts any program, and a stock Alpine minirootfs boots unchanged
(M94).

The Debian lane boots bookworm unmodified from a disk image built without root
privileges (`tools/images/mk-debian-image.sh`), with glibc as the libc (M111).
The kernel log is levelled and timestamped the way Debian's tools expect. The
systemd profile of the same image runs systemd 252 as PID 1 up to
`graphical.target`, with cgroup v2, mount propagation, devtmpfs and Weston on
DRM (M112). `tests/debian-smoke.sh` runs Debian's own tools against the kernel
and grades them by their results.

KDE Plasma runs on the `kde` image: `kwin_wayland` and `plasmashell` on atomic
DRM through elogind and eudev, with a painted desktop about seven seconds after
boot (M113, `tests/kde-smoke.sh`).
