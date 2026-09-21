# Kernel roadmap

Status: `[x]` completed · `initial` usable first implementation · `partial`
incomplete or limited · `planned` not implemented · `deferred` postponed ·
`wontfix` declined.

This is the kernel's roadmap. The distribution built on it has its own, in
[../distro/roadmap.md](../distro/roadmap.md); its phases name the kernel
milestones they block on, and M127 is ahead of M125 because systemd's model
rests on cgroup v2.

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

## M102b: amdgpu on RX 6600 (render-only) + radeonsi

- [ ] `planned` Build without DC; scanout on GOP framebuffer, render offscreen and blit.
- [ ] `planned` PSP firmware, SMU 11, GFX10.3 KIQ/MQD, GPUVM; VRAM windowing behind a 256 MB BAR.
- [ ] `planned` `libdrm_amdgpu` + `libLLVM.so` with AMDGPU target; gaps fixed in M101 shim.

## M102c: nouveau

- [ ] `planned` Pick generation (pre-Turing without signed firmware vs GSP); import unmodified, fix the shim.

## M124: Missing modern system calls

Per-call state: [processes-and-system-calls.md](processes-and-system-calls.md).

- [x] `done` Process and threading: `openat2` (`RESOLVE_*`), `pidfd_getfd`, `kcmp`, futex2 (`futex_waitv`, `futex_wake`, `futex_wait`), `process_mrelease`, `process_madvise`, `sched_setattr`/`sched_getattr`.
- [x] `done` Security: the key retention service (`keyctl`, `add_key`, `request_key`), Landlock (ABI 3, enforced in the VFS and on exec), protection keys (x86 PKU: per-thread PKRU, `pkey_alloc`/`pkey_free`/`pkey_mprotect`, execute-only mappings, `SEGV_PKUERR`), and `memfd_secret`, whose pages are removed from every kernel mapping of physical memory.
- [x] `done` Filesystems and memory: `statmount`/`listmount` with never-reused mount ids, `cachestat`, `remap_file_pages`, `mbind`/`get_mempolicy`/`set_mempolicy`/`set_mempolicy_home_node` with single-node semantics, and `quotactl`/`quotactl_fd` on the imported filesystems — ext4's own quotas, enforced.
- [x] `done` A vDSO on x86_64 and aarch64: `clock_gettime`, `gettimeofday`, `time` and `clock_getres` without entering the kernel for the TSC/CNTVCT clocks (32 ns vs 262 ns per call on KVM); musl and glibc both use it.
- [x] `done` Each call is probed in the Debian lane by its result and errno (66/66) and in `m124_smoke` on both arches; protection keys are proved on a CPU that has them in the `pku` lane (QEMU TCG `-cpu max`), and quotas against the distribution's own `mkfs.ext4 -O quota`, `setquota`, `repquota` and `e2fsck`. The unmapped-syscall log line stays silent through a Plasma session. Chromium was not run.

## M125: io_uring

Per-feature detail: [processes-and-system-calls.md](processes-and-system-calls.md).

- [x] `done` `io_uring_setup`/`io_uring_enter`/`io_uring_register` against Linux 6.18.51's uapi header, vendored rather than retyped: the shared rings, `IORING_SETUP_SQPOLL` (a kernel thread that adopts the owner's address space and descriptor table, with `sq_thread_idle` and `IORING_SQ_NEED_WAKEUP`), `IOPOLL`, `CQE32`, `SQE128`, `NO_SQARRAY`, `DEFER_TASKRUN` and `R_DISABLED`. What is still refused is refused at setup with Linux's own errno: `SQ_AFF` (nothing can pin the thread), `ATTACH_WQ`, `NO_MMAP`, `REGISTERED_FD_ONLY`, `HYBRID_IOPOLL`, `CQE_MIXED`.
- [x] `done` 48 opcodes, and nothing else pretends to work: the filesystem set (`OPENAT`/`OPENAT2`/`STATX`/`FALLOCATE`/`FTRUNCATE`/`RENAMEAT`/`UNLINKAT`/`MKDIRAT`/`SYMLINKAT`/`LINKAT`), the socket set (`SENDMSG`/`RECVMSG`/`SEND_ZC`/`SENDMSG_ZC`/`SHUTDOWN`/`SOCKET`/`BIND`/`LISTEN`), `SPLICE`/`TEE`/`EPOLL_CTL`/`MSG_RING`/`WAITID`/`PIPE`/`FIXED_FD_INSTALL`, and the advisory pair. An unimplemented opcode is `EINVAL` and `IORING_REGISTER_PROBE` names exactly the set that works.
  Provided buffers come in both shapes (`PROVIDE_BUFFERS`/`REMOVE_BUFFERS` and `REGISTER_PBUF_RING`, selected with `IOSQE_BUFFER_SELECT` and reported in `IORING_CQE_F_BUFFER`), multishot poll/accept/recv/read with `IORING_CQE_F_MORE`, direct descriptors including `IORING_FILE_INDEX_ALLOC`, and the register operations `BUFFERS2`/`BUFFERS_UPDATE`/`FILES2`/`RESTRICTIONS`/`SYNC_CANCEL`/`FILE_ALLOC_RANGE`/`IOWQ_MAX_WORKERS`/`PBUF_STATUS`.
- [x] `done` Completion from the existing ISR→wakeup paths (M70) rather than a thread per request, and a link chain is issued in a **loop**: one C frame per chain, not per link — `fpos.t` links 2048 reads and one frame each ran off the 256 KiB kernel stack.
- [x] `done` Proof: 79 checks in the posix lane drive the rings by hand, and liburing 2.12's own 217 tests run in the Debian lane in six parts at 4 GiB. Against the same harness on the branch point: **89 → 106 pass, 73 → 45 fail, 48 → 28 skip, 7 → 13 timeout**; the baseline ran all 217, this tree runs 192 because the defect below kills two of the six boots. An intermediate build of the same feature set, whose boots happened not to hit it, measured **116 pass, 57 fail, 31 skip, 13 timeout over all 217** — that is what the features are worth once the defect is fixed. The extra timeouts are tests that used to stop at a refused setup and now reach a UDP `bind(port = 0)`, which this kernel does not give an ephemeral port — the gap `accept.t` already named. `fio --ioengine=io_uring` moves data, plain and with registered files and buffers.
- [ ] `open` **A heap use-after-free in the imported ext4, newly reachable.** Running liburing's two syzkaller reproducers `232c93d07b74` and `a0908ae19763` in one boot, together with the rest of that part of the suite, panics later in `__ext4_new_inode` (a register holding the allocator's poison) or in `ext4_writepages` at the closing `sync`. It needs BOTH reproducers — each alone is clean — and it disappears if `IORING_OP_OPENAT` is refused, because the reproducers then create no files. So the trigger is file creation under the process and memory churn two fork bombs make, and the defect is in the create path's error handling, not in io_uring; it is recorded here because io_uring is what made it reachable. Not root-caused. Reproduce with `DEBIAN_MEM_MB=4096 DEBIAN_EXTRA_CMDLINE=b1nix.liburing-part=1/6 sh tests/debian-smoke.sh`.

## M126: Observability

Per-feature detail: [processes-and-system-calls.md](processes-and-system-calls.md).

- [x] `done` `perf_event_open`: software counters, the CPU's own PMU (cycles, instructions, cache and branch events through CPUID's architectural leaf, on the fixed counters where a KVM guest only backs those), `PERF_TYPE_RAW` and the `HW_CACHE` pairs an architectural PMU really has. Timer-driven sampling with user call chains, the mmap'd ring buffer, `attr.inherit`, `enable_on_exec`, group reads and `PERF_FORMAT_LOST`. `/proc/sys/kernel/perf_event_paranoid` decides what an unprivileged caller may profile.
- [x] `done` `userfaultfd`: MISSING and write-protect faults handed to a monitor, `UFFDIO_COPY`, `ZEROPAGE`, `WAKE`, `WRITEPROTECT`, `REGISTER`/`UNREGISTER`, `poll(2)` and the non-blocking read. MINOR faults are refused (there is no page cache behind the mappings to show), and a monitor that closes its descriptor releases every waiter instead of freezing the process.
- [x] `done` `fanotify`: marks on a file or on a whole mount, `FAN_OPEN`/`ACCESS`/`MODIFY`/`CLOSE_WRITE`/`CLOSE_NOWRITE`, an open descriptor per event, and permission events (`FAN_OPEN_PERM`) whose `FAN_DENY` really makes `open(2)` return EPERM. The same hooks gave inotify the `IN_OPEN`, `IN_ACCESS` and `IN_CLOSE_*` events it never had.
- [x] `partial` eBPF: the whole instruction set in an interpreter, hash and array maps, ten helpers, `BPF_PROG_TEST_RUN`, and programs attached to a perf event with `PERF_EVENT_IOC_SET_BPF`. The verifier walks every path with a model of each register and refuses uninitialised reads, out-of-bounds stack and map access, a map value used before it is tested for NULL, and backward jumps. **No JIT, no BTF and no CO-RE**, no kprobe or tracepoint attach points, and no bounded loops — a program from `bpftrace` or a CO-RE toolchain will not load, and says so at load time with the reason in the verifier log.
- [x] `done` The ad-hoc kernel profiler is gone: `kprof`'s RIP histogram and its `b1nix.sysprof` boot flag were 200 lines answering the question perf now answers better, and `/proc/b1nix-kprof` says where to look instead. What stays there is what perf cannot answer here — the tick distribution, the interrupts-off sections, the wait sites.
- [x] `done` Proof, three ways. 22 checks in the posix lane (`m126_smoke`, `m126_uffd_smoke`, `m126_fanotify_smoke`, `m126_bpf_smoke`) each make the thing they measure happen: 120 ms of work retires 400 million instructions on the PMU and a blocked child is credited with 21 thousand, a monitor fills a page a thread is waiting on, `FAN_DENY` turns an open into EPERM, and a BPF program counts 60 samples into a map. The **distribution's own perf** runs in the Debian lane: `perf stat` counts 2.34 billion instructions in 976 million cycles, `perf record` takes 80 samples through the ring buffer, and `perf report` reads its own `perf.data` back and attributes them to symbols.

## M127: Resource control

Ahead of M125 and M126: it blocks phase C of
[../distro/roadmap.md](../distro/roadmap.md), and no ISO ships before it.

Per-controller detail: [memory-and-scheduling.md](memory-and-scheduling.md).

- [x] `done` cgroup v2 controllers, each advertised only because it is enforced: `memory` (`memory.current` measured from the members' page tables, `memory.max` enforced from the fault path on an exact measurement), `cpu` (`cpu.weight` as a stride on the M117 scheduler, `cpu.max` as a quota per period), `io` (per-device `io.stat` and rate ceilings at the block layer) and `pids`.
- [x] `done` An OOM killer that ranks by resident size and `oom_score_adj` — one walk shared by the machine-wide and the per-cgroup killer — and a `memory.events` that counts only what happened.
- [x] `done` PSI at `/proc/pressure/{cpu,memory,io}`, measured from stall regions in the block layer, reclaim and swap-in. No per-cgroup pressure files: they would be copies of the global one.
- [x] `done` Compressed swap and reclaim inside a cgroup: `/dev/zram0` (a block device whose blocks live LZ4-compressed in RAM, sized through `/sys/block/zram0/disksize`, measured by `mm_stat`), zswap in front of a real device, and a `memory.max` that now reclaims the cgroup's OWN pages before it kills anything. Every page written out is charged to the cgroup that owned it, so `memory.swap.current`, `memory.swap.max` and `memory.stat`'s `pgscan`/`pgsteal` are measured rather than reported as zero.
- [x] `done` Proved twice: `m127_smoke` and `m127_swap_smoke` on the Alpine lane (22 checks, every one an observed kill, refusal, ratio or byte that came back out of a compressed device) and Debian's own systemd on the systemd lane, where a `MemoryMax=48M` unit's `tail /dev/zero` dies of `SIGKILL` inside it within a second while PID 1 carries on, two `CPUWeight=` units at 100 and 1000 divide the CPU 1:9.8 by their own `cpu.stat`, and `/proc/pressure/cpu` moves under that load.

## M128: Large memory and NUMA

- [ ] `planned` Direct map and PMM beyond 64 GiB verified on real hardware or a large QEMU guest (M41 verified 16 GiB); 5-level paging where the CPU supports LA57.
- [ ] `planned` NUMA topology from ACPI SRAT/SLIT; per-node PMM zones and node-local allocation for kernel and page cache.
- [ ] `planned` Memory policies behind `mbind`/`set_mempolicy` become real on multi-node machines; `/sys/devices/system/node`.
- [ ] `planned` Transparent huge pages for anonymous memory, with the page tables and COW paths that implies.

## M129: Power management

- [ ] `planned` Idle: cpuidle with ACPI `_CST`/intel_idle-style MWAIT C-states instead of plain HLT; tickless idle CPUs.
- [ ] `planned` Frequency: cpufreq with intel_pstate/HWP and ACPI `_PSS`; governors exposed under `/sys/devices/system/cpu`.
- [ ] `planned` Suspend: s2idle first, then ACPI S3 with device suspend/resume ordering (NVMe, xHCI, i915, e1000e); `systemctl suspend` returns to a working desktop.
- [ ] `planned` Battery and thermals on a laptop: ACPI battery/AC, thermal zones, vendor ACPI hotkeys; measured idle power against Linux on the same machine.

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
