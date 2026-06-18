# Roadmap

Status:

- `[x]`: completed.
- `initial`: usable first implementation.
- `partial`: incomplete or limited implementation.
- `stub`: placeholder only.
- `planned`: not implemented.
- `deferred` / `parked`: intentionally postponed.

Supporting documents:

- POSIX work: [`posix-requirements.md`](posix-requirements.md).
- Architecture ports: [`porting-guide.md`](porting-guide.md).
- Userspace ABI: [`abi.md`](abi.md).

## Current POSIX Estimate

- Practical compatibility: about 75-82%.
- VFS/path/files: about 90-95%.
- Shell/coreutils: about 90-94%.
- Remaining shell gaps include advanced parameter expansion and `VAR=x cmd`.
- Remaining system gaps are mostly permission edge cases and full conformance
  testing.

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

- [x] Parse the Multiboot2 memory map and manage reusable physical frames.
- [x] Implement x86_64 paging, higher-half mapping, and a direct-map window.
- [x] Link **both kernels at a higher-half VA** (x86_64 at 0xFFFFFFFF80000000;
  i686 into the direct map at 0x80000000), loaded at physical 1M. Kernel symbols
  no longer share the low address range with userspace (base 0x2000000), so a
  large kernel image (e.g. an embedded Mesa initramfs) can no longer overlap and
  corrupt a userspace process's view of kernel data. Both arches boot, single +
  `-smp 4`.
- [x] Add the kernel heap, map/unmap helpers, and lazy page allocation.
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

- [x] Add boot framebuffer, graphical console, input, and a basic compositor.
- [x] Add VirtIO GPU mode setting, rendering, cursor, and dirty-region updates.
- [x] Use RAM shadow buffers and event-driven compositor wakeups.

## M8: Advanced VFS and Filesystems

- [x] Add the standard root directory layout and initramfs fallback.
- [x] Add FAT32 import and ext1/ext2/ext3/ext4 read/write support.
- [x] Add ext3/ext4 journaling and recovery hardening.
- [x] Add durable timestamps, directory updates, and persistence tests.
- [x] Formalize VFS node reference ownership.
- [x] Add unified page, inode, and directory caches.
- [x] Add fine-grained directory/inode locking.
- [x] Replace the global descriptor table with dynamic per-process tables.
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
- [x] Complete the userspace signal ABI and red-zone-safe frames.
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
- [x] Add a standard C library profile and basic account/file utilities.
- [x] Validate userspace pointers across IPC interfaces.

## M16: Userspace Applications and TUI

- [x] Add a two-panel file manager, text editor, and `make` clone.
- [x] Share TUI input/rendering and raw-terminal handling.
- [x] Add file-manager copy, move, mkdir, and delete operations.
- [x] Test editor save/reload and file-manager workflows.
- [x] `partial` Rich compositor-backed applications remain deferred.

## M17: POSIX Compliance and Self-Hosting

- [x] Add the core POSIX process, file, pipe, memory, socket, and terminal APIs.
- [x] Document syscall constants and the ELF ABI.
- [x] Port GCC and GNU Binutils for `x86_64-b1nix`.
- [x] Build the kernel inside B1NIX and boot the resulting artifact.
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

- [x] Add a real TTY with canonical/raw modes and line discipline.
- [x] Handle terminal control keys, EOF, signals, and job control.
- [x] Route keyboard input through `/dev/tty` and FD 0.
- [x] Add shell pipes, redirections, PATH lookup, and exit statuses.
- [x] Enforce controlling-terminal and background-I/O rules.

## M21: Persistent Root Filesystem

- [x] Boot from a writable ext2 root image with initramfs fallback.
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
- [x] Run userspace on APs; later remove the temporary Big Kernel Lock in M28.
- [x] Deliver preemptive SMP scheduling in M28 and POSIX threads in M29.

## M25: Minimal Native C Toolchain

- [x] Define the userspace ABI, `crt0.o`, linker script, headers, and libc.
- [x] Add an external clang-backed `b1nix-cc`.
- [x] Build and run native ELF programs from the VFS.
- [x] Port TinyCC and compile programs inside B1NIX.
- [x] Expand libc formatting, scanning, math, file, signal, and dynamic-loader
  compatibility APIs.
- [x] Harden kernel heap metadata and validation.

## M26: Full Toolchain and Self-Hosting

- [x] Port Binutils, GCC, libstdc++, and GNU Make.
- [x] Build larger programs and the kernel with the cross toolchain.
- [x] Compile and link the full kernel inside B1NIX; boot the exact result.
- [x] Provide an in-guest assembler/linker/make workflow.
- [x] Fix kernel-stack sizing and improve physical-frame allocation.
- [x] Add heap splitting, coalescing, page return, and a large-allocation arena.
- [x] Make swap reclaim work under pressure.
- [x] Complete self-hosting at 256 MiB; lower memory targets are out of scope.

## M27: Terminal OS Polish

- [x] Add GRUB choices and kernel command-line parsing.
- [x] Add `/etc/rc`, service supervision, and login-shell respawn.
- [x] Add account lookup, login, shutdown, reboot, and emergency-shell paths.
- [x] Keep text/serial operation as a first-class mode.
- [x] Add first-boot persistent-root setup and a usage guide.
- [x] Document the POSIX compatibility matrix.

## M28: Preemptive SMP Scheduling and Fine-Grained Locking

- [x] Add per-CPU LAPIC scheduling ticks and preemptive yields.
- [x] Replace the Big Kernel Lock with subsystem-specific locking.
- [x] Add lock-order documentation, lockdep, TLB shootdown, and reschedule IPIs.
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
- [x] Detect `PT_INTERP` and ship `/lib/ld-b1nix.so` as a compatibility stub.
- [x] Add PIE relocation tests and POSIX-shaped `dl*` stubs.
- [x] Add eager ELF64 startup linking with `DT_NEEDED`, SysV symbol lookup,
  GOT/PLT relocation, and shared `libc.so.1`.

## M31: Users, Passwords, and Permissions

- [x] Add `/etc/shadow`, password lookup, and salted SHA-512-based hashes.
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
- [x] Add ICMPv6 error delivery, MLDv1 membership handling, and
  `IPV6_V6ONLY`.

## M32b: SSH Daemon Prerequisites

- [x] Port Dropbear with its crypto libraries and kernel-backed randomness.
- [x] Add persistent Ed25519 host keys and password authentication.
- [x] Add PTYs, terminal control, socket options, and session environment setup.
- [x] Add account policy, authorized-key support, and login validation.
- [x] Add `/etc/init.d/sshd` lifecycle management.
- [x] Verify localhost key exchange, authentication, command execution, and PTY
  support.

## M32c: External SSH Access

- [x] Add a host-forwarded QEMU SSH test on `127.0.0.1:2222`.
- [x] Keep sshd loopback-only by default; require `b1nix.ssh-external` to expose
  it on all interfaces.
- [x] Enable normal NIC/DHCP startup and inbound TCP service handling.
- [x] Add idle, keepalive, authentication, key-storage, and logging defaults.
- [ ] `deferred` Verify Dropbear access from another machine on bare metal.

## M33: POSIX Shell and Job Control

- [x] Add command substitution, subshells, functions, `case`, and loops.
- [x] Add globbing, arithmetic expansion, here-documents, and parameter
  expansion.
- [x] Complete foreground/background job control and concurrent pipelines.
- [x] Add common coreutils flags and basic `trap` support.
- [x] Verify asynchronous signal-triggered traps in upstream BusyBox `ash`.
- [x] The in-kernel shell was later retired in favor of upstream BusyBox `ash`.

## M34: Virtual Filesystems

- [x] Add dynamic `/proc` system and per-process files.
- [x] Add `/sys` kernel, CPU, memory, device, and block information.
- [x] Add `free`, `top`, `ps`, and `sysctl` integration.

## M35: Core Dumps and Analysis

- [x] Generate ELF core dumps for fatal userspace faults.
- [x] Add `kallsyms` and symbolized kernel backtraces.
- [x] Expose process diagnostics for post-mortem analysis.

## M36: Kernel Debugging and Tracing

- [x] Add an opt-in serial GDB remote stub.
- [x] Add an opt-in function tracer with a symbolized ring buffer.

## M37: Real Hardware Booting

- [x] Add a generic network-device layer and Intel e1000/e1000e support.
- [x] Improve DHCP recovery and physical-link diagnostics.
- [x] Add xHCI USB HID keyboard support.
- [x] Produce one hybrid BIOS/UEFI bootable USB ISO through GRUB.
- [x] Discover CPUs and interrupt routing through ACPI/MADT and IOAPIC.
- [x] `partial` GRUB-provided VBE/GOP framebuffer works; runtime mode switching
  does not.
- [x] `parked` ISO9660/loop-backed live boot works in QEMU but awaits installer
  and real-hardware hardening.
- [x] `parked` Read-only exFAT live-image fallback is experimental.

## M38: Sound

- [x] Add Intel HDA PCI controller driver with CORB/RIRB codec verb transport.
- [x] Expose a simple sound device API (`struct sound_device`) and `/dev/dsp`.
- [x] Add a WAV parser/player smoke test (`m38_sound`) with kernel self-test.

## M39: Configurable Init System

- [x] Keep B1NIX `/bin/init` as PID 1.
- [x] Parse `/etc/inittab`.
- [x] Add runlevels and `telinit` (via the `/run/initctl` control file, since
  PID 1 is an in-kernel task).
- [x] Spawn independent TTY/serial `getty` sessions — `/dev/ttyS0`/`/dev/ttyS1`
  are real per-device ttys (own line discipline, termios, session/job-control
  state); the full `getty → login → bash` chain runs on the serial line while
  the console bash session stays independent.
- [x] Replace hardcoded boot programs with file-based services (inittab-driven
  supervisor with a SysV rate-based respawn storm guard; legacy path kept as
  fallback).
- Verified by `M39-INIT` smoke markers
  (single-CPU and `-smp 4`, both arches) and the end-to-end
  `tests/serial-getty.sh` login-over-serial test.

## M40: Linux ABI Compatibility

Source-level ports remain preferable to a Linux compatibility layer.

- [ ] `planned` Translate Linux x86_64 syscall numbers and semantics.
- [ ] `planned` Load static Linux binaries and glibc's `PT_INTERP`.
- [ ] `planned` Add Linux-shaped process startup, auxv, vDSO, TLS, and signals.
- [ ] `planned` Fill Linux-compatible `/proc` and `/sys` entries.
- [ ] `planned` Detect Linux ELFs through a separate binary personality.

## M41: Large Physical Memory

- [x] Remove the old x86_64 64 GiB ceiling and verify a 16 GiB boot.
- [ ] `partial` Raise the i686 direct-map limit from 1 GiB toward 1.5-1.75 GiB.
- [ ] `planned` Add per-CPU high-memory mappings for up to 4 GiB on i686.
- [ ] `planned` Verify full usable memory and defensive e820 handling on real
  hardware.

## M42: Upstream BusyBox Port

- [x] Cross-build upstream BusyBox as an isolated static multicall binary.
- [x] Port core file, text, archive, process, account, storage, and networking
  applets in tested waves on x86_64 and i686.
- [x] Add required libc, `/proc`, `/sys`, raw socket, loop, and netlink support.
- [x] Promote upstream `ash` to `/bin/sh`.
- [x] Add an explicit applet-selection manifest.
- [x] Retire the local in-kernel BusyBox-style utility implementation.
- [x] Replace userspace-provided `sa_restorer` with a kernel-owned signal-return
  trampoline. The ELF loader maps a per-process **read-only, executable** page
  (a tiny `mov $SYS_SIGRETURN, %eax; int $0x80`/`syscall` stub) one page below
  the TLS/stack region; `arch_build_signal_frame` points the handler's return
  address at it instead of trusting a userspace `sa_restorer` (which a setuid
  process could not be allowed to control). `sa_restorer` is now only a fallback
  if the page could not be mapped (OOM). Confirmed used on both arches by
  forcing the fallback to a garbage address and observing the M15 signal-
  handler/mask tests still pass (x86 744/0, x86_64 745/0).

## M43: Real-Filesystem Validation and NTFS

- [x] Validate genuine ext2/ext3/ext4/exFAT/NTFS images and large-file reads.
- [x] Verify persistent writes on ext2/ext3/ext4.
- [x] Fix AHCI page-crossing DMA, ext2 xattr parsing, and exFAT filename case.
- [x] Add a read-only NTFS driver with resident/non-resident data and indexes.
- [x] Creation at runtime-created mountpoints works (was `planned`): the
  downward mount-crossing matches the mountpoint by node identity, and a
  `mkdir`-at-runtime node is pinned by the mount, so lookups resolve into the
  mounted fs. Verified by `M43: ok create-runtime-mountpoint` — `mkdir /mnt/w4`
  at runtime, `mount -t ext4 sata0 /mnt/w4`, then create a file + subdir inside
  and read them back, both arches.
- [ ] `planned` Add exFAT and NTFS write support.
- [ ] `planned` Populate large filesystem directory trees lazily.

## M44: BusyBox 1.38.0

- [x] Upgrade the build/configuration pipeline from BusyBox 1.36.1 to 1.38.0.
- [x] Add `sha384sum`, `uuidgen`, `tsort`, `vmstat`, `tree`, xattr tools, and
  upstream `lsblk`.
- [x] Extend `/proc` and `/sys` for the new applets.
- [x] Promote remaining utilities and retire duplicate native implementations.
- [x] Retire the in-kernel shell and utility table.
- [ ] `stub` BusyBox `ssl_server` is disabled pending a dedicated TLS port and
  handshake test.

## M45: GNU bash

- [x] Cross-build upstream GNU bash 5.2.37 against the b1nix userspace ABI and
  embed it as `/bin/bash`.
- [x] Make bash the login shell everywhere — the console terminal `/bin/init`
  launches and the `/etc/passwd` shell used by SSH/login; `/bin/sh` stays
  BusyBox `ash` for `#!/bin/sh` scripts. Required shipping `/etc/shells` and
  fixing a libc `fgets(size<=1)` bug that hung dropbear's `/etc/shells` parser.
- [x] Add the libc surface bash needs (`sigsetjmp`/`siglongjmp`,
  `setgrent`/`getgrent`, `setlinebuf`, `ffs`, `sigset_t` via `<sys/select.h>`).
- [x] Add a UTF-8 wide-character libc module (`wchar.c`: `mbrtowc`/`wcwidth`/
  `mbsrtowcs`/…) and build bash with `HANDLE_MULTIBYTE` so it is UTF-8
  character-aware. UTF-8 is the libc-wide default (`MB_CUR_MAX` 4 globally;
  `mbtowc`/`mbstowcs`/`wcstombs` are UTF-8 for every port).
- Verified by `BASH-SMOKE` +
  `M32B-SSH` markers on i686 and x86_64, single-CPU and `-smp 4`.

## M46: VFS Integrity and POSIX Process Conformance

A corruption-focused VFS audit plus a POSIX process-management gap audit;
findings and full details in [`vfs-process-audit.md`](vfs-process-audit.md).

- [x] Lock the ext4/ext2 block & inode allocators (per-fs sleeping mutex) —
  fixes cross-file block double-allocation under parallel writes.
- [x] Make O_APPEND sample the file size under the inode lock (no lost
  concurrent appends) and make truncate drop/zero stale page-cache pages.
- [x] Fix the shared fd-table lifecycle: last-user close+free under
  `g_mm_release_lock`, shared fd_lock for CLONE_FILES siblings, pointer
  propagation on table growth, atomic fetch-and-clear in `close()`.
- [x] Fix rename link-count leak and cross-parent directory `..` rewriting
  (ext4 + ext2); lock `vfs_mount` slot claim; fix `vfs_link` error-path
  refcount.
- [x] Separate exit-status from signal-death encoding (`exit(139)` no longer
  reads as SIGSEGV); fix `kill(0)`/`kill(-1)` targets; `waitpid(-pgid)` and
  `waitpid(0)` group waits; ESRCH/EINVAL errnos; full `setpgid` POSIX rules;
  fork inherits the blocked-signal mask.
- [x] Add `getpgid`, `nice`/`getpriority`/`setpriority`, `setreuid`/
  `setregid`, and in-kernel `#!` interpreter execution.
- [x] Add `exit_group` semantics (process exit terminates all threads).
- [x] Add controlling-terminal linkage (`setsid` ctty detach) and
  orphaned-process-group SIGHUP+SIGCONT.
- [x] Add per-task CPU accounting for `times()`/`getrusage`;
  `setresuid`/`setresgid`; `waitid`.
- [x] Make the nice value bias the cooperative scheduler
  (it round-trips via a side-table; mapping it onto the strict-priority
  `pick_next_task` scan starves tasks — see the audit doc).

### Open hardening (second-round audit)

All critical and high-severity bugs from Part 3 of the audit are fixed; details
in [`vfs-process-audit.md`](vfs-process-audit.md). One known open item remains:
the intermittent `#GP` under heavy graphics load (stale data-segment register on
kernel→user return path). Lower-priority pending items: futex PROCESS_SHARED
cross-mmap wakeups, journal crash-atomicity, and swap/eviction table locking
(all inactive in the smoke suite).

- [x] All critical and high-severity bugs from the audit fixed (file locks, unix-socket UAF, journal mutex, block-cache dedup, xattr locking, page-cache race, loop-device refcount, icache dangling pointer, orphaned-pgrp false-negative, shm leak+lock). See [`vfs-process-audit.md`](vfs-process-audit.md) for details.
- [ ] `bug` Remaining lower-severity open items: futex PROCESS_SHARED wakeups, journal crash-atomicity, swap/eviction table locking, and an intermittent `#GP` under heavy graphics load. Details in [`vfs-process-audit.md`](vfs-process-audit.md).

## M47: Userspace Display Server

Own compositor, initially validated with a temporary Wayland-shaped protocol
that M49 replaced with real Wayland.

- [x] Expose an mmap-able `/dev/fb0` (mode query + dirty-rect flush
  ioctl) over virtio-gpu; kernel `compositor.c` becomes the console fallback
  with claim/reclaim handoff. — Device, shared-frame mmap (`mmap_phys_cb` +
  `VMM_SHARED`), `FBIOGET_INFO`/`FBIOFLUSH`, and the claim side (compositor
  stops flushing once userspace maps fb0) are done. VMA lifecycle hooks now
  release ownership on munmap/exec/exit and request a full kernel-compositor
  redraw; `M47-DSP: ok console-reclaim` covers the displayd exit path.
- [x] Add evdev-style `/dev/input/event*` devices for PS/2 keyboard and
  mouse (pollable, raw keycodes; keymaps in userspace) — per-client queues,
  O_NONBLOCK/EAGAIN, signal-interruptible blocking reads; verified by the
  `M47-GFX` input markers (kernel-injected burst through the real
  queue/read path; i8042 decode wiring exercised on real HW only).
- [x] Define the initial display protocol and compositor lifecycle; replaced
  by the real Wayland protocol in M49.
- [x] Implement the `displayd` compositor: damage-driven SHM
  compositing into `/dev/fb0`, cursor, focus, alt-tab, minimal decorations;
  started from `/etc/inittab`, clean console handback on exit. — Includes
  draggable title bars, click-to-focus/raise, `Alt+Tab`, `Alt+F4`, runlevel-5
  supervision and restart after framebuffer reclaim.
- [x] Add `libb1gui` plus demo clients (`gclock`, `gterm`,
  `gpaint`) and extend `tests/graphics-smoke.sh` with `M47-GFX` markers
  (Wayland SHM clients, console reclaim and restart; green on x86_64 and x86).
- [x] Make the status bar (PANEL) interactive and functional like macOS:
  clickable system, active-app, File/Edit/View and clock headers; server-side
  dropdowns with hover/disabled states; close/quit, Cut/Copy/Paste,
  next-window and bring-to-front actions; click-away and Escape dismissal.

## M48: UNIX-Socket FD Passing and memfd

Kernel prerequisite for real Wayland (M49) with standalone POSIX value
(dbus-style daemons, privilege separation).

- [x] Add `sendmsg`/`recvmsg` with ancillary data on UNIX sockets.
- [x] Add `SCM_RIGHTS` fd transfer with correct refcounting,
  including in-flight fds when the receiver dies (fd-table lifetime is a
  known sharp edge — see the M46 fd-table fixes).
- [x] Add `SCM_CREDENTIALS` and `memfd_create`.
- [x] Add memfd + `SCM_RIGHTS` display buffers; used directly by `wl_shm`
  in M49.

## M49: Wayland Protocol Compatibility

Builds on M47's Wayland-shaped core + M48's fd passing; mapping is 1:1 by
construction.

- [x] Port upstream `libwayland-client` 1.25.0 (plus libffi 3.5.2) and run it
  against `displayd`; the compositor remains a deliberately small native
  server rather than pulling in the unused upstream server event loop.
- [x] Port upstream `libwayland-server` core with a poll-backed event loop;
  SHM remains implemented by `displayd` until b1nix has pthread TLS/SIGBUS ABI.
- [x] Teach `displayd` the real protocol: `wl_shm`,
  `wl_compositor`/`wl_surface`, `wl_seat`, and an `xdg-shell` subset.
- [x] Send the standard `no_keymap` keyboard map fd and repeat metadata;
  clients consume the existing raw evdev keycodes without an xkbcommon port.
- [x] Run the stock SHM/xdg-shell wire flow (`m49-smoke`, equivalent to the
  weston-simple-shm protocol path).
- [ ] `parked` TinyX/kdrive `Xfbdev` on the same `/dev/fb0` + input
  substrate, as the route to real X11 apps.

## M50: DRM/KMS and Graphics Memory

- [x] Expose `/dev/dri/card0` over the existing VirtIO GPU driver.
- [x] Add dumb-buffer allocation, mapping, and framebuffer handles.
- [x] Add mode discovery and synchronous page-flip presentation.
- [x] Verify mapped graphics buffers from a userspace smoke test.
- [x] Support multiple dumb buffers/framebuffers, `SETCRTC`, `RMFB`,
  poll/read flip events, and close/munmap cleanup.

## M51: Desktop Graphics Stack

- [x] Prerequisite: port a real libm (openlibm) — the previous `math.h` had no
  working runtime libm (recursive-inline `jmp .` trap). `M51-GFX: ok libm`.
- [x] Port pixman (generic C). `M51-GFX: ok pixman`.
- [x] Port FreeType (TrueType + smooth rasterizer); bundle the project's own
  B1nix Mono font. `M51-GFX: ok freetype`.
- [x] Port Fontconfig (+ expat); scans `/share/fonts` and matches families.
  `M51-GFX: ok fontconfig`.
- [x] Port Cairo (image surface + FreeType backend); render text end-to-end.
  `M51-GFX: ok cairo`.
- [x] Port xkbcommon (keymap compile + keycode→keysym); ships a built-in
  keymap, no xkeyboard-config dependency. `M51-GFX: ok xkbcommon`.
- [x] Port HarfBuzz (HB_TINY, built-in OpenType shaper, no FreeType/glib/icu).
  Built with the cross g++; `-fno-exceptions/-rtti/-threadsafe-statics` keep it
  off the C++ runtime so it links with no libstdc++. `M51-GFX: ok harfbuzz`.
- [x] Complete the Wayland protocol surface: `wl_output` (mode/scale/geometry)
  and clipboard (`wl_data_device` selection with fd-forwarded transfer) added;
  input (`wl_seat`) and window (`xdg-shell`) already present from M47-M49.
  `M51-GFX: ok wl-output`, `ok clipboard`.
- [x] Run a Cairo Wayland app with scalable fonts via displayd
  (`m51_cairo_wayland`, `M51-GFX: ok cairo-wayland`); shaped text via HarfBuzz,
  keyboard keysyms via xkbcommon, clipboard round-trip, and font matching via
  Fontconfig all verified.

**M51 complete** — all six libraries ported (pixman, FreeType, Fontconfig,
HarfBuzz, Cairo, xkbcommon; plus libm + expat), the Wayland protocol surface
completed, and the Cairo desktop app demo running. Nothing deferred.

## M52: Mesa and Accelerated OpenGL

Software EGL/TinyGL + Mesa OSMesa softpipe + GLSL shaders + VirGL 3D hardware
acceleration. Full implementation notes in [`browser-platform.md`](browser-platform.md).

- [x] `done` Software OpenGL/EGL via TinyGL (`M52-GFX: ok egl/tinygl/gl-triangle/path-software`). Both arches.
- [x] `done` VirtIO-GPU 2D software renderer path via displayd.
- [x] `done` VirGL 3D acceleration over VirtIO-GPU with host GPU pixel verification (`M52-GFX: ok virgl-negotiate/virgl-capset/virgl-3d-clear/path-accelerated`).
- [x] `done` Upstream Mesa OSMesa + Gallium softpipe via meson cross-build (`M52-GFX: ok mesa-context/mesa-render/mesa`). Both arches under KVM.
- [x] `done` GLSL programmable shader pipeline through Mesa compiler (`M52-GFX: ok shader-compile/shader-link/shader-render/glsl`). Both arches.

## M53: Browser Platform

Target browser: **NetSurf** (pure-C, framebuffer/Wayland frontend). Full
implementation notes in [`browser-platform.md`](browser-platform.md).

- [x] `done` Image/video codecs: zlib, libpng, libjpeg, libwebp (VP8), libvpx — all with smoke-verified roundtrips.
- [x] `done` Mesa through VirGL — host-GPU-accelerated OpenGL exposed to userspace via `/dev/virtio-gpu` ioctls (`M53-VIRGL`, `M53-GFX`).
- [x] `done` NetSurf libraries ported: libwapcaplet, libparserutils, libhubbub, libcss, libdom, libnsgif/libnsbmp/libnslog/libnsutils, libnsfb.
- [x] `done` NetSurf framebuffer browser cross-built and rendering real pages (`M53-NS: ok load/redraw/render`).
- [x] `done` Web access: HTTP via libcurl (`M53-WEB`), HTTPS via mbedTLS (`M53-HTTPS`), public-internet HTTPS (`M53-EXT-HTTPS`).
- [x] `done` On-screen `/dev/fb0` frontend, Wayland windowed frontend, interactive keyboard/mouse input (`M53-FB`, `M53-WL`, `M53-INPUT`). Seven render/interaction paths green both arches.
- [x] `done` TCP zero-window stall fixed; heavy-page OOM robustness (OOM-killer, TLB shootdown, freelist validation).
- [x] `done` SVG (libsvgtiny), JavaScript (Duktape), public-suffix list (libnspsl), JPEG-XL (libjxl), RISC-OS sprites (librosprite), utf8proc — all enabled features verified.
- [ ] `stubbed` Chromium assessment: blocked on sandbox/namespaces, GPU EGL/GBM/Vulkan, full C++ runtime. Details in [`chromium-assessment.md`](chromium-assessment.md).

## M54: Third-Party Port Feature Enablement

Re-enable upstream features that were switched off when each port was first
brought up, as the OS gains the syscalls/libc APIs they need. Full inventory,
status table and the conditions for each remaining item:
[`port-functionality.md`](port-functionality.md).

- [x] `done` **Foundation (libc/kernel).** Timed futex
  (`pthread_cond_timedwait`/`mutex_timedlock`, atomic+futex semaphores),
  `SYS_SETTIMEOFDAY` (`settimeofday`/`clock_settime`), `SYS_SCHED_GETAFFINITY`
  (+`cpu_set_t`/`CPU_*`), `scandir`/`alphasort`, POSIX `remove()`; the autotools
  wrapper now drops libc-provided `-l` names so `--enable-threads` links in any
  port.
- [x] `done` **Ports.** curl: `file://`, unix-sockets, alt-svc, HSTS,
  WebSockets, headers-api, dateparse, threaded resolver, **`libpsl`+`libidn2`**
  (IDN/PSL), **brotli** (`Content-Encoding: br` decode). mbedTLS:
  `HAVE_TIME`+`HAVE_TIME_DATE` (cert date validation), **`TIMING_C`** (timing
  layer over `gettimeofday`). bash: `/dev/tcp`, `/dev/udp`. dropbear: zlib +
  host lookup. wget: zlib + threads + PSL/IDN. NetSurf: JPEG + WebP + **JPEG-XL
  (libjxl)** codecs, **SVG (libsvgtiny)**, **JavaScript** (bundled Duktape via
  nsgenbind), `utf8proc`. Locale: `setlocale`/`localeconv`/`nl_langinfo` +
  `iconv` (UTF-8/UTF-16/UCS4/Latin1/ASCII). BusyBox: real `sched_getaffinity`.
- [x] `done` **Milestone closeout (M54).** The remaining feature flips are
  either landed (above), correctly **declined**, or **deferred** behind a real
  subsystem/large-library port. Full status: [`port-functionality.md`](port-functionality.md).
  - **Declined (low value / conflicts):** dropbear `--enable-syslog` (moves logs
    off the per-service `/var/log/sshd.log` the M32B smoke asserts);
    dropbear/curl `--harden` (`-fPIE -pie` conflicts with the fixed `0x2000000`
    load model); mbedTLS `NET_C` (the custom socket wrapper works); utmp/pam/
    lastlog port flips (cosmetic who/last accounting — libc shim already exists).
  - **Deferred (each its own large effort, revisit conditions in
    [`port-functionality.md`](port-functionality.md)):**
    - Modern HTTP: nghttp2 (HTTP/2), nghttp3+ngtcp2 (HTTP/3/QUIC), zstd
      content-encoding — needs those libraries ported to the b1nix ABI.
    - Mesa JIT + on-device windowing: port LLVM (LLVMpipe shader JIT) and add a
      DRI/GLX/EGL/GBM path beyond OSMesa-to-memory. LLVM is the long pole
      (tracked into M59). The softpipe path already works.
    - Full wide-char locale / NLS tables → bash `HANDLE_MULTIBYTE`, `nls`. libc
      is UTF-8-wired (`mbrtowc`/`wcrtomb`) + `iconv`; full locale catalogues are
      not present.

## M55: C++ Runtime

Prerequisite for every Chromium-class engine. Feasibility background and the
revisit conditions are in [`chromium-assessment.md`](chromium-assessment.md).

- [x] `done` Build and **run** libstdc++ with C++ exceptions (DWARF `.eh_frame`
  unwinding, registered with `libgcc` by `crt0`), RTTI, and thread-safe statics.
  The cross GCC 13.2 builds `libstdc++.a`/`libsupc++.a` (`--enable-threads=posix`,
  staged by `tools/enable-cxx-toolchain.sh`); `tools/b1nix-c++` links them with
  `libgcc` via `userspace/linker-cxx.ld` (keeps `.eh_frame`/`.gcc_except_table`).
  Verified end-to-end by `userspace/bin/cxx_smoke.cpp` (`CXX-SMOKE: ok` for
  ctors, stl, exceptions, **rtti** `dynamic_cast`/`typeid`/`bad_cast`,
  **static-init** `__cxa_guard`, **threads** `std::thread`/`mutex`/`atomic`).
- [x] `done` `std::thread`/`mutex`/`atomic` over the M29 pthread layer (verified
  in `cxx_smoke`). `std::condition_variable` links via gthr-posix.
- [ ] `deferred` `std::filesystem` and locale/`iostream` (`std::cout`) — thin
  wrappers over the existing VFS / UTF-8 libc; add when a port needs them.
- [x] `done` Validate the runtime with a real modern engine: **litehtml** (C++
  HTML/CSS layout engine + bundled gumbo HTML parser) is ported
  (`tools/build-litehtml.sh`, CMake cross-build against b1nix libstdc++) and
  runs end-to-end on b1nix — `userspace/bin/m55_litehtml.cpp` feeds it a styled
  page, and the engine parses HTML, cascades CSS, lays out the box tree and
  emits draw calls. Verified both arches: `M55-LITEHTML: ok parse/layout/draw`
  (asserts the cascade — `h1` 32px above `p` 16px — and block ordering). This
  exercises libstdc++ exceptions/RTTI, STL and `shared_ptr` on a non-trivial
  codebase. (Closing this needed a real libc gap fixed: `fegetenv`/`fesetenv`/
  `feholdexcept`/`feupdateenv`, which openlibm's `nearbyint` pulls in.)
  Ladybird LibWeb/LibJS remains a future option if a heavier engine is wanted.

## M56: Event Loop and IPC Primitives

- [ ] `planned` Add `epoll` (edge-triggered), `eventfd`, `timerfd`, and
  `signalfd` for the Chromium/base message loop.
- [ ] `planned` Implement `SCM_RIGHTS` ancillary FD passing over `AF_UNIX`
  (`sendmsg`/`recvmsg` exist; cmsg FD-passing does not).
- [ ] `planned` Add memfd sealing (`F_ADD_SEALS`) and cross-process shared
  `mmap` (memfd base in M48), enough to carry Mojo shared buffers.

## M57: Multiprocess Model

- [ ] `planned` Make fork/exec, FD inheritance, and FD brokering robust enough
  for the `--no-sandbox` model (drops the sandbox, keeps multiple processes).
- [ ] `planned` Bring up a minimal Mojo core over the M56 primitives.

## M58: V8

- [ ] `planned` Port V8 interpreter-only (`--jitless`, Ignition, no TurboFan) to
  avoid W^X mappings and GC/deopt signals; run JS through `d8`.
- [ ] `deferred` Add the optimizing JIT (W^X executable mmap, GC/deopt signals).

## M59: EGL and GL for the Browser

- [ ] `planned` Add an EGL implementation over the M52/M53 virgl host-GPU GL (or
  ANGLE → b1nix GL), closing the missing DRI/EGL/GBM path.
- [ ] `planned` Provide a software Skia (Ganesh) raster fallback.

## M60: Ozone Platform

- [ ] `planned` Add an Ozone platform backend (headless first, then a
  displayd/Wayland-shaped one): window, surface, input, and vsync.

## M61: Chromium Build Target

- [ ] `planned` Add b1nix to GN/Ninja `build/config`, `base/`, `//net`,
  `sandbox/` (stubbed), and Ozone, and integrate the cross-toolchain (a
  multi-month, ~100 GB build that upstream does not support).

## M62: content_shell

- [ ] `planned` Render a page to a bitmap with `content_shell --headless
  --no-sandbox` and verify the pixels — the "Chromium runs" milestone.

## M63: Sandbox

- [ ] `deferred` Add the real sandbox (seccomp-bpf + user/PID/net namespaces +
  setuid sandbox); none exist today. Only if process isolation is required.
