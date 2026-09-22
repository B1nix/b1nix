# Kernel roadmap

Status: `[x]` completed · `initial` usable first implementation · `partial`
incomplete or limited · `planned` not implemented · `deferred` postponed ·
`wontfix` declined.

This is the kernel's roadmap. The distribution built on it has its own, in
[../distro/roadmap.md](../distro/roadmap.md); its phases name the kernel
milestones they block on.

Userspace is not written here. It comes from existing distributions — Debian
trixie for the distribution itself and the glibc ABI lane, Alpine for the fast
smoke image. The repository keeps the kernel, its tests, the image and test
scripts, `b1cc`, and the distribution's packaging. See M121 for how the
userspace of this tree was dropped, and the distribution roadmap for what
replaced it.

AArch64 is a second target of this same kernel, not a milestone: each gap
belongs to the milestone that owns the mechanism.

What the finished milestones built is described by subject, a few milestones
per guide: [platforms.md](platforms.md) (targets, AArch64, distribution
userspace), [memory-and-scheduling.md](memory-and-scheduling.md),
[processes-and-system-calls.md](processes-and-system-calls.md),
[filesystems-and-storage.md](filesystems-and-storage.md),
[networking.md](networking.md),
[isolation-and-security.md](isolation-and-security.md) and
[drivers-and-graphics.md](drivers-and-graphics.md). Build rules are in
[build-conventions.md](build-conventions.md). Version numbers, and which one
answers which question, are in [../versioning.md](../versioning.md).

## Closed milestones

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
| M63 Sandbox | done | seccomp-bpf, NO_NEW_PRIVS, every namespace kind; completed by M123. |
| M64 Clang/LLVM Toolchain | done | Cross clang++, native in-QEMU clang. |
| M65 Install to Disk | cancelled | Installer and disk-image script removed in M121; a distribution installs itself. |
| M66 Chromium Frontend | cancelled | Userspace; superseded by the distribution's browser (M121). |
| M67 Rust Toolchain Port | retired | `x86_64-unknown-b1nix` target spoke the native ABI; its blob and checks are in `archive/` (M121). |
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
| M80 Kernel ptrace + Crash Capture | done | Full `ptrace(2)`, Yama, `/proc/<pid>/task`, XSAVE state, crash capture, fork child cmdline. |
| M81 Chromium GPU Acceleration | cancelled | Userspace (M121); the kernel side is M101/M102. |
| M82 System NSS / Kerberos | cancelled | Userspace libraries come from the distribution (M121). |
| M83 Unicode ctype | done | Provided by musl (M92). |
| M84 IP routing + TCP | done | IPv4/IPv6 FIB, policy routing, ECMP, SACK, window scale, DHCPv6. |
| M85 libc Tier-A pass | retired | — |
| M86 Per-thread CPU accounting | done | Thread/process CPU clocks, tkill/tgkill, exit vs exit_group. |
| M87 Loader + Rust proc-macros | retired | — |
| M88 Kernel correctness | done | PROT_NONE guards. |
| M89 LLVM libc++ | retired | libc++ comes from Alpine (M121). |
| M90 GCC-free toolchain | done | Pure LLVM cross toolchain. |
| M91 Skia | retired | Userspace (M121). |
| M92 musl libc | done | musl as dynamic libc, ring-3 init. |
| M93 Ring 0 Cleanup | done | In-kernel dynamic linker and build orchestrator removed. |
| M94 Foreign Userspace | done | `init=`, all-dynamic rootfs, stock Alpine minirootfs boots. |
| M95 LKM framework | done | W^X module loader, init/finit/delete_module, fs/HDA modules. |
| M96 LKM network + params | done | Protocol registry, module params, modules.dep/alias, IPv6 as module. |
| M97 GNU-free ISO | done | Limine ISO. |
| M98 Driver Infrastructure | done | netconsole, PAT/WC, PCI caps, MSI/MSI-X, stolen memory decode. |
| M99 linuxkpi layer | done | Own headers: idr, workqueue, dma-mapping, bounce, IOMMU-aware. |
| M100 DRM Core | done | dma-fence, GPU scheduler, sg-backed GEM on virtio-gpu. |
| M100a DMA bounce pool | done | Boot-reserved <4 GiB pool with stats. |
| M100b IOMMU (VT-d) | done | DMAR, second-level tables, NVMe domain, fault blocking. |
| M100c IOMMU domains/IR | done | Per-device domains, ACS/ARI grouping, interrupt remapping. |
| M100d AMD-Vi | done | IVRS, device table, command ring, NVMe in translated domain. |
| M101 linuxkpi for DRM | done | Upstream DRM core unmodified; atomic commits, `/dev/dri/card1`, master lease; virgl GLES on the host GPU draws a composed frame (`RENDER-SMOKE: ok accel-frame`). |
| M102a Intel i915 + Mesa iris | done | i915 from Linux 6.18.51 unmodified; sway on iris on the passed-through UHD 630 and on a UHD 620 laptop panel over PXE; fence arrays for multi-fence flips. |
| M104 Alpine packages | done | From-source ports replaced by pinned Alpine packages; `bpkg` retired. |
| M105 PAM | done | OpenPAM + `pam_unix.so`; dropbear authenticates through PAM. |
| M106 DNS resolver | done | Outbound name resolution, `/dev/fd`, `/proc/self/fd/N`, 64 KiB pipes. |
| M107 BusyBox applets blocked on kernel subsystems | done | Netlink route, VTs, loop, kmsg, inotify, RTC, watchdog, SMBus; MTD/UBI `wontfix`. |
| M108 Hand base tools to BusyBox | done | BusyBox `su`/`passwd`/`login`, BusyBox init as PID 1 with OpenRC. |
| M109 Alpine applet parity | done | 283 of 321 applets; AF_PACKET, VLAN/bridge/bond/gretap, veth and four namespace kinds, pivot_root, `mdev` uevents; per-namespace TCP/UDP, several IPv4 addresses, IPv6 state, `udhcpc`. |
| M110 Unix block-device names | done | `sda`/`vda`/`nvme0n1` from enumeration; device selection by bus/content. |
| M111 Debian userspace and Linux-shaped boot log | done | Debian bookworm boots unmodified; levelled, timestamped kernel log. |
| M112 systemd as PID 1 | done | Debian systemd 252 reaches `graphical.target`; cgroup v2, mount propagation, devtmpfs, Weston on DRM, PCI driver links. |
| M113 KDE Plasma | done | kwin_wayland + plasmashell on atomic DRM via elogind/eudev; painted desktop in ~7 s (from 194 s); `tests/kde-smoke.sh`. |
| M114 The layers under the missing applets | done | `readahead`, `TIOCCONS`, software RAID, NBD, ATAPI, CFI NOR MTD, `nsenter`/`unshare`. |
| M115 Kernel boot and syscall stacks | done | 256 KiB stacks with guard pages; peak usage asserted. |
| M116 One page-table entry, two meanings | done | `VMM_SHARED` off the GLOBAL bit, no `CR4.PGE` on APs; fixed SMP `SIGILL`. |
| M117 nice in the scheduler | done | Stride weights by nice on every CPU; APs preempt ring-3 ticks. |
| M118 Arch Linux userspace | cancelled | Duplicated the Debian lane (M121); the kernel faults it found stay fixed. |
| M119 Ask the processor instead of guessing | done | Real CPU name, `RNDR`, PARange, TSC from CPUID, cpuinfo flags/Features. |
| M120 Linux's own filesystems, through linuxkpi | done | btrfs, ext4 and jbd2 from Linux 6.18.51 unpatched (moved from 6.6 with the DRM core and i915); btrfs root on both arches, ext2/3/4 are the imported ext4, native ext drivers removed. |
| M121 Kernel only | done | Own userspace replaced by Alpine and Debian packages, native syscall ABI archived (`archive/kernel/native-abi/`); self-hosted kernel boots; Debian glibc lane 41/41; aarch64 wedges, zstd btrfs and a dentry-list heap corruption fixed. |
| M122 Known corruption and SMP defects | done | Block-cache writeback claims, PMM metadata off the AP trampoline, page-cache insert race, task claim by lease CAS, no double reap; proven by `fsverify` at 6 CPUs (14/14) and soak runs. |
| M123 Namespaces for containers | done | User, IPC, cgroup, PID (real init) and time namespaces; mount table tracks attachment for binds and `pivot_root`; `unshare`, `bwrap`, rootless `podman` and `systemd-nspawn` run. |
| M124 Missing modern system calls | done | `openat2`, `pidfd_getfd`, `kcmp`, futex2, `process_mrelease`/`process_madvise`, `sched_setattr`; keyrings, Landlock, PKU, `memfd_secret`; `statmount`/`listmount`, `cachestat`, `remap_file_pages`, mempolicy, ext4's own `quotactl`; a vDSO on both arches. Probed 66/66 in the Debian lane. |
| M125 io_uring | done | Every opcode this uapi names but `RECV_ZC`, SQPOLL/IOPOLL, provided buffers in both shapes, multishot, direct descriptors, personalities and restrictions; a completion thread per ring, so a request completes while its owner watches the ring from userspace. liburing 2.12: 89 → 129 pass over all 217. One rare panic in part 3 of that suite is unexplained — see [processes-and-system-calls.md](processes-and-system-calls.md). |
| M126 Observability | done | `perf_event_open` with the CPU's own PMU, sampling, `inherit`, `enable_on_exec` and group reads — the distribution's `perf stat`/`record`/`report` run on it; `userfaultfd`; `fanotify` with permission events; eBPF as an interpreter with a path-walking verifier (`partial`: no JIT, BTF, CO-RE or kprobes). The ad-hoc `kprof` profiler is gone. |
| M127 Resource control | done | cgroup v2 `memory`/`cpu`/`io`/`pids`, an OOM killer that ranks by RSS and `oom_score_adj`, PSI, zram and zswap, and reclaim inside a cgroup with every swapped page charged to its owner. Proved on Alpine and against Debian's own systemd units. |

## M102b: amdgpu on RX 6600 (render-only) + radeonsi

- [ ] `planned` Build without DC; scanout on GOP framebuffer, render offscreen and blit.
- [ ] `planned` PSP firmware, SMU 11, GFX10.3 KIQ/MQD, GPUVM; VRAM windowing behind a 256 MB BAR.
- [ ] `planned` `libdrm_amdgpu` + `libLLVM.so` with AMDGPU target; gaps fixed in M101 shim.

## M102c: nouveau

- [ ] `planned` Pick generation (pre-Turing without signed firmware vs GSP); import unmodified, fix the shim.

## M128: Large memory and NUMA

Per-subject detail: [memory-and-scheduling.md](memory-and-scheduling.md).

- [x] `done` A 72 GiB guest boots and runs the whole suite on a 27 GiB host (`SMOKE_BIGMEM=1`). It found the defect that mattered: with that much RAM the 64-bit PCI window sits above the direct map, and every driver reaching a BAR through it faulted in ring 0.
- [x] `done` Five-level paging behind `b1nix.la57`: a PML5 whose entries 0 and 511 both name the PML4, so every existing four-level walk stays correct. Proved by the `la57` lane (TCG `-cpu max,la57=on`, 856 checks), which also found the APs booting with their caches disabled.
- [x] `done` NUMA from SRAT and SLIT: per-node free lists in the buddy allocator, allocation from the running CPU's node, and a node lookup that costs one compare on a single-node machine.
- [x] `done` `mbind`/`set_mempolicy`/`get_mempolicy`/`set_mempolicy_home_node` really place pages — MPOL_BIND enforced, MPOL_INTERLEAVE by page offset, MPOL_MF_MOVE migrating — with `/sys/devices/system/node` for `numactl`. Proved on a two-node guest, per page and by each node's free memory moving.
- [x] `done` Transparent huge pages for anonymous memory, on both arches and off unless `b1nix.thp` asks: a 2 MiB directory entry on x86_64, a level-2 block descriptor on aarch64, installed only by the fault path — everything else splits a block first or handles all 512 pages. A `fork` shares a block copy-on-write and a whole-block `mprotect` keeps it, and khugepaged collapses the 4 KiB ranges the fault path can never reach. Twelve checks on bytes per arch, including the three that closed the last gaps: a recycled address range is block-backed again, a cgroup whose memory is all blocks is reclaimed rather than killed, and a collapsed range holds every byte it held before. Found on the way: a split copied the block's read-only flag into the page table's own directory entry, which overrules every leaf under it, and nothing on aarch64 had ever seeded the buddy allocator, so no contiguous block could be allocated at all.

## M129: Power management

- [x] `done` Idle: one `cpuidle_enter` for every park (MWAIT C-states where the CPU has them, HLT/WFI otherwise) with per-CPU counters under `/sys/devices/system/cpu/cpuN/cpuidle`, and a tickless idle CPU — an idle second costs 620 timer interrupts instead of 1998, a busy one still takes its thousand. `/proc/interrupts` gained `LOC`, `/proc/b1nix-tick` the rate and the cap.
- [x] `partial` Frequency: HWP, or the older ratio request, read through faulting-safe MSR accessors, with a writable `scaling_governor` and an honest `none` on a guest whose hypervisor hides the leaves. Not yet run on hardware that has HWP; ACPI `_PSS` is not read.
- [x] `partial` Suspend: s2idle end to end on both arches — `/sys/power/state`, a freezer that is not SIGSTOP, and the RTC alarm as a wake source — proved by eleven checks including a hole of the right length in a frozen child's own clock, and by util-linux's `rtcwake -m freeze` on the Debian lane. ACPI S3 is absent rather than broken (it needs device suspend/resume and a resume trampoline), and a plain `echo freeze` with no alarm armed sleeps to the ten-second ceiling because the input wake source counts as armed on a console nobody types at.
- [x] `partial` Battery and thermals: no longer blocked — M134's interpreter reads `_BST`/`_BIF`/`_PSR`/`_TMP` and publishes `/sys/class/power_supply` and `/sys/class/thermal` for what the firmware declares. No machine here declares any, so those paths are unexercised; hotkeys and the idle-power comparison are untouched.

## M130: More hardware through linuxkpi

- [ ] `planned` Wi-Fi: iwlwifi with mac80211 and cfg80211 imported unmodified, nl80211 for `iw`/`wpa_supplicant`/iwd; an Intel Wireless-AC 8265 associates with WPA2/WPA3.
- [ ] `planned` Storage and network drivers from Linux through the shim (nvme, e1000e, igb/igc, r8169) alongside or in place of the native ones where Linux's version is better.
- [ ] `planned` Bluetooth (btusb, HCI core) and USB audio/webcam class drivers where the shim already carries USB.
- [ ] `planned` GPUs continue as M102b (amdgpu) and M102c (nouveau); a patch to imported source still means a shim bug.

## M131: KVM

- [ ] `planned` Import Linux's KVM (x86 core + VMX, then SVM) through linuxkpi; `/dev/kvm` with the vCPU ioctl ABI.
- [ ] `planned` EPT/NPT second-level paging, in-kernel LAPIC/IOAPIC, eventfd-based irqfd/ioeventfd, VMX nested off.
- [ ] `planned` Distribution QEMU with `-accel kvm` boots Alpine, then b1nix itself, inside b1nix; on bare metal and nested under the host's KVM.
- [ ] `planned` aarch64 KVM (VHE) after x86 works.

## M132: Xperia 5 without the 64 MiB boot image limit

Detail in [xperia5-ufs-usb.md](xperia5-ufs-usb.md).

- [x] UFS host driver (PCI + Qualcomm `qcom,ufshc`): keeps the bootloader's link, else HCE reset + link startup; LUNs as `sd*`, GPT in 4 KiB units with partition names, writes fenced to `b1nix.ufs-rw` / `b1nix-root` partitions. UFS-SMOKE on QEMU `-device ufs`; `/` from a GPT partition boots to login.
- [x] `make bahamut-ufs` + `flash_ufs_rootfs.sh`: full rootfs on `system_a`, trimmed ramdisk kept as rescue.
- [x] UFS on the phone: `/` from `system_a`, boots to login (DMA confined to the hypervisor-allowed window at 0xf0000000).
- [x] DWC3 USB gadget (CDC-ECM `usb0`, 172.16.42.1): enumerates on macOS; ping (4-30 ms), `ssh root@172.16.42.1` and netconsole work.
- [x] The page allocator's window on SM8150 is clipped to ABL's `/memory` banks: it covered a 52 MiB hypervisor hole (0xbcc00000-0xc0000000) that froze the CPU on first touch.
- [ ] `partial` `reboot bootloader`: `LINUX_REBOOT_CMD_RESTART2` reaches the kernel (was EINVAL; iommu smoke lane restarts through it), the SPMI PMIC arbiter driver writes the PON restart reason and IMEM word and configures a warm reset — but ABL still boots normally and every reset lands cold. Open: which value/reset path this ABL honours; slot A's GPT tries are not reset by b1nix (`gpt` in `b1nix.ufs-rw` opens the tables for that).

## M133: Close the ABI gaps

Every open row of [abi-gaps.md](abi-gaps.md) that no other milestone owns,
collected as one piece of work because they share a definition of done: the
Debian lane green with no workaround, and that table shorter. This is the
milestone the distribution waits on — the phases in
[../distro/roadmap.md](../distro/roadmap.md) trip over these, not over NUMA or
Wi-Fi — so it runs before M128-M131.

- [ ] `planned` A relocatable kernel, so the distribution boots under UEFI: multiboot2 tag 10 and page tables that do not assume a load at 1 MiB. Every machine sold in the last fifteen years boots that way; today the ISO goes through Limine's BIOS path.
- [ ] `planned` The four Debian units that still fail: `/proc/self/mountinfo` naming a mount by the path the caller used rather than the path it was recorded with (`tmp.mount`, `run-lock.mount`), `sched_setscheduler` accepting what `CPUSchedulingPolicy=` asks for (`e2scrub_reap.service` exits `214/SETSCHEDULER`), and `systemd-sysusers`.
- [ ] `planned` `apt-get update` against a `file:` repository: `symlink()` with an empty target, and whatever apt's `store:` method does to read an index.
- [ ] `planned` The AHCI probe stops hanging on a port with an empty ATAPI device, so QEMU's q35 boots without `-machine pc`.
- [ ] `planned` The io_uring remainder: `RECV_ZC`, io-wq affinity, NAPI busy-poll, buffer cloning, ring resizing, memory regions and the query interface, and a `SEND_ZC` that pins the caller's pages instead of copying.
- [ ] `planned` The observability remainder: tracepoints and kprobes, and eBPF with a JIT, BTF and CO-RE, so a `bpftrace` script or a CO-RE toolchain's program loads instead of being refused with a reason.
- [ ] `planned` Proof: the Debian and systemd lanes pass with no lane-side workaround, an ISO boots on a UEFI machine, and every row this milestone names is gone from the gap table.

## M134: ACPI methods

Per-subject detail: [platforms.md](platforms.md).

- [x] `done` An AML interpreter: the DSDT and every SSDT are loaded and the object tree built — scopes, devices, methods, packages, fields, operation regions. On QEMU's own firmware that is 354 objects, 105 methods, 53 devices, with no term the loader could not decode.
- [x] `done` An evaluator for what those objects need: the opcodes, control flow, conversions, method calls with arguments and locals, and field access through SystemMemory and SystemIO. A runaway method spends a step budget and errors rather than hanging.
- [x] `done` A refusal is a refusal: PCI config, embedded-controller, SMBus and CMOS regions return an error that propagates out of the evaluation and is recorded in `/proc/b1nix-acpi`, never a zero.
- [x] `partial` The consumers: `/sys/class/power_supply/{BAT0,AC0}` and `/sys/class/thermal/thermal_zoneN` from `_STA`/`_BIF`/`_BST`/`_PSR`/`_TMP`, published only for devices the firmware declares — and no machine here declares any, so the code is written and unexercised.
- [ ] `planned` What it is not used for yet: powering the machine off through `\_S5`, `_PSS` for cpufreq, GPE dispatch and `Notify` handlers, `_PRT` for interrupt routing.
- [x] `done` Proof: thirteen checks against QEMU's own DSDT — `\_S5_` a package of four, `_HID` exactly `PNP0A03`, a 156-byte `_CRS`, a method with an argument that writes a CPU selector to an I/O port and reads the enabled bit back (0x0F for CPU 0, 0 for CPU 200), and a region refusal recorded rather than faked. On aarch64: no tables, roots only, nothing published.
