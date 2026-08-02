# Roadmap

Status:

- `[x]`: completed.
- `initial`: usable first implementation.
- `partial`: incomplete or limited implementation.
- `planned`: not implemented.
- `deferred`: intentionally postponed.


## M0: Boot and Diagnostics

- [x] Build a freestanding kernel ELF and GRUB ISO.
- [x] Boot x86_64 through Multiboot2 in QEMU.
- [x] Provide serial/VGA output, panic/assert handling, and kernel logging.
- [x] Keep architecture-specific code behind narrow interfaces.

## M1: Architecture Layer

- [x] Add `arch_init`, x86_64 exception handling, and page-fault diagnostics.
- [x] Add PIT/PIC timer interrupts and architecture-local context switching.
- [x] Map common CPU faults to userspace signals.

## M2: Memory

- [x] Parse Multiboot2 memory map and manage physical frames.
- [x] Implement x86_64 paging, higher-half mapping, and direct-map window.
- [x] Link x86_64 kernel at higher-half VA (`0xFFFFFFFF80000000`), loaded at physical 1M.
- [x] Add kernel heap, map/unmap helpers, and lazy page allocation.
- [x] Add swap bookkeeping and page eviction.
- [x] Add per-process page tables, protection checks, and copy-on-write fork.
- [x] Implement `mmap`, `munmap`, and `mprotect`.

## M3: Scheduling

- [x] Add kernel threads, context switching, and round-robin scheduling.
- [x] Add blocking queues, timer preemption, sleep/yield, and priorities.
- [x] Implement zombie lifecycle and parent wait bookkeeping.
- [x] Add process groups, sessions, and foreground job ownership.

## M4: Userspace

- [x] Add user address spaces, syscall dispatch, and safe copy helpers.
- [x] Load initramfs and run `/bin/init`.
- [x] Load native ELF64 programs with argv/envp/auxv and ring-3 execution.
- [x] Keep built-in program fallback for failed ELF loads.

## M5: VFS and Devices

- [x] Add VFS, file descriptors, devfs, tmpfs, tarfs, and `/dev/tty`.
- [x] Add mount dispatch, ACL/permission metadata, and terminal standard FDs.
- [x] Add hard links, symlinks, `readlink`, and symlink-aware `stat`.
- [x] Normalize paths, mount traversal, open modes, and POSIX errors.
- [x] Enforce parent-directory, ownership, and permission rules.

## M6: Network

- [x] Add VirtIO network probing and device integration.
- [x] Implement Ethernet, ARP, IPv4, ICMP, UDP, DHCP, and DNS.
- [x] Add socket-backed TCP and UDP client/server behavior.

## M7: Graphics

- [x] Add boot framebuffer, graphical console, input, and basic compositor.
- [x] Add VirtIO GPU mode setting, rendering, cursor, and dirty-region updates.
- [x] Use RAM shadow buffers and event-driven compositor wakeups.

## M8: Advanced VFS and Filesystems

- [x] Add standard root directory layout and initramfs fallback.
- [x] Add FAT32 import and ext1/ext2/ext3/ext4 read/write support.
- [x] Add ext3/ext4 journaling and recovery hardening.
- [x] Add durable timestamps, directory updates, and persistence tests.
- [x] Formalize VFS node reference ownership.
- [x] Add unified page, inode, and directory caches.
- [x] Add fine-grained directory/inode locking.
- [x] Replace global descriptor table with dynamic per-process tables.
- [x] Add asynchronous VFS I/O and completion queues.

## M9: Hardware Drivers

- [x] Add real VirtIO block, network, and GPU drivers.
- [x] Add PS/2 keyboard input and full PCI enumeration.
- [x] Add block-device caching plus MBR/GPT discovery.
- [x] Add AHCI and NVMe storage drivers.

## M10: Full Network Stack

- [x] Complete Ethernet, ARP, IPv4, ICMP, UDP, DHCP, and DNS paths.
- [x] Add socket descriptors with POSIX errors and nonblocking connect.
- [x] Add TCP active/passive open, retransmission, close lifecycle, and timeouts.
- [x] Add socket options and `select`/`poll`.
- [x] Use a preallocated network TX buffer pool.

## M11: Shell and Utilities

- [x] Add shell built-ins, environment variables, PATH lookup, and job control.
- [x] Add real pipes, descriptor duplication, and standard redirections.
- [x] Add background-job tracking and terminal job control.
- [x] Add the `selfhost` status command.

## M12: Syscalls and Process Management

- [x] Add process, priority, identity, time, filesystem, and terminal syscalls.
- [x] Implement `fork`, `execve`, `waitpid`, `brk`, and memory mapping.
- [x] Add process groups, sessions, signals, and signal masks.
- [x] Add mount/filesystem metadata and descriptor ownership syscalls.
- [x] Implement userspace signal delivery and `sigreturn`.

## M13: Userspace ABI, libc, and POSIX Runtime

- [x] Verify argc/argv/envp, stack alignment, and exec ABI behavior.
- [x] Provide core syscall, stdio, and memory-allocation libc wrappers.
- [x] Verify descriptor inheritance, `dup2`, and close-on-exec.
- [x] Complete userspace signal ABI and red-zone-safe frames.
- [x] Normalize libc error handling to POSIX `errno`.
- [x] Enforce background TTY I/O with `SIGTTIN` and `SIGTTOU`.

## M14: Storage, Swap, and Filesystems

- [x] Add block caching, AHCI, NVMe, ext-family filesystems, and swap.
- [x] Add demand paging, OOM hooks, journaling, and file locking.
- [x] Add `sync`/`fsync`, write-back eviction, and persistent root images.
- [x] Enforce block-cache and VFS lock ordering.

## M15: IPC, Security, and Standard OS Features

- [x] Add POSIX signals, message queues, shared memory, and UNIX sockets.
- [x] Add UID/GID management, file permissions, and capability metadata.
- [x] Add standard C library profile and basic account/file utilities.
- [x] Validate userspace pointers across IPC interfaces.

## M16: Userspace Applications and TUI

- [x] Add two-panel file manager, text editor, and `make` clone.
- [x] Share TUI input/rendering and raw-terminal handling.
- [x] Add file-manager copy, move, mkdir, and delete operations.
- [x] Test editor save/reload and file-manager workflows.
- [x] `partial` Rich compositor-backed applications remain deferred.

## M17: POSIX Compliance and Self-Hosting

- [x] Add core POSIX process, file, pipe, memory, socket, and terminal APIs.
- [x] Document syscall constants and the ELF ABI.
- [x] Port GCC and GNU Binutils for `x86_64-b1nix`.
- [x] Build kernel inside B1NIX and boot resulting artifact.
- [x] Formalize VFS references, inode locking, and expected error matrices.

## M18: Real Userspace and ELF Loader

- [x] Load ELF64 `PT_LOAD` segments into per-process address spaces.
- [x] Build user stacks with argc/argv/envp/auxv.
- [x] Implement image-replacing `execve` and close-on-exec.
- [x] Add exit status, zombie reaping, ring-3 entry, and QEMU tests.
- [x] Reject malformed ELF segments and bound stack/environment copies.

## M19: Process Model and FD Tables

- [x] Implement COW `fork` with correct parent/child register results.
- [x] Add per-process descriptor tables and standard descriptors 0/1/2.
- [x] Apply inheritance and close-on-exec rules.
- [x] Store cwd, environment, umask, process group, and session per process.
- [x] Complete `waitpid`, zombie, and terminal ownership semantics.
- [x] Add thread-safe descriptor-table locking.

## M20: Terminal, TTY, and Interactive Shell

- [x] Add real TTY with canonical/raw modes and line discipline.
- [x] Handle terminal control keys, EOF, signals, and job control.
- [x] Route keyboard input through `/dev/tty` and FD 0.
- [x] Add shell pipes, redirections, PATH lookup, and exit statuses.
- [x] Enforce controlling-terminal and background-I/O rules.

## M21: Persistent Root Filesystem

- [x] Boot from writable ext2 root image with initramfs fallback.
- [x] Add mount/umount, mount flags, listing, and sync-on-shutdown.
- [x] Complete common file mutation and open-flag behavior.
- [x] Add standard root directories and root-image build/run commands.
- [x] Probe Btrfs metadata without exposing it as a supported filesystem.

## M22: Core Terminal Utilities

- [x] Add common file, process, text, filesystem, and network utilities.
- [x] Keep multicall dispatch for small programs.
- [x] Support common flags such as recursive copy/remove and numbered output.
- [x] Cover utility execution through both init and interactive shell paths.

## M23: Networking for Terminal Use

- [x] Add UDP/TCP socket descriptors and server support.
- [x] Add libc DNS resolution and address conversion helpers.
- [x] Add `ifconfig`, `ping`, `nc`, and HTTP download support.
- [x] Handle missing network devices gracefully.
- [x] Integrate readiness with `select` and `poll`.

## M24: Reliability and Diagnostics

- [x] Validate syscall arguments and normalize recoverable errors.
- [x] Add symbolized kernel backtraces and panic diagnostics.
- [x] Add kernel log levels, ring buffer, and `dmesg`.
- [x] Add broad boot, shell, scheduler, VFS, terminal, and socket tests.
- [x] Add scheduler stress coverage and static analysis via `make analyze`.

## M24b: SMP and Multithreading

- [x] Boot APs with INIT-SIPI-SIPI and per-CPU architecture state.
- [x] Add LAPIC timers, per-CPU storage, run queues, and idle tasks.
- [x] Add spinlocks, SMP-safe task allocation, and per-CPU `current_task`.
- [x] Add cross-CPU work stealing for eligible kernel workers.
- [x] Run userspace on APs; remove Big Kernel Lock in M28.
- [x] Deliver preemptive SMP scheduling in M28 and POSIX threads in M29.

## M25: Minimal Native C Toolchain

- [x] Define userspace ABI, `crt0.o`, linker script, headers, and libc.
- [x] Add external clang-backed `b1nix-cc`.
- [x] Build and run native ELF programs from VFS.
- [x] Port TinyCC and compile programs inside B1NIX.
- [x] Expand libc formatting, scanning, math, file, signal, and loader APIs.
- [x] Harden kernel heap metadata and validation.

## M26: Full Toolchain and Self-Hosting

- [x] Port Binutils, Clang/GCC, libstdc++, and GNU Make.
- [x] Build larger programs and kernel with cross toolchain.
- [x] Compile and link full kernel inside B1NIX; boot exact result.
- [x] Provide in-guest assembler/linker/make workflow.
- [x] Fix kernel-stack sizing and improve physical-frame allocation.
- [x] Add heap splitting, coalescing, page return, and large allocation arena.
- [x] Make swap reclaim work under pressure.
- [x] Complete self-hosting at 256 MiB memory floor target.

## M27: Terminal OS Polish

- [x] Add GRUB choices and kernel command-line parsing.
- [x] Add `/etc/rc`, service supervision, and login-shell respawn.
- [x] Add account lookup, login, shutdown, reboot, and emergency shell.
- [x] Keep text/serial operation as first-class mode.
- [x] Add first-boot persistent-root setup and usage guide.
- [x] Document POSIX compatibility matrix.

## M28: Preemptive SMP Scheduling and Fine-Grained Locking

- [x] Add per-CPU LAPIC scheduling ticks and preemptive yields.
- [x] Replace Big Kernel Lock with subsystem-specific locking.
- [x] Add lock-order docs, lockdep, TLB shootdown, and reschedule IPIs.
- [x] Protect VFS tree walks and memory-management paths.
- [x] Fix cross-CPU task state and stack-lifetime races.
- [x] Optimize PMM/heap contention and verify SMP self-hosting.
- [x] Benchmark context-switch and syscall overhead.

## M29: POSIX Threads and Futexes

- [x] Implement `clone` with shared VM, FD, signal, TLS, and clear-TID flags.
- [x] Add hashed futex wait/wake queues.
- [x] Add `%fs`-based thread-local storage.
- [x] Implement libc pthread create/join/detach, mutexes, condvars, and once.
- [x] Verify thread-safe memory and file operations.

## M30: ELF Dynamic Linking and Shared Libraries

- [x] Load `ET_DYN`/PIE binaries and apply `R_X86_64_RELATIVE` relocations.
- [x] Detect `PT_INTERP` and ship `/lib/ld-b1nix.so` compatibility stub.
- [x] Add PIE relocation tests and POSIX-shaped `dl*` stubs.
- [x] Add eager ELF64 startup linking with `DT_NEEDED`, SysV lookup, and shared `libc.so.1`.

## M31: Users, Passwords, and Permissions

- [x] Add `/etc/shadow`, password lookup, and salted SHA-512 hashes.
- [x] Enforce VFS access checks for user-owned and protected paths.
- [x] Add setuid executable handling.
- [x] Verify identity changes, denied privilege escalation, and setuid launch.

## M32: Advanced Network Stack

- [x] Add TCP sliding windows, Reno congestion control, and fast retransmit.
- [x] Add loopback TCP server/client and HTTP workflows.
- [x] Port upstream curl and GNU Wget.
- [x] Implement `select` over existing readiness infrastructure.
- [x] Cover resolver, TCP, and HTTP behavior with deterministic tests.

## M32a: Network Client Features

- [x] Add kernel entropy, CA bundle handling, and TLS build support.
- [x] Add curl HTTPS through mbedTLS and Wget HTTPS through OpenSSL.
- [x] Add IPv6 libc APIs, loopback, TCP/UDP, ICMPv6, NDP, SLAAC, and routing.
- [x] Add external IPv4/IPv6 HTTP and HTTPS support.
- [x] Add PCRE2, IRI/IDN, and NTLM support for Wget.
- [x] Make system timestamps and `time_t` Y2038-safe.
- [x] Add ICMPv6 error delivery, MLDv1 membership handling, and `IPV6_V6ONLY`.

## M32b: SSH Daemon Prerequisites

- [x] Port Dropbear with crypto libraries and kernel-backed randomness.
- [x] Add persistent Ed25519 host keys and password authentication.
- [x] Add PTYs, terminal control, socket options, and session environment.
- [x] Add account policy, authorized-key support, and login validation.
- [x] Add `/etc/init.d/sshd` lifecycle management.
- [x] Verify localhost key exchange, auth, command execution, and PTYs.

## M32c: External SSH Access

- [x] Add host-forwarded QEMU SSH test on `127.0.0.1:2222`.
- [x] Keep sshd loopback-only by default; require `b1nix.ssh-external`.
- [x] Enable normal NIC/DHCP startup and inbound TCP handling.
- [x] Add idle, keepalive, authentication, key-storage, and logging defaults.
- [x] Verify Dropbear access from another machine on bare metal.

## M33: POSIX Shell and Job Control

- [x] Add command substitution, subshells, functions, `case`, and loops.
- [x] Add globbing, arithmetic expansion, here-documents, and parameter expansion.
- [x] Complete foreground/background job control and concurrent pipelines.
- [x] Add common coreutils flags and basic `trap` support.
- [x] Verify asynchronous signal-triggered traps in upstream BusyBox `ash`.
- [x] Retire in-kernel shell in favor of upstream BusyBox `ash`.

## M34: Virtual Filesystems

- [x] Add dynamic `/proc` system and per-process files.
- [x] Add `/sys` kernel, CPU, memory, device, and block information.
- [x] Add `free`, `top`, `ps`, and `sysctl` integration.

## M35: Core Dumps and Analysis

- [x] Generate ELF core dumps for fatal userspace faults.
- [x] Add `kallsyms` and symbolized kernel backtraces.
- [x] Expose process diagnostics for post-mortem analysis.

## M36: Kernel Debugging and Tracing

- [x] Add opt-in serial GDB remote stub.
- [x] Add opt-in function tracer with symbolized ring buffer.

## M37: Real Hardware Booting

- [x] Add generic network-device layer and Intel e1000/e1000e support.
- [x] Improve DHCP recovery and physical-link diagnostics.
- [x] Add xHCI USB HID keyboard support.
- [x] Produce hybrid BIOS/UEFI bootable USB ISO through GRUB.
- [x] Discover CPUs and interrupt routing via ACPI/MADT and IOAPIC.

## M38: Sound

- [x] Add Intel HDA PCI driver with CORB/RIRB codec verb transport.
- [x] Expose simple sound device API (`struct sound_device`) and `/dev/dsp`.
- [x] Add WAV parser/player smoke test with kernel self-test.

## M39: Configurable Init System

- [x] Keep B1NIX `/bin/init` as PID 1.
- [x] Parse `/etc/inittab`.
- [x] Add runlevels and `telinit` via `/run/initctl`.
- [x] Spawn independent TTY/serial `getty` sessions (`/dev/ttyS0`/`ttyS1`).
- [x] Replace hardcoded boot programs with inittab-driven supervisor.

## M40: Linux ABI Compatibility

- [x] Translate Linux x86_64 syscall numbers and semantics (~230 calls: stat/statfs/uname/getdents64/termios layouts, positional I/O, the time calls, sched/credential/capability calls, real `mlock`, `sethostname`, the xattr fd forms).
- [x] Load Linux binaries: static ET_EXEC at its own base, and dynamic ones through their own `PT_INTERP` (a stock Alpine rootfs boots on musl's `ld-musl` — see M94).
- [x] Linux process startup: full auxv (AT_PHDR/BASE/ENTRY/RANDOM/EXECFN/UID…), `arch_prctl`, signal delivery with signo remap, and `rt_sigreturn`.
- [x] Linux-compatible `/proc` and `/sys`: `/proc/<pid>/{environ,statm,limits,cwd,root,mounts}`, `/proc/{swaps,modules,sys/kernel/*,sys/fs/*}`, `/sys/class/net/<if>/*`.
- [x] Detect Linux ELFs through personality (`EI_OSABI` and `NT_GNU_ABI_TAG`).
- [x] `d_ino` is the filesystem's real inode number (ext2/3/4) or the file's stable on-disk identity (FAT/exFAT/ISO first cluster / extent LBA), and `st_ino` reports the same value — readdir and stat now name the same file.
- [x] The privilege and placement calls behave as they claim: capabilities are a real per-task set (a dropped one stays dropped, and root loses the operation it guarded), `setfsuid`/`setfsgid` move the id the VFS checks against (clearing the file-related capabilities as Linux does), `sched_setaffinity` really pins a task, and a `ptrace` tracer that is not the tracee's parent can attach and drive it. `open(2)`'s `O_CREAT` mode is applied (masked by umask) instead of being ignored.
- [x] The rest of the surface, implemented rather than stubbed: `ptrace` (TRACEME/ATTACH/GETREGS/SETREGS/PEEK/POKE/CONT/SINGLESTEP/DETACH/KILL), SysV semaphores (with SEM_UNDO) and message queues (type-selective receive), `chroot` with a real per-task root, runtime `swapon`/`swapoff`, `tee`/`vmsplice`, `ioprio_*`, `name_to_handle_at`/`open_by_handle_at`, `rseq` (live `cpu_id` plus critical-section abort), and legacy `getdents`.

## M29 follow-up: the pthread SMP wedge

- [x] Fixed the long-standing `M29 stress-smp` hang at `-smp 2` (three real races, not one): a futex wake landing while the waiter was momentarily RUNNING was lost; `CLONE_*_SETTID` wrote the child's tid *after* the child could already have exited and had it cleared; and the `CLONE_CHILD_CLEARTID` write — which is what releases musl's thread-list lock — could silently fail, leaving the lock owned by a dead thread.
- [x] `filelock_lock` is taken with interrupts disabled; a preempted holder used to deadlock against its own CPU (observed as `SPINLOCK LOCKUP on cpu 0`).
- [x] The watchdog task dump now prints the futex wait queues (key, owner, expected vs current value) and the last thread deaths, which is what made these diagnosable.

## M41: Large Physical Memory

- [x] Remove 64 GiB ceiling and verify 16 GiB boot.
- [x]   Verify usable memory and defensive e820 handling on real hardware.

## M42: Upstream BusyBox Port

- [x] Cross-build upstream BusyBox as isolated static multicall binary.
- [x] Port core file, text, archive, process, account, storage, and net applets.
- [x] Add required libc, `/proc`, `/sys`, raw socket, loop, and netlink support.
- [x] Promote upstream `ash` to `/bin/sh`.
- [x] Add explicit applet-selection manifest.
- [x] Retire local in-kernel BusyBox utility implementation.
- [x] Replace userspace `sa_restorer` with kernel-owned signal-return trampoline.

## M43: Real-Filesystem Validation and NTFS

- [x] Validate ext2/3/4, exFAT, and NTFS images with large-file reads.
- [x] Verify persistent writes on ext2/ext3/ext4.
- [x] Fix AHCI page-crossing DMA, ext2 xattr parsing, and exFAT filename case.
- [x] Add read-only NTFS driver with resident/non-resident data and indexes.
- [x] Creation at runtime mountpoints supported and verified.

## M44: BusyBox 1.38.0

- [x] Upgrade build pipeline from BusyBox 1.36.1 to 1.38.0.
- [x] Add `sha384sum`, `uuidgen`, `tsort`, `vmstat`, `tree`, xattr tools, and `lsblk`.
- [x] Extend `/proc` and `/sys` for new applets.
- [x] Promote remaining utilities and retire duplicate native implementations.
- [x] Retire in-kernel shell and utility table.

## M45: GNU bash - Retired 

## M46: VFS Integrity and POSIX Process Conformance

- [x] Lock ext2/4 block & inode allocators with per-fs sleeping mutex.
- [x] Make `O_APPEND` sample file size under inode lock and truncate page-cache drop.
- [x] Fix shared FD table lifecycle and atomic fetch-and-clear in `close()`.
- [x] Fix rename link-count leak and directory `..` rewriting.
- [x] Separate exit-status from signal-death encoding and fix `waitpid`/`kill` targets.
- [x] Add `getpgid`, `nice`, `setreuid`/`setregid`, and in-kernel `#!` execution.
- [x] Add `exit_group` semantics (process exit terminates all threads).
- [x] Add controlling-terminal linkage (`setsid` detach) and orphaned SIGHUP+SIGCONT.
- [x] Add per-task CPU accounting for `times()`/`getrusage`, `setresuid`, and `waitid`.
- [x] Make nice value bias cooperative scheduler via side-table.

## M47: Userspace Display Server

- [x] Expose mmap-able `/dev/fb0` (mode query + dirty-rect flush) over VirtIO GPU.
- [x] Add evdev-style `/dev/input/event*` devices for PS/2 keyboard and mouse.
- [x] Define display protocol and compositor lifecycle.
- [x] Implement `displayd` compositor with SHM rendering, cursor, focus, and window controls.
- [x] Add `libb1gui` and demo clients (`gclock`, `gterm`, `gpaint`).
- [x] Make status bar (PANEL) interactive with clickable menus and actions.

## M48: UNIX-Socket FD Passing and memfd

- [x] Add `sendmsg`/`recvmsg` with ancillary data on UNIX sockets.
- [x] Add `SCM_RIGHTS` FD transfer with refcounting across process death.
- [x] Add `SCM_CREDENTIALS` and `memfd_create`.
- [x] Add memfd + `SCM_RIGHTS` display buffers for `wl_shm`.

## M49: Wayland Protocol Compatibility

- [x] Port upstream `libwayland-client` 1.25.0 and `libffi` 3.5.2.
- [x] Port upstream `libwayland-server` core with poll-backed event loop.
- [x] Teach `displayd` real protocol (`wl_shm`, `wl_surface`, `wl_seat`, `xdg-shell`).
- [x] Send `XKB_V1` keyboard keymap and modifier metadata.
- [x] Run stock SHM/xdg-shell wire flow (`m49-smoke`) and window operations.
- [x] Extended protocol coverage (`wl_data_device`, `wp_viewporter`, `wl_subcompositor`, `wl_touch`).

## M50: DRM/KMS and Graphics Memory

- [x] Expose `/dev/dri/card0` over VirtIO GPU driver.
- [x] Add dumb-buffer allocation, mapping, and framebuffer handles.
- [x] Add mode discovery and synchronous page-flip presentation.
- [x] Verify mapped graphics buffers from userspace smoke test.
- [x] Support multiple dumb buffers, `SETCRTC`, `RMFB`, and flip events.

## M51: Desktop Graphics Stack

- [x] Port openlibm for runtime math functions.
- [x] Port pixman generic C library.
- [x] Port FreeType rasterizer and bundle B1nix Mono font.
- [x] Port Fontconfig and expat for font family matching.
- [x] Port Cairo image surface with FreeType backend.
- [x] Port xkbcommon for keymap compilation and keysym mapping.
- [x] Port HarfBuzz with built-in OpenType shaper.
- [x] Complete Wayland surface (`wl_output` mode/scale and clipboard).
- [x] Run Cairo Wayland app demo (`m51_cairo_wayland`) with shaped text and fonts.

## M52: Mesa and Accelerated OpenGL

- [x] Software OpenGL/EGL via TinyGL.
- [x] VirtIO-GPU 2D software renderer path via displayd.
- [x] VirGL 3D acceleration over VirtIO-GPU with host GPU pixel verification.
- [x] Upstream Mesa OSMesa + Gallium softpipe via meson cross-build.
- [x] GLSL programmable shader pipeline through Mesa compiler.

## M53: Browser Platform

- [x] Image/video codecs: zlib, libpng, libjpeg, libwebp, libvpx with smoke tests.
- [x] Mesa through VirGL host GPU acceleration over `/dev/virtio-gpu`.
- [x] NetSurf libraries ported (libwapcaplet, libparserutils, libcss, libdom, etc.).
- [x] NetSurf framebuffer browser cross-built and rendering real pages.
- [x] Web access: HTTP via libcurl, HTTPS via mbedTLS, and public HTTPS.
- [x] On-screen `/dev/fb0` and Wayland windowed frontends with input.
- [x] TCP zero-window stall fix and heavy-page OOM robustness.
- [x] NetSurf HTTPS regression fix with `--with-mbedtls`.
- [x] SVG, JavaScript (Duktape), public-suffix list, JPEG-XL, and utf8proc enabled.
- [ ] `stubbed` Chromium assessment: blocked on sandbox, GPU EGL/GBM/Vulkan, C++ runtime.

## M54: Third-Party Port Feature Enablement

- [x] Foundation: timed futexes, `settimeofday`, sched_getaffinity, `scandir`, `remove`.
- [x] Ports: curl (brotli, PSL, IDN2, unix-sockets), mbedTLS cert date validation, bash `/dev/tcp`, dropbear zlib, Wget threads/PSL, NetSurf JPEG-XL/SVG/Duktape, locale iconv.
- [x] Milestone closeout: remaining feature flips landed, declined, or deferred.

## M55: C++ Runtime

- [x] Build and run libstdc++ with DWARF unwinding, RTTI, and thread-safe statics.
- [x] `std::thread`/`mutex`/`atomic` over pthread layer.
- [x] `std::filesystem` and `iostream` (`std::cout`/`cin`/`cerr`/`stringstream`).
- [x] Validate runtime with **litehtml** C++ layout engine and gumbo parser.

## M56: Event Loop and IPC Primitives

- [x] `epoll` (level/ET/oneshot), `eventfd`, `timerfd`, and `signalfd`.
- [x] SCM_RIGHTS` ancillary FD passing over UNIX sockets.
- [x] memfd sealing (`F_ADD_SEALS`/`F_GET_SEALS`) and cross-process shared `mmap`.

## M57: Multiprocess Model

- [x] COW fork/exec, FD inheritance, `SCM_RIGHTS` brokering, `socketpair`, and `F_DUPFD_CLOEXEC`.
- [x] Bring up a minimal Mojo core over M56 primitives.

## M58: V8

- [x] Port, build, link, and run real V8 `d8` running m58.js suite.
- [x] All execution tiers: Ignition interpreter, Sparkplug baseline JIT, and TurboFan JIT.
- [x] Multi-CPU SMP support under Sparkplug and TurboFan.
- [x] Maglev mid-tier JIT and external code cage space enabled.
- [x] Temporal — V8 with Rust integration (`temporal_rs`/`temporal_capi`).
- [x] WebAssembly (`v8_enable_webassembly`) without trap handler.
- [x] Sandbox (`v8_enable_sandbox`, TrustedSpace, sandboxed pointers).
- [x] i18n (`v8_enable_i18n_support`) with embedded ICU data.
- [x] on-tree Duktape as lightweight `/bin/js` runner.
- [x] POSIX memory wins: `madvise`, `MAP_NORESERVE`, and `sigaltstack` with `SA_ONSTACK`.

## M59: EGL and GL for the Browser

- [x] Real EGL 1.4/1.5 over Mesa OSMesa softpipe (off-screen pbuffer and displayd window path).
- [x] Software Skia (Ganesh) raster fallback.

## M60: Ozone Platform

- [x] Headless Ozone backend for b1nix Chromium build.
- [ ] `deferred` Displayd/Wayland Ozone backend for on-screen rendering (gated on M62).

## Frozen - M61: Chromium Build Target

- [x] b1nix is a working GN/Ninja build target.
- [x] Foundational and services layers compile (`base`, `net`, `crypto`, `ui`, `mojo`, `services`).
- [ ] `partial` Compile remaining Blink renderer and `content/` layers.

## Frozen - M62: content_shell

- [ ] `in-progress` Compile Blink/content and link content_shell ELFs.
- [ ] `planned` Render page to bitmap (`--headless --no-sandbox`) and verify pixels.

## M63: Sandbox

- [x] Classic seccomp-bpf syscall filtering with `PR_SET_NO_NEW_PRIVS`.
- [ ] `deferred` User/PID/net namespaces and setuid-sandbox helper.

## M64: Optional Clang/LLVM Toolchain

- [x] Phase 1 — optional cross `clang++` frontend (`b1nix-clang++`).
- [x] Phase 2 — V8 Clang build profiles (`b1nix-jit-clang` and `b1nix-jit-clang-libcxx`).
- [x] Phase 3 — Clang as primary C compiler while C++ stays on shared GNU runtime.
- [x] Native self-host Clang build under `build/native-clang/b1nix/usr`.
- [x] Native self-host Clang in-QEMU proof (`M64-NATIVE-CLANG: ok compile`).

## M65: Install to Disk

- [x] Standalone install to real disk with bootable MBR/ext4 image.
- [x] `tools/images/mk-disk-image.sh` host script for bootable disk images.
- [x] `/bin/b1nix_install` in-guest whole-disk installer.
- [x] Block I/O fixes for raw `/dev/sataN` and bulk DMA fast path.

## Frozen - M66: Chromium Browser Frontend (planned)

- [ ] `planned` Windowed browser UI over Chromium content layer via displayd Wayland Ozone backend.

## M67: Rust Toolchain Port (for Chromium) — DONE

- [x] Ported Rust to b1nix via `x86_64-unknown-b1nix` target spec and cross-driver.

## M68: Native Rust Compiler (self-hosted) — DONE

- [x] Native rustc 1.98.0 running on b1nix with dynamic `librustc_driver.so` and `libLLVM.so`.

## M69: Dynamic Loading (ELF dynamic linker) — Retired 

## M70: Interrupt-Driven I/O — DONE

- [x] Replace busy-poll storage/NIC drivers with ISR->wakeup completion model.

## M71: ASLR and PIE-by-Default

- [x] PIE-by-default execution and opt-in kernel cmdline ASLR (`b1nix.aslr`) with 2 MiB random jitter.

## M72: msync

- [x] `msync` syscall and durable MAP_SHARED page-cache dirtying across reclaim.

## M73: Modern I/O and Introspection Syscalls

- [x] `sendfile`, `copy_file_range`, `splice`, `fallocate`, and `statx` syscalls.
- [x] `inotify` file monitoring (`inotify_init1`, `add_watch`, `rm_watch`).
- [x] `ptrace` implemented in `kernel/sched/ptrace.c`; `io_uring` and `clone3` deferred.

## M74: Real-Time Signals

- [x]  `SIGRTMIN..SIGRTMAX`, `sigqueue` payload queuing, native `SA_SIGINFO`, and POSIX timers.

## M75: On-Device GPU Path

- [x] Add LLVMpipe (and later EGL/GBM/DRI) on-device GPU path.
- [x] Dynamic `libLLVM.so` for b1nix (72 MB ELF DYN exporting ORC/MCJIT/X86 codegen).
- [x] Mesa llvmpipe built against b1nix `libLLVM.so`.
- [x] GL demo ELF linking llvmpipe + `libLLVM.so`.
- [x] Shared-library `DT_INIT_ARRAY` constructors running via `AT_B1NIX_DSO_INIT` auxv.

## M76: USB Host Stack

- [x] General xHCI controller driver, USB core enumeration, and Mass Storage class driver.

## M77: Raise Global Resource Caps

- [x] `done` Dynamic hard caps for TCP connections (64), VFS pipes (128), core dumps (1 MiB), and `SHMMAX`

## M79: Audio Stack

- [x] Audio subsystem with HDA/AC'97 driver, mixer, and ALSA-compatible userspace shim.

## M80: Kernel ptrace + Crash Capture

- [x] Implement `ptrace(2)` and register/memory reading (`PTRACE_PEEKTEXT`, `PTRACE_GETREGS`, etc.).
- [x] `PTRACE_GETREGSET`/`SETREGSET` (NT_PRSTATUS/NT_PRFPREG), `GETFPREGS`/`SETFPREGS`,
      `GETSIGINFO`, `SEIZE`/`INTERRUPT`; a tracer now also gets first refusal on a
      fatal fault, so a traced crash stops instead of dying.
- [x] `/proc/<pid>/task/<tid>/` (status, stat, comm, maps, statm), `/proc/<pid>/auxv`,
      `/proc/<pid>/mem`, plus Tgid/Threads/TracerPid in status and direct `/proc/<pid>` lookup.
- [x] Crash capture: the fault handler records signal/address/si_code, surfaced through
      SA_SIGINFO `si_addr` and `PTRACE_GETSIGINFO`; `prctl(PR_SET_PTRACER)` and
      `/proc/sys/kernel/yama/ptrace_scope` gate who may attach. Proven end to end by
      `userspace/bin/m80_smoke.c`, which runs the crashpad handshake (crashing client →
      handler over a socket → attach → enumerate threads → dump).
- [x] `PTRACE_SETOPTIONS` with the fork/vfork/clone/exec events + `PTRACE_GETEVENTMSG`
      (a traced child is attached and stopped before it runs), `PTRACE_LISTEN`,
      `PTRACE_O_EXITKILL`, `NT_X86_XSTATE`, `process_vm_readv/writev` and `SO_PEERCRED`.
      (`PTRACE_O_TRACEEXIT` and `TRACESYSGOOD` followed in the next item.)
- [x] `PTRACE_SYSCALL` entry/exit stops (`PTRACE_O_TRACESYSGOOD`, and a tracer-written
      return value reaches userspace), `PTRACE_O_TRACEEXIT`, and a tracee that stops
      for signals its own process ignores — the remaining ptrace(2) gaps.
- [x] Real XSAVE in the context switch: AVX (YMM) state now survives a switch, and
      `NT_X86_XSTATE` reports the area the kernel actually manages. Capped at
      x87|SSE|AVX; a CPU needing a larger area stays on FXSAVE rather than losing state.
- [x] **Upstream Crashpad ported and running** (`tools/ports/build-crashpad.sh`):
      `/bin/crashpad_handler` attaches to a crashing process and writes a real
      minidump. Getting there closed six more ABI gaps: `SOCK_SEQPACKET` on AF_UNIX,
      `SO_PASSCRED`/`SO_PEERCRED`, a tracer's `waitpid` seeing ptrace stops without
      WUNTRACED, Linux-shaped `Uid/Gid/Groups` lines in `/proc/<pid>/status`, an
      ordered `/proc/<pid>/maps` that names the executable's and interpreter's own
      mappings, and reading pages the 4 KiB-only page walk could not see.
      Proven by `userspace/bin/crashpad_smoke.cpp` (CRASHPAD-SMOKE markers).
- [x] CPU clock measured against the PIT during LAPIC calibration and published in
      `/proc/cpuinfo` (`cpu MHz`) and `/sys/devices/system/cpu/cpuN/cpufreq/*`. The
      files exist only once the clock has actually been measured.
- [x] Crashpad builds from **unpatched upstream sources**: the platform defines live
      in the b1nix toolchain wrapper and the UAPI headers (`linux/`, `asm/`,
      `sys/cdefs.h`) are staged into the cross sysroot — a linux-headers package, not
      a per-port shim. The only build flags are the ones an APKBUILD would carry.
      Every mapping also reports the device it lives on: a filesystem now carries a
      real st_dev (the block device's number, or an anonymous one for a RAM/pseudo
      filesystem), kept separate from the per-mount id that keys the inode cache.
      Chasing that separation also found a page-cache aliasing bug: a filesystem
      builds its tree inside its own mount callback, so every inode created there
      carried fs_id 0 — and the page cache, keyed by (fs_id, ino), then shared
      entries between two filesystems that reuse the same on-disk inode number.
      One mount read the other's file contents (M14 block-cache). The mount id is
      now published before the callback runs.
      Closing the last of its complaints fixed three more kernel defects: huge-page
      mappings were invisible to every reader of another task's memory
      (`paging_user_phys`), `/proc/<pid>/maps` reported file offset 0 for every
      segment (so each looked like a module start), and file-backed mappings had no
      inode (so a reader treated them as anonymous).

## M81: Chromium GPU Acceleration

- [ ] `planned` Link graphics `.so`s (SwiftShader Vulkan, ANGLE) and wire VirGL GPU acceleration for `content_shell`.

## M82: System NSS / Kerberos (optional, low priority)

- [ ] `planned` Port system NSS cert DB and MIT-krb5 GSSAPI if needed.

## M83: Unicode-aware ctype / wctype

- [x] Unicode property/case tables provided natively via musl libc port (M92).

## M84: Real IP routing + TCP robustness

- [x] `done` IPv4 FIB (`kernel/net/route.c`): longest-prefix-match with host
  routes, metric tie-break and coexisting gateways. `ipv4_send` routes through
  it instead of assuming a /24 plus one gateway.
- [x] `done` The FIB is the single source of truth: DHCP installs the lease's
  prefix (option 1) and default route, `/proc/net/route` renders the real
  table, and `SIOCADDRT`/`SIOCDELRT` mutate it (BusyBox `route add/del`).
- [x] `done` TCP options: MSS and window scale are sent in SYN/SYN-ACK and
  parsed on receipt; the peer's window is applied with the negotiated shift
  and segments are capped at the peer's MSS.
- [x] `done` TCP out-of-order reassembly queue (16 segs / 64 KiB): segments
  past a hole are buffered and dup-ACKed instead of dropped, already-delivered
  bytes are trimmed off retransmissions, and closing the hole drains
  everything that became contiguous.
- [x] `done` SACK (RFC 2018), both directions: ACKs carry up to three blocks
  built from the reassembly queue (most-recent first), and incoming blocks mark
  queued segments so neither the RTO nor fast retransmit resends them — fast
  retransmit picks the first segment the peer has *not* selectively ACKed.
- [x] `done` Per-connection heap receive buffer (64 KiB) replaces the 16 KiB
  inline array, so the advertised window scale is genuinely non-zero and the
  256-slot table costs ~50 KB of BSS instead of 4 MiB.
- [x] `done` IPv6 FIB with the same model: prefix-length LPM, host routes,
  metric, ECMP and per-interface routes. `ipv6_link_output` routes through it,
  NDP router advertisements install the on-link prefix and default route,
  fe80::/10 + ::1/128 + ff02::/16 are standing on-link routes, and
  `/proc/net/ipv6_route` plus `SIOCADDRT` on an AF_INET6 socket (`struct
  in6_rtmsg`) expose it.
- [x] `done` Per-interface routing end to end: interfaces have stable 1-based
  indices (`eth<N>`), routes carry an output interface, and ARP/NDP
  solicitations plus the frame itself leave through that device
  (`net_send_ethernet_dev`, `arp_resolve_dev`, `ndp_resolve_dev`).
- [x] `done` D-SACK (RFC 2883) both ways: duplicates are reported back as a
  leading D-SACK block, and a received one undoes the congestion reduction the
  spurious retransmission caused.
- [x] `done` RFC 6675 SACK scoreboard: per-segment lost marks (DUPTHRESH
  above), a pipe estimate that governs transmission during recovery, and
  recovery that keeps cwnd at ssthresh instead of Reno's +3 MSS inflation.
- [x] `done` ECMP hashes the full 5-tuple and the group width is unbounded
  (two-pass selection, no candidate array).
- [x] `done` Policy routing: numbered tables plus priority-ordered rules
  matching source prefix and input interface, for both families. Managed
  through `/proc/net/rt_tables` and `/proc/net/rt_rules` (text command
  grammar mirroring `ip route` / `ip rule`, since there is no rtnetlink).
- [x] `done` DHCPv6 client (RFC 8415): DUID-LL, IA_NA,
  Solicit/Advertise/Request/Reply with Renew and Rebind on T1/T2, DNS option
  23, started by the M/O flags of a router advertisement. The assigned address
  is installed with its on-link /128; routers still come from RAs.
- [x] `done` Verified by `M84-ROUTE:`, `M84-ROUTE6:`, `M84-POLICY:`,
  `M84-DHCP6:` and `M84-TCP:` in-kernel self-tests.

## M85: libc Tier-A correctness pass (musl-grade) - Retired

## M86: Per-thread CPU accounting + signal targeting

- [x] TSC-exact per-thread user/system CPU accounting (ring-3 entry/exit + context-switch boundaries); tick sampling removed.
- [x] Real `CLOCK_THREAD_CPUTIME_ID` / `CLOCK_PROCESS_CPUTIME_ID` plus the dynamic per-task clock ids of `clock_getcpuclockid`/`pthread_getcpuclockid`.
- [x] `times(2)`, `getrusage` (SELF = thread group, THREAD, CHILDREN, `ru_nvcsw`/`ru_nivcsw`, `ru_maxrss` as a true high-water mark sampled before every unmap) and `/proc/<pid>[/task/<tid>]/stat` report real CPU time.
- [x] `tkill(2)`/`tgkill(2)` syscalls with thread-group validation; `kill(2)` is process-directed (picks a thread that does not block the signal, group-wide stop/continue that also wakes threads parked in a blocking syscall).
- [x] `exit(2)` ends one thread: a leader that calls it (musl's `pthread_exit` from main) waits for its remaining threads instead of killing them, and hands them its tid futex first.

## M87: Dynamic-loader maturation + Rust proc-macros - Retired

## M88: Kernel correctness fixes (ext4 indirect-block, PROT_NONE guard)

- [x] Enforce `sys_mmap(PROT_NONE)` guard pages in page-fault handler.
- [x] Ext4 single and double-indirect block traversal and allocation for block-mapped inodes in `kernel/fs/ext2.c`.

## M89: Migrate the C++ standard library to LLVM libc++ (shared)

- [x] Shared LLVM libc++ built, default in `b1nix-c++`, hosted C++ smoke running on it.
- [x] NetSurf/litehtml migrated to libc++.
- [x] Mesa and d8/V8 migrated to libc++.
- [x] Native LLVM/clang toolchain rebuilt GCC-free on libc++.
- [x] All GCC shared libraries (`libstdc++.so.6`, `libgcc_s.so`) removed from ISO.
- [x] Replace remaining GCC libstdc++ references with LLVM libc++ across external ports.

## M90: Complete elimination of GCC from the cross and native toolchains (Pure LLVM/Clang Toolchain)

- [x] Pure LLVM/Clang cross-toolchain on host side.
- [x] Pure LLVM/Clang native toolchain inside VM.
- [x] Migrate remaining ports off GCC components.
- [x] GCC-free Rust toolchain (cross rust and native rustc).

## M91: Skia 2D Graphics Library (standalone)

- [x] Standalone Skia cross-build for b1nix (`libskia.a`).
- [x] Skia raster backend verified (`M91-SKIA: ok raster-draw`).
- [x] Skia Ganesh GPU backend on OSMesa/EGL (`M91-SKIA: ok gpu-draw`).
- [x] Skia Graphite CPU backend (`M91-SKIA: ok graphite-cpu`).
- [x] Skia Graphite GPU backend via Dawn/OpenGL ES (`M91-SKIA: ok graphite-dawn`).
- [x] fontconfig integration with Skia (`M91-SKIA: ok text-draw`).
- [x] Dynamic Mesa linking for M91/M52/M59 demos (`libOSMesa.so.8`).
- [x] Skottie (Lottie animation) support verified (`M91-SKIA: ok skottie`).
- [x] Real shared `.so` for EGL/GL/fontconfig (`libEGL.so`, `libGLESv2.so`, `libfontconfig.so`).

## M92: musl libc Port

- [x] Linux ABI expansion: ~30 missing syscall mappings for musl.
- [x] Futex expansion: WAIT_BITSET, WAKE_BITSET, REQUEUE, CMP_REQUEUE.
- [x] clone: CLONE_PARENT_SETTID + CLONE_CHILD_SETTID.
- [x] ELF auxv: populate 15+ missing entries.
- [x] Linux ABI struct translations: sigaction + termios.
- [x] Cross-compile musl as static libc and build integration.
- [x] Kernel bugfix: AT_PHDR for ET_EXEC binaries.
- [x] Kernel bugfix: `vm_find_free_area` unsorted VMA list.
- [x] Kernel bugfix: fork TLS inheritance.
- [x] Kernel bugfix: `set_thread_area` (NR 205) no-op.
- [x] Musl smoke test: 9/9 pass.
- [x] Musl as dynamic libc (`libc.so` / `ld-musl-x86_64.so.1`).
- [x] Ring 0 to Ring 3 Migration: PID 1 `/bin/init` and PID 5 `/bin/netd`.
- [x] Full Initramfs Purge: minimal bootstrap without legacy `.inc` headers.
- [x] Ext4 Primary Root Filesystem across all boot modes.
- [x] VFS Disk Node Fixes for ext4 disk nodes.

## M93: Ring 0 Cleanup

- [x] In-Kernel Dynamic Linker Purge: delegated to userspace `ld-musl`.
- [x] Kernel Build Automation Migration: replaced kernel build orchestrator with userspace scripts.

## M94: Foreign Userspace Independence & Kernel Decoupling

- [x] Generic Boot & Init System Orchestration (`init=` cmdline, PID 1 reservation, OpenRC boot).
- [x] Init-Agnostic Runtime Control: FIFOs (RAM + ext2/4 inodes), tmpfs/ramfs/devtmpfs, working `posix_spawn`/`popen`, `flock`. OpenRC boots as PID 1 and `openrc-shutdown` powers off through `/run/openrc/init.ctl` (`iso-openrc` smoke instance); BusyBox init also runs as PID 1. runit/s6 not ported.
- [x] OpenRC -> Dynamic musl Linking (and every other port): `tools/check-dynamic.sh` fails the build on a static executable in the rootfs, exceptions only via `tools/configs/static-allowlist.txt` with a reason. The legacy fixed-base static link path is gone from the cc wrappers, and with it every userspace linker script (`linker.ld`, `linker-cxx.ld`, `linker-libcxx.ld`) — lld's default layout plus `--eh-frame-hdr` keeps `PT_GNU_EH_FRAME`, verified by `CXX-SMOKE: ok exceptions`.
- [x] Standardized Virtual Memory Address Space Layout: no fixed load base left in the build (`userspace/linker.ld` deleted). One boot runs a stock Linux `ET_EXEC` at `0x200000`, b1nix `ET_EXEC` images at `0x2000000` and musl PIEs placed by the loader (randomized under `b1nix.aslr`); the kernel is higher-half, so none of them can collide with it.
- [x] Linux ABI Conformance: every syscall the ported software actually reached (`clock_getres`, `sched_getaffinity`, `faccessat2`, `rt_sigtimedwait`, `statx` with a real dirfd, `sysinfo`, `times`) is translated or implemented — a boot logs no unmapped syscall. **A stock Alpine 3.20 minirootfs boots on b1nix**: its BusyBox init runs as PID 1 through Alpine's own `ld-musl`, its coreutils/awk/sed/pipes work, and `poweroff` shuts the machine down (`tools/try-alpine-rootfs.sh`). runit/s6 remain unported.

## M95: Loadable Kernel Modules — Framework and Device/FS Modules

- [x] Module region at `0xFFFFFFFFC0000000` (128 MiB, PML4 slot 511 so every address space shares its page tables) with real W^X: `module_alloc` hands out RW+NX pages, the loader re-protects the text span RX once relocation is done. Needed EFER.NXE, now enabled on the BSP (`boot.S`) and every AP (`ap_trampoline.S`, `x86_syscall_init`) behind a CPUID check.
- [x] `ET_REL` (`.ko`) loader in `kernel/module/module.c`: section layout, symbol resolution against the kernel's `EXPORT_SYMBOL` table plus already-loaded modules, R_X86_64_{64,PC32,PLT32,32,32S,PC64,GOTPCRELX} relocations, `.modinfo` vermagic gate, `struct module` list with `try_module_get`/`module_put` (a referenced module cannot be removed).
- [x] `init_module(2)` / `finit_module(2)` / `delete_module(2)` — native 243/245/244 and the Linux ABI numbers 175/313/176 — all gated on `CAP_SYS_MODULE`. `/proc/modules` is real, `/proc/filesystems` now enumerates the live registry instead of a hardcoded list, and `/bin/kmod` is a multi-call `insmod`/`rmmod`/`lsmod`/`modinfo`/`modprobe`.
- [x] `ntfs`, `btrfs`, `isofs` and the Intel HDA sound driver ship as `.ko` in the initramfs and the rootfs; the kernel loads them at boot through `request_module`, and boots and passes its tests with any of them absent. `tools/kernel/check-module-syms.sh` fails the build (not the insmod) on an unexported symbol.
- [x] `M95-SMOKE` covers the framework end to end, including a vermagic-corrupted module being rejected and an unprivileged `delete_module` reporting `EPERM`.
- [ ] `open` **Unloading a filesystem module while one of its filesystems is mounted is not refused.** `vfs_unregister_fs` has no mount refcount behind it, so `rmmod isofs` with an ISO mounted frees the code the mount is still using. The protocol modules are protected (the dispatch-drain guard in `proto.c`); the filesystems are not.
- [ ] `open` `VFS_FS_NODEV` is set on six pseudo filesystems only, so `/proc/filesystems` mislabels the rest of them.

## M96: Loadable Kernel Modules — Network Protocols and Module Parameters

- [x] `proto_register` scaffolding (`kernel/net/proto.c` + `<b1nix/netproto.h>`): the L2 demux, the net daemon's tick, the loopback drain and the IPv6 transmit path all dispatch through the registry, so IPv6, NDP and NTP could move into `ipv6.ko`, `ndp.ko` and `ntp.ko`. Unregistration drains in-flight dispatches before the module's text is freed.
- [x] `module_param` exposed at `/sys/module/<name>/parameters/<name>`, readable and — where the permission bits allow — writable (`ipv6_hop_limit`, `ntp_server_name`, `hda_tone_ms` writable; `hda_sample_rate`, `hda_dam_buf_sz` read-only). Parameters can also be set on the `insmod` command line; an unknown name fails the load.
- [x] In-kernel `request_module` (loads from `/lib/modules`, resolving aliases and dependencies) plus a userspace `modprobe` with `-r`, both reading the same index files.
- [x] `modules.dep` and `modules.alias` are generated from the objects themselves (`tools/kernel/gen_modules_initramfs.sh`) — a dependency is an undefined symbol another module `EXPORT_SYMBOL`s, so `ndp: ipv6` falls out of the code rather than a hand-written list.
- [x] `M96-SMOKE` covers protocol modules, sysfs parameters (read, write, read-only rejection, insmod-time), `modules.dep`, alias resolution and dependency-ordered `modprobe`.
- [ ] `open` IPv6 neighbour enumeration for netlink (`ip -6 neigh`) — `ndp.ko` exposes no snapshot/add/delete API, so M107's `RTM_GETNEIGH` returns an empty AF_INET6 table and `RTM_NEWNEIGH` reports `EAFNOSUPPORT`. IPv4 is complete.

## M97: GNU-Free ISO (Limine bootloader + BSD build tools)

- [x] Bootloader GNU GRUB -> **Limine** (BSD-2-Clause): `tools/mkiso.sh` (Limine + xorriso) replaced `grub-mkrescue` for all nine ISO targets and both in-guest proof harnesses; `boot/limine/limine.conf.in` replaced `boot/grub/grub.cfg`. The disk image gained a side benefit — `limine bios-install` writes the boot stages into the image file, so `mk-disk-image.sh` no longer needs root/losetup/mount.
- [x] Build automation GNU Make -> **bmake** (BSD 3-clause, `/bin/make`) + **samurai** (0BSD Ninja, `/bin/samu`, `/bin/ninja`). `tools/toolchain/build-make.sh` is deleted, `/bin/make` left the static allowlist, and `tools/inguest/Makefile` is now portable BSD make. Verified in-guest by `M98-SMOKE`.
- [x] GNU Wget -> curl / BusyBox wget (done earlier; `/bin/wget` is the BusyBox applet).
- [x] Shell GNU bash -> **zsh** (MIT-like) as `/bin/zsh`, the login shell in `/etc/passwd`, with BusyBox `ash` as `/bin/sh`. zsh needs a terminal library, which b1nix never had (bash carried its own bundled termcap), so **netbsd-curses** (BSD) was ported alongside it. `tools/ports/build-bash.sh` is deleted and the packaged bash is purged from the rootfs by `install-ports.sh`. Verified by `ZSH-SMOKE` (12 checks, the same feature surface `BASH-SMOKE` covered).
- [x] Gnulib left the tree with bash — it was only ever bundled inside it.

## M98: Driver Infrastructure (netconsole, memory typing, modern PCI)

- [x] `netconsole` (`kernel/dev/netconsole.c`) — the klog ring drained by a kthread and shipped as UDP datagrams, configured with `b1nix.netconsole=<ip>:<port>`. Draining from a thread rather than inside `console_write` is what keeps it off the console lock, which is held IRQs-off from interrupt context. Host collector: `tools/netconsole-collect.sh`.
- [x] `IA32_PAT` programming (`kernel/arch/x86_64/memtype.c`) on the BSP and every AP, a `VMM_WC` flag threaded through `vmm_map_page`/`vmm_map_mmio`, and `clflush`/`wbinvd`/`mfence`/`sfence` primitives with a range-flush helper. Only PAT slot 5 is rewritten, so no live mapping changes meaning.
- [x] PCI BAR enumeration and sizing (decode-disabled, register restored), bus-master and decode enables, standard capability walk, PCIe extended capabilities over ACPI-MCFG ECAM, and MSI / MSI-X programming.
- [x] Intel stolen memory read from the **host bridge** GGC `0x50` / BDSM `0x5C` / BGSM `0x70`. Reports absence on the QEMU bridges, which is the correct answer there; the decode path is exercised for real only on Intel graphics hardware.
- Markers: `M98-DRV-SMOKE: ok {netconsole-cmdline,netconsole-udp,pat-msr,pat-wc,clflush,bar-enum,bar-restore,cap-walk,bus-master,msi-config,msix-config,stolen}`. Details: `docs/driver-infrastructure.md`.
- [ ] `open` **MSI/MSI-X delivery is unproven** — only the programming is checked, by reading the device's own config space and MSI-X table back. Firing an interrupt would mean reprogramming a device the suite is driving, so the probe also skips mass-storage functions (class 0x01) and byte-restores what it touched. Delivery gets proven by the first driver that consumes it (M101/M102).
- [ ] `open` Intel stolen memory: the GMS/GGMS decode arithmetic runs only on real Intel graphics. The gate now requires the IGD at 00:02.0, because QEMU's Q35 emulates an Intel host bridge whose 0x5C/0x70 hold unrelated registers — reading them produced a plausible 1 MiB-aligned base for an aperture that does not exist.
- [ ] `open` The `wbinvd` path is the CLFLUSH-less fallback and is unreachable on every CPU QEMU emulates.

## M99: linuxkpi Compatibility Layer (own MIT headers)

- [x] `kernel/include/lkpi/` written from scratch (MIT): `types.h`, `idr.h`, `completion.h`, `workqueue.h`, `scatterlist.h`, `firmware.h`, `io.h`, `dma-mapping.h`, `lock.h`. No Linux header text is copied — those are GPL and the drivers this layer will carry are MIT.
- [x] `idr` (flat growable table with a rotating hint), `completion` over the scheduler's two-phase wait, workqueue over `kthread_create` with FIFO ordering and delayed items, `scatterlist` with run coalescing, `request_firmware` over the VFS.
- [x] `ioremap`/`ioremap_wc`/`ioremap_wb` over `vmm_map_mmio`, dma-mapping (coherent alloc, single/sg map, cache sync for non-snooping devices), and spinlock/sleeping-mutex wrappers that keep b1nix's "never sleep under a spinlock" rule visible in the type.
- Markers: `M99-SMOKE: ok {idr,completion,workqueue,workqueue-delayed,scatterlist,ioremap,dma-mapping,request-firmware,locks}`. Details: `docs/driver-infrastructure.md`.
- [ ] `open` No IOMMU and no bounce buffers: `dma_map_single` refuses an address the page tables cannot translate rather than copying it into a reachable buffer, so a device with a narrow DMA window still has nowhere to fall back to.

## M100: DRM Core — dma-fence, scheduler, GEM (proven on virtio-gpu)

- [x] `dma-fence` (`kernel/drm/dma_fence.c`) — refcounted, one-shot, callback lists, error propagation, sleeping and timed waits.
- [x] Minimal `drm_gpu_scheduler` (`kernel/drm/gpu_scheduler.c`) — one thread owns the ring, per-entity FIFOs, round-robin between entities, job dependencies resolved in the scheduler thread.
- [x] `vgpu_submit_stream` converted off `virtio_gpu_wait_used`: userspace VirGL submissions now queue a scheduler job and park on its fence instead of busy-spinning on the virtqueue used index with `vgpu_udev_lock` held IRQs-off. The synchronous path remains only as a pre-scheduler fallback.
- [x] GEM buffer objects are scatter-gather backed — pages allocated individually, described by an `sg_table`, mapped linearly into a per-object kernel window for scanout and one page at a time into userspace through the new `mmap_handle_page_phys_cb`. Handles moved onto an `idr`.
- [x] `drm_ioctl` split into one handler per command (was cyclomatic 56 / cognitive 161 in a single switch), behaviour unchanged.
- Markers: `M100-SMOKE: ok {fence-signal,fence-error,fence-refcount,sched-submit,sched-fairness,gem-sg,gem-create,gem-info,gem-map,gem-mmap-pages,gem-scanout,gem-destroy,gem-handle-reuse}`. Details: `docs/driver-infrastructure.md`.
- [ ] `open` Discontiguous sg-backed buffers are not asserted (`gem-sg-discontig` is emitted as a skip): under QEMU the allocator hands back contiguous frames, so the path that matters — a BO whose pages are scattered — is exercised only incidentally.
- [ ] `open` The GEM kernel vmap window needs its PML4 slot to exist before any process is created, so `drm_dev_init` touches one page there to install the path. A GEM mapping that faults in one process but not another points at this.

### M108 — open

- [ ] `open` BusyBox `su` fails to authenticate against `/etc/shadow` (`su-uid-and-shell`, `su-password-auth`, `su-accepts-passwd-hash`): the setuid layout, `passwd`'s own write path and the PAM read path all verify, so the failure is in how the applet reads or compares the hash. Unproven either way until it is traced in the guest.
- [ ] `open` `useradd`/`userdel`/`groupadd`/`groups`/`id`/`whoami` are still dedicated ELFs.
- [ ] `open` BusyBox `passwd` locks `/etc/shadow` with `fcntl(F_SETLK)` + `link()`; whether b1nix's ext4 honours both is unconfirmed, so a concurrent password change is not known to be safe.
- [ ] `open` The BusyBox-init instance's `getty` respawn is unexercised — the smoke's done-marker kills the VM before a respawn cycle.

## M101: Intel i915 (Gen8/Gen9.5) + Mesa iris

- [ ] `planned` GTT/PPGTT, execlists submission, contexts; no firmware needed on Gen8/Gen9.
- [ ] `planned` Bare metal on the Pavilion's Gen8, logs over netconsole (the guaranteed path — no OS on the laptop, ISO boots from USB).
- [ ] `planned` *If VT-d and BIOS ever allow it:* KVM + VFIO passthrough for a faster loop (keeps virtual COM1).
- [ ] `planned` `EXECBUFFER2` softpin-only + Mesa `iris` (own NIR backend — no LLVM rebuild).

## M102: amdgpu on RX 6600 (render-only) + radeonsi

- [ ] `planned` Build without DC: scanout stays on the GOP framebuffer, render offscreen and blit.
- [ ] `planned` PSP (`psp_v11_0`) firmware loading, SMU 11 mailbox, GFX10.3 KIQ/MQD, GPUVM.
- [ ] `planned` Visible/invisible VRAM windowing (8 GB behind a 256 MB BAR without ReBAR).
- [ ] `planned` `libdrm_amdgpu` + `libLLVM.so` rebuilt with the AMDGPU target (currently X86 only).

## M104: Native Package Manager (`bpkg`) + Ports-Out-Of-Tree Migration Plan

- [x] `initial` `/bin/bpkg` — a real C userspace ELF (`userspace/bin/bpkg.c`), built/linked exactly like every other musl binary, no shell/curl/tar/sha256sum shelling. Own RFC 1951/1952 DEFLATE+gzip decoder, ustar+GNU-longname tar extractor, and FIPS 180-4 SHA-256 — zero third-party dependency.
- [x] `initial` Speaks the house **flat** index format (`tools/packages/bpkg-publish.sh` output, sha256-verified) AND a **real Alpine repository**: `APKINDEX.tar.gz` (P:/V:/A:/D: records) + `.apk` (the real 3-gzip-member signature/control/data container). Verified against byte-for-byte real `tar`/`gzip`-produced fixtures (see `docs/bpkg-package-manager.md`).
- [x] `initial` Transitive dependency resolution, `update`/`install`/`remove`/`list`/`search`/`info`, `/var/lib/bpkg` state compatible with the existing `tools/packages/install-ports.sh` "download" mode.
- [ ] `planned` TLS (`https://`) — currently HTTP- and `file://`-only; no RSA signature or APKINDEX `C:` checksum verification.
- [ ] `planned` Full mass migration of `tools/ports/*.sh` off the from-source build path — `docs/ports-migration-plan.md` is a concrete phased plan plus a one-port (`hello`-class fixture, standing in for `zlib`) proof of concept; no real port has been moved yet and no `tools/ports/*.sh` script has been deleted, per the project's own migration ground rules.
- Details: `docs/bpkg-package-manager.md` (the tool), `docs/ports-migration-plan.md` (the migration plan + PoC + verification status).

## M105: PAM (OpenPAM + pam_unix.so)

- [x] `done` OpenPAM `libpam.so.2` + b1nix's `pam_unix.so` (`/etc/shadow` via musl `crypt(3)`); dropbear runs with `DROPBEAR_SVR_PAM_AUTH`.
- [x] `done` Verified end to end: correct password authenticates, wrong one gives `PAM_AUTH_ERR`, unknown user gives `PAM_USER_UNKNOWN`.

## M106: DNS resolver

- [ ] `planned` Outbound name resolution does not work: `nslookup` and `curl` both fail with "can't resolve" against the QEMU forwarder in `/etc/resolv.conf` (10.0.2.3), while the same fetch by raw IP returns 200. This is the single thing standing between b1nix and installing packages straight from the Alpine mirror — `bpkg` itself already fetches, parses a real `APKINDEX.tar.gz` and resolves dependencies once it is handed an address.
- [ ] `planned` Suspect the CNAME chain first (`dl-cdn.alpinelinux.org` is a CNAME onto Fastly) and confirm whether the resolver follows it, then whether `getaddrinfo`/`/etc/resolv.conf` are consulted at all.

## M107: BusyBox applets blocked on missing kernel subsystems

BusyBox 1.38 ships 439 applets; 114 are compiled in and 137 are exposed. The
rest are pure-userspace and free to enable — except the following, which need
kernel work first. Each line is the subsystem, not the applet.

- [x] `done` **netlink** — `AF_NETLINK`/`NETLINK_ROUTE` in `kernel/net/netlink.c`: RTM_GETLINK/GETADDR/GETROUTE/GETNEIGH dumps rendered from the netdev registry, the M84 FIB and the ARP cache, plus RTM_NEW/DEL for routes, neighbours and addresses. `ip`, `route`, `arp`, `netstat` enabled. IPv6 neighbours are deferred (the NDP cache has no enumeration API yet); an administrative link up/down returns EOPNOTSUPP because b1nix has no per-interface admin state to change.
- [x] `done` **VT switching / console fonts** — `kernel/dev/vt.c`: six virtual terminals on `/dev/tty0../dev/tty6` with VT_ACTIVATE/WAITACTIVE/OPENQRY/DISALLOCATE/GETSTATE/SETMODE and KDSETMODE/KDGKBMODE, a replaceable console face behind PIO_FONT/GIO_FONT/KDFONTOP, and a real keysym table the PS/2 driver translates through so KDSKBENT/KDGKBENT (`loadkmap`/`dumpkmap`) change what keys produce. Alt+Fn switches.
- [x] `done` **loop devices** — `loop_info64` with the backing file name, `lo_offset`/`lo_sizelimit`, a write path, read-only following the descriptor, and `/dev/`-prefixed mount sources, so `losetup` and `mount -o loop` both work.
- [x] `done` **fuller `/proc`** — `/proc/<pid>/fd/N` readlinks to a file's full path instead of its basename, and `maps` labels `[heap]`/`[stack]`. `lsof`, `fuser`, `pmap` enabled.
- [x] `done` **syslog / `/proc/kmsg`** — `kernel/dev/kmsg.c`: a structured record ring with sequence numbers, timestamps and priorities behind `/dev/kmsg`, `/proc/kmsg` and `syslog(2)`. `syslogd`, `klogd`, `logread`, `logger` enabled.
- [x] `done` **inotify** — the M73 core plus IN_MOVED_FROM/IN_MOVED_TO with a shared cookie, IN_ATTRIB on chmod/chown/utimes, IN_DELETE_SELF, and the IN_MASK_ADD constant fixed. `inotifyd` enabled.
- [x] `done` **RTC + watchdog ioctls** — `/dev/rtc0` reads and writes the CMOS clock (century register, BCD and 12-hour encodings) with RTC_RD_TIME/RTC_SET_TIME/RTC_ALM_*/RTC_WKALM_*/RTC_IRQP_*; `/dev/watchdog` is a software watchdog with the WDIOC_* interface and a deadline that really resets the machine. `hwclock`, `rtcwake`, `watchdog` enabled.
- [x] `done` **i2c** — `/dev/i2c-0` on the PIIX4/ICH9 SMBus host controller, with I2C_SLAVE/I2C_FUNCS/I2C_SMBUS. The node exists only when a controller was probed, and I2C_FUNCS declines to claim raw-I2C support the silicon cannot deliver, so `i2ctransfer` reports the limit instead of misbehaving.
- [ ] `planned` **MTD/UBI** — `flash*`, `nandwrite`, `ubi*`. Only worth it if b1nix ever targets flash storage.
- [ ] `deferred` **loadable modules** — `insmod`, `rmmod`, `lsmod`, `modprobe`, `depmod`, `modinfo`. Not a gap to close: the kernel is monolithic by design, so these stay unavailable rather than unimplemented.

### M107 — open

- [ ] `open` `vfs_fd_abspath()` cannot rebuild a path whose parent chain crosses the ext4 root's lazily materialised directories, so `LOOP_GET_STATUS64` reports an empty `lo_file_name` and BusyBox `losetup -a` shows no backing file (`loop-attach`, `loop-write`, `applet-losetup`). The association itself works — reads and writes go to the right file.
- [ ] `open` IPv6 neighbours: `RTM_GETNEIGH` returns an empty AF_INET6 table and `RTM_NEWNEIGH` reports `EAFNOSUPPORT` (needs the enumeration API listed under M96).
- [ ] `open` `ip link set <if> up/down` reports `EOPNOTSUPP` — b1nix has no per-interface administrative state.
- [ ] `open` `i2ctransfer` is unsupported: an SMBus host controller cannot issue raw I2C messages, so `I2C_FUNCS` does not advertise `I2C_FUNC_I2C`. QEMU's bus also has no slave, so `i2cdetect` sees an empty bus.
- [ ] `open` `PIO_FONTX`/`GIO_FONTX` report `EOPNOTSUPP` (`KDFONTOP` covers the same request); the renderer is 8 pixels wide and rejects other widths rather than mis-drawing.
- [ ] `open` `/dev/kmsg` and `/proc/kmsg` share one read cursor each rather than one per descriptor — correct for a single reader (klogd, syslogd), wrong for two.
- [ ] `open` MTD/UBI applets stay unbuilt.

## M108: Hand ownership of base tools to BusyBox

Alpine's own init is BusyBox, and once packages come from Alpine the b1nix-specific
replacements become the odd ones out. Retire them in favour of the multicall ELF.

- [x] `done` `su` and `passwd` are BusyBox applets. The dedicated ELFs (`userspace/bin/{su,passwd}.c`) are deleted. The setuid bit moved off `/bin/*` onto a second copy of the multicall ELF, `/opt/busybox/bin/busybox-suid` (mode 4755 root), which only `su`/`passwd`/`login` symlink to; `CONFIG_FEATURE_SUID=y` makes libbb drop euid for every other applet.
- [x] `done` One shadow format end to end: BusyBox writes SHA-512 `$6$` (`CONFIG_FEATURE_DEFAULT_PASSWD_ALGO`), which is exactly what `pam_unix.so` (M105) reads — a password changed with the applet authenticates through PAM and vice versa, verified by `M108-SMOKE`.
- [x] `done` BusyBox init as an alternative PID 1 via `/etc/inittab`, selected with the M94 `init=/opt/busybox/bin/init` cmdline. `openrc-init` stays the **default** (it owns the `/run/openrc/init.ctl` control channel); inittab drives the same OpenRC runlevels, so both inits reach the same system. New `iso-bbinit` smoke instance proves it; `iso-openrc` is untouched.
- [x] `done` Kernel fix found on the way: `execve` published the *pre*-exec euid/egid and `AT_SECURE=0` in the auxv of a setuid image, so musl's ld.so would have honoured `LD_PRELOAD` in `/bin/su`. The auxv now describes the post-exec credentials.
- [x] `done` The "deliberately NOT listed" block in `tools/configs/applet-manifest.conf` no longer mentions `su`/`passwd`; both are listed as `upstream`.
