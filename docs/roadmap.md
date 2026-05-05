# Roadmap

Status legend:

- `done`: implemented for the current B1NIX architecture and covered by at least a basic build or boot path.
- `initial`: usable first implementation exists, but it is not complete enough to call production/POSIX-complete.
- `partial`: important pieces exist, but behavior is incomplete, narrow, or has known gaps.
- `stub`: source/API placeholder exists, but it is not a real feature yet.
- `planned`: not implemented yet.

## M0: Boot and Diagnostics

- [x] `done` Build freestanding kernel ELF.
- [x] `done` Boot on x86_64 in QEMU through Multiboot2.
- [x] `done` Add serial and VGA text output.
- [x] `done` Add panic, assert, and early logging.
- [x] `done` Keep architecture-specific code behind narrow interfaces.
- [x] `done` Add GRUB ISO boot packaging through the top-level Makefile.
- [x] `done` Add basic kernel log infrastructure used by diagnostics and `dmesg`.

## M1: Architecture Layer

- [x] `done` Define `arch_init`.
- [x] `done` Add interrupt descriptor table on x86_64 for CPU exceptions 0-31.
- [x] `done` Add page fault diagnostics with `CR2` reporting.
- [x] `done` Add PIT timer interrupt through PIC IRQ0.
- [x] `initial` Add AArch64 QEMU `virt` boot path.
- [x] `done` Keep CPU halt, interrupt control, and context switch arch-local.
- [x] `done` Add x86_64 interrupt-to-signal mapping for common CPU faults.
- [x] `initial` Add AArch64 serial, interrupt, bootinfo, and context-switch scaffolding.

## M2: Memory

- [x] `done` Parse boot memory map from Multiboot2.
- [x] `done` Add initial physical frame allocator.
- [x] `done` Add initial x86_64 virtual memory mapping.
- [x] `done` Add initial frame-backed bump kernel heap.
- [x] `done` Add page fault diagnostics.
- [x] `done` Replace bump frame allocator with reusable bitmap/free-list allocator.
- [x] `initial` Add unmap/remap helpers.
- [x] `initial` Add higher-half kernel mapping and direct-map window.
- [x] `partial` Add lazy page allocation hooks in the x86 page-fault path.
- [x] `partial` Add swap slot bookkeeping and swap in/out helpers.
- [ ] `planned` Add full per-process page tables, protection enforcement, and copy-on-write.
- [ ] `planned` Add full `mmap`/`munmap`/`mprotect` semantics with file-backed mappings.

## M3: Scheduling

- [x] `done` Add initial kernel threads.
- [x] `done` Add cooperative round-robin scheduler.
- [x] `done` Add x86_64 kernel context switching.
- [x] `done` Add blocking and wakeup queues.
- [x] `initial` Add preemptive scheduling from timer ticks.
- [x] `done` Add task sleep/yield APIs backed by timer ticks.
- [x] `initial` Add task priorities.
- [x] `initial` Add zombie lifecycle and parent wait bookkeeping.
- [x] `partial` Add process groups and session metadata.
- [ ] `planned` Add full POSIX scheduling/session/job-control semantics.

## M4: Userspace

- [x] `initial` Add user address-space objects.
- [x] `initial` Add syscall dispatcher ABI.
- [x] `done` Add initramfs.
- [x] `done` Run `/bin/init`.
- [x] `done` Add basic shell.
- [x] `initial` Add ELF64 loading from VFS with argc/argv/envp/auxv stack metadata.
- [x] `initial` Add syscall `copyin`/`copyout`/`copyinstr` helpers.
- [x] `partial` Add built-in program fallback when ELF loading fails.
- [ ] `planned` Add hardware-enforced ring3 userspace entry/return for normal programs.
- [ ] `planned` Add arbitrary native ELF execution with real user registers and page tables.

## M5: VFS and Devices

- [x] `done` Add file descriptors.
- [x] `done` Add VFS.
- [x] `done` Add devfs.
- [x] `done` Add tmpfs.
- [x] `done` Add tarfs/initramfs.
- [x] `initial` Add virtio-blk as an early device, later replaced by the real driver.
- [x] `done` Add `/dev/tty` VFS device.
- [x] `done` Add `/dev` integration for terminal-backed stdin/stdout/stderr.
- [x] `initial` Add VFS mount table and mountpoint dispatch.
- [x] `partial` Add ACL fields and permission metadata in VFS nodes.
- [ ] `planned` Add symlinks, hard links, `readlink`, full permissions, and robust path normalization.

## M6: Network

- [x] `initial` Add virtio-net probe/demo device, later replaced by the real driver.
- [x] `done` Add Ethernet frame parsing.
- [x] `done` Add ARP.
- [x] `done` Add IPv4.
- [x] `done` Add ICMP echo.
- [x] `initial` Add UDP.
- [x] `initial` Add DHCP client.
- [x] `initial` Add DNS client.
- [ ] `planned` Add full socket-driven TCP/UDP server behavior.

## M7: Graphics

- [x] `done` Add boot framebuffer path.
- [x] `done` Add graphical console.
- [x] `done` Add input.
- [x] `initial` Add basic compositor.
- [x] `stub` Explore/register VirtIO GPU.
- [ ] `planned` Add a real VirtIO GPU mode-setting/rendering path.

## M8: Advanced VFS and Filesystems

- [x] `done` Add hierarchical directory structure (`/bin`, `/dev`, `/etc`, `/home`, `/tmp`, `/var`).
- [x] `initial` Add FAT32 or ext2 filesystem driver.
- [x] `done` Add initramfs fallback tree.
- [x] `partial` Add FAT32 read/import path with limited feature support.
- [x] `partial` Add ext1 legacy read-only support.
- [x] `initial` Add ext2 read/write support.
- [x] `stub` Add ext3/ext4 source scaffolding.
- [ ] `planned` Add complete ext2 indirect blocks, timestamps, durable directory updates, and fsck-friendly metadata.

## M9: Hardware Drivers

- [x] `done` Add virtio-blk real block read/write implementation.
- [x] `done` Add virtio-net real PCI/VirtIO initialization path.
- [x] `done` Connect PS/2 keyboard input to shell and TTY paths.
- [x] `initial` Add PCI device enumeration.
- [x] `initial` Add block-device abstraction and cache.
- [x] `partial` Add AHCI driver support.
- [x] `partial` Add NVMe driver support.
- [x] `stub` Add VirtIO GPU driver registration.

## M10: Full Network Stack

- [x] `done` Ethernet frame parsing and sending.
- [x] `done` ARP resolution.
- [x] `initial` IPv4 routing.
- [x] `done` ICMP ping responses.
- [x] `initial` UDP protocol.
- [x] `initial` DHCP client.
- [x] `initial` DNS client.
- [x] `initial` Add socket ABI integration for UDP/TCP-style descriptors.
- [x] `partial` Add minimal TCP client path for terminal tools.
- [ ] `planned` Add `listen`, `accept`, TCP lifecycle, socket options, and `select`/`poll` integration.

## M11: Shell and Utilities

- [x] `done` Shell built-in commands: `ps`, `mem`, `ping`, `reboot`.
- [x] `done` Pipes and redirection.
- [x] `initial` Environment variables.
- [x] `partial` Job control.
- [x] `done` PATH lookup against VFS.
- [x] `done` Descriptor redirection for `<`, `>`, `>>`, `2>`, and descriptor duplication.
- [x] `done` Pipeline execution through real `pipe()` and `dup2()`.
- [x] `done` Add `selfhost` status command.
- [ ] `planned` Add full background-job tracking and POSIX terminal job control.

## M12: Syscalls and Process Management

- [x] `initial` Task priorities in scheduler.
- [x] `done` `exit` syscall.
- [x] `initial` `exec` syscall.
- [x] `initial` `wait` syscall.
- [x] `initial` `mmap` syscall.
- [x] `done` `sleep` syscall.
- [x] `initial` `kill`, `signal`, and `getpid`.
- [x] `initial` UID/GID syscalls.
- [x] `done` `getcwd`, `uname`, `time`, and `dmesg` syscalls.
- [x] `initial` `fork`, `execve`, and `waitpid` syscalls.
- [x] `initial` `brk`, `munmap`, `ioctl`, and termios syscalls.
- [ ] `planned` Add full POSIX signal ABI: `sigaction`, masks, `sigreturn`, and real handler entry.

## M13: AArch64 Port Completion

- [x] `done` Hook up AArch64 to the build (`KERNEL_SOURCES`).
- [x] `initial` C kernel entry and memory map parsing for AArch64.
- [x] `initial` Implement AArch64 architecture layer.
- [x] `done` Ensure common code compiles for both platforms.
- [x] `initial` Add AArch64 boot, serial console, interrupt, paging, and context-switch files.
- [x] `done` Add AArch64 network/socket stubs so common VFS socket code links cleanly.
- [ ] `planned` Bring AArch64 to feature parity with x86_64 boot, scheduler, MMU, devices, and userspace.

## M14: Advanced Storage, Swap & File Systems

- [x] `initial` Block device abstraction layer and caching.
- [x] `partial` SATA/AHCI driver support.
- [x] `partial` NVMe driver support.
- [x] `initial` Ext2 filesystem driver with read/write support.
- [x] `partial` Ext1 legacy read-only support.
- [x] `partial` Page swapping support.
- [x] `partial` Demand paging optimization and OOM fallback hooks.
- [x] `initial` Journaling abstraction for VFS.
- [x] `initial` Advanced file locking (`flock`/`fcntl`).
- [x] `done` `sync()` and `fsync()` flush block cache paths.
- [x] `initial` Persistent ext2 root-image creation and boot overlay.
- [ ] `planned` Add complete AHCI/NVMe production paths, robust journal replay, and full file-lock blocking semantics.

## M15: IPC, Security & Standard OS Features

- [x] `initial` Process signals (`SIGINT`, `SIGKILL`, `SIGSEGV`, etc.).
- [x] `partial` Inter-process communication through message queues and socket-like descriptors.
- [x] `initial` POSIX-style shared memory (`shmget`, `shmat`, `shmdt`, `shmctl`).
- [x] `initial` User and group ID management (`uid`, `euid`, `gid`, `egid`, `setuid`, `setgid`).
- [x] `partial` File permissions, capabilities, and ACL metadata.
- [x] `initial` Standard C library profile for B1NIX userspace.
- [x] `partial` BusyBox-style standard utilities.
- [ ] `planned` Add UNIX domain sockets.
- [ ] `planned` Enforce permissions/capabilities consistently through VFS and process credentials.
- [ ] `planned` Replace signal stubs with real userspace signal delivery.

## M16: User Space Applications & TUI

- [x] `done` Mini File Manager (TUI-based, Midnight Commander style).
- [x] `done` File tracking and build automation utility (`make` clone).
- [x] `done` Text editor (`vi`/`nano`-style clone).
- [x] `done` Shared TUI input/rendering helpers.
- [x] `partial` File manager copy/move clipboard actions reserved for future work.
- [ ] `planned` Add richer editor persistence/workflow tests and TUI app smoke tests.

## M17: POSIX Syscall Compliance & Self-Hosting

- [x] `initial` POSIX Process Management: `fork()`, standard `execve()`, `waitpid()` initial ABI.
- [x] `initial` POSIX File I/O: `stat()`, `lseek()`, `unlink()`, `mkdir()`, `chdir()`, `getdents()`.
- [x] `initial` POSIX Pipes & FDs: `pipe()`, `dup2()`, `fcntl()`.
- [x] `initial` POSIX Memory: user-space `mmap()`, `munmap()`, `brk()` initial heap ABI.
- [x] `initial` POSIX Sockets: `socket()`, `bind()`, `connect()`, `send()`, `recv()` initial socket FD ABI.
- [x] `initial` POSIX Terminal: `ioctl()` and `termios` support for TTY.
- [x] `done` Add syscall ABI constants and userspace syscall header mirrors.
- [x] `done` Add `docs/abi.md` for the userspace ELF ABI and calling convention.
- [x] `initial` Add `SYS_SELFHOST_STATUS` and `/bin/selfhost` status reporting.
- [ ] `planned` Cross-compile and port GCC specifically for `x86_64-b1nix`.
- [ ] `planned` Port GNU Binutils (`as`, `ld`, `objcopy`, `ar`) and GNU Make.
- [ ] `planned` Achieve self-hosting: compile the B1NIX kernel inside B1NIX using ported GCC.

## M18: Real Userspace and ELF Loader

- [x] `initial` Load ELF64 executables from VFS instead of relying only on built-in programs.
- [x] `partial` Build a user address-space record per process.
- [x] `done` Add syscall `copyin`/`copyout` helpers for safe user pointers.
- [x] `initial` Create a user stack with `argc`, `argv`, `envp`, and auxiliary vector basics.
- [x] `initial` Implement `execve()` as image replacement, not only built-in dispatch.
- [x] `initial` Add process exit status propagation and zombie reaping semantics.
- [x] `done` Add QEMU tests that boot, launch `/bin/init`, and execute a VFS-loaded program.
- [x] `done` Add external clang-backed `b1nix-cc` wrapper for early ELF builds.
- [ ] `planned` Add hardware-enforced ring3 entry and return.

M18 establishes the loader and process-image ABI while B1NIX still uses kernel
threads as the execution substrate. ELF64 files are read through VFS, PT_LOAD
segments are copied into per-process image state, initial stack metadata is
constructed with `argc`, `argv`, `envp`, and basic auxv entries, and `/bin/init`
can boot from a VFS-loaded ELF image before starting the shell. Full
hardware-enforced ring3 entry and copy-on-write address spaces remain follow-up
work.

## M19: Process Model and FD Tables

- [x] `partial` Implement `fork()` with copied process metadata and FD view.
- [x] `done` Add per-process file descriptor tables instead of a single global FD table.
- [x] `done` Inherit and close FDs according to POSIX rules, including close-on-exec.
- [x] `done` Make `stdin`, `stdout`, and `stderr` real descriptors `0`, `1`, and `2`.
- [x] `initial` Store per-process cwd, environment, umask, process group, and session metadata.
- [x] `initial` Implement `waitpid()` options and zombie lifecycle.
- [x] `partial` Add basic process groups and terminal foreground job ownership.
- [x] `done` Add refcounted VFS handles/open-file descriptions.
- [ ] `planned` Add MMU-backed fork with copied or copy-on-write address spaces.
- [ ] `planned` Add exact POSIX child/parent register-return semantics.

M19 moves descriptor ownership out of the global VFS handle namespace and into
per-task fd tables. VFS handles are now open-file descriptions with refcounts,
while process-visible descriptors are inherited on spawn/fork, closed on task
exit, and honor `FD_CLOEXEC`. Descriptor `0`, `1`, and `2` are initialized as
real task-local TTY descriptors. `waitpid()` supports non-blocking `WNOHANG`,
and zombies remain reapable until the parent waits. Because B1NIX still runs
user images on cooperative kernel threads, fork copies process metadata/FD view
rather than hardware page tables.

## M20: Terminal, TTY, and Interactive Shell

- [x] `done` Add a real TTY device with line discipline.
- [x] `initial` Support canonical and raw terminal modes through `termios`.
- [x] `done` Handle Ctrl-C, Ctrl-D, Ctrl-Z, backspace, arrows, and EOF behavior.
- [x] `done` Route keyboard input through `/dev/tty` and FD `0`.
- [x] `done` Replace temporary-file shell pipes with real `pipe()` and `dup2()` wiring.
- [x] `done` Add shell redirection `<`, `>`, `>>`, `2>`, and descriptor duplication.
- [x] `done` Implement `PATH` command lookup against the VFS.
- [x] `done` Improve shell errors and exit statuses.
- [x] `initial` Route terminal control characters into the signal/process metadata path.
- [ ] `planned` Add full controlling-terminal/process-group signal behavior.

M20 adds `/dev/tty` as a VFS device, initializes descriptors `0`, `1`, and `2`
to the terminal, and routes keyboard input through the TTY line discipline.
Canonical mode, echo, signal-control characters, EOF, and raw-mode toggling are
represented through the initial `termios` ABI. The shell now resolves commands
through `PATH`, uses real `pipe()`/`dup2()` descriptors for pipelines, and
supports basic input/output/error redirection using the M19 per-process fd
tables.

## M21: Persistent Root Filesystem

- [x] `done` Boot with a persistent root filesystem from a disk image.
- [x] `initial` Add mountpoints and a real `mount`/`umount` VFS model.
- [x] `initial` Stabilize writable ext2 as the first reliable root filesystem target.
- [x] `done` Add `rename()`, `rmdir()`, `fstat()`, `fsync()`, and open flags (`O_CREAT`, `O_TRUNC`, `O_APPEND`, `O_DIRECTORY`).
- [x] `done` Flush block cache on shutdown and reboot.
- [x] `done` Add `/etc`, `/bin`, `/dev`, `/home`, `/tmp`, and `/var` layout.
- [x] `done` Add an image creation/install script for local development.
- [x] `done` Add `make root-image` and `make run-root` workflows.
- [x] `initial` Overlay attached ext2 root over initramfs fallback files.
- [x] `done` Add mount listing for active VFS mount table entries.
- [ ] `planned` Add complete mount option handling.

M21 is complete for the current boot model. The VFS tracks mounted sources in a
mount table and exposes `mount()`, `umount()`, and `sync()` syscalls. The root
tree is initialized with the standard terminal OS layout, and `make root-image`
creates a seeded ext2 disk. When that disk is attached as `virtio-blk0`, ext2 is
mounted at `/` and overlays the initramfs fallback files, so persistent files can
live at the root while built-in `/bin/init` remains available until the full
disk userland is populated.

## M22: Core Terminal Utilities

- [x] `done` Add `pwd`, `ls`, `cp`, `mv`, `rm`, `mkdir`, `rmdir`, `chmod`, `chown`, and `ln`.
- [x] `done` Add `ps`, `kill`, `sleep`, `date`, `uname`, `id`, and `whoami`.
- [x] `done` Add text tools: `cat`, `head`, `tail`, `grep`, `find`, `wc`, `sort`, and `uniq`.
- [x] `done` Add filesystem tools: `mount`, `df`, `sync`, and `hexdump`.
- [x] `done` Keep BusyBox-style multi-call dispatch for small binaries.
- [x] `done` Add `clear`, `mem`, `dmesg`, `ifconfig`, `ping`, `nc`, and `wget` utilities.
- [x] `partial` Implement `ln` as copy-style fallback rather than real hard link.
- [x] `done` Implement `mount` command mounting path and active mount listing.
- [ ] `planned` Add utility smoke tests that run from the shell and from init scripts.
- [ ] `planned` Add option-compatible behavior for common utility flags.

## M23: Networking for Terminal Use

- [x] `initial` Turn the socket ABI into UDP-capable socket descriptors.
- [x] `partial` Add minimal TCP client support.
- [x] `initial` Add DNS resolver integration through libc-style calls and shell commands.
- [x] `done` Add `ifconfig`-style interface status.
- [x] `partial` Add `ping`, `nc`, and a tiny `wget`/HTTP client.
- [x] `done` Handle missing network devices gracefully in user-facing network paths.
- [x] `done` Keep AArch64 network syscalls/socket paths as explicit unavailable stubs.
- [ ] `planned` Add TCP server support: `listen`, `accept`, close states, retransmission, and timeout handling.
- [ ] `planned` Add socket options and `select`/`poll` readiness.

## M24: Reliability and Diagnostics

- [x] `initial` Add syscall argument validation and error returns.
- [x] `initial` Add kernel backtraces or symbolized panic locations.
- [x] `partial` Replace avoidable panics with recoverable errors.
- [x] `partial` Add regression/smoke tests for VFS, scheduler, pipes, terminal, and sockets.
- [x] `initial` Add QEMU smoke tests for x86_64 and AArch64.
- [x] `done` Track implemented, initial, stub, and planned features explicitly in docs.
- [x] `done` Add kernel log levels and a ring buffer readable from userspace.
- [x] `done` Add `/bin/dmesg` backed by `SYS_DMESG`.
- [ ] `planned` Add CI-grade interactive shell utility tests.
- [ ] `planned` Make syscall errors consistently map to userspace `errno`.

## M25: Minimal Native C Toolchain

- [x] `done` Define the B1NIX userspace ELF ABI and calling convention.
- [x] `done` Add `crt0.o` startup code for B1NIX userspace programs.
- [x] `done` Add a userspace linker script for B1NIX ELF binaries.
- [x] `initial` Build a minimal libc profile with syscall wrappers, `string`, `stdio`, `stdlib`, and simple `malloc`.
- [x] `done` Add an external `b1nix-cc` wrapper backed by clang for early userland builds.
- [x] `done` Build and run a VFS-loaded `hello.c` ELF program.
- [x] `partial` Import TinyCC/TCC source and wire an early `/bin/tcc` build target.
- [x] `partial` Prepare installed userspace headers, `libb1nix.a`, and `crt0.o` in the rootfs image.
- [ ] `planned` Compile and run `hello.c` inside B1NIX using `/bin/tcc`.
- [ ] `planned` Compile one simple shell utility inside B1NIX.
- [ ] `planned` Document the exact path from external cross-builds to in-guest compilation after it works end-to-end.

## M26: Full Toolchain and Self-Hosting

- [x] `initial` Define the `x86_64-b1nix` target ABI document in `docs/abi.md`.
- [ ] `planned` Port Binutils (`as`, `ld`, `objcopy`, `ar`) for `x86_64-b1nix`.
- [ ] `planned` Port GCC after the minimal C toolchain and filesystem are stable.
- [ ] `planned` Build larger user programs with the external cross toolchain.
- [ ] `planned` Build the B1NIX kernel inside B1NIX.
- [ ] `planned` Add native `make`/assembler/linker workflow usable from the B1NIX shell.

## M27: Terminal OS Polish

- [ ] `planned` Add boot menu options and kernel command line parsing.
- [ ] `planned` Add init scripts and a simple service supervisor.
- [ ] `planned` Add users, passwords or login shell basics.
- [ ] `planned` Add stable shutdown, reboot, and emergency shell paths.
- [ ] `planned` Document everyday usage from boot to editing/building files.
- [ ] `planned` Keep the system usable without graphics as a first-class target.
- [ ] `planned` Add first-boot setup for persistent root images.
- [ ] `planned` Add a clear POSIX compatibility matrix for application ports.
