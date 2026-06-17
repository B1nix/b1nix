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
- [ ] `planned` Replace userspace-provided `sa_restorer` with a kernel-owned
  signal-return trampoline.

## M43: Real-Filesystem Validation and NTFS

- [x] Validate genuine ext2/ext3/ext4/exFAT/NTFS images and large-file reads.
- [x] Verify persistent writes on ext2/ext3/ext4.
- [x] Fix AHCI page-crossing DMA, ext2 xattr parsing, and exFAT filename case.
- [x] Add a read-only NTFS driver with resident/non-resident data and indexes.
- [ ] `planned` Fix creation at runtime-created mountpoints.
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

### Open hardening (second-round audit — Part 3 of the audit doc)

A follow-up SMP/lifetime sweep of the subsystems outside the VFS/process core
found further real bugs, none yet fixed. Ordered by severity; details and
mechanisms in [`vfs-process-audit.md`](vfs-process-audit.md) Part 3.

- [x] `bug` **filelock has no lock at all** (FL-1, critical) — FIXED: global
  spinlock around file_locks[] (dropped before the F_SETLKW sleep), NULL-checked
  alloc, and lock ownership keyed by the process tgid so a CLONE_FILES thread's
  locks release at fd-table teardown (FL-3).
- [x] `bug` **`terminate_group_siblings` resurrects DEAD/REAPING siblings**
  (M46-1, high) — FIXED: per-state CAS instead of a plain store. — plain `state = READY` store instead of a CAS; UAF/double-run
  under CLONE_THREAD on APs. Regression in the exit_group code.
- [x] `bug` **UNIX-socket peer back-pointer UAF** (F1-unix, high) — FIXED:
  refcounted unix_socket_data; peer links + backlog slots are counted refs,
  send/poll pin the peer while using it, close tears the link + marks the peer
  disconnected.
- [x] `bug` **concurrent journal transactions corrupt heap + on-disk journal**
  (F1-journal, high) — FIXED: a global sleeping mutex serializes whole
  transactions (start→commit/abort), removing the handles[]/s_start races.
- [x] `bug` **block cache keeps two valid entries per (dev,lba)** (F2-blk, high)
  — FIXED: re-find after evict, and a BUSY in-progress entry is hash-published
  before the DMA so concurrent misses wait instead of filling a duplicate.
- [x] `bug` **xattr list mutated/walked with no inode lock** (X-1, high) —
  FIXED: inode write-lock for set/remove, read-lock for get/list.
- [x] `bug` **`page_cache_flush_inode` races truncate's in-place zeroing**
  (PC-1, medium-high) — FIXED: fsync/close hold the inode lock across the flush.
- [x] `bug` **loop device stores backing node with no `vfs_node_get`** (F4-loop,
  medium) — FIXED: SET_FD pins the node + rejects non-regular files; CLR_FD
  releases it; block cache invalidated on swap.
- [x] `bug` **icache stores raw `vfs_inode*` with no reference** (IC-1, medium,
  latent) — FIXED (dangling pointer): vfs_inode_put invalidates the icache entry
  before freeing the inode; full reference-pinning still deferred.
- [x] `bug` **orphaned-pgrp false-negative with ≥2 children in one pgrp**
  (M46-2, medium) — FIXED: is_pgrp_orphaned ignores the exiting task as a
  parent (its children are about to be reparented to init).
- [ ] `bug` Lower-severity / pending-verification leads: mqueue & shm have no
  locking (shm also leaks on exit — `shm_detach_all` is never called); futex
  lacks exit-time waiter cleanup and PROCESS_SHARED wakeups; aio ctx UAF on
  exit; UNIX accept/recv lost-wakeups; journal crash-atomicity (write-ahead
  ordering, pre-commit fs writes, unbounded recovery walk); swap/eviction ring
  tables unlocked; signal-death exit skips reparent/orphan handling (M46-3).

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

Software EGL/TinyGL plus Mesa OSMesa softpipe, GLSL shaders, and VirGL 3D
hardware acceleration (host virtio-gpu-gl). VirGL needs a host with
virglrenderer; the smoke auto-detects it and honestly skips elsewhere.

- [x] `done` Software OpenGL + EGL: ported TinyGL (software GL 1.1 subset) as
  `libTinyGL.a` and a b1nix `libEGL.a` shim (`userspace/libegl/b1egl.c`,
  `userspace/include/EGL/egl.h`). `eglCreateWindowSurface` targets a b1gui
  wl_shm window; `eglSwapBuffers` blits the rendered framebuffer and commits to
  displayd. End-to-end app `m52_gl_smoke` clears + draws a 3D triangle through
  the real GL pipeline and verifies the presented pixels.
  `M52-GFX: ok egl/tinygl/gl-triangle/path-software`. Both arches green.
- [x] `done` Software renderer presented through the VirtIO-GPU 2D scanout
  (`kernel/dev/virtio_gpu.c`, RESOURCE_CREATE_2D / SET_SCANOUT /
  TRANSFER_TO_HOST_2D / RESOURCE_FLUSH, from M50) via displayd.
- [x] `done` VirGL 3D acceleration over VirtIO-GPU. `kernel/dev/virtio_gpu.c`
  negotiates `VIRTIO_GPU_F_VIRGL` with a virglrenderer-backed host device,
  queries the VIRGL capset, creates a 3D context + render-target resource, and
  submits a hand-built virgl command stream (CREATE_OBJECT SURFACE +
  SET_FRAMEBUFFER_STATE + CLEAR — byte-compatible with Mesa's
  `virgl_protocol.h`) via `SUBMIT_3D`. It then `TRANSFER_FROM_HOST_3D`s the
  GPU-rendered pixels back to guest memory and verifies them: the host GPU
  renders the clear and the read-back BGRA pixel matches the requested colour
  exactly (`0xff4080bf`). `M52-GFX: ok virgl-negotiate/virgl-capset/
  virgl-3d-clear/path-accelerated`. Exercised on the Linux/KVM host
  (QEMU 11 `-device virtio-gpu-gl-pci -display egl-headless`, virglrenderer
  1.3.0, AMD Radeon via `/dev/dri/renderD128`); the smoke harness auto-detects
  the virgl-capable device and the selftest is a clean no-op (recorded as a
  skip, software path still verified) on hosts without it (e.g. macOS QEMU
  built without virglrenderer). Full Mesa-on-virgl (gallium `virgl` driver +
  libdrm winsys) is the remaining layer above this transport.
- [x] `done` Port **real upstream Mesa** (OSMesa + Gallium **softpipe**, no
  LLVM) via a meson cross-build (`tools/build-mesa.sh` + `b1nix-mesa-cc` +
  `enable-cxx-toolchain.sh`). `m52_osmesa` drives the unmodified OSMesa API
  (`OSMesaCreateContext`/`MakeCurrent`) through the softpipe rasterizer, renders
  a 3D triangle off-screen, pixel-verifies it, and presents to displayd:
  `M52-GFX: ok mesa-context/mesa-render/mesa`. **Verified on both arches**
  (x86_64 625/0, i686 624/0, single-CPU + `-smp 4`) under KVM.
  - Required converting **both kernels to higher-half** so the ~12 MB Mesa demo
    (embedded in the initramfs) no longer pushes the kernel image over the
    0x2000000 userspace base and into userspace's address range — see the
    higher-half kernel item under M2 (Memory Management). Plus an ELF
    loader shared-page fix, main-thread TLS, a virtio-gpu TSC-bounded wait, and
    a libc/toolchain round (memalign, gthr-posix libstdc++, open_memstream, ...).
- [x] `done` Programmable GLSL shader pipeline on top of Mesa. `m52_glsl`
  compiles and links a `#version 120` vertex+fragment shader pair through the
  real Mesa GLSL compiler, draws an interleaved VBO with
  `glBindAttribLocation`/`glDrawArrays`, and pixel-verifies the resulting
  Gouraud (per-vertex-interpolated) triangle rendered by softpipe before
  presenting to displayd: `M52-GFX: ok shader-compile/shader-link/
  shader-render/glsl`. Verified on both arches (x86_64 629/0, i686 628/0).
  (Full EGL + GLES2 ES contexts need Mesa's EGL/gbm stack, which rides on the
  VirGL/libdrm work; desktop GLSL shaders are the verifiable surface today.)

## M53: Browser Platform

Target browser: **NetSurf** (small, modular, pure-C, framebuffer/Wayland
frontend). Building the prerequisite codec + runtime stack first, each with a
no-fake-pass smoke test.

- [x] `done` Image + video-keyframe codecs (NetSurf loader dependencies), all
  freestanding-compiled against the b1nix userspace ABI and verified by
  encode/decode roundtrips with pixel checks (`tests/smoke.sh`):
  - **zlib** 1.3.1 (`tools/build-zlib.sh`) — one-shot + streaming deflate/inflate
    + crc32. `M53-ZLIB: ok compress/uncompress/roundtrip/crc32/stream`.
  - **libpng** 1.6.43 (`tools/build-libpng.sh`, over zlib + libm) — PNG
    encode→decode, pixels byte-for-byte identical. `M53-PNG: ok
    encode/decode-header/decode`.
  - **libjpeg** (IJG v9f, `tools/build-libjpeg.sh`) — JPEG encode→decode, pixels
    within tolerance. `M53-JPEG: ok encode/decode-header/decode`.
  - **libwebp** 1.4.0 (`tools/build-libwebp.sh`) — WebP lossless encode→decode
    byte-identical; WebP lossy is a VP8 intra (keyframe) decoder, the WebM video
    bitstream family. `M53-WEBP: ok encode/info/decode`.
  - Added the standard `*_10_EXP` macros to `userspace/include/float.h` (libpng
    gamma math needs them).
- [x] `done` Full-motion video codec: **libvpx** VP8 decode (the WebM / browser
  video codec). `tools/build-libvpx.sh` runs libvpx's own configure for the
  portable `generic-gnu` target to generate its `vpx_config.h` + `*_rtcd.h`
  headers, then recompiles the VP8-decode C sources with the b1nix toolchain.
  `m53_libvpx_smoke` cross-verifies against libwebp: it encodes a lossy WebP
  (which *is* a VP8 keyframe), pulls the raw VP8 bitstream from the RIFF "VP8 "
  chunk, decodes it with libvpx to an I420 frame, and checks the luma plane
  reproduces the original within tolerance. `M53-VPX: ok
  webp-vp8-frame/decode-init/decode/luma`.
- [~] `partial` Mesa **through VirGL** — host-GPU-accelerated 3D.
  - [x] `done` Kernel exposes the VirGL 3D transport (M52) to userspace via
    `/dev/virtio-gpu` (ioctls: GET_CAPS, RES_CREATE + mmap window, SUBMIT a virgl
    command stream, TRANSFER_FROM_HOST; a single implicit 3D context). `m53_virgl_smoke`
    creates a render target, submits a virgl CLEAR **from userspace**, and reads
    the GPU-rendered pixel back through the mmap: `M53-VIRGL: ok
    caps/resource/submit/path-accelerated`. Same kernel/userspace split a Mesa
    virgl winsys uses; auto-skips on non-virgl hosts.
  - [ ] `planned` A Mesa gallium `virgl` winsys on this ABI + Mesa rebuilt with
    `-Dgallium-drivers=virgl`, so the full OpenGL/GLES API runs on the host GPU
    (vs softpipe).
- [ ] `planned` Runtime gaps: robust pthread, futex, TLS (mostly done in M29),
  real dynamic loading (`dlopen` of `.so` — currently a stub), ICU.
- [x] `done` Port NetSurf's own libraries and the framebuffer frontend, and
  **render a real page**. The full dependency chain is ported and freestanding-
  compiled against the b1nix ABI, each with a no-fake-pass smoke:
  **libwapcaplet** (string internment, `M53-WAPCAPLET`), **libparserutils**
  (input + bundled charset codecs, `M53-PARSERUTILS`), **libhubbub** (HTML5
  tokeniser, `M53-HUBBUB`), **libcss** (CSS parse + cascade/selection,
  `M53-LIBCSS`), **libdom** (DOM via the hubbub binding, `M53-LIBDOM`),
  **libnsutils/libnsgif/libnsbmp/libnslog** (`M53-NSUTILS/NSGIF/NSBMP/NSLOG`),
  and **libnsfb** (framebuffer surface + plotters). The complete **NetSurf
  framebuffer browser** is then cross-built for b1nix with its native build
  system driven by the b1nix cross-gcc (`tools/build-netsurf-fb.sh`), packaged
  into the initramfs with its resources + a test page
  (`tools/gen_netsurf_initramfs.sh`), and a headless `-T` render self-test loads
  a local `file://` HTML page (styled text + a PNG image), lays it out
  (extents 782x552) and paints it into a framebuffer (117849 non-background
  pixels verified). `M53-NS: ok load/redraw/render`. Runtime note: the file://
  fetcher uses the `fread()` path (`HAVE_MMAP` off — b1nix can't mmap an
  initramfs object).
- [x] `done` **Web access** — NetSurf fetches pages over a real TCP/HTTP
  connection through libcurl, not just `file://`. libcurl is enabled with
  genuine cookie, zlib (gzip) and MIME support (no stubbed/skipped options) and
  staged into the build via a `libcurl.pc`; `NETSURF_USE_CURL=YES`. A minimal
  in-VM loopback HTTP server (`m53_httpd`) serves a styled page, NetSurf fetches
  `http://127.0.0.1:8080/` over the network stack and renders it
  (`M53-HTTPD: ready`, `M53-WEB: has-content=1 / ok render`). The render
  self-test advances time with `nanosleep` (kernel tick sleep) rather than
  spinning on `gettimeofday`.
- [x] `done` **HTTPS** — NetSurf fetches over a real TLS 1.2 connection with
  certificate verification. A loopback HTTPS server (`m53_httpsd`, mbedTLS, using
  the M32 test PKI with SAN IP:127.0.0.1) serves a styled page; NetSurf fetches
  `https://127.0.0.1:8443/` with `--ca_bundle` pointed at the test CA, so
  libcurl (mbedTLS) verifies the server certificate. `M53-HTTPS: has-content=1 /
  ok render`. libcurl is built with genuine cookie + zlib + MIME support.
- [x] `done` **On-screen frontend** — NetSurf draws straight to the real
  hardware framebuffer, not just an off-screen buffer. A b1nix `/dev/fb0` libnsfb
  surface (`-f b1nix`) opens the M47 fb device, mmaps it and flushes damage via
  `B1NIX_FBIOFLUSH`; NetSurf lays out and paints a page at the real screen
  resolution (1280x800) and presents it on the virtio-gpu display.
  `M53-FB: ok render`.
- [x] `done` **Public-internet HTTPS** — NetSurf fetches a real public website
  (`https://example.com/`) over off-link TLS and renders it, verifying the cert
  against the shipped Mozilla CA bundle (the same libcurl/mbedTLS path M32's
  `ext-https` uses). It is optional: it skips cleanly (`M53-EXT-HTTPS:
  unsupported`) when the usernet has no off-link route, so the offline smoke
  stays green; with outbound enabled it fetches and paints the full page.
- [x] `done` **TCP zero-window stall fixed** — a multi-host HTTPS page (e.g.
  google.com) used to take ~20 s because each fresh connection whose TLS cert
  flight overflowed the 4 KiB receive buffer advertised a zero window and then
  sat through the peer's persist-timer backoff (~5 s per connection). The kernel
  TCP stack now sends an unsolicited window-update ACK when the app drains a
  throttled receive buffer (`kernel/net/tcp.c`), and the receive buffer/window
  grew 4 KiB → 16 KiB. Measured against a real page: a fresh-host TLS handshake
  dropped 5.5 s → 60 ms and full google.com load ~20 s → ~1.1 s.
- [x] `done` **Heavy-page memory robustness.** A real google.com tab with JS can
  exhaust RAM; that exposed kernel OOM-path bugs. Fixed: (1) `freelist_pop`
  validates each node before dereferencing it, so a freed-frame corruption
  degrades to a logged recovery (bitmap scan) instead of a GP-fault **panic**;
  (2) the page-eviction/swap path issues a cross-CPU **TLB shootdown**
  (`tlb_shootdown_page`) after marking a page swapped — the local `invlpg` left
  another CPU's stale TLB mapping a freed frame (the use-after-free behind the
  panic); (3) a last-resort **OOM-killer** SIGKILLs the userspace task demanding
  the memory (sparing kernel threads and init) instead of returning ENOMEM into
  a console-flooding retry storm; (4) the OOM diagnostic is throttled to once per
  pressure episode. `make run` now defaults to `-m 1024` (QEMU's 128 MB default
  OOM'd on heavy pages) — google.com loads and renders at 1 GB. Verified: forced
  256 MB cleanly OOM-kills the memory-heavy tests (`[OOM-KILL] killing ...`) with
  no panic; both arches stay green.
- [x] `done` **SVG images + JavaScript + public-suffix list.** SVG is decoded by
  **libsvgtiny** (`tools/build-libsvgtiny.sh`, over libdom's expat XML binding)
  and `NETSURF_USE_NSSVG=YES`; the framebuffer frontend's `plot->path` (an
  upstream no-op stub) is given a real polygon-fill/stroke implementation so SVG
  actually paints. **JavaScript** runs via the compiled-in Duktape engine with
  the `enable_javascript` option flipped on. The **public-suffix list**
  (**libnspsl**, `tools/build-libnspsl.sh`, `NETSURF_USE_NSPSL=YES`) scopes
  cookies to a registrable domain. The render self-test loads a page with an
  `<img>` SVG (solid green block) and a script that paints a solid blue block,
  and asserts all three colours appear in the framebuffer: `M53-NS: ok svg /
  ok js / ok jxl`.
  Also enabled: **RISC-OS sprite** decoding (**librosprite**,
  `tools/build-librosprite.sh`, `NETSURF_USE_ROSPRITE=YES`), **utf8proc**
  (`tools/build-libutf8proc.sh`, `NETSURF_USE_UTF8PROC=YES`) for IDNA Unicode in
  `utils/idna.c`, and **JPEG-XL** (**libjxl** 0.11.1 + highway + brotli + skcms,
  `tools/build-libjxl.sh` — a CMake cross-build with the b1nix C++ toolchain,
  decode-only, `NETSURF_USE_JPEGXL=YES`). Enabling libjxl required completing the
  C++ toolchain: `userspace/include/math.h` filled out to full C99 (erf/lgamma/
  tgamma/long-double variants, double_t/float_t) with the classification
  helpers made C++-safe, and `enable-cxx-toolchain.sh` turning on libstdc++'s
  C99 math/stdint/fenv feature macros (the toolchain had been built against an
  empty sysroot) — HarfBuzz and Mesa re-verified against the updated toolchain.
  A real `<img>` JXL (magenta block) is decoded and asserted: `M53-NS: ok jxl`.
  Still off: **OpenSSL** (redundant — mbedTLS is the TLS backend); **PDF export**
  (the **libharu** library *is* ported — `tools/build-libharu.sh` builds
  `libhpdf.a` — but NetSurf 3.11's PDF glue is upstream bit-rot: `font_haru.c`
  needs the removed `desktop/font.h` and a `struct font_functions` model that
  `print_make_settings` no longer uses, so enabling it needs that dead code
  revived, not just a flag); and **video** (GStreamer — a whole multimedia
  framework on GLib/GObject with a dlopen plugin model, not a self-contained
  codec; the realistic path is a custom libvpx-based content handler). All
  *browsing* formats (every image codec incl. JPEG-XL, JS, HTTPS, Unicode, PSL)
  are now enabled.
- [x] `done` **Wayland frontend** — NetSurf runs as a windowed client of the
  b1nix display server (displayd) over the b1display/libgui (Wayland-shaped)
  protocol. A new libnsfb "displayd" surface gives NetSurf a compositor window
  to paint into, presents damage with b1gui_present, and translates compositor
  pointer/keyboard events to libnsfb events. `M53-WL: ok render`.
- [x] `done` **Interactive input** — the framebuffer frontend is interactive:
  the /dev/fb0 libnsfb surface reads /dev/input/event* (keyboard + mouse) and
  feeds pointer motion, mouse buttons and keysyms into NetSurf's event loop. A
  self-test (-I) confirms synthesized move + click + key events all arrive
  (`M53-INPUT: ok mouse-move/mouse-click/key`). Both libnsfb surfaces (`b1nix`
  and `displayd`) share one scancode→keysym map (`libnsfb-b1keymap.h`) that is
  shift-aware and resolves the editing/navigation keys NetSurf text fields need
  — backspace, delete, the arrows, home/end and page up/down — so typing,
  deletion and caret movement in the address bar all work. gterm grew the
  matching arrow/nav→ANSI-escape handling, so line editing and shell history
  work in the terminal too. SysV `SHMMAX` was raised 1 MB → 32 MB so a single
  shared segment can hold a full-screen graphics/framebuffer buffer.
  Seven render/interaction paths green both arches (x86_64 739/0, x86 738/0):
  file:// (off-screen + on-screen /dev/fb0 + windowed displayd), loopback HTTP,
  loopback HTTPS, public-internet HTTPS, and keyboard/mouse input.
- [ ] `stubbed` Use that port to assess Chromium; add Vulkan only when a
  browser or another real application requires it. Assessment in progress and
  tracked in [`chromium-assessment.md`](chromium-assessment.md) — current
  verdict: blocked on whole subsystems (sandbox/namespaces, GPU EGL/GBM/Vulkan,
  full C++ runtime), not pursued; revisit conditions listed there.

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
  WebSockets, headers-api, dateparse, threaded resolver. mbedTLS:
  `HAVE_TIME`+`HAVE_TIME_DATE` (cert date validation). bash: `/dev/tcp`,
  `/dev/udp`. dropbear: zlib + host lookup. wget: zlib + threads. NetSurf:
  JPEG + WebP codecs and **JavaScript** (bundled Duktape via nsgenbind).
  BusyBox: real `sched_getaffinity` (affinity stub removed). Verified both
  arches (x86 727/0, x86_64 728/0).
- [ ] `deferred` **curl `--with-libpsl`/`--with-libidn2`** — libs exist (wget
  uses them) but curl's cross libpsl conftest cannot link the transitive
  `idn2`/`unistring` chain. wget already covers PSL/IDN.
- [ ] `planned` **Full feature parity** — the remaining disabled features, each
  blocked on a real subsystem or a large library port (details and enable
  conditions in [`port-functionality.md`](port-functionality.md)):
  - System logging + login accounting: **foundation done** — kernel `/dev/log`
    sink (→ kernel log) + libc `syslog`/`utmp`/`wtmp` + `pam_*` shim, verified by
    M29 smoke. Per-port flips evaluated and left off (low value): dropbear
    `--enable-syslog` would break the per-service `/var/log/sshd.log` the M32B
    lifecycle smoke asserts; `--enable-harden` conflicts with the fixed load
    address; utmp/pam/lastlog are cosmetic who/last accounting. See
    [`port-functionality.md`](port-functionality.md).
  - PAM: a minimal `libpam`/policy engine → dropbear `--enable-pam`,
    login/su/passwd stacks.
  - Full locale / multibyte: wide-char/locale + a real `iconv` → bash
    `HANDLE_MULTIBYTE`, `nls`, NetSurf `libiconv`/`utf8proc`.
  - Modern HTTP + compression: port nghttp2/nghttp3/ngtcp2/brotli/zstd (and
    libjxl) → curl/wget HTTP/2+HTTP/3, NetSurf JPEG-XL.
  - Mesa JIT + windowing: port LLVM (LLVMpipe shader JIT) and add a
    DRI/GLX/EGL/GBM path beyond OSMesa-to-memory.
  - TLS timing/native-net + harden: mbedTLS `TIMING_C`/`NET_C`, dropbear/curl
    `--harden`, bash `bash-malloc`.
