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
- [ ] SATA/AHCI driver support.
- [ ] NVMe driver support.
- [x] Ext2 filesystem driver (Read-only implemented).
- [ ] Ext1 (legacy) read-only support (optional).
- [ ] Page Swapping (Swap space) support.
- [ ] Demand Paging optimization and OOM killer.
- [ ] Journaling abstraction for VFS.
- [ ] Advanced file locking (flock/fcntl).

## M15: IPC, Security & Standard OS Features

- [ ] Process Signals (SIGINT, SIGKILL, SIGSEGV, etc.).
- [ ] Inter-process communication (UNIX domain sockets, message queues).
- [ ] POSIX shared memory (`shmget`, `shmat`).
- [ ] User and Group ID management (UID/GID) and ring transitions.
- [ ] File permissions, capabilities, and ACLs.
- [ ] Standard C library (libc) port (e.g., newlib or musl).
- [ ] Port of standard utilities (GNU coreutils / BusyBox).

## M16: User Space Applications & TUI

- [ ] Mini File Manager (TUI-based, Midnight Commander style).
- [ ] File tracking and build automation utility (`make` clone).
- [ ] Text editor (e.g., `vi` or `nano` clone).

## M17: POSIX Syscall Compliance & Self-Hosting

- [ ] POSIX Process Management: `fork()`, standard `execve()`, `waitpid()`.
- [ ] POSIX File I/O: `stat()`, `lseek()`, `unlink()`, `mkdir()`, `chdir()`, `getdents()`.
- [ ] POSIX Pipes & FDs: `pipe()`, `dup2()`, `fcntl()`.
- [ ] POSIX Memory: Proper user-space `mmap()`, `munmap()`, `brk()`.
- [ ] POSIX Sockets: `socket()`, `bind()`, `connect()`, `send()`, `recv()`.
- [ ] POSIX Terminal: `ioctl()` and `termios` support for proper TTY.
- [ ] Cross-compile and port GCC (C/C++ compiler) specifically for `x86_64-b1nix`.
- [ ] Port GNU Binutils (`as`, `ld`) and GNU Make.
- [ ] Achieve self-hosting: compile the B1NIX kernel *inside* B1NIX using ported GCC.
