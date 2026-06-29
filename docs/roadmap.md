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
- [x] Link the x86_64 kernel at a higher-half VA (0xFFFFFFFF80000000), loaded at
  physical 1M. Kernel symbols
  no longer share the low address range with userspace (base 0x2000000), so a
  large kernel image (e.g. an embedded Mesa initramfs) can no longer overlap and
  corrupt a userspace process's view of kernel data.
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

- [x] Port Binutils, Clang/GCC, libstdc++, and GNU Make.
- [x] Build larger programs and the kernel with the cross toolchain.
- [x] Compile and link the full kernel inside B1NIX; boot the exact result.
- [x] Provide an in-guest assembler/linker/make workflow.
- [x] `done` **Self-host the kernel with the native Clang/LLVM toolchain.**
  `make selfhost-clang` (tools/inguest/selfhost-proof.sh) ships clang-22 + ld.lld
  in an ext4 ram0 module; with `b1nix.selfhostbuild` the kernel compiles all 107
  kernel C TUs in-guest with its own clang and links a complete kernel.elf with
  its own ld.lld — the same toolchain the host uses (`M26-SELFHOST: ok kernel-elf`).
  The 6 hand-written `.S` stubs + generated `kallsyms.o` are pre-staged (clang's
  in-guest `cc1as` re-exec can't resolve its own path on b1nix yet; tracked as a
  follow-up). RAM floor: a clean **`b1nix.selfhostonly`** boot mode (init idles —
  no smoke suite, no production services competing for RAM) + the disk-sourced
  toolchain (`b1nix.selfhostdisk`, sata0) completes the full 107-TU compile + link
  down to **2 GiB** (`SELFHOST_DISK=1 SELFHOST_MEM_MB=2048`, verified at 2 GiB and
  4 GiB). The old 16 GiB figure was the concurrent `b1nix.test=1` smoke sequence
  (Mesa/V8/NetSurf) competing for RAM, not the loader; at 2 GiB the link grinds
  slowly through page-cache eviction but still finishes.
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

Source-level ports remain preferable to a Linux compatibility layer. A real
foundation is in place: a static Linux x86_64 ELF runs on b1nix via a
syscall-number translation layer keyed off a per-image binary personality
(`/bin/m40-linux-hello`, `M40-LINUX: ok run-static`, x86_64 only).

- [ ] `partial` Translate Linux x86_64 syscall numbers and semantics.
  Number-translation table (`kernel/syscall/linux_abi.c`, ~85 calls incl.
  gettid/link/fchmod/fchown/ftruncate/alarm/sync/fchdir/setpriority) maps the
  Linux x86_64 ABI to the existing native handlers for Linux-personality tasks;
  unmapped calls return `-ENOSYS`. `stat`/`fstat`/`lstat`, `uname` and
  `getdents64` additionally get a semantic translation to the Linux x86_64
  `struct stat` / `struct utsname` / `struct linux_dirent64` layouts
  (`linux_stat_from_b1nix`, `linux_utsname_from_b1nix`, `sys_linux_getdents64`;
  verified by `M40-LINUX: ok fstat` / `ok uname` / `ok getdents64`). Remaining
  struct/flag differences (signal-frame layout, ioctl/termios) are not yet
  translated.
  Linux binaries also reuse the matching native calls verified end to end:
  anonymous `mmap`/`munmap` (flags/prot already match) and
  `clock_gettime` (timespec matches), via `M40-LINUX: ok mmap` / `ok clock`.
- [ ] `partial` Load static Linux binaries and glibc's `PT_INTERP`.
  Static (`ET_EXEC`) Linux binaries load and run. Dynamic `PT_INTERP` / glibc
  ld.so is `planned` (the kernel does eager in-kernel linking, not an ld.so
  handoff).
- [ ] `partial` Add Linux-shaped process startup, auxv, vDSO, TLS, and signals.
  The existing SysV initial-stack (argc/argv/envp + minimal auxv AT_ENTRY/AT_PHDR)
  and x86 TLS apply to Linux tasks too; `arch_prctl(ARCH_SET_FS/GET_FS)` is
  translated to the per-task FS base (`M40-LINUX: ok arch-prctl`). A full Linux
  auxv vector and vDSO are `planned`. Basic signal delivery works: a
  Linux-personality task can `rt_sigaction` a handler, `kill` itself and have the
  handler run and return, with a full Linux<->b1nix signo remap
  (`linux_signo_to_b1nix`/`b1nix_signo_to_linux`, b1nix `SIGUSR1=19` vs Linux
  `10`) applied to rt_sigaction, kill and the delivered handler's signo argument,
  plus a Linux-personality sigreturn trampoline emitting `rt_sigreturn` (15) (not
  b1nix `SYS_SIGRETURN` (99), which would re-translate to `sysinfo`). Verified by
  `M40-LINUX: ok signal`. `rt_sigprocmask` also remaps the sigset_t bit positions
  and the swapped Linux/b1nix `SIG_UNBLOCK`/`SIG_SETMASK` how-values
  (`M40-LINUX: ok sigmask`). `tkill`/`tgkill` self-signal (glibc raise/pthread_kill
  path) work with signo remap (`M40-LINUX: ok tgkill`). `SA_SIGINFO` 3-arg
  handlers get a kernel-built Linux `siginfo_t` (si_signo) and `ucontext_t`
  (uc_mcontext gregs from the interrupted frame, fpregs NULL) placed above the
  sigframe (`M40-LINUX: ok siginfo`). Still `planned`: `sigqueue` and a fully
  populated siginfo for fault signals (si_addr/si_code).
- [ ] `planned` Fill Linux-compatible `/proc` and `/sys` entries.
- [x] `done` Detect Linux ELFs through a separate binary personality.
  `elf64_is_linux_binary` tags `PERSONALITY_LINUX` from `EI_OSABI==ELFOSABI_LINUX`
  or a GNU `NT_GNU_ABI_TAG` note with OS=Linux; it gates the translation layer.

## M41: Large Physical Memory

- [x] Remove the old x86_64 64 GiB ceiling and verify a 16 GiB boot.
- [ ] `planned` Verify full usable memory and defensive e820 handling on real
  hardware.

## M42: Upstream BusyBox Port

- [x] Cross-build upstream BusyBox as an isolated static multicall binary.
- [x] Port core file, text, archive, process, account, storage, and networking
  applets in tested waves.
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
- Verified by `BASH-SMOKE` + `M32B-SSH` markers, single-CPU and `-smp 4`.

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
  SHM stays implemented natively in `displayd` by design (evaluated: adopting
  upstream `wl_shm` needs SIGBUS-on-EOF + the full server object model — a
  net-negative rewrite; `displayd` validates buffer extents itself).
- [x] Teach `displayd` the real protocol: `wl_shm`,
  `wl_compositor`/`wl_surface`, `wl_seat`, and an `xdg-shell` subset.
- [x] Send a real `XKB_V1` keyboard keymap (US/evdev, embedded in
  `userspace/bin/xkb_keymap_us.h`) plus `wl_keyboard.modifiers` and repeat
  metadata, so stock toolkits (GTK/Qt/SDL) translate keys correctly.
- [x] Run the stock SHM/xdg-shell wire flow (`m49-smoke`, equivalent to the
  weston-simple-shm protocol path); `xdg_wm_base.ping`/`pong` hung-client
  detection and the `xdg_toplevel` WM requests (title/app_id/ack/close +
  move/resize/maximize/fullscreen). Full conformance delta (what is omitted) in
  [`wayland-conformance.md`](wayland-conformance.md).
- [x] Extended protocol coverage: full `wl_data_device` drag-and-drop (on top of
  clipboard), `wp_viewporter`, `wl_subcompositor`, `wp_presentation`,
  `zwp_linux_dmabuf_v1` (advertised, import honestly rejected — no GEM path),
  `wl_touch` from a virtio touchscreen (`/dev/input/event2`), and
  `xdg_toplevel.set_minimized` backed by a panel taskbar. Capacities raised
  (clients/surfaces/buffers 32). The internal architecture items (`wl_shm` via
  `libwayland-server`; `wayland-scanner` codegen) were evaluated and
  intentionally **kept native** — both are net-negative rewrites for zero
  functional gain (rationale in [`wayland-conformance.md`](wayland-conformance.md)).

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
  staged by `tools/toolchain/enable-cxx-toolchain.sh`); `tools/toolchain/bin/b1nix-c++` links them with
  `libgcc` via `userspace/linker-cxx.ld` (keeps `.eh_frame`/`.gcc_except_table`).
  Verified end-to-end by `userspace/bin/cxx_smoke.cpp` (`CXX-SMOKE: ok` for
  ctors, stl, exceptions, **rtti** `dynamic_cast`/`typeid`/`bad_cast`,
  **static-init** `__cxa_guard`, **threads** `std::thread`/`mutex`/`atomic`).
- [x] `done` `std::thread`/`mutex`/`atomic` over the M29 pthread layer (verified
  in `cxx_smoke`). `std::condition_variable` links via gthr-posix.
- [x] `done` `std::filesystem` and locale/`iostream`. `std::cout`/`cin`/`cerr`,
  `ostringstream`/`istringstream` and `std::filesystem` (create/iterate/stat/
  remove) run end-to-end against the hosted libstdc++ over the b1nix VFS/UTF-8
  libc — verified by `userspace/bin/m55_iostream.cpp` (`M55-IOSTREAM: ok
  cout/sstream/cin/filesystem`, both arches; `cin` reads a real fd 0 fed by a
  pipe). Needed no new libc symbols; the root-cause fix was `linker-cxx.ld` not
  collecting libstdc++'s legacy `.ctors.NNNNN` init sections, so the iostream
  `ios_base::Init` global never ran and the first `std::cout` faulted.
- [x] `done` Validate the runtime with a real modern engine: **litehtml** (C++
  HTML/CSS layout engine + bundled gumbo HTML parser) is ported
  (`tools/ports/build-litehtml.sh`, CMake cross-build against b1nix libstdc++) and
  runs end-to-end on b1nix — `userspace/bin/m55_litehtml.cpp` feeds it a styled
  page, and the engine parses HTML, cascades CSS, lays out the box tree and
  emits draw calls. Verified both arches: `M55-LITEHTML: ok parse/layout/draw`
  (asserts the cascade — `h1` 32px above `p` 16px — and block ordering). This
  exercises libstdc++ exceptions/RTTI, STL and `shared_ptr` on a non-trivial
  codebase. (Closing this needed a real libc gap fixed: `fegetenv`/`fesetenv`/
  `feholdexcept`/`feupdateenv`, which openlibm's `nearbyint` pulls in.)
  Ladybird LibWeb/LibJS remains a future option if a heavier engine is wanted.

## M56: Event Loop and IPC Primitives

- [x] `done` `epoll` (level + `EPOLLET` + `EPOLLONESHOT`), `eventfd`, `timerfd`,
  `signalfd` — built on the existing `vfs_poll` readiness layer, all pollable
  via `poll`/`select` and `epoll` (`M56-SMOKE: ok eventfd/epoll/timerfd/signalfd`).
- [x] `done` `SCM_RIGHTS` ancillary FD passing over `AF_UNIX` — already worked
  (Wayland passes buffers/keymap fds through it); verified in M48/M49/M57.
- [x] `done` memfd sealing (`F_ADD_SEALS`/`F_GET_SEALS`, enforced on
  write/ftruncate) and cross-process shared `mmap` (Wayland shm). The libwayland
  port now uses these real primitives instead of emulating them.

## M57: Multiprocess Model

- [x] `done` fork/exec, FD inheritance, and FD brokering for the `--no-sandbox`
  model: COW fork with shared open-file offsets, `FD_CLOEXEC` across exec,
  `SCM_RIGHTS` fd-brokering incl. in-flight-fd cleanup on peer death (audited
  correct), plus added `socketpair()` (AF_UNIX only — all POSIX requires) and
  `F_DUPFD_CLOEXEC`
  (`M57-SMOKE: ok fork-fdshare/cloexec/exec-inherit/fd-broker/fd-broker-death/dupfd-cloexec`).
- [ ] `planned` Bring up a minimal Mojo core over the M56 primitives.

## M58: V8

**DONE — real V8 (`d8`) runs JavaScript on b1nix: all three tiers, single + multi-CPU.**
Full bring-up history in `tools/patches/v8/PORT-PLAN.md`.

- [x] `done` Port, build, link, and **run real V8 `d8`** off an ext4 disk. The GN
  build was taught `b1nix` as a `target_os` (the `tools/patches/v8/` skeleton — the
  earlier "NO-GO, GN wall" assessment was wrong), then the engine compiled and
  linked. Runs the 12-test `m58.js` suite (loops/arrays/objects/JSON/GC/recursion/
  closures/try-catch/Map-Set/typed-arrays/regex) to `M58-V8: done`.
- [x] `done` **All three execution tiers:** Ignition interpreter (`--jitless`),
  **Sparkplug** baseline JIT (`--no-opt`), and **TurboFan** optimizing JIT
  (`b1nix.v8opt`, incl. fib25 tier-up). Required W^X executable mmap, GC/deopt
  signals, per-thread ELF TLS, real x87 `fenv` (`fnstenv`/`fldenv`), and a
  FS-base-preserving ring3 entry path.
- [x] `done` **Multi-CPU:** `-smp 2` runs both Sparkplug and TurboFan to completion,
  0 faults (3/3 runs each, full suite). Required fixing an SMP `tlb_shootdown`
  deadlock in `sys_mmap` (drain in-flight shootdowns per page) — v0.58.5.
- [x] `done` **Maglev** (mid-tier JIT) **and the code cage** (external code space)
  now both enabled and verified: the full m58.js suite (12 markers + `done`) runs
  fault-free under Sparkplug AND TurboFan with `v8_enable_maglev=true` and
  `v8_enable_external_code_space=true` (`tools/v8/v8-gen-jit.sh`). The long-standing
  "code-cage zero-base / `0x400` crash" was never a V8 bug — it was a b1nix
  **`sys_mmap`** bug: it honored V8's sub-4 GiB cage hint, which collides with the
  supervisor-only low-4 GiB identity map (2 MB huge pages cloned into every address
  space), so a user access there faulted as a present supervisor page. Fixed by
  relocating low non-FIXED hints (as `vm_find_free_area` already did) — v0.58.6.
- Config: pointer-compression data cage **on**, **Maglev on**, **code cage on**,
  **sandbox on**, **i18n on** (embedded ICU), **WebAssembly on**, **Temporal on**
  (see below — now that b1nix has a Rust toolchain). d8 ships **inside the ISO** as
  a GRUB `module2` (→ `ram0`, mounted `/mnt/v8`) — no separate SATA disk. Run via
  `b1nix.test=1 b1nix.v8run b1nix.v8jit [b1nix.v8opt]`.
- [x] `done` **Temporal — V8 with Rust (v0.69.8).** `v8_enable_temporal_support`
  pulls in the Rust `temporal_capi`/`temporal_rs` (+ `icu_calendar`, `diplomat`,
  ~33 crates) — the first Rust running *inside* V8 on b1nix. Built with the b1nix
  cross-rust (`build/rust-native/.../x86_64-unknown-linux-gnu/stage2`, target
  `x86_64-unknown-b1nix`). GN wiring: `apply.sh` Patches **7** (real b1nix
  `rust_abi_target`), **7b** (known-target-triples), **7c** (profiler_builtins is
  chromium-toolchain-only); `tools/v8/setup-rust-hoststd.sh` grafts a host std
  (with `.rmeta`) for proc-macros/build-scripts; `tools/v8/v8-link-d8.sh` pulls the
  rust rlibs + rust std into the d8 link (GN passes them outside `d8.rsp`). d8 needs
  `--harmony-temporal` (added to the `kernel/main.c` argv — Temporal is an
  in-progress harmony flag). Verified in QEMU: `M58-V8: ok temporal-plaindate`,
  `ok temporal-duration`, `ok temporal-add` (calendar arithmetic runs in Rust).
- [x] `done` **WebAssembly** (`v8_enable_webassembly`). Trap handler off on b1nix
  (`apply.sh` **Patch 20** → explicit in-code bounds checks). The startup abort
  (`AllowHeapAllocationInRelease::IsAllowed()` during startup-snapshot deserialize)
  was **not** a V8/TLS-seeding bug — it was a b1nix main-thread TLS-placement bug:
  the kernel put the thread pointer at `region + round_up(p_memsz, p_align)` while
  the b1nix linker emits local-exec offsets as `symbol_offset - p_memsz` (un-rounded),
  so any binary whose TLS `p_memsz` is not an `align` multiple (wasm d8: `0x108`,
  align `0x10`) read every `__thread` 8 bytes off. Fixed by placing the TP at
  `region + p_memsz` in `kernel/user/process.c` (main thread) and
  `userspace/libc/pthread.c` (workers). Verified `M58-V8: ok wasm`.
- [x] `done` **Sandbox** (`v8_enable_sandbox`, TrustedSpace + sandboxed pointers).
  The "V8_ENABLE_SANDBOX not propagating across gn source-sets" was a stale-object
  artifact — a clean rebuild fixed it. Two real fixes: **Patch 21** disables the
  Linux sandbox-testing crash filter on b1nix (`<sys/ucontext.h>`/`greg_t`/`SI_KERNEL`),
  **Patch 22** compiles `partition_alloc` `stack_trace_linux.cc` for `CollectStackTrace`.
  Runtime: a `sys_mmap` hang on the sandbox's ~1.4 TiB cage + 256×4 GiB Smi-range
  PROT_NONE reservations — fixed by skipping eager per-page PTEs for `prot==PROT_NONE`
  (the `#PF` fast path faults them in lazily). Verified Sparkplug + TurboFan.
- [x] `done` **i18n** (`v8_enable_i18n_support`) with ICU data embedded
  (`icu_use_data_file=false`, no external `icudtl.dat`); needed `LC_MESSAGES` in the
  libc `<locale.h>`. Verified `M58-V8: ok intl`.
- [ ] `parked` **Temporal** (`v8_enable_temporal_support`). Needs the Rust
  `temporal_rs`/`temporal_capi` crate (`temporal_rs_*` symbols); b1nix has no
  Rust→b1nix toolchain. Separate large port, like the AArch64 effort.
- [x] `done` (earlier pragmatic alt, kept) the in-tree **Duktape** (M54/NetSurf) as
  a standalone `/bin/js` runner/REPL — the lightweight JS vector; superseded for
  capability by real V8.
- [x] `done` cheap independent POSIX wins surfaced by the probe:
  `madvise(MADV_DONTNEED/FREE/hints)`, `MAP_NORESERVE` (lazy-commit), and
  `sigaltstack` with working `SA_ONSTACK` signal delivery — per-process alt stack
  in scheduler side-tables (M29 invariant). Help any large-heap allocator, not
  just V8. Verified by `MM-SMOKE: ok madvise/noreserve/sigaltstack` (both arches).
  *Caveats:* `MADV_FREE` is implemented as `MADV_DONTNEED` (b1nix has no
  lazy-reclaim queue, so contents are discarded, not preserved on read-back);
  `madvise` only acts on anonymous, non-shared mappings (file/`MAP_SHARED` =
  safe no-op); `MAP_NORESERVE` has no reservation accounting (b1nix lazy-commits
  regardless — nothing faked).

## M59: EGL and GL for the Browser

- [x] `done` Real EGL 1.4/1.5 over the M52 Mesa OSMesa softpipe (`b1egl_mesa.c`):
  `eglGetDisplay`/`Initialize`/`ChooseConfig`/`CreateContext`/`MakeCurrent`/
  `SwapBuffers` + the previously-missing off-screen `eglCreatePbufferSurface`
  (the DRI/GBM-shaped path) and the on-screen window path to displayd. Verified
  by `m59_smoke` (`M59-SMOKE: ok egl-init/egl-context/egl-render` — clears + draws
  a triangle and checks real read-back pixels, both arches). The existing
  TinyGL-backed `b1egl.c` stays for that path. *Caveat:* the verified path is the
  software OSMesa softpipe (off-screen pbuffer); the on-screen window path and the
  M53 virgl host-GPU path share the same EGL surface model but the smoke runs the
  off-screen software path.
- [ ] `deferred` Software Skia (Ganesh) raster fallback — **assessed: a separate
  GN/Ninja port wall** (Skia is V8/Chromium-scale; not in-tree). Defer as its own
  milestone.

## M60: Ozone Platform

- [x] `done` **Headless Ozone backend** wired for the b1nix Chromium build
  (`ozone_platform_headless=true`, `ozone_auto_platforms=false`; ANGLE/libpci
  fallbacks patched in `tools/patches/chromium/apply.sh`).
- [ ] `deferred` A **displayd/Wayland-shaped** Ozone backend (window, surface,
  input, vsync) for on-screen rendering — tracked under M66. **Gated on the
  frozen M62**: an Ozone platform backend is only useful once `content_shell`
  links and renders (M62), and M61/M62 are frozen. The display-server pieces it
  would sit on (M47–M49 Wayland compositor + M52/M59 EGL/Mesa) already exist;
  what's missing is the Chromium engine to drive. Revisit if Chromium is unfrozen.

## Frozen - M61: Chromium Build Target

- [x] `done` **b1nix is a working GN/Ninja target** — `gn gen out/b1nix`
  (~68.5k edges) succeeds with the shared `//build` `target_os=="b1nix"` +
  `//build/toolchain/b1nix` cross-toolchain (clang + bundled libc++, Rust on).
- [x] `done` **Foundational + services layers compile** with the b1nix
  toolchain: `base/`, `//net`, `crypto/`, `ui/`, `mojo/`, `services/`, most
  `third_party` (webrtc/skia/dawn/angle/swiftshader). ~50 libc/header gaps filled
  + GN disables for absent system libs (NSS/kerberos/libpci/alsa/pulse/udev). See
  `docs/chromium-port-log.md`; deferred items in `docs/chromium-port-debt.md`.
- [ ] `partial` **Compile the rest** (Blink renderer + `content/`) under
  `ninja -k 0` so the `.so` link blocker doesn't halt the object grind.

##  Frozen - M62: content_shell

- [ ] `in-progress` `content_shell` is the active ninja target; foundational +
  services layers compile (M61). **Remaining: (1) compile Blink/content,
  (2) the link phase** — hand-relink content_shell + the 4 graphics `.so`s as
  b1nix ELFs via `tools/v8/chromium-link.sh` (extends `v8-link-d8.sh`; the
  graphics `.so`s are runtime-dlopen'd so content_shell links independently).
- [ ] `planned` Then render a page to a bitmap (`--headless --no-sandbox`) and
  verify pixels — the "Chromium runs" milestone (reuses V8 M58 + EGL/OSMesa M59).

## M63: Sandbox

- [x] `done` **seccomp-bpf syscall filtering.** A task installs an immutable
  classic-BPF filter (`SYS_SECCOMP`/`prctl(PR_SET_SECCOMP)`) or enters strict
  mode (`SECCOMP_MODE_STRICT`); `kernel/sched/seccomp.c` runs the filter chain
  over a `struct seccomp_data` on every syscall (hooked at the top of
  `syscall_dispatch_impl_inner`, gated on a single side-table load so only
  filtered tasks pay anything) and enforces the verdict by Linux precedence
  (`KILL_PROCESS > KILL_THREAD > TRAP > ERRNO > TRACE > LOG > ALLOW`): ALLOW
  proceeds, ERRNO short-circuits with `-errno`, KILL terminates the task with
  SIGSYS, TRAP raises SIGSYS. A full classic-BPF interpreter (LD/LDX/ST/ALU/JMP/
  RET/MISC, bounds-checked, fails closed) backs it. Filters are refcount-shared
  on fork/clone, survive execve, and can only be added (a descendant is always
  at least as restricted); `PR_SET_NO_NEW_PRIVS` round-trips. Verified by
  `userspace/bin/m63_smoke.c` (`M63-SMOKE: ok seccomp-errno/kill/strict/inherit/
  nnp`). Filter state lives in scheduler side-tables (struct task cannot grow —
  M29 LAPIC-PT invariant).
- [ ] `deferred` **user/PID/net namespaces + the setuid-sandbox helper.** Each
  namespace type is its own subsystem (a cloneable, refcounted namespace object
  threaded through the VFS mount table / PID allocator / netdev+socket layer with
  `CLONE_NEW*` in `clone`), and the setuid helper needs a trusted broker binary.
  Large, and only required for the **layered** Chromium sandbox (which is frozen)
  — seccomp-bpf alone already gives real syscall-surface reduction. Revisit when
  process/namespace isolation is concretely needed.
- The seccomp-bpf layer is what **Chromium's syscall sandbox** uses; the b1nix
  Chromium build still runs `--no-sandbox` until namespaces land too (port debt,
  `chromium-port-debt.md`).

## M64: Optional Clang/LLVM Toolchain — DONE

`done` — Clang is available across the toolchain alongside the proven GCC path.
**Clang is in fact the primary C compiler**: the kernel and the whole userspace
libc + smoke binaries build with it. An optional cross `clang++` frontend, a
native self-host Clang (build + in-QEMU proof), and separate Clang/Clang-libc++
V8 GN configs all exist and are exercised. GCC remains the **default C++**
compiler for the libstdc++-linked C++ ports and the M26 self-host path — a
deliberate choice (a `libc++`/`libc++abi`/`libunwind` port is a non-goal), not a
gap.

- [x] `done` **Phase 1 — optional cross `clang++` frontend (x86_64).**
  `tools/toolchain/bin/b1nix-clang++` compiles against the staged GCC 13 headers and links the
  existing libstdc++/libsupc++/libgcc and `libb1nix`; GCC remains the default.
  `m64_clang_smoke` covers STL, exceptions and RTTI in the regular smoke harness.
- [x] `done` **Phase 2 — V8 Clang build (separate GN config).** The Clang V8
  build paths exist and are wired alongside the proven GCC one:
  `tools/v8/v8-build-run.sh` takes `FRONTEND=clang` (host clang frontend + the
  GCC libstdc++ runtime, GN profile `b1nix-jit-clang`) **and** `clang-libcxx`
  (host clang + Chromium's bundled **libc++** + Temporal + cross-Rust, profile
  `b1nix-jit-clang-libcxx` — the Chromium-direction C++23 toolchain), with `gcc`
  kept as the default fallback. So the milestone's actual ask — *a separate
  Clang GN output/config that doesn't disturb the working GCC build* — is
  satisfied, including a real libc++ path. What is deliberately **not** done is
  promoting the Clang V8 build to the *default* smoke profile: the routinely
  built+verified V8 in `tests/smoke.sh` is the GCC one (the clang/clang-libcxx
  profiles are opt-in), because running a second full V8 link in every smoke is
  pure cost with the GCC path already green. The capability is there; only the
  default-path choice favors GCC.
- [x] `done` **Phase 3 — Clang is the primary C compiler; C++ ports stay on the
  shared GNU runtime by design.** All **C** code already compiles with Clang —
  the kernel (`clang --target=x86_64-elf`) and the entire userspace libc + smoke
  binaries (`CC := clang`). The **C++** ports (V8, Chromium, Mesa, NetSurf,
  litehtml, the native toolchain) build with the cross **GCC g++** because they
  link the one C++ runtime b1nix ships — GCC's `libstdc++`/`libsupc++`; the
  optional cross `clang++` (Phase 1) links that *same* libstdc++. Moving those
  ports off g++ is **declined, not pending**: a full `libc++`/`libc++abi`/
  `libunwind` port is an explicit non-goal (see the note below), and clang+GCC-
  libstdc++ has real ABI edges (e.g. the i686 `size_t` mangling mismatch that
  already pins `m64_clang_smoke` to x86_64). So "everything is on Clang" is true
  for C; C++ ports are intentionally on the shared GNU C++ runtime, with a clang
  frontend available when wanted.
- [x] `done` **Native self-host Clang (build).** `tools/build-native-clang.sh --b1nix-elf`
  cross-builds a b1nix-native `clang`/`clang++` under `build/native-clang/b1nix/usr`
  with b1nix as the host triple; `make install-native-toolchain` stages only this
  b1nix ELF build into the rootfs, not the Linux-hosted cross compiler.
- [x] `done` **Native self-host Clang (in-QEMU proof).** `make clang-proof`
  (tools/clang/clang-proof.sh) ships clang-22 in an ext4 GRUB module (ram0); with
  `b1nix.clangrun` the kernel runs it on b1nix: `clang --version` prints
  `clang version 22.1.8` (load+execute proof) and `clang -c hello.c -o hello.o`
  exits 0 emitting a valid ELF object (`M64-NATIVE-CLANG: ok compile`). b1nix
  compiles a b1nix object with its own native Clang.
- A full GCC-to-Clang migration and a libc++/libc++abi/libunwind port are not
  goals. Add either only when a measured incompatibility makes the GNU path fail.

## M65: Install to Disk

- [x] `done` Standalone **install to a real disk** — the machine boots b1nix on
  its own (no live USB/ISO) with a writable on-disk root. Verified end-to-end:
  the host builds a bootable image, `b1nix_install` copies it to a target disk,
  and that disk boots GRUB → kernel → `rootfs: sata0p1 mounted at /` → userland.
- [x] `done` **`tools/images/mk-disk-image.sh`** (host): builds a standalone-bootable
  `b1nix-disk.img` — MBR + real pre-baked BIOS GRUB via loopback
  `grub-install` + an ext4 root staged with the full userland (busybox + bash +
  applet symlinks + `/etc`). **No in-guest ports** (no in-guest grub/mkfs).
  Excludes V8/Chromium by construction. `make disk-image`.
- [x] `done` **`/bin/b1nix_install`** (in-guest): validated whole-disk copy of
  the image onto a target block device (size check, confirm, progress, fsync).
- [x] `done` **Block-I/O fixes that unblocked raw `/dev/sataN` use** (also fix
  fdisk/dd on any installed system): rebind block device nodes after the
  root-switch (`vfs_repopulate_after_root_mount` → `blk_create_dev_nodes`);
  device size via `lseek(SEEK_END)`; block-aligned **bulk DMA** fast path
  (bounce through a kernel buffer, chunked to a PRDT-safe size) in
  `blkdev_node_read/write`; AHCI PRDT bounds check; cache-invalidate before raw
  writes. *Caveat:* throughput is gated by QEMU's polled-AHCI latency (no IRQ
  path yet) — correct but not fast; interrupt-driven AHCI is future work.

  ## Frozen - M66: Chromium Browser Frontend (planned)

- [ ] `planned` A real **browser UI on top of the Chromium content layer** — once
  M62 `content_shell` renders pages, build a windowed browser frontend (address
  bar, navigation, tabs) running on b1nix's compositor. Path: a displayd/Wayland
  **Ozone backend** (M60 has the headless one) so Chromium draws into a real
  window via the M47-49 display server + M52/M59 EGL/Mesa, plus the chrome UI
  itself. **Gated on M62** (the engine must render first). Lighter alternative if
  full Chromium UI is too heavy: drive `content_shell --window-size` output into
  a libgui window.

## M67: Rust Toolchain Port (for Chromium) — DONE

- [x] `done` Ported **Rust to b1nix**: an `x86_64-unknown-b1nix` rustc target spec
  (`tools/patches/rust/x86_64_unknown_b1nix.rs`) reusing Rust's `unix` PAL +
  `-Zbuild-std` against `libb1nix`, plus the cross-toolchain driver. Host rustc
  emits b1nix ELFs; wired into Chromium's `enable_rust` path. Merged (PR#23).

## M68: Native Rust Compiler (self-hosted) — DONE

- [x] `done` **rustc runs ON b1nix.** Native rustc + `librustc_driver.so` +
  `libLLVM.so` built as ET_DYN (`--host=x86_64-unknown-b1nix`, dynamic-linking +
  PIE + initial-exec TLS), loaded by the M69 kernel exec-time linker. Proven in
  QEMU (`tools/rust/rust-proof.sh`: `M68-RUST: ok rustc-load` + the real
  `rustc 1.98.0-nightly` banner). Merged (PR#23).


## M69: Dynamic Loading (ELF dynamic linker) — DONE

Implemented as a real userspace `ld.so` in `userspace/libc/dlfcn.c` (mmap +
mprotect, no kernel changes): the kernel still loads the main PIE binary and its
startup objects eagerly (M30); this handles everything after the process is
running. Verified end-to-end by the m30-dynamic smoke (`M69-DL*` + `M69-PLUGIN`
ctor/dtor markers).

- [x] `done` **Phase 1 — lookup in startup-loaded libc.** `dlopen` recognizes
  `libc.so.1`, `dlsym` walks its ELF `DT_HASH`/`DT_SYMTAB`/`DT_STRTAB`, and
  `dlclose` succeeds without unloading the process-lifetime object. The dynamic
  smoke resolves and calls `strlen` through the returned pointer.
- [x] `done` **Phase 2 — load new objects at runtime.** `dlopen` of a new
  ET_DYN maps its PT_LOAD segments, parses `PT_DYNAMIC`, loads non-libc
  `DT_NEEDED` deps, applies `RELATIVE`/`GLOB_DAT`/`JUMP_SLOT`/`64` relocations
  (resolving against the loaded objects), `mprotect`s segments to final perms,
  and runs `DT_INIT`/`DT_INIT_ARRAY` constructors. Proven by a real plugin .so
  whose ctor runs and whose exported function is dlsym'd and called.
- [x] `done` **Phase 3 — lifetimes and full lookup scopes.** Objects are
  reference-counted (re-`dlopen` returns the same handle), `dlclose` runs
  `DT_FINI_ARRAY`/`DT_FINI` and `munmap`s at zero, and `RTLD_DEFAULT` /
  `RTLD_NEXT` lookup scopes are implemented.
- **Motivation:** unblocks rustc **proc-macros** and compiler **plugins** (both
  loaded as `.so`) plus general shared-library support.
- [x] `done` **Kernel exec-time dynamic linker runs the native rustc.** The M68
  rustc came out *dynamic* (`rustc` → `librustc_driver.so` → `libLLVM.so` →
  `libgcc_s.so`, no `PT_INTERP`), so the kernel ELF64 loader resolves the whole
  ~250 MB `DT_NEEDED` graph at exec. Extensions to `kernel/user/process.c`:
  dynamic `ET_EXEC` (not just PIE), `R_X86_64_COPY` (e.g. `environ`),
  `R_X86_64_TPOFF64` with a process-wide variant-II static-TLS layout across all
  objects, a 2 KB symbol-name buffer (Rust mangled names reach ~750 chars), and
  `$ORIGIN/../lib` bundle-relative `DT_NEEDED` resolution. Proven in QEMU
  (`b1nix.rustrun`, `tools/rust/rust-proof.sh`): `M68-RUST: ok rustc-load` then
  the real `rustc 1.98.0-nightly` banner printed from b1nix.

## M69b: Dynamic-loader performance

The M69 exec-time linker is correct but unoptimized. As the C/C++ port binaries
move onto real dynamic linking (the `--enable-shared` cross GCC gives them
`DT_NEEDED libgcc_s.so`; `/lib/libgcc_s.so` now ships in the initramfs and the
kernel resolves it), the per-spawn linking cost matters. Eliminate the speed
problems so "everything dynamic" carries no penalty:

- [x] `done` **O(1) symbol resolution.** `elf64_resolve_symbol` /
  `elf64_resolve_tls_symbol` now walk the SysV `DT_HASH` table
  (`elf64_sysv_hash` + `elf64_hash_lookup`) instead of a linear dynsym scan —
  O(relocs × symbols) → O(relocs). (v0.69.3)
- [ ] `planned` **Share/cache loaded shared objects across processes.** The
  loader re-reads + re-`kzalloc`s + re-relocates each `.so` (libgcc_s.so,
  libc.so, ...) on every `spawn`. Load + relocate once and share the read-only
  pages (copy-on-write the writable ones), like a real mmap-backed `.so`.
- [ ] `planned` **Lazy PLT binding.** All `JUMP_SLOT` relocations are resolved
  eagerly at load; bind on first call so unused imports cost nothing.
- [ ] `planned` **Faster segment lookup.** `elf64_stage_ptr` is O(segments) per
  call; binary-search / hash the vaddr ranges.

**Loader correctness — `R_X86_64_COPY` relocation order (v0.69.6).** The eager
linker relocated the executable before its libraries, so a non-PIE exe's COPY
relocs (`stdout`/`stderr`/`stdin`/`errno`/`environ`, imported from libgcc_s.so)
copied the source library's pointer *before* the library's own `R_X86_64_RELATIVE`
reloc had fixed it — the exe's `stdout` got a garbage pointer and the first
`fprintf()` crashed. Every NetSurf render (the whole M53 cluster) died this way.
Fixed by relocating libraries before the executable (only the non-PIE exe carries
COPY relocs). With this, the M69 loader is correct for the full C/C++ port set;
**x86_64 smoke is fully green (833/0)**.

**Closeout — remaining perf items deferred (profile-gated).** The headline cost
(O(relocs × symbols) linear dynsym scan) is gone with DT_HASH; the loader is
correct and fast enough that nothing in the suite or the port set is
loader-bound. The three items below are real but each is either large/risky
against a now-green exec path or a micro-opt below the noise floor, so they wait
for a profiled bottleneck rather than speculative churn:
- Share/cache `.so` across spawns — the genuine win for *repeated* big-binary
  spawns (re-reading + re-relocating `libLLVM.so` ~184 MB per spawn), but a large
  change (cross-process page sharing + COW writable segments + refcount/teardown +
  invalidation). No current repeated-spawn hot loop justifies the regression risk.
- Lazy PLT binding — helps single-startup of a huge-export library; needs a PLT
  trampoline + on-first-call resolver. Deferred with the same gate.
- Faster `elf64_stage_ptr` — YAGNI: a linear scan over the handful of
  cache-resident segments is not the bottleneck (relocation time is dominated by
  the per-segment `memcpy`), and a sorted/binary-search variant would add an
  invariant to maintain across the during-load callers for sub-ms savings.

## M69c: Dynamic linking by default + matured loader relocations

The base system now links **dynamically by default** (relates to M71 PIE-by-default
and the M69b loader): every base userspace program is a PIE that links against the
shared `libc.so.1` through `/lib/ld-b1nix.so` + the in-kernel M69 loader, instead
of a fully static ET_EXEC. `LINK=static` in `userspace/Makefile` restores the
historical static link. **x86_64 smoke 834/0** with every base ELF loaded at the
PIE base (`0x500000000000`).

- [x] `done` **Dynamic-by-default base.** `userspace/Makefile` generic program
  rule links `-pie` against `libc.so.1` with the dynamic crt0; `libc.so.1` ships in
  every initramfs. The whole M12–M58 smoke suite runs as dynamic PIEs.
- [x] `done` **`struct dirent` ABI fix (v0.69.11).** The Chromium-port grind had
  inserted `d_type` *before* `d_name`, shifting `d_name` off the offset the
  prebuilt cross-GCC libstdc++/Fontconfig/busybox/curl ecosystem reads it at;
  under dynamic-by-default this broke `readdir` for dynamic consumers. `d_type`
  moved after `d_name` (offset 8 restored), and `libc.so.1`'s PIC objects gained
  the header-dependency tracking they were missing (stale shared-libc bug).
- [x] `done` **Matured `dlfcn.c` relocations (v0.69.12).** The userspace runtime
  loader now also handles **general-dynamic TLS** in a dlopen'd object
  (`R_X86_64_DTPMOD64`/`DTPOFF64` + a real `__tls_get_addr` that materialises a
  per-module TLS block), **IFUNC** (`R_X86_64_IRELATIVE`, resolver called in
  process), and **`R_X86_64_COPY`**. TLS-GD is smoke-verified (`M69-DL5: tls-gd
  ok`); IRELATIVE/COPY are implemented but not smoke-exercised (the b1nix clang
  target ignores `__attribute__((ifunc))` and normal `.so`s emit no COPY).
- [x] `done` **Default-ISO ports flipped to dynamic (v0.69.14 / v0.69.15).** The
  shared autotools port link recipe (`tools/toolchain/bin/b1nix-autotools-cc`) now
  defaults `B1NIX_LINK=dynamic`: the executable ports it links — `bash`, `curl`,
  `dropbear`, `wget` — are dynamically-linked `ET_EXEC`s importing libc from the
  shared `libc.so.1` via `/lib/ld-b1nix.so` (the in-kernel M69 loader resolves the
  COPY relocs for `environ`/`stdout`/`stderr`/`errno` and the JUMP_SLOTs eagerly at
  spawn); `B1NIX_LINK=static` restores the historical static link. `busybox`
  (cross-GCC, v0.69.13) is likewise dynamic (`libc.so.1` + `libgcc_s.so`, interp
  `/lib/ld64.so.1` — the kernel reads `PT_INTERP` only for logging and links
  in-kernel, so both interpreter strings resolve identically). The compile-only
  library ports the recipe touches (mbedTLS, pcre2, libidn2, ...) are byte-for-byte
  unchanged — they remain static `.a` archives, statically folded into the now
  dynamic executables, so "mbedTLS dynamic" is moot (it ships no executable).
  `wget` x86_64 also needed two pre-existing build fixes to rebuild at all
  (gnulib's unconditional aclocal/automake regen rules → neutralised with
  `ACLOCAL=true` etc.; gnulib replacing `timegm`/`mktime` against b1nix's
  `static inline timegm` → `ac_cv_func_timegm=yes`/`gl_cv_func_working_mktime=yes`).
  Verified by the full **x86_64 834/0** smoke with every M22/M11/BB-W*/M32-NET/TLS/
  BASH-SMOKE marker driven by the dynamic binaries.
- [x] `done` **M69 loader `.bss`-tail COPY fix (v0.69.15).** bash's own
  getenv/setenv set manipulates `environ` directly, so its dynamic link emits an
  `R_X86_64_COPY` for `environ` (+ `stdin`/`stdout`/`stderr`/`errno`) into bash's
  `.bss` plus a paired `R_X86_64_GLOB_DAT` for `environ`. The 4-byte `errno` COPY
  lands in the *final* bytes of the RW PT_LOAD (its `memsz` ends exactly 4 bytes
  after `errno`). `elf64_apply_rela_table` pre-staged a fixed 8-byte `target` slot
  for *every* reloc and failed (`va+8 > vaddr+memsz`) — but `R_X86_64_COPY` never
  writes through `target` (it stages dst/src with the symbol's real size), so the
  probe is now skipped for COPY. This re-enabled bash dynamic and unblocks any
  binary whose COPY datum sits at a segment boundary (relevant for future dynamic
  V8/rustc). The other dynamic ports COPY only into `.data` (within `filesz`).
- [x] `done` **V8 `d8` flipped to dynamic (v0.69.16).** `tools/v8/v8-link-d8.sh`
  gained a `B1NIX_LINK=dynamic` path (now the default) that links the gn-compiled
  d8 object set against the shared `libc.so.1` via `/lib/ld-b1nix.so` (crt0-dynamic,
  `--hash-style=sysv -z norelro`) instead of whole-archiving `libb1nix.a`. This is a
  *relink* of the existing objects — no multi-hour ninja rebuild. Verified by
  re-running d8 in QEMU on two profiles: `b1nix-jit-maglev` (no-rust) and the
  everything-on `b1nix-jit-temporal` (Rust + Temporal). The dynamic d8 loads via the
  in-kernel loader (`PT_INTERP=/lib/ld-b1nix.so`, `NEEDED libc.so.1`, 145 UND syms
  all satisfied by `libc.so.1`) and runs JavaScript **and Rust-backed Temporal**
  (`M58-V8: ok hello/loop-sum/temporal-plaindate/temporal-duration/temporal-add →
  done`) — identical markers to the static temporal d8. (The COPY-at-segment-boundary
  loader fix above is what unblocked it.) `B1NIX_LINK=static` restores the historical
  whole-archive link.
- [x] `done` **Graphics/GUI/browser smoke binaries flipped to dynamic (v0.69.17).**
  A `BESPOKE_*` link mode in `userspace/Makefile` (non-PIE dynamic ET_EXEC: crt0-
  dynamic + `--dynamic-linker /lib/ld-b1nix.so` + shared `libc.so.1`, with bundled
  static archives still folded — the d8 model) converted **27 of 28** bespoke-static
  binaries: GUI demos, libwayland, mbedTLS nettool/httpsd, the M51 desktop stack
  (pixman/freetype/cairo/harfbuzz) and the M53 NetSurf codec/lib smokes. Verified by
  the full **x86_64 834/0** smoke. `m53_libpng_smoke` stays static (residual debt):
  libpng's setjmp/longjmp error path takes the absolute address of `longjmp`,
  emitting R_X86_64_32 relocs a non-PIE link cannot form against an imported symbol.
- [x] `done` **Mesa OSMesa/EGL demo executables flipped to dynamic (v0.69.18).** The
  four real-Mesa demos (`m52_osmesa`, `m52_glsl`, `m53_mesa_virgl`, `m59_smoke`) are
  linked by per-demo scripts (`tools/demos/build-m5{2,3,9}-*.sh`) that fold the OSMesa
  softpipe `.a` + libstdc++/libsupc++/libgcc; each gained a `B1NIX_LINK=dynamic`
  (default) path linking the shared `libc.so.1` via `/lib/ld-b1nix.so`. All four relink
  dynamically (no absolute-reloc gap — unlike libpng) and run GPU-accelerated under the
  full smoke: `M52-GFX`, `M53-VIRGL: ok …`, `M53-GFX: ok gl-accelerated/gl-triangle`,
  `M59-SMOKE: ok egl-context/egl-render`. **x86_64 834/0.**
- [x] `done` **NetSurf `nsfb` flipped to dynamic (v0.69.19).** nsfb links via the raw
  cross-GCC (GCCSDK convention) and was already a dynamic executable (it imports
  `libgcc_s.so`), but folded libc statically because gcc's implicit `-lc` resolved to
  the static `libb1nix.a` in the cross-GCC's own sysroot — which had no `libc.so`.
  `tools/ports/build-netsurf-fb.sh` now stages the shared `libc.so.1` into that
  sysroot as `libc.so` (default; `B1NIX_LINK=static` skips it), so `-lc` resolves to
  the shared library exactly like `libgcc_s.so`. nsfb now imports `libc.so.1` and
  renders end-to-end under the full smoke: `M53-NS ok js/jxl`, `M53-FB ok
  load/redraw/render/svg`, `M53-INPUT ok mouse/key`. **x86_64 834/0.**
- [x] `done` **Native `rustc` flipped to dynamic libc (v0.69.20).** The whole rust
  toolchain now imports libc from the shared `libc.so.1` and runs end-to-end on b1nix
  (`M68-RUST: ok rustc-load` + the `rustc 1.98.0-nightly` banner). Six layers were
  peeled, each a real wall the static-libc port design had baked in:
  (1) `bootstrap.toml` `crt-static = false` so the rustc binary is a dynamic PIE (was
  `+crt-static`); (2) `build-rust-native.sh` stages the PIC `crt0-dynamic.o` (the
  static crt0 has a non-PIE `__register_frame_info` reloc); (3) the target spec pulls
  the shared `libc.so` in early (`--push-state --no-as-needed -lc`) so the C funcs
  resolve dynamically before the COMBINED `-lm` archive folds them, plus a late
  `-lgcc_s` for the `_Unwind_*`/`__popcountdi2` the static-libc chain used to drag in;
  (4) `download-ci-llvm = false` + `[llvm] ldflags`/`targets`/`ccache` builds the host
  + b1nix LLVM from source as static `.a`s folded into `librustc_driver.so` (so there
  is no separate static-libc `libLLVM.so` anymore — the 141 MB driver imports every
  libc/math symbol UND from `libc.so.1`); (5) `libc.so.1` folds PIC openlibm
  (musl-style: libm IS libc) so the shared libc exports `log1p/cosh/tan/…` too;
  (6) `userspace/libc/openlibm_compat.c` adds the `crealf/cimagf/scalbnl` openlibm
  references b1nix lacked. The only static residue is the rustc binary's
  `compiler_builtins` `memcpy`/`memset` intrinsics, which Rust statically links into
  every binary by design. Verified: full **x86_64 834/0** with the new openlibm-`libc.so.1`,
  and the dynamic rustc runs in QEMU (`tools/rust/rust-proof.sh`, hash-agnostic).
- **Still static (heavy tail):** `m53_libpng_smoke` (libpng `&longjmp` absolute reloc —
  non-PIE physics, not fixable) and Chromium (intentionally deferred — doesn't run
  yet). Rust proc-macros remain deferred (need the dynamic loader's dlopen path).

## M70: Interrupt-Driven I/O — DONE

- [x] `done` Replace busy-poll storage/NIC drivers with ISR→wakeup completion.
  A generic device-IRQ registration layer (`kernel/include/b1nix/irq.h` +
  `interrupts.c`: `irq_register_handler`/`irq_dispatch`/`irq_unmask`, shared-line
  aware) replaces the hard-coded per-device vector dispatch. The three storage
  drivers (`ahci.c`, `virtio_blk.c`, `nvme.c`) now block on a per-device wait
  channel and are woken by the completion IRQ instead of busy-yielding on a
  status register (AHCI previously polled `PxCI` over MMIO = a VM-exit per read).
  The NIC RX path wakes `net_task` immediately on the device IRQ rather than
  draining only on the ~100 Hz poll tick.
- The wait uses `scheduler_wait_prepare_timeout()` (M70 scheduler addition): it
  publishes BLOCKED with a full barrier and re-checks the completion predicate
  *after* publishing — closing the check-then-block lost-wakeup window — while
  arming a watchdog deadline so a genuinely-lost interrupt degrades to a re-poll
  instead of a wedge. A brief no-MMIO CPU spin first catches the sub-µs KVM-fast
  completion with no context switch, and `scheduler_can_block()` falls back to
  the old cooperative yield-poll before the scheduler is live (boot-time root
  mount / IDENTIFY) or from IRQs-off callers. NVMe masks its interrupt vector in
  the ISR and unmasks on consume to avoid a level-triggered INTx storm; AHCI and
  virtio-blk ack their own IS/ISR registers. PCI INTx-disable is cleared and the
  lines unmasked at the IOAPIC only after each driver is configured.
- Verified by the full **x86_64 837/0** smoke (root mount + M14 SATA/NVMe
  mount/persistence/block-cache + swap + M32-NET all green, no regression).

## M71: ASLR and PIE-by-Default

- [ ] `deferred` Randomize the load base (fixed at `0x2000000`/`PIE_LOAD_BASE`,
  no randomization) and accept `-fPIE -pie` hardened binaries. **Deferred — it
  unwinds a load-bearing invariant.** A *large* amount of b1nix assumes the fixed
  userspace base: the in-kernel ELF loader/relocator, the M69 dynamic loader, the
  signal trampoline placement, the per-process TLS layout, and several ports
  linked `-Wl,-Ttext-segment=0x2000000` (the native toolchain, V8, rustc). The
  base is also load-bearing on the (frozen) 32-bit port, where `0x400000` lands
  *inside* the kernel image. The M69c work already made the **base system PIE by
  default** (every base program is an `ET_DYN` loaded at the PIE base
  `0x500000000000`), so the PIE-acceptance half is effectively done; what remains
  is **randomizing** that base per-exec and auditing every fixed-`0x2000000`
  assumption out — a security hardening with no functional consumer demand yet,
  and real regression risk against the green loader. Needs a kernel entropy
  source for the base (present) plus a careful sweep of the loader/relocator/TLS/
  trampoline/port-link assumptions. Revisit as a dedicated hardening pass.

## M72: Writable Foreign Filesystems and msync

- [x] `partial` **`msync` syscall** (`SYS_MSYNC` = 173). The libc `msync` is no
  longer a return-0 stub; the kernel handler walks the range,
  `paging_test_and_clear_dirty()`s each page (new M72 paging helper) and, for the
  pages userspace actually wrote, writes that page-frame's contents straight to
  the backing file via the inode `write_cb` (the durable path; it also marks any
  resident page-cache entry dirty for `fsync`/eviction). `MS_ASYNC` schedules
  (mark-dirty only); `MS_INVALIDATE` is a no-op (mappers share the frame).
  Argument validation is verified deterministically by `userspace/bin/m73_smoke.c`
  (`M72-SMOKE: ok msync`: `MS_SYNC|MS_ASYNC` → `EINVAL`, unmapped range →
  `ENOMEM`, valid `MS_SYNC` → 0).
  - **Known limitation (verification deferred):** the end-to-end mmap-store
    durability round-trip (write through a `MAP_SHARED` mapping, `msync`, read it
    back via a fresh fd) is **not** reliably observable, because a writable
    `MAP_SHARED` file page sets only the hardware PTE dirty bit on an mmap store —
    the page-cache `DIRTY` flag stays clear, so under memory pressure the page
    cache can reclaim the page as "clean" and disturb its dirty state/mapping
    before `msync` runs (observed non-deterministically during the smoke boot).
    Closing this needs an **eviction-layer fix**: page-cache reclaim must consult
    the PTE dirty bit of mapped file pages (a reverse-mapping / dirty-aware
    reclaim pass) and write them back to their file before dropping. Tracked as a
    follow-up; the `msync` syscall itself flushes any page that is still
    resident-and-dirty when it runs.
- [ ] `deferred` **Write support for NTFS / FAT32 / exFAT / btrfs.** Each
  foreign filesystem is currently read-only; adding a writer to any one is a
  large, self-contained effort (allocation bitmap/runlist mutation, directory
  index updates, journal/log replay for NTFS, FAT chain management). The native
  ext2/3/4 path is fully writable and is what b1nix uses for its root; foreign
  FS writes are revisited per-filesystem when a concrete need arises. b1nix
  mounts these read-only honestly rather than risking corruption with a partial
  writer.

## M73: Modern I/O and Introspection Syscalls

- [x] `done` **File-movement + introspection syscalls.** `sendfile`,
  `copy_file_range`, `splice`, `fallocate`, and `statx` are real kernel syscalls
  (`SYS_SENDFILE`/`COPY_FILE_RANGE`/`SPLICE`/`FALLOCATE`/`STATX` = 165..169) with
  libc wrappers. `sendfile`/`copy_file_range`/`splice` share one kernel fd→fd
  pump (`file_copy_range`) that preserves the descriptor's own offset when an
  explicit offset argument is supplied (POSIX semantics) and `splice` is no
  longer the ENOSYS stub it was. `fallocate` mode 0 extends + zero-fills via the
  existing on-demand allocation (KEEP_SIZE reserves without growing; hole-punch/
  collapse/zero-range honestly report `EOPNOTSUPP` — the block drivers have no
  preallocation primitive). `statx` maps `struct b1nix_stat` into the Linux
  `struct statx` layout (path + `AT_EMPTY_PATH`). Verified by `userspace/bin/
  m73_smoke.c` (`M73-SMOKE: ok statx/sendfile/copy-file-range/fallocate/splice`),
  data/offset round-trips asserted, x86_64 **843/0**.
- [ ] `deferred` `io_uring`, `ptrace`, `inotify`, `clone3`. Each is its own
  subsystem rather than a syscall: `io_uring` is a shared-ring submission/
  completion engine; `ptrace` needs the stop/continue + cross-process register/
  memory machinery tracked under **M80** (crashpad); `inotify` needs VFS
  modify/create/delete event hooks + a per-watch event queue; `clone3` is the
  extended-args `clone` variant (the M29 `clone` flags already cover the thread/
  fork models in use). Revisit when a port concretely needs one.

## M74: Real-Time Signals

- [ ] `deferred` Add `SIGRTMIN..SIGRTMAX` and `sigqueue` payload queuing.
  **Designed, deferred deliberately (defer-over-hack).** The implementation is
  scoped — the pending/blocked masks are already `u64` (room to bit 62) and the
  M29 side-table pattern sidesteps the struct-task-growth paging landmine — but
  it is an invasive change spread across the *fragile, heavily-tested* signal
  delivery path: (1) raise `_NSIG` and route every variable-indexed
  `task->sigactions[sig-1]` site (~12, several in `arch_check_and_deliver_
  signals` / `scheduler_kill` / the EINTR-restart loop) through a
  `task_sigaction(t, sig)` helper that returns a side-table slot for the RT
  range; (2) a per-task RT payload queue (side-table FIFO of `(signo, sigval)`)
  so RT signals **queue** instead of coalescing; (3) `sigqueue`/`rt_sigqueueinfo`
  to enqueue; (4) FIFO-ordered delivery (lowest signo, oldest payload first)
  dequeuing one entry per delivery and clearing the pending bit only when the
  queue for that signo drains; (5) **native `SA_SIGINFO`** (today only the Linux
  personality builds a `siginfo_t`) carrying `si_value`. The risk/reward is poor
  *right now*: b1nix runs its own musl-style libc, so there is **no current RT-
  signal consumer** (glibc's internal `SIGRTMIN` users don't run here) to
  validate the edge cases a rushed change to the signal core would introduce.
  Revisit when a real consumer (a ported glibc program, or POSIX timers/AIO
  completion via RT signals) lands.

## M75: On-Device GPU Path

- [ ] `deferred` Add EGL/GBM/DRI + LLVMpipe (GPU is virtio-gpu only today);
  unblocks the Chromium GPU process and HW rendering on real hardware.
  **Multi-week subsystem, deferred.** The long pole is **LLVMpipe**, which needs
  a full **LLVM** port to the b1nix ABI (a shader/JIT codegen backend — LLVM is
  V8/Chromium-scale on its own); on top of that a real DRI/GBM/EGL stack
  (`gbm_bo` buffer objects, a DRM render-node path, EGLDevice/EGLStream or
  surfaceless contexts) beyond today's OSMesa-softpipe-to-memory. The existing
  software path (OSMesa softpipe + the VirGL host-GPU passthrough) already
  renders; native on-device GPU acceleration is the gap. Revisit alongside any
  LLVM port (it would also unblock Mesa JIT, M54's deferred LLVMpipe item).

## M76: USB Host Stack

- [ ] `deferred` Add a general xHCI stack (HID + mass storage); input is PS/2 +
  narrow xHCI keyboard only. **Whole device class, deferred.** A general xHCI
  controller driver (command/event/transfer rings, slot/endpoint context
  management, port routing) plus the USB core (device enumeration, descriptor
  parsing, configuration, the hub driver) and at least two class drivers (HID
  boot+report protocol; USB Mass Storage = BBB/UASP over SCSI, which then plugs
  into the existing block layer). Each layer is substantial and the current
  narrow xHCI-keyboard path (M37) covers the only hard requirement (input on
  real hardware). Revisit when USB mass storage or broader HID is concretely
  needed on bare metal.

## M77: Raise Global Resource Caps

- [ ] `planned` Make-dynamic hard caps: TCP conns (64), VFS pipes (128),
  core-dump size (1 MiB), `SHMMAX` (32 MiB) and many other.

## M79: Audio Stack

- [ ] `planned` Add a real **audio subsystem** — an HDA (or AC'97) driver + a
  mixer + an ALSA-compatible userspace shim (or a native b1nix audio API) — so
  Chromium/`media` and other ports get sound. b1nix has **no audio
  hardware/driver path at all** today, so `use_alsa`/`use_pulseaudio` are off
  (Chromium port C31). Large — a whole new device class.

## M80: Kernel ptrace + Crash Capture

- [ ] `planned` Implement **`ptrace(2)`** (returns ENOSYS now) + `/proc/<pid>/
  task` + reading another process's registers/memory/coredump, so **crashpad**
  can actually capture crashes and on-device gdb-style debugging works. Crashpad
  is compile-only today via the lss→libc shim (Chromium port C27/C28). Large.

## M81: Chromium GPU Acceleration

- [ ] `planned` Link the b1nix-target graphics `.so`s (SwiftShader Vulkan ICD +
  ANGLE `libEGL`/`libGLESv2`) as b1nix ELFs (`tools/v8/chromium-link.sh`) and wire
  ANGLE→SwiftShader-Vulkan / virtio-gpu (reuse M52/VirGL, M75) so `content_shell`
  renders **GPU-accelerated** instead of software/headless-only. Gated on M62
  (content_shell links + runs).

## M82: System NSS / Kerberos (optional, low priority)

- [ ] `planned` Port system **NSS** (cert DB) and/or **MIT-krb5** (GSSAPI) ONLY
  if a concrete need arises. Chromium's built-in cert verifier
  (`use_nss_certs=false`) and no-Negotiate-auth are the correct defaults for
  b1nix, so this is large-effort / low-value — listed for completeness, not
  scheduled.

## M83: Unicode-aware ctype / wctype

- [ ] `planned` Replace the ASCII-only wide classification/case-fold in
  `userspace/libc/wctype.c` (`iswalpha`/`towlower`/… cast `wint_t` into the byte
  ctype table → every non-ASCII codepoint mis-classified, never folded) with real
  Unicode property + case-mapping tables. The only **pervasive wrong-result**
  class in libc; gates correct i18n in bash multibyte, NetSurf, etc.

## M84: Real IP routing + TCP robustness

- [ ] `planned` Kernel networking assumes a **single /24 subnet** — no routing
  table (`net/ipv4.c:137` hardcodes the mask, drops off-net packets; `SIOCADDRT/
  DELRT` are no-ops; `/proc/net/route` is fabricated). Add a real FIB shared by
  ioctl + procfs + `ipv4_send`. TCP is **in-order-only** (`net/tcp.c:1160`, no
  reassembly queue / window scaling / SACK; fixed 16 KB recv buf) → add an
  out-of-order reassembly queue + window scaling. IPv6 is loopback-only for
  non-link traffic.

## M85: libc Tier-A correctness pass (musl-grade)

- [x] `partial` Fix the correctness-relevant libc gaps the audit flagged.
  **Done this pass:** `strtoull`/`strtoll` are now real 64-bit parsers (full
  unsigned range, base 0/2-36, overflow → `ERANGE`-clamped — no more
  cast-through-`strtol` truncation); `abort()` raises `SIGABRT` (re-raises with
  it unblocked, then `_exit(127)`) instead of a plain `exit(127)`; `perror`
  writes `"<s>: <strerror(errno)>"` to stderr instead of a literal `"error"`;
  `sysconf(_SC_NPROCESSORS_ONLN/CONF)` returns the **real** online-CPU count via
  `sched_getaffinity`+`CPU_COUNT` (was a hardcoded 1 — a Chromium-port-debt item
  that capped browser thread-pool sizing to one core). Verified by
  `M73-SMOKE: ok strtoull/sysconf-ncpu/abort-sigabrt`.
- [ ] `deferred` **Remaining audit items** (each its own focused musl-port):
  `realpath` resolving (`.`/`..`/symlink walk — needs per-component lstat, not a
  `strcpy`); fully-buffered stdio + multi-read `fread` (today's stdio is
  unbuffered with single-`read()` semantics — correct results, lower throughput);
  correctly-rounded `strtod` and real `strtof`/`strtold` (not double); richer
  `getaddrinfo` (numeric-port/multi-result). Revisit as the second musl pass.

## M86: Per-thread CPU accounting + signal targeting

- [ ] `planned` Add per-task CPU-time accounting in the scheduler so
  `CLOCK_PROCESS/THREAD_CPUTIME_ID`, `clock()`, `getrusage` return real CPU time
  (currently wall/uptime). Add a kernel `tkill`/`tgkill` so libc `pthread_kill`
  targets one thread (today routes via `kill(tid)` — can hit the whole process);
  make `pthread_exit(retval)` actually deliver `retval` to the joiner.

## M87: Dynamic-loader maturation + Rust proc-macros

- [ ] `planned` b1nix is static-by-default with `dlopen`/`dlsym` as no-ops, which
  **blocks native rustc from building proc-macro crates** (serde-derive etc.,
  loaded as dylibs) — caps self-host. Mature `ld-b1nix.so`/`libc.so.1` (extend the
  reloc handler: TLS/IFUNC/COPY currently rejected) and add an opt-in
  `LINK=dynamic` base-build mode (default stays static; keep the dynamic path
  green via smoke) so the loader is exercised across all binaries. Also revisit
  Rust `panic=abort` (no unwinding) once exceptions/unwind are wanted.

## M88: Kernel correctness fixes (ext4 indirect-block, PROT_NONE guard)

- [ ] `deferred` Two real kernel correctness/safety bugs from the audit:
  **(1)** `fs/ext4.c` `ext4_get_block` returns 0 (hole) past the single-indirect
  range on block-mapped (non-extent) inodes → silent zero-reads/corruption on
  large files of a classic ext2/3 image; add double/triple-indirect traversal
  (and indirect-block write allocation). **(2)** `sys_mmap(PROT_NONE)`
  reservations skip PTE setup, so a wild access zero-fills via the anonymous
  fault path instead of faulting — add a reserved/no-access VMA class the #PF
  handler honors before zero-filling. **Deferred — both touch hot, load-bearing
  paths and need a dedicated test harness.** (1) only bites *block-mapped* (ext2/
  legacy) inodes with files larger than the single-indirect range (~12 + 256
  blocks); b1nix's own root and every modern image use ext4 **extents**, so it
  needs a hand-built large block-mapped ext2 image to even exercise/verify, and
  the fix mutates the read+write block-allocation path. (2) lands in exactly the
  `sys_mmap` PROT_NONE + `#PF` machinery that the **working V8 sandbox** depends
  on (its 256 × 4 GiB Smi-range + ~1.4 TiB cage PROT_NONE reservations — a wrong
  guard-page change would regress a green, delicate path) — V8 never *touches*
  those reservations, so the latent "PROT_NONE is readable" bug doesn't manifest
  today. Both are real but the risk/reward says do them deliberately with a
  fault-injection / large-file harness, not as a closeout afterthought.
