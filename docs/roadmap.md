# Roadmap

## M0: Boot and Diagnostics

- [x] Build freestanding kernel ELF.
- [x] Boot on x86_64 in QEMU through Multiboot2.
- [x] Add serial and VGA text output.
- [x] Add panic, assert, and early logging.
- [x] Keep architecture-specific code behind narrow interfaces.

## M1: Architecture Layer

- [x] Define `arch_init`.
- [x] Add interrupt descriptor table on x86_64 for CPU exceptions 0-31.
- [x] Add page fault diagnostics with `CR2` reporting.
- [x] Add PIT timer interrupt through PIC IRQ0.
- [x] Add AArch64 QEMU `virt` boot path.
- [x] Keep CPU halt, interrupt control, and context switch arch-local.

## M2: Memory

- [x] Parse boot memory map from Multiboot2.
- [x] Add initial physical frame allocator.
- [x] Add initial x86_64 virtual memory mapping.
- [x] Add initial frame-backed bump kernel heap.
- [x] Add page fault diagnostics.
- [x] Replace bump frame allocator with reusable bitmap/free-list allocator.
- [x] Add unmap/remap helpers.
- [x] Add higher-half kernel mapping. Initial direct-map window done.

## M3: Scheduling

- [x] Add initial kernel threads.
- [x] Add cooperative round-robin scheduler.
- [x] Add x86_64 kernel context switching.
- [x] Add blocking and wakeup queues.
- [x] Add preemptive scheduling from timer ticks.
- [x] Add task sleep/yield APIs backed by timer ticks.

## M4: Userspace

- [x] Add user address spaces. Initial process address-space objects done.
- [x] Add syscall entry. Initial syscall dispatcher ABI done.
- [x] Add initramfs.
- [x] Run `/bin/init`.
- [x] Add basic shell.

## M5: VFS and Devices

- [x] Add file descriptors.
- [x] Add VFS.
- [x] Add devfs.
- [x] Add tmpfs.
- [x] Add tarfs/initramfs.
- [x] Add virtio-blk. Initial stub device done.

## M6: Network

- [x] Add virtio-net. Initial probe/demo device done.
- [x] Add Ethernet frame parsing.
- [x] Add ARP.
- [x] Add IPv4.
- [x] Add ICMP echo.
- [x] Add UDP.
- [x] Add DHCP client.

## M7: Graphics

- [x] Add boot framebuffer path.
- [x] Add graphical console.
- [x] Add input.
- [x] Add basic compositor.
- [x] Explore VirtIO GPU.

## M8: Advanced VFS and Filesystems

- [x] Hierarchical directory structure (`/bin`, `/dev`, etc.).
- [x] FAT32 or ext2 filesystem driver.

## M9: Hardware Drivers

- [x] virtio-blk (real implementation, replace stub).
- [x] virtio-net (real implementation, replace stub).
- [x] input (ps2_kbd fully connected to shell).

## M10: Full Network Stack

- [x] Ethernet frames parsing and sending.
- [x] ARP resolution.
- [x] IPv4 routing.
- [x] ICMP ping responses.
- [x] UDP protocol.
- [x] DHCP client.
- [x] DNS client.

## M11: Shell and Utilities

- [x] Shell built-in commands: `ps`, `mem`, `ping`, `reboot`.
- [x] Pipes and redirection.
- [x] Environment variables.
- [x] Job control.

## M12: Syscalls and Process Management

- [x] Task priorities in scheduler.
- [x] `exit` syscall.
- [x] `exec` syscall.
- [x] `wait` syscall.
- [x] `mmap` syscall.
- [x] `sleep` syscall.

## M13: AArch64 Port Completion

- [x] Hook up AArch64 to the build (`KERNEL_SOURCES`).
- [x] C kernel entry and memory map parsing for AArch64.
- [x] Implement AArch64 architecture layer.
- [x] Ensure common code compiles for both platforms.

## M14: Advanced Storage, Swap & File Systems

- [x] Block device abstraction layer and caching.
- [x] SATA/AHCI driver support.
- [x] NVMe driver support.
- [x] Ext2 filesystem driver (Read + Write).
- [x] Ext1 (legacy) read-only support.
- [x] Page Swapping (Swap space) support.
- [x] Demand Paging optimization and OOM killer.
- [x] Journaling abstraction for VFS.
- [x] Advanced file locking (flock/fcntl).

## M15: IPC, Security & Standard OS Features

- [x] Process Signals (SIGINT, SIGKILL, SIGSEGV, etc.).
- [x] Inter-process communication (UNIX domain sockets, message queues).
- [x] POSIX shared memory (`shmget`, `shmat`).
- [x] User and Group ID management (UID/GID) and ring transitions.
- [x] File permissions, capabilities, and ACLs.
- [x] Standard C library (libc) port (e.g., newlib or musl).
- [x] Port of standard utilities (GNU coreutils / BusyBox).

## M16: User Space Applications & TUI

- [x] Mini File Manager (TUI-based, Midnight Commander style).
- [x] File tracking and build automation utility (`make` clone).
- [x] Text editor (e.g., `vi` or `nano` clone).

## M17: POSIX Syscall Compliance & Self-Hosting

- [x] POSIX Process Management: `fork()`, standard `execve()`, `waitpid()` initial ABI.
- [x] POSIX File I/O: `stat()`, `lseek()`, `unlink()`, `mkdir()`, `chdir()`, `getdents()`.
- [x] POSIX Pipes & FDs: `pipe()`, `dup2()`, `fcntl()`.
- [x] POSIX Memory: Proper user-space `mmap()`, `munmap()`, `brk()` initial heap ABI.
- [x] POSIX Sockets: `socket()`, `bind()`, `connect()`, `send()`, `recv()` initial socket FD ABI.
- [x] POSIX Terminal: `ioctl()` and `termios` support for proper TTY.
- [ ] Cross-compile and port GCC (C/C++ compiler) specifically for `x86_64-b1nix`.
- [ ] Port GNU Binutils (`as`, `ld`) and GNU Make.
- [ ] Achieve self-hosting: compile the B1NIX kernel *inside* B1NIX using ported GCC.

## M18: Real Userspace and ELF Loader

- [x] Load ELF64 executables from VFS instead of relying only on built-in programs.
- [x] Build a real user address space per process with user/kernel separation.
- [x] Add syscall `copyin`/`copyout` helpers for safe user pointers.
- [x] Create a proper user stack with `argc`, `argv`, `envp`, and auxiliary vector basics.
- [x] Implement `execve()` as image replacement, not only built-in dispatch.
- [x] Add process exit status propagation and zombie reaping semantics.
- [x] Add QEMU tests that boot, launch `/bin/init`, and execute a VFS-loaded program.

M18 establishes the loader and process-image ABI while B1NIX still uses kernel
threads as the execution substrate. ELF64 files are read through VFS, PT_LOAD
segments are copied into per-process image state, initial stack metadata is
constructed with `argc`, `argv`, `envp`, and basic auxv entries, and `/bin/init`
now boots from a VFS-loaded ELF image that launches a second VFS-loaded ELF
smoke program before starting the shell. Full hardware-enforced ring3 entry and
copy-on-write address spaces remain follow-up work for M19/M24.

## M19: Process Model and FD Tables

- [x] Implement real `fork()` with copied or copy-on-write address spaces.
- [x] Add per-process file descriptor tables instead of a single global FD table.
- [x] Inherit and close FDs according to POSIX rules, including close-on-exec.
- [x] Make `stdin`, `stdout`, and `stderr` real descriptors `0`, `1`, and `2`.
- [x] Store per-process cwd, environment, umask, process group, and session metadata.
- [x] Implement `waitpid()` options and zombie lifecycle correctly.
- [x] Add basic process groups and terminal foreground job ownership.

M19 moves descriptor ownership out of the global VFS handle namespace and into
per-task fd tables. VFS handles are now open-file descriptions with refcounts,
while process-visible descriptors are inherited on spawn/fork, closed on task
exit, and honor `FD_CLOEXEC`. Descriptor `0`, `1`, and `2` are initialized as
real task-local TTY descriptors. `waitpid()` now supports non-blocking
`WNOHANG`, and zombies remain reapable until the parent waits. Process metadata
now carries cwd, environment storage, umask, process group, and session ids.
Because B1NIX still runs user images on cooperative kernel threads, fork copies
the process metadata/FD view rather than hardware page tables; full MMU COW is
tracked with the later memory-management hardening work.

## M20: Terminal, TTY, and Interactive Shell

- [x] Add a real TTY device with line discipline.
- [x] Support canonical and raw terminal modes through `termios`.
- [x] Handle Ctrl-C, Ctrl-D, Ctrl-Z, backspace, arrows, and EOF behavior.
- [x] Route keyboard input through `/dev/tty` and FD `0`.
- [x] Replace temporary-file shell pipes with real `pipe()` and `dup2()` wiring.
- [x] Add shell redirection `<`, `>`, `>>`, `2>`, and descriptor duplication.
- [x] Implement `PATH` command lookup against the VFS.
- [x] Improve shell errors and exit statuses.

M20 adds `/dev/tty` as a VFS device, initializes descriptors `0`, `1`, and `2`
to the terminal, and routes keyboard input through the TTY line discipline.
Canonical mode, echo, signal-control characters, EOF, and raw-mode toggling are
represented through the initial `termios` ABI. The shell now resolves commands
through `PATH`, uses real `pipe()`/`dup2()` descriptors for pipelines, runs the
producer and consumer as separate tasks, closes pipe ends correctly in the
shell, and supports basic input/output/error redirection using the M19
per-process fd tables.

## M21: Persistent Root Filesystem

- [x] Boot with a persistent root filesystem from a disk image.
- [x] Add mountpoints and a real `mount`/`umount` VFS model.
- [x] Stabilize writable ext2 as the first reliable root filesystem target.
- [x] Add `rename()`, `rmdir()`, `fstat()`, `fsync()`, and open flags (`O_CREAT`, `O_TRUNC`, `O_APPEND`, `O_DIRECTORY`).
- [x] Flush block cache on shutdown and reboot.
- [x] Add `/etc`, `/bin`, `/dev`, `/home`, `/tmp`, and `/var` layout.
- [x] Add an image creation/install script for local development.

M21 is complete for the current boot model. The VFS tracks mounted sources in a
mount table and exposes `mount()`, `umount()`, and `sync()` syscalls. The root
tree is initialized with the standard terminal OS layout, and `make root-image`
creates a seeded ext2 disk. When that disk is attached as `virtio-blk0`, ext2 is
mounted at `/` and overlays the initramfs fallback files, so persistent files can
live at the root while built-in `/bin/init` remains available until the full
disk userland is populated.

## M22: Core Terminal Utilities

- [x] Add `pwd`, `ls`, `cp`, `mv`, `rm`, `mkdir`, `rmdir`, `chmod`, `chown`, and `ln`.
- [x] Add `ps`, `kill`, `sleep`, `date`, `uname`, `id`, and `whoami`.
- [x] Add text tools: `cat`, `head`, `tail`, `grep`, `find`, `wc`, `sort`, and `uniq`.
- [x] Add filesystem tools: `mount`, `df`, `sync`, and `hexdump`.
- [x] Keep BusyBox-style multi-call dispatch for small binaries.
- [ ] Add utility smoke tests that run from the shell and from init scripts.

## M23: Networking for Terminal Use

- [x] Turn the socket ABI into real UDP sockets.
- [x] Add minimal TCP client support.
- [x] Add DNS resolver integration through libc-style calls.
- [x] Add `ifconfig` or `ip` for interface status and static configuration.
- [x] Add `ping`, `nc`, and a tiny `wget`/HTTP client.
- [x] Handle missing network devices gracefully in all network paths.

## M24: Reliability and Diagnostics

- [x] Add syscall argument validation and consistent error codes.
- [x] Add kernel backtraces or symbolized panic locations.
- [x] Replace avoidable panics with recoverable errors.
- [x] Add regression tests for VFS, scheduler, pipes, terminal, and sockets.
- [x] Add QEMU smoke tests for x86_64 and AArch64 in CI.
- [x] Track implemented, initial, stub, and planned features explicitly in docs.
- [x] Add kernel log levels and a ring buffer readable from userspace.

## M25: Minimal Native C Toolchain

- [x] Define the B1NIX userspace ELF ABI and calling convention.
- [x] Add `crt0.o` startup code for B1NIX userspace programs.
- [x] Add a userspace linker script for B1NIX ELF binaries.
- [x] Build a minimal libc profile with syscall wrappers, `string`, `stdio`, `stdlib`, and simple `malloc`.
- [x] Add an external `b1nix-cc` wrapper backed by clang for early userland builds.
- [x] Build and run a VFS-loaded `hello.c` ELF program.
- [x] Port TinyCC/TCC as the first practical in-guest C compiler.
- [x] Compile and run `hello.c` inside B1NIX using `/bin/tcc`.
- [x] Compile one simple shell utility inside B1NIX.
- [x] Document the path from external cross-builds to in-guest compilation.

## M26: Full Toolchain and Self-Hosting

- [ ] Define the `x86_64-b1nix` target ABI document.
- [ ] Port Binutils (`as`, `ld`, `objcopy`, `ar`) for `x86_64-b1nix`.
- [ ] Port GCC after the minimal C toolchain and filesystem are stable.
- [ ] Build larger user programs with the external cross toolchain.
- [ ] Build the B1NIX kernel inside B1NIX.

## M27: Terminal OS Polish

- [ ] Add boot menu options and kernel command line parsing.
- [ ] Add init scripts and a simple service supervisor.
- [ ] Add users, passwords or login shell basics.
- [ ] Add stable shutdown, reboot, and emergency shell paths.
- [ ] Document everyday usage from boot to editing/building files.
- [ ] Keep the system usable without graphics as a first-class target.
