# Roadmap

Status: `[x]` completed · `initial` usable first implementation · `partial`
incomplete or limited · `planned` not implemented · `deferred` postponed ·
`wontfix` declined.

b1nix is a kernel. Userspace comes from existing distributions: Alpine (the
default image) and Debian (the glibc ABI lane). The repository keeps the kernel,
its tests, the image and test scripts, and `b1cc`. See M121.

AArch64 is a second target of this same kernel, not a milestone: each gap
belongs to the milestone that owns the mechanism. See
[aarch64-parity.md](aarch64-parity.md).

## Closed milestones M0–M79

| Milestone | Status | Summary |
|---|---|---|
| M0 Boot and Diagnostics | done | Freestanding kernel ELF, Multiboot2 boot, serial/VGA, panic, klog. |
| M1 Architecture Layer | done | Exceptions, timer interrupts, context switch, faults → signals. |
| M2 Memory | done | PMM, higher-half paging, kheap, swap, per-process tables, COW, mmap/mprotect. |
| M3 Scheduling | done | Kernel threads, preemption, sleep/yield, zombies, pgrp/sessions. |
| M4 Userspace | done | Syscall dispatch, initramfs `/bin/init`, ELF64 ring-3 loading. |
| M5 VFS and Devices | done | VFS, devfs/tmpfs/tarfs, links, path normalization, permissions. |
| M6 Network | done | VirtIO-net, Ethernet/ARP/IPv4/ICMP/UDP/DHCP/DNS, TCP/UDP sockets. |
| M7 Graphics | done | Framebuffer console, VirtIO GPU, basic compositor. |
| M8 Advanced VFS | done | FAT32, ext2/3/4 r/w + journaling, page/inode/dentry caches, async I/O. |
| M9 Hardware Drivers | done | VirtIO blk/net/gpu, PS/2, PCI, MBR/GPT, AHCI, NVMe. |
| M10 Full Network Stack | done | TCP lifecycle/retransmit, socket options, select/poll. |
| M11 Shell and Utilities | done | Built-ins, pipes, redirections, job control. |
| M12 Syscalls and Processes | done | fork/execve/waitpid/brk, signals, sigreturn. |
| M13 Userspace ABI | done | argv/envp ABI, libc wrappers, errno, SIGTTIN/SIGTTOU. |
| M14 Storage and Swap | done | Block cache, demand paging, fsync, lock ordering. |
| M15 IPC and Security | done | mqueue, shm, UNIX sockets, UID/GID, capabilities. |
| M16 TUI Applications | retired | File manager and editor removed in M121. |
| M17 POSIX and Self-Hosting | done | Core POSIX APIs, GCC/Binutils port, kernel built in-guest. |
| M18 ELF Loader | done | PT_LOAD, auxv, execve, malformed-ELF rejection. |
| M19 Process Model and FD Tables | done | COW fork, per-process fd tables, cloexec, cwd/umask. |
| M20 TTY | done | Line discipline, control keys, controlling terminal rules. |
| M21 Persistent Root | done | Writable ext2 root, mount/umount, sync-on-shutdown. |
| M22 Core Utilities | done | Multicall file/text/process/network utilities. |
| M23 Terminal Networking | done | ifconfig/ping/nc/HTTP, resolver helpers. |
| M24 Reliability | done | Symbolized backtraces, dmesg, stress tests, `make analyze`. |
| M24b SMP | done | AP boot, LAPIC timers, per-CPU runqueues, work stealing. |
| M25 Minimal Native C Toolchain | done | crt0/libc/headers, in-guest C compiler. |
| M26 Self-Hosting | done | Full toolchain in-guest, heap/swap fixes, 256 MiB floor. |
| M27 Terminal OS Polish | done | cmdline, rc/services, login, first-boot setup. |
| M28 Preemptive SMP | done | BKL removed, lockdep, TLB shootdown, reschedule IPIs. |
| M29 POSIX Threads | done | clone, futexes, TLS, pthreads; follow-up fixed the `-smp 2` wedge. |
| M30 Dynamic Linking | done | ET_DYN/PIE, PT_INTERP, DT_NEEDED. |
| M31 Users and Permissions | done | Shadow SHA-512, VFS access checks, setuid. |
| M32 Advanced Network Stack | done | TCP windows/Reno, curl/wget. |
| M32a Network Clients | done | TLS, HTTPS, IPv6 (NDP/SLAAC/MLD), Y2038-safe time. |
| M32b SSH Prerequisites | done | Dropbear, PTYs, host keys, sshd lifecycle. |
| M32c External SSH | done | Inbound SSH over QEMU forward and bare metal. |
| M33 POSIX Shell | done | Full shell features; upstream BusyBox ash replaces in-kernel shell. |
| M34 Virtual Filesystems | done | Dynamic /proc and /sys. |
| M35 Core Dumps | done | ELF core dumps, kallsyms. |
| M36 Debugging and Tracing | done | GDB stub, function tracer. |
| M37 Real Hardware | done | e1000/e1000e, xHCI HID, hybrid ISO, ACPI/MADT/IOAPIC. |
| M38 Sound | done | Intel HDA, `/dev/dsp`. |
| M39 Configurable Init | done | inittab, runlevels, telinit, gettys. |
| M40 Linux ABI | done | ~230 syscalls translated, Linux binaries by personality. |
| M41 Large Physical Memory | done | 64 GiB ceiling removed, 16 GiB verified. |
| M42 Upstream BusyBox | done | Upstream applets, ash as `/bin/sh`, kernel signal trampoline. |
| M43 Real Filesystems and NTFS | done | ext/exFAT validation, read-only NTFS, AHCI ATAPI-safe probe. |
| M44 BusyBox 1.38.0 | retired | Own build replaced by Alpine's BusyBox in M121. |
| M45 GNU bash | retired | — |
| M46 VFS/POSIX Conformance | done | Allocator locking, rename/waitpid fixes, exit_group, orphaned pgrp SIGHUP. |
| M47 Display Server | done | `/dev/fb0`, evdev, displayd, libb1gui. |
| M48 FD Passing and memfd | done | SCM_RIGHTS/CREDENTIALS, memfd_create. |
| M49 Wayland | done | libwayland client/server, xdg-shell. |
| M50 DRM/KMS | done | `/dev/dri/card0`, dumb buffers, page flips. |
| M51 Desktop Graphics Stack | done | pixman, FreeType, fontconfig, Cairo, xkbcommon, HarfBuzz. |
| M52 Mesa/OpenGL | done | TinyGL, VirGL, OSMesa (superseded: Mesa is now Alpine's packages). |
| M53 Browser Platform | done | Codecs, NetSurf with HTTPS/SVG/JS. Chromium assessment `stubbed`. |
| M54 Port Feature Enablement | done | Timed futexes, affinity, port flags landed/declined. |
| M55 C++ Runtime | done | Unwinding, RTTI, std::thread/filesystem, litehtml. |
| M56 Event Loop Primitives | done | epoll, eventfd, timerfd, signalfd, memfd seals. |
| M57 Multiprocess Model | done | socketpair, F_DUPFD_CLOEXEC, minimal Mojo core. |
| M58 V8 | cancelled | d8 with all tiers worked; standalone engine removed, V8 comes with Alpine's Chromium. |
| M59 EGL/GL for Browser | done | EGL over OSMesa (superseded with M52). |
| M60 Ozone Platform | cancelled | Headless Ozone done; browser ports left the tree with M121 (Chromium comes from the distribution). |
| M61 Chromium Build Target | cancelled | Own Chromium build dropped in M121; Alpine's Chromium runs on the kernel (M102). |
| M62 content_shell | cancelled | Superseded by the distribution's browser (M121). |
| M63 Sandbox | partial | seccomp-bpf, NO_NEW_PRIVS, mount/UTS/net namespaces (clone and unshare), unshare-shaped pid namespaces; user, IPC and cgroup namespaces and `CLONE_NEWPID` on clone are refused. |
| M64 Clang/LLVM Toolchain | done | Cross clang++, native in-QEMU clang. |
| M65 Install to Disk | cancelled | Installer and disk-image script removed in M121; a distribution installs itself. |
| M66 Chromium Frontend | cancelled | Userspace; superseded by the distribution's browser (M121). |
| M67 Rust Toolchain Port | done | `x86_64-unknown-b1nix` target. |
| M68 Native Rust Compiler | done | rustc 1.98.0 in-guest. |
| M69 Dynamic Loading | retired | — |
| M70 Interrupt-Driven I/O | done | ISR→wakeup completions replace busy-poll. |
| M71 ASLR and PIE | done | PIE-by-default, opt-in `b1nix.aslr`. |
| M72 msync | done | Durable MAP_SHARED dirtying. |
| M73 Modern I/O Syscalls | done | sendfile, splice, statx, inotify, ptrace (io_uring deferred). |
| M74 Real-Time Signals | done | SIGRT*, sigqueue, POSIX timers. |
| M75 On-Device GPU Path | done | llvmpipe on `libLLVM.so`. |
| M76 USB Host Stack | done | xHCI, USB core, mass storage. |
| M77 Resource Caps | done | Dynamic caps for TCP, pipes, core dumps, SHMMAX. |
| M79 Audio Stack | done | HDA/AC'97, mixer, ALSA shim. |

## M80: Kernel ptrace + Crash Capture

- [x] Full `ptrace(2)` (regs/mem, SEIZE, syscall stops, events), Yama gating, `/proc/<pid>/task`, XSAVE state.
- [x] Upstream Crashpad writes real minidumps unpatched, on x86_64 and aarch64.
- [x] `/proc/<pid>/cmdline` of a fork/clone child that has not exec'd is the parent's (`M80-SMOKE: ok fork-cmdline`).

## M81: Chromium GPU Acceleration

- [ ] `cancelled` SwiftShader/ANGLE for `content_shell`: userspace, left the tree with M121. The kernel side (virtio-gpu/i915 DRM) is tracked in M101/M102.

## M82: System NSS / Kerberos (optional)

- [ ] `cancelled` Userspace libraries; they come from the distribution (M121).

## Closed milestones M83–M100d

| Milestone | Status | Summary |
|---|---|---|
| M83 Unicode ctype | done | Provided by musl (M92). |
| M84 IP routing + TCP | done | IPv4/IPv6 FIB, policy routing, ECMP, SACK, window scale, DHCPv6. |
| M85 libc Tier-A pass | retired | — |
| M86 Per-thread CPU accounting | done | Thread/process CPU clocks, tkill/tgkill, exit vs exit_group. |
| M87 Loader + Rust proc-macros | retired | — |
| M88 Kernel correctness | done | PROT_NONE guards, ext4 indirect blocks. |
| M89 LLVM libc++ | done | Shared libc++ everywhere; GCC shared libs removed. |
| M90 GCC-free toolchain | done | Pure LLVM cross, native and Rust toolchains. |
| M91 Skia | done | Standalone Skia (demo superseded with M52). |
| M92 musl libc | done | musl as dynamic libc, ring-3 init/netd, ext4 primary root. |
| M93 Ring 0 Cleanup | done | In-kernel dynamic linker and build orchestrator removed. |
| M94 Foreign Userspace | done | `init=`, all-dynamic rootfs, stock Alpine minirootfs boots. |
| M95 LKM framework | done | W^X module loader, init/finit/delete_module, fs/HDA modules. |
| M96 LKM network + params | done | Protocol registry, module params, modules.dep/alias, IPv6 as module. |
| M97 GNU-free ISO | done | Limine, bmake, samurai, curl, zsh. |
| M98 Driver Infrastructure | done | netconsole, PAT/WC, PCI caps, MSI/MSI-X, stolen memory decode. See [driver-infrastructure.md](driver-infrastructure.md). |
| M99 linuxkpi layer | done | Own headers: idr, workqueue, dma-mapping, bounce, IOMMU-aware. |
| M100 DRM Core | done | dma-fence, GPU scheduler, sg-backed GEM on virtio-gpu. |
| M100a DMA bounce pool | done | Boot-reserved <4 GiB pool with stats. |
| M100b IOMMU (VT-d) | done | DMAR, second-level tables, NVMe domain, fault blocking. |
| M100c IOMMU domains/IR | done | Per-device domains, ACS/ARI grouping, interrupt remapping. |
| M100d AMD-Vi | done | IVRS, device table, command ring, NVMe in translated domain. |

## M101: linuxkpi for DRM — run upstream drivers unmodified

Vendor drivers are imported verbatim; every fix goes into our shim.

- [x] Upstream Linux 6.6 `drivers/gpu/drm` core (41 objects) builds unmodified on our primitives (kref, ww_mutex, xarray, RCU, kobject…).
- [x] In-kernel client proves atomic commits by reading scanout pixels; `/dev/dri/card1` served to ring 3, DRM master lease works.
- [ ] `partial` Hardware rendering as a second path (software stays first-class, chosen by `render-select.sh`): accelerated frame not yet produced — the node serves no `DRM_IOCTL_VIRTGPU_*`. See [render-path.md](render-path.md).

## M102a: Intel i915 (Gen8/Gen9.5) + Mesa iris

Detail in [i915-gen9-passthrough.md](i915-gen9-passthrough.md).

- [x] i915 imported unmodified; sway drives the passed-through UHD 630 at 1920x1080 ([image](images/m102a-sway-on-monitor.jpg)) and survives client churn.
- [x] GT runs (execlists, GGTT/PPGTT, completion IRQs); `EXECBUFFER2` served; `gl_probe` renders with Mesa iris.
- [ ] `planned` Compositor submissions fail: sway on gles2/iris gets `-ENOSPC` from `eb_reserve` (softpin binding path).
- [ ] `planned` Bare metal on Gen8 laptop with netconsole logs.

## M102b: amdgpu on RX 6600 (render-only) + radeonsi

- [ ] `planned` Build without DC; scanout on GOP framebuffer, render offscreen and blit.
- [ ] `planned` PSP firmware, SMU 11, GFX10.3 KIQ/MQD, GPUVM; VRAM windowing behind a 256 MB BAR.
- [ ] `planned` `libdrm_amdgpu` + `libLLVM.so` with AMDGPU target; gaps fixed in M101 shim.

## M102c: nouveau

- [ ] `planned` Pick generation (pre-Turing without signed firmware vs GSP); import unmodified, fix the shim.

## M104: Alpine packages

- [x] `bpkg`, the in-guest package manager, retired in M121.
- [x] 49 of 54 from-source ports replaced by pinned Alpine packages.
- [x] The last from-source ports (`busybox`, `openrc`, `libcxx`, `rust`) moved to Alpine or were dropped in M121; only `musl` headers for `b1cc` are built.

## M105: PAM

- [x] OpenPAM + `pam_unix.so`; dropbear authenticates through PAM.

## M106: DNS resolver

- [x] Outbound name resolution (fixed UDP source port, `recvfrom`/`recvmsg` sender address).
- [x] `/dev/fd`, `/proc/self/fd/N` opens and 64 KiB pipes.

## M107: BusyBox applets blocked on kernel subsystems

- [x] Netlink route sockets, VTs, loop devices, `/proc` maps/fd, kmsg/syslog, inotify extensions, RTC, watchdog, SMBus.
- [ ] `wontfix` MTD/UBI applets (no flash on any target; later partly covered by M114 NOR MTD).

## M108: Hand base tools to BusyBox

- [x] `su`/`passwd`/`login`/`id` etc. are BusyBox applets with a safe setuid copy; shadow locking race fixed.
- [x] BusyBox init as PID 1 with OpenRC; execve refreshes creds/caps/fsuid.

## M109: Alpine applet parity

- [x] 283 of 321 applets built and each proved through `/bin`.
- [x] AF_PACKET, VLAN/bridge/bonding/gretap, four namespace kinds with veth, pivot_root, uevent netlink for `mdev`, per-namespace IPv4 config.
- [x] Single-device gaps triaged (`wontfix`: rfkill, floppy, `i2ctransfer`).
- [ ] `partial` Namespaces still share IPv6 interface state, allow one IPv4 address each, share one `EADDRINUSE` table, and DHCP runs only in the initial namespace.

## M110: Unix block-device names

- [x] `sda`/`vda`/`nvme0n1` naming derived from enumeration index; old names removed; USB storage in the `sd` sequence.
- [x] Device selection by bus/content instead of name prefix; live-USB loop root switch fixed (`tests/liveusb.sh`).

## M111: Debian userspace and Linux-shaped boot log

- [x] Debian bookworm (glibc, dash, coreutils, sysvinit PID 1) boots unmodified (`make debian-image`, `make debian-smoke`).
- [x] Timestamped, levelled, subsystem-prefixed kernel log shared by console, `dmesg` and `/dev/kmsg`.

## M112: systemd as PID 1

- [x] Debian systemd 252 reaches `graphical.target` (32/32 checks; `make systemd-image`, `make systemd-smoke`).
- [x] cgroup v2 (`pids` enforced only), mount propagation/bind/remount, real devtmpfs; ~40 Linux-ABI defects fixed.
- [x] Debian Weston draws on `/dev/dri/card1` ([image](images/m112-debian-weston.png)), `tests/debian-graphics-smoke.sh` 12 checks.
- [ ] `partial` PCI topology under `/sys/devices` published, but no `driver` link (kernel records no binding).
- Note: `tests/systemd-smoke.sh` does not rebuild `debian-systemd.ext4`; run `PROFILE=systemd sh tools/images/mk-debian-image.sh` first.

## M113: KDE Plasma

- [x] kwin_wayland + plasmashell on real DRM via elogind/eudev ([image](images/m113-plasma-drm.png)); `tests/kde-smoke.sh`.
- [x] Boot to painted desktop 194 s → ~16 s; evdev input and DRM framebuffer console before the compositor.
- [ ] `partial` kwin uses the legacy modeset path (`CURSOR_PLANE_HOTSPOT` is Linux 6.7; universal planes not offered).
- [ ] `partial` Remaining costs: dbus/elogind session stalls, vmm read-lock per copyin. (Dirty pages now go through the `pcflush` writeback thread; the lkpi header macro warnings were fixed in M121.)

## M114: The layers under the missing applets

- [x] `readahead(2)`, `TIOCCONS`, software RAID (b1nix format, no resync), NBD client, ATAPI read-only CD-ROM.
- [x] MTD over CFI NOR flash (`/dev/mtd0`, `/dev/mtdblock0`); NAND out of scope.
- [x] `nsenter`/`unshare` enabled and checked for real isolation.

## M115: Kernel boot and syscall stacks

- [x] Boot and syscall stacks raised to 256 KiB with unmapped guard pages (the `iommu` flake was a boot-stack overflow).
- [x] Guards and peak usage are reported and asserted by the suite.

## M116: One page-table entry, two meanings

- [x] `VMM_SHARED` moved off bit 8 (GLOBAL) and APs no longer set `CR4.PGE`; fixes SMP `SIGILL`/`#GP` on valid instructions.
- [x] `SMP-CPUSTATE` censuses CR0/CR4/XCR0/EFER across CPUs; graphics lane back to 4 CPUs.

## M117: nice in the scheduler

- [x] `nice()` stored and round-trips; the check uses a shared deadline on one pinned CPU (`M46-SMOKE: ok nice-applied`).
- [x] Stride biases the picker on every CPU: x86_64 secondaries now preempt ring-3 ticks, which is what left a hog on an AP ignoring nice and affinity (`M80-SMOKE: ok nice-share`, ~9:1 for nice 0 vs 19).

## M118: Arch Linux userspace

Cancelled in M121: Debian is the glibc ABI lane, and a second systemd distribution duplicated it. The image and test scripts were removed; the nine kernel faults it found (`TCGETS2`, `/proc/self/fd` reopen flags, `CLONE_NEW*`, pidfds, `close_range`, `fchmodat2`, …) stay fixed. The new mount API it needed is tracked under the Debian lane.

## M119: Ask the processor instead of guessing

- [x] `/proc/cpuinfo` names the real CPU (CPUID brand / `MIDR_EL1`).
- [x] aarch64 CSPRNG seeded from `RNDR`; `TCR_EL1.IPS` from `ID_AA64MMFR0_EL1.PARange`.
- [x] x86_64 TSC frequency from CPUID 15h (16h base frequency when the crystal is unreported), PIT calibration as fallback.
- [x] `flags` (x86_64, CPUID) / `Features` (aarch64, ID registers) line in `/proc/cpuinfo`; `M80-SMOKE: ok cpu-flags` checks it against the processor.

## M120: Linux's own filesystems, through linuxkpi

Upstream Linux 6.6 `fs/btrfs`, `fs/ext4`, `fs/jbd2` built unpatched on our shim. Detail in [linuxkpi-fs.md](linuxkpi-fs.md).

- [x] 105 imported TUs link (`B1NIX_FS_IMPORT=btrfs`, `=1` adds ext4).
- [x] `initial` btrfs mount/read/write verified by host `btrfs check`; `mount -t btrfs-lkpi` bridges b1nix VFS to it.
- [x] The root filesystem is the imported btrfs (`mkfs.btrfs --rootdir`; `ROOT_FS=ext4` still builds the old image). x86_64 smoke 1419/1 on it. Bugs a real root exposed: out-of-order spinlock release re-enabled IRQs, `schedule()` spun instead of sleeping, linked inodes evicted with their delalloc data, lookup nodes shared page-cache keys, preempt count per CPU. Detail in [linuxkpi-fs.md](linuxkpi-fs.md).
- [ ] `partial` aarch64 root on the imported btrfs: user page faults now read with IRQs on, but three lanes still hit memory corruption (page-cache LRU, `end_bio_extent_readpage` NULL, `set_mask_bits` alignment). aarch64 keeps an ext4 root meanwhile (1359/3).
- [ ] `planned` Move ext4 to Linux's ext4 through lkpi and retire the native driver (the M14 data disks and `ROOT_FS=ext4`); needs buffer-head write helpers (`block_page_mkwrite` is still `-EOPNOTSUPP`).

## M121: Kernel only

- [x] Own apps removed: `bpkg`, `b1fetch`, installer, `mc`/`ne` (M16).
- [x] BusyBox is Alpine's 1.36.1 at `/bin/busybox`; `flash_erase`, `getfattr`, `lsblk`, `uuidgen` come from their own packages.
- [x] Kernel gaps the real tools exposed: `LOOP_CONFIGURE`, `/proc/mtd` and MTD char numbers, `modules.builtin`.
- [x] OpenRC is Alpine's 0.54 under BusyBox init; the `openrc-init` PID 1 instance is gone (Alpine does not build it), IOMMU instances boot the default init.
- [x] libc++, libunwind and compiler-rt builtins are Alpine's (LLVM 17; libc++abi needs `libgcc_s`, accepted).
- [x] Own native clang/Rust toolchain builds and the `b1nix-pkgs` download removed; nothing third-party is built from source.
- [x] Self-host (M26) on Alpine's clang17/lld with the host build's own per-TU commands: 653/653 compile and the link succeeds in-guest.
- [x] execve no longer drops arguments past 256 (a 684-argument link ran on the first 256 objects); oversized vectors are E2BIG.
- [ ] `partial` The self-hosted kernel.elf did not boot: native ext4 lost data written into a file extended by ftruncate (`M14-SMOKE: ext4-shared-mmap-durable` still fails on the native-ext4 data disk). The build disk is btrfs now; boot of the guest-built kernel not yet re-run.
- [x] aarch64 builds the imported btrfs and prints debug tracing on test boots (the M40 personality line).
- [ ] `partial` aarch64 smoke 1357/0, but one run in two wedged sys/posix: READY tasks not picked and futex wakes missed under TCG; not yet traced.
- [x] Wall clock no longer runs backwards: NTP slewed by stepping whole seconds; now one monotonic-based wall clock on both arches, NTP offset in ns, slew at <=500 ppm.
- [x] `telinit` and the fake M39 inittab markers removed (M39 keeps its real serial-tty checks).
- [x] The IOMMU instances end with `reboot -f`; the check passes only when QEMU (-no-reboot) then exits on its own.
- [ ] `planned` Move the tests to the Linux ABI, then remove the native b1nix syscall ABI.
- [ ] `planned` Debian lane to full parity as the glibc ABI check (needs the post-`mount(2)` API); the Arch lane was dropped.
