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
| M128 Large memory and NUMA | done | A 72 GiB guest on a 27 GiB host (the 64-bit PCI window above the direct map faulted every BAR access), five-level paging behind `b1nix.la57`, NUMA from SRAT/SLIT with per-node free lists and `mbind`/`set_mempolicy` that really place pages, and transparent huge pages for anonymous memory on both arches with khugepaged, huge COW at `fork` and cgroup-aware blocks. |
| M129 Power management | done | cpuidle with MWAIT C-states and a tickless idle CPU (620 timer interrupts an idle second, not 1998), cpufreq over HWP, the ratio request or ACPI `_PSS`/`_PCT`, s2idle on both arches and ACPI S3 on x86_64 — CPU, clocks, secondaries and every device's PCI header and driver state brought back, proved by a modeset, an 880 Hz tone captured after the resume and a re-addressed USB keyboard — and a battery and thermal zone read from AML against a firmware that declares them. What manages power rather than merely performing it is M135. |
| M133 Close the ABI gaps | done | UEFI boot from a relocatable kernel, the last failing Debian units, apt on a `file:` repository, q35 without `-machine pc`; io_uring complete down to zero-copy receive, NAPI and affinity; `bpftrace` with kprobes, uprobes and BTF. |
| M134 ACPI methods | done | An AML interpreter and evaluator: DSDT and every SSDT loaded (354 objects on QEMU, no undecoded term), the opcodes, control flow, conversions, method calls and SystemMemory/SystemIO fields, a step budget instead of a hang, and a refused region (PCI config, EC, SMBus, CMOS) propagating an error rather than a zero. It is what `/sys/class/power_supply` and `/sys/class/thermal` read and what cpufreq and the S3 sleep run on. `_PRT` interrupt routing is the one consumer still unwritten. |
| M135 Power management, the rest | done | The machine now manages its own power. ACPI events arrive: SCI, GPE and `Notify`, so the buttons, the lid, AC, the battery and the thermal zones report changes, and power-off goes through `\_S5`. `ondemand` follows the load under the firmware's `_PPC` ceiling, `_CST` states are chosen by a menu governor, and the thermal zones act on their trips (fans, passive throttling, a critical shutdown). The rest: hibernation to a swap area on x86_64, runtime PM (NVMe to D3hot), `/sys/power` as Linux lays it out, and a resume callback on every device, with in-flight disk I/O brought through S3. aarch64 has neither S4 nor a deep sleep. |

## M102b: amdgpu on RX 6600 (render-only) + radeonsi

- [ ] `planned` Build without DC; scanout on GOP framebuffer, render offscreen and blit.
- [ ] `planned` PSP firmware, SMU 11, GFX10.3 KIQ/MQD, GPUVM; VRAM windowing behind a 256 MB BAR.
- [ ] `planned` `libdrm_amdgpu` + `libLLVM.so` with AMDGPU target; gaps fixed in M101 shim.

## M102c: nouveau

- [ ] `planned` Pick generation (pre-Turing without signed firmware vs GSP); import unmodified, fix the shim.

## M130: More hardware through linuxkpi

- [ ] `planned` Wi-Fi: iwlwifi with mac80211 and cfg80211 imported unmodified, nl80211 for `iw`/`wpa_supplicant`/iwd; an Intel Wireless-AC 8265 associates with WPA2/WPA3.
- [ ] `planned` Storage and network drivers from Linux through the shim (nvme, e1000e, igb/igc, r8169) alongside or in place of the native ones where Linux's version is better.
- [ ] `planned` Bluetooth (btusb, HCI core) and USB audio/webcam class drivers where the shim already carries USB.
- [ ] `planned` GPUs continue as M102b (amdgpu) and M102c (nouveau); a patch to imported source still means a shim bug.

## M131: KVM

- [x] Import Linux's KVM (x86 core + VMX and SVM) through linuxkpi; `/dev/kvm` with the vCPU ioctl ABI. Unmodified 6.18 KVM picks VMX on the KVM lanes and SVM on the pku lane's AMD model (m131_kvm_smoke on both).
- [x] EPT/NPT second-level paging, in-kernel LAPIC/IOAPIC/PIT, eventfd-based irqfd/ioeventfd, nested virtualization off. Guest timers fire from the tick; irqfd/ioeventfd use the process's own eventfds.
- [ ] `partial` Distribution QEMU with `-accel kvm` boots a distribution kernel, then b1nix itself, inside b1nix; on bare metal and nested under the host's KVM. Nested under the host (Debian lane): SeaBIOS guest, Debian's cloud kernel on 2 vCPUs with a virtio disk, b1nix itself on 2 vCPUs through GRUB. Open: bare metal untested.
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
[../distro/roadmap.md](../distro/roadmap.md) trip over these, not over Wi-Fi or
KVM — so it runs before M130-M132 and M135.

- [x] `done` A relocatable kernel, so the distribution boots under UEFI: the multiboot2 header says the image may be moved, the early boot derives the offset from its own program counter, and the kernel's own page-table window follows it. Under OVMF the loader places it at +8 MiB and the machine reaches OpenRC; under BIOS the offset is zero and nothing changed. `tests/uefi-smoke.sh` is that lane.
- [x] `done` The four Debian units that still failed. Three are closed and were not what the table said they were: `e2scrub_reap.service` exited 225 (EXIT_NETWORK), not 214, because `PrivateNetwork=` needs a record lock on a socketpair and locks were refused on anything but a file; `systemd-update-utmp` died because `NETLINK_AUDIT` handed out a socket and then failed the send instead of refusing the family; `systemd-sysusers` starts. The two mount units are not fixed, but the boot no longer stops behind them: the mount-table notification was firing over and over until sd-event rate-limited systemd's own monitor and dropped the change, and with one event per descriptor the installed system reaches `multi-user.target`. The mount units were graded `protocol` because systemd watches libmount's epoll, and asking that nested epoll whether it was ready consumed the mountinfo change it was about to report; a nested epoll now only looks. `tmp.mount`, `run-lock.mount` and `sys-kernel-tracing.mount` mount cleanly.
- [x] `done` `apt-get update` and `apt-get install` against a `file:` repository shared over 9p, in the distribution lane: 9p falls back to a read-only open on a read-only share, and apt's `store:` method reads its indexes.
- [x] `done` The AHCI probe stops hanging on a port with an empty ATAPI device, so QEMU's q35 boots without `-machine pc`: the probe's packet command is bounded and stops the port before its buffer goes back, while the I/O path keeps waiting for ever because its buffer is a caller's. The q35 lane carries an empty optical drive now, so the shape is exercised on every run.
- [x] `done` The io_uring remainder: io-wq and SQPOLL affinity (the ring's threads now run on any CPU), NAPI busy-polled waits, ring resizing, the query interface, and zero-copy receive (`IORING_REGISTER_ZCRX_IFQ`, `RECV_ZC`) with its area, refill ring and 32-byte completions. The data of `RECV_ZC` and `SEND_ZC` is copied — the ABI and buffer life cycle are Linux's, and `IORING_SEND_ZC_REPORT_USAGE` says so — because no NIC driver here splits headers or sends from scattered pages. Along the way: `MSG_WAITALL`, `SO_ZEROCOPY` with its error queue, `sendmsg` past 1 MiB, a TCP `shutdown(SHUT_WR)` that sends its FIN, and requests issued on readiness that never block the ring (see [processes-and-system-calls.md](processes-and-system-calls.md), [networking.md](networking.md)).
- [x] `done` The observability remainder: `bpftrace` runs — kprobe, uprobe, tracepoint and `BEGIN` programs, maps, `printf` through the output ring — on a verifier with state pruning and reference tracking, the helpers it calls (`probe_read*`, `get_stackid`, ring buffers, `perf_event_output`), `bpf_link`, the kprobe and uprobe PMUs, and BTF from the kernel's own DWARF.
- [x] `done` Proof: the Debian lane (116 checks) and the systemd lane (49) pass with no lane-side workaround, the ISO boots under OVMF, and the rows this milestone named are gone from the gap table.

## M135: Power management, the rest

M129 made the machine sleep, wake and scale: s2idle and ACPI S3, the P-states
the firmware declares, cpuidle with a tickless idle, and a battery and thermal
zone read out of AML. What it did not make is a machine that manages its own
power — nothing reacts to a closed lid, nothing lowers the clock because the
machine is idle, and nothing acts on a temperature. This milestone is that
remainder, collected because the pieces share one shape: the mechanisms exist
and nothing drives them.

Per-subject detail, item by item:
[memory-and-scheduling.md](memory-and-scheduling.md).

- [x] `done` ACPI events. The SCI handler, GPE dispatch and `Notify` bring the
  power and sleep buttons, the lid, AC, battery changes, thermal notifications
  and PCI hotplug into the kernel. Userspace sees them as evdev keys and
  `change` uevents. Power-off uses the firmware's `\_S5`.
- [x] `done` The resume remainder. Intel HDA, virtio-input, virtio-console, the
  i8042 keyboard and mouse, and VT-d and AMD-Vi all have resume callbacks; the
  IOMMUs resume first. MSI and MSI-X state is saved and restored. Disk I/O in
  flight survives the sleep: the freezer waits for a driver's critical section
  to end, and NVMe's resume no longer mistakes its emptied queue for completed
  commands. Both IOMMU lanes sleep once and prove it with remapped NVMe
  interrupts and a writer running through the S3.
- [x] `done` Frequency and idle respond to the machine. `ondemand` follows the
  load, `_PPC` caps it, and cpufreq uses Linux's `policy*` layout with
  statistics. `_CST` idle states are chosen by a menu governor from the
  predicted idle length, and each state can be disabled per CPU.
- [x] `done` Heat and charge are acted on. Thermal zones have `_CRT`, `_HOT`,
  `_PSV` and `_ACx`/`_ALx` trips, processor and fan cooling devices, passive
  throttling and a critical-temperature shutdown. The battery reads `_BIX`,
  takes a `_BTP` alarm, and offers `charge_behaviour` through `_BMD`/`_BMC`.
  ACPI defines no charge thresholds; on Linux those come from vendor drivers,
  so there is none here.
- [x] `done` Hibernation on x86_64, and the `/sys/power` surface. A snapshot of
  every used page is written to a swap area (`resume=`, `/sys/power/disk`) and
  restored by the next boot. `/sys/power` gains `mem_sleep`, the
  `wakeup_count` handshake, `sync_on_suspend` and `/sys/class/wakeup`. Runtime
  PM puts an idle NVMe in D3hot. Still missing on aarch64: hibernation (it
  needs secondary CPUs brought back with PSCI `CPU_ON` and the GIC ITS tables
  handed from the boot kernel to the image) and PSCI `SYSTEM_SUSPEND`, which
  QEMU does not implement.
