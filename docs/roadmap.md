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

- [x] Translate roughly 230 Linux x86_64 syscalls and their structure layouts.
- [x] Load Linux binaries: static at their own base, dynamic through their own interpreter.
- [x] Build Linux process startup: full auxv, `arch_prctl`, signal frames, TLS.
- [x] Provide the `/proc` and `/sys` files Linux programs read.
- [x] Detect Linux binaries by personality rather than by guesswork.
- [x] Report real inode numbers in directory entries.
- [x] Make capabilities, `setfsuid`, affinity and `ptrace` behave as they claim.
- [x] Implement the rest rather than stubbing it: SysV IPC, `chroot`, `rseq`, swap, `mount(2)`.

## M29 follow-up: the pthread SMP wedge

- [x] Fix three races behind the `-smp 2` hang: a lost futex wake, a late thread-id write, and a clear that never landed.
- [x] Take the file-lock lock with interrupts disabled so a preempted holder cannot deadlock.
- [x] Print the futex wait queues in the watchdog task dump.

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

- [x] Add the foundation the ports wanted: timed futexes, `settimeofday`, affinity queries, `scandir`, `remove`.
- [x] Turn on the port features that depend on it, across curl, mbedTLS, dropbear and the shells.
- [x] Close the milestone: every remaining flag landed, declined or deferred.

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

- [x] Implement `ptrace(2)`: register and memory access, `SEIZE`/`INTERRUPT`, `GETSIGINFO`, register sets.
- [x] Add syscall entry/exit stops, fork/exec/clone events, and `PTRACE_O_*` options.
- [x] Gate attachment with `PR_SET_PTRACER` and Yama `ptrace_scope`.
- [x] Add `/proc/<pid>/task/<tid>/`, `auxv`, `mem`, and Linux-shaped status fields.
- [x] Record fault signal, address and code, and deliver them through `SA_SIGINFO`.
- [x] Save AVX state across context switches with XSAVE.
- [x] Run upstream Crashpad unpatched: it attaches to a crashing process and writes a real minidump.
- [x] Publish measured CPU clock in `/proc/cpuinfo` and sysfs cpufreq.
- [x] Give every filesystem a real `st_dev`, kept apart from the mount id the inode cache keys on.

## M81: Chromium GPU Acceleration

- [ ] `planned` Link graphics `.so`s (SwiftShader Vulkan, ANGLE) and wire VirGL GPU acceleration for `content_shell`.

## M82: System NSS / Kerberos (optional, low priority)

- [ ] `planned` Port system NSS cert DB and MIT-krb5 GSSAPI if needed.

## M83: Unicode-aware ctype / wctype

- [x] Unicode property/case tables provided natively via musl libc port (M92).

## M84: Real IP routing + TCP robustness

- [x] Add an IPv4 FIB with longest-prefix match, host routes, metrics and gateways.
- [x] Make the FIB the single source of truth for DHCP, `/proc/net/route` and `SIOCADDRT`/`SIOCDELRT`.
- [x] Add TCP MSS and window-scale option negotiation.
- [x] Add out-of-order reassembly, SACK and D-SACK in both directions.
- [x] Add an RFC 6675 scoreboard with a pipe estimate and per-segment loss marks.
- [x] Give each connection a 64 KiB heap receive buffer.
- [x] Add an IPv6 FIB with the same model, fed by router advertisements.
- [x] Route per interface: stable interface indices, output device on every route, ARP and NDP through it.
- [x] Add ECMP over the full 5-tuple and policy routing with numbered tables and rules.
- [x] Add a DHCPv6 client with Solicit/Request/Renew/Rebind.

## M85: libc Tier-A correctness pass (musl-grade) - Retired

## M86: Per-thread CPU accounting + signal targeting

- [x] Account user and system CPU time per thread from TSC at ring transitions and switches.
- [x] Implement `CLOCK_THREAD_CPUTIME_ID`, `CLOCK_PROCESS_CPUTIME_ID` and per-task clock ids.
- [x] Report real values from `times(2)` and `getrusage` for thread, process and children.
- [x] Add `tkill(2)` and `tgkill(2)`; keep `kill(2)` process-directed.
- [x] Make `exit(2)` end one thread and `exit_group(2)` the whole group.

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

- [x] Honour `init=` from the cmdline and reserve PID 1 for it.
- [x] Add the runtime an init system needs: FIFOs, tmpfs, ramfs, devtmpfs, `posix_spawn`, `popen`, `flock`.
- [x] Boot two inits: BusyBox init by default, `openrc-init` through `init=`, each proved by the same checks.
- [x] Link every rootfs executable dynamically and fail the build on a static one.
- [x] Remove every fixed load base from the build; the loader places images.
- [x] Translate every syscall the ported software reaches, so a boot logs no unmapped call.
- [x] Boot a stock Alpine minirootfs on its own `ld-musl`, down to `poweroff`.

## M95: Loadable Kernel Modules — Framework and Device/FS Modules

- [x] Add a module region with W^X: writable while relocating, read-execute once loaded.
- [x] Add an `ET_REL` loader with symbol resolution against exported kernel symbols.
- [x] Add `init_module(2)`, `finit_module(2)` and `delete_module(2)`, gated on `CAP_SYS_MODULE`.
- [x] Ship ntfs, btrfs, isofs and the HDA driver as modules; boot with any of them absent.
- [x] Load modules with BusyBox modutils and retire the in-tree `kmod`.
- [x] Refuse to unload a filesystem module while one of its filesystems is mounted.
- [x] Report loaded modules and filesystem types from the live registry.

## M96: Loadable Kernel Modules — Network Protocols and Module Parameters

- [x] Register network protocols through a registry, so a protocol can be a module.
- [x] Expose module parameters under `/sys/module/<name>/parameters/`.
- [x] Load modules on demand from the kernel and resolve aliases and dependencies.
- [x] Generate `modules.dep` and `modules.alias` from the objects themselves.
- [x] Ship IPv6 and NDP as modules, including netlink neighbour administration.

## M97: GNU-Free ISO (Limine bootloader + BSD build tools)

- [x] Replace GRUB with Limine for BIOS and UEFI boot.
- [x] Replace GNU make with bmake and ninja with samurai.
- [x] Replace GNU wget with curl and the BusyBox applet.
- [x] Replace bash with zsh as the login shell, keeping BusyBox ash as `/bin/sh`.
- [x] Remove gnulib from the tree.

## M98: Driver Infrastructure (netconsole, memory typing, modern PCI)

- [x] Ship the kernel log over UDP from a kernel thread, configured from the cmdline.
- [x] Program the PAT on every CPU and add write-combining mappings.
- [x] Add cache-maintenance primitives, including a deliberate `wbinvd` fallback path.
- [x] Enumerate and size PCI BARs without disturbing the running driver.
- [x] Walk standard and PCIe extended capabilities, and enable bus mastering.
- [x] Program MSI and MSI-X from a dedicated vector range with one owner per vector.
- [x] Prove delivery with a driver that uses it: NVMe takes its completions over MSI-X.
- [x] Decode Intel graphics stolen memory as a pure function, tested against the spec.

## M99: linuxkpi Compatibility Layer (own MIT headers)

- [x] Write the headers from scratch under MIT, copying no Linux source.
- [x] Add idr, completion, workqueue, scatterlist and firmware loading.
- [x] Add ioremap variants over the kernel's MMIO mapper.
- [x] Add dma-mapping: coherent allocation, single and sg mapping, cache sync.
- [x] Add bounce buffers for a device whose address window misses the memory it is handed.
- [x] Bounce an sg table into one block when possible, so a device sees no extra segments.
- [x] Wrap spinlocks and sleeping mutexes so the never-sleep rule shows in the type.
- [x] Map through the IOMMU when one is present, so a narrow mask means "map it low" instead of "copy it".

## M100: DRM Core — dma-fence, scheduler, GEM (proven on virtio-gpu)

- [x] Add refcounted one-shot fences with callbacks, errors and timed waits.
- [x] Add a GPU scheduler: one ring thread, per-entity queues, round-robin, dependencies.
- [x] Move VirGL submission onto the scheduler instead of spinning on the virtqueue.
- [x] Back GEM objects with scatter-gather pages, mapped linearly for the kernel and per page for userspace.
- [x] Assert that a buffer object's pages really are discontiguous.
- [x] Install the GEM window's page tables at boot so every address space shares them.
- [x] Split the DRM ioctl switch into one handler per command.

## M100a: DMA bounce pool

- [x] Reserve the bounce pool at boot, below 4 GiB, sized from the cmdline.
- [x] Hand out slots from the pool; fall back to the allocator for a mask the pool cannot reach.
- [x] Bounce an sg table a block per run when no single block is available.
- [x] Report pool occupancy, high-water mark and mapping count.

## M100b: IOMMU

- [x] Parse the ACPI DMAR table and bring up the Intel VT-d unit.
- [x] Program root and context tables, and enable translation without breaking any driver.
- [x] Identity-map memory with 2 MiB pages when the unit offers no pass-through.
- [x] Add second-level page tables with map, unmap and translate.
- [x] Add a device address allocator with reuse.
- [x] Route `dma_map_single` and `dma_map_sg` through translation for an attached device.
- [x] Run NVMe in its own domain with only its queues and buffers mapped, and read through it.
- [x] Record and report DMA faults, so "the device touched nothing else" is the unit's answer.
- [x] Block a violation: the codec is given its descriptor list but not its audio buffer, and the read it was not granted is stopped and recorded.

## M100c: IOMMU — per-device domains, groups, interrupt remapping

- [x] Give each device its own domain instead of one shared translated domain.
- [x] Group functions that cannot be isolated from each other, and move a group as a unit.
- [x] Treat everything behind a bridge as one group, since the bridge can put its own requester id on their traffic.
- [x] Take domain ids from the unit's own capability, and give a destroyed domain's page tables back.
- [x] Prove isolation between domains: a page mapped for one device does not exist for another.
- [x] Program the interrupt remapping table and enable it, keeping compatibility-format interrupts working.
- [x] Route NVMe's MSI-X through a remapped entry, and keep delivery working.
- [x] Reject an interrupt whose entry was taken away, and record the refusal.
- [x] Use ACS to split a group: walk up until a port can keep its children apart, and group everything under the topmost one that cannot.
- [x] Handle ARI and SR-IOV: a device numbering functions across the bus owns the bus, so the group is the bus.
- [x] Decide the ACS policy once at boot (`b1nix.acs=on|keep|off`) and keep grouping a read-only question.
- [x] Spare named ports from the policy (`b1nix.acs-keep=<bdf>,...`), and report what each ACS port ended up doing.

## M100d: AMD-Vi

- [x] Parse ACPI IVRS and bring up the unit.
- [x] Program the flat device table, every device passthrough until its driver asks.
- [x] Add the command ring, and wait on completion rather than assuming.
- [x] Add AMD's page-table format, with map, unmap and translate.
- [x] Read faults out of the event log.
- [x] Move NVMe into a translated domain and read a block through it, with the event log staying empty — which is also what proves the page-table format is the one the unit walks.

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

## M104: Native Package Manager (`bpkg`)

- [x] `initial` Write `/bin/bpkg` as a plain C ELF with its own inflate, tar and SHA-256.
- [x] `initial` Read both the house flat index and a real Alpine repository.
- [x] `initial` Resolve dependencies and support update, install, remove, list, search and info.
- [ ] `planned` Add HTTPS transport and package signature verification.
- [ ] `planned` Migrate the from-source ports to packages.

## M105: PAM (OpenPAM + pam_unix.so)

- [x] Port OpenPAM and write `pam_unix.so` against `/etc/shadow`.
- [x] Authenticate dropbear logins through PAM, with the shipped stack under `/etc/pam.d`.

## M106: DNS resolver

- [ ] `planned` Resolve names outbound: `nslookup` and `curl` both fail today.
- [ ] `planned` Check CNAME chain handling and the response parser first.

## M107: BusyBox applets blocked on missing kernel subsystems

- [x] Add netlink route sockets: link, address, route and neighbour queries.
- [x] Add virtual terminals, console fonts and keymaps.
- [x] Add loop devices with offsets, size limits and a write path.
- [x] Extend `/proc`: full paths in `fd/`, named map regions, per-process memory.
- [x] Add a structured kernel log ring with syslog, `/dev/kmsg` and `/proc/kmsg`.
- [x] Add inotify move cookies, attribute changes and self deletion.
- [x] Add RTC read and write plus watchdog ioctls with a real reset deadline.
- [x] Add SMBus i2c on the host controller, reporting what it cannot do.
- [x] Give block-backed device nodes `S_IFBLK`, and honour the interface name in `SIOCGIF*`.
- [x] Administer IPv6 neighbours and interface up/down state for real.
- [x] Give every `/dev/kmsg` reader its own cursor.
- [ ] MTD and UBI applets stay unbuilt.

## M108: Hand ownership of base tools to BusyBox

- [x] Make `su`, `passwd`, `login`, `id`, `whoami` and `groups` BusyBox applets and delete the local ELFs.
- [x] Keep the setuid bit on a second copy of the multicall binary, reached by three names only.
- [x] Write and read one shadow format end to end: SHA-512, shared with PAM.
- [x] Boot BusyBox init as PID 1 from `/sbin/init`, with OpenRC driving the runlevels.
- [x] Exercise the inittab getty respawn: kill it and require PID 1 to replace it.
- [x] Delete three forged init markers from the smoke hook and let the real paths report.
- [x] Fix `execve` to publish post-exec credentials in the auxv and refresh capabilities and fsuid.
- [x] Group `userspace/bin` by purpose.
- [ ] Confirm `/etc/shadow` locking under concurrent password changes.

## M109: Alpine applet parity

- [x] Build 283 of Alpine's 321 applets and prove each one through `/bin`.
- [x] Add `/dev/zero`, `/dev/urandom` and `/dev/random`, unblocking `shred`, `who` and `cpio`.
- [ ] `planned` Add `AF_PACKET` for the DHCP clients and raw-frame tools.
- [ ] `planned` Add namespaces for `unshare` and `nsenter`.
- [ ] `planned` Add `pivot_root(2)` and `switch_root`.
- [ ] `planned` Add bridge, VLAN, bonding and tunnel devices.
- [ ] `planned` Add inode attribute flags for `chattr` and `lsattr`.
- [ ] `planned` Add discard so `fstrim` and `blkdiscard` work.
- [ ] `planned` Add I/O priorities for `ionice`.
- [ ] `planned` Add a uevent channel for `mdev`.
- [ ] `planned` Add filesystem UUID and label probing for `findfs`.
- [ ] `planned` Add the remaining single-device gaps: CD-ROM, MTD, rfkill, md, nbd, floppy, serial config.
- [ ] `wontfix` `i2ctransfer` needs raw I2C an SMBus controller cannot issue.
