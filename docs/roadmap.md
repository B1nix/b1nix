# Roadmap

Status legend:

- `done`: implemented for the current B1NIX architecture and covered by at least a basic build or boot path.
- `initial`: usable first implementation exists, but it is not complete enough to call production/POSIX-complete.
- `partial`: important pieces exist, but behavior is incomplete, narrow, or has known gaps.
- `stub`: source/API placeholder exists, but it is not a real feature yet.
- `planned`: not implemented yet.

Detailed closeout instructions for the POSIX-facing `VFS/path/files` and
`Shell/coreutils` branches live in [`docs/posix-branches.md`](posix-branches.md).
The stable no-surprises checklist for closing POSIX-facing work lives in
[`docs/posix-requirements.md`](posix-requirements.md).

## Current POSIX Estimate

- Overall practical POSIX compatibility: roughly 70-78%.
- `VFS/path/files`: roughly 90-95%.
- `Shell/coreutils`: roughly 80-85%.

These percentages mean "can run small real workflows", not "passes a POSIX
conformance suite". The biggest remaining blockers are full shell parsing,
broader utility flag coverage, permission edge cases, and kernel backtrace
diagnostics.

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
- [x] `done` Keep CPU halt, interrupt control, and context switch arch-local.
- [x] `done` Add x86_64 interrupt-to-signal mapping for common CPU faults.

## M2: Memory

- [x] `done` Parse boot memory map from Multiboot2.
- [x] `done` Add initial physical frame allocator.
- [x] `done` Add initial x86_64 virtual memory mapping.
- [x] `done` Add initial frame-backed bump kernel heap.
- [x] `done` Add page fault diagnostics.
- [x] `done` Replace bump frame allocator with reusable bitmap/free-list allocator.
- [x] `done` Add unmap/remap helpers.
- [x] `done` Add higher-half kernel mapping and direct-map window.
- [x] `done` Add lazy page allocation hooks in the x86 page-fault path.
- [x] `done` Add swap slot bookkeeping and swap in/out helpers.
- [x] `done` Add per-process page tables, protection enforcement, and copy-on-write fork isolation.
- [x] `done` Add `mmap`/`munmap`/`mprotect` support with MAP_FIXED, page-alignment validation, and smoke coverage.

## M3: Scheduling

- [x] `done` Add initial kernel threads.
- [x] `done` Add cooperative round-robin scheduler.
- [x] `done` Add x86_64 kernel context switching.
- [x] `done` Add blocking and wakeup queues.
- [x] `done` Add preemptive scheduling from timer ticks.
- [x] `done` Add task sleep/yield APIs backed by timer ticks.
- [x] `done` Add task priorities.
- [x] `done` Add zombie lifecycle and parent wait bookkeeping.
- [x] `done` Add process groups and session metadata (`setsid`, `getpgrp`, `setpgrp` with POSIX session checks; foreground job ownership fully implemented).
- [x] `done` Add scheduling/session/job-control semantics for the core process-group and foreground-terminal paths.

## M4: Userspace

- [x] `done` Add user address-space objects.
- [x] `done` Add syscall dispatcher ABI.
- [x] `done` Add initramfs.
- [x] `done` Run `/bin/init`.
- [x] `done` Add basic shell.
- [x] `done` Add ELF64 loading from VFS with per-process page mapping via VMM, argc/argv/envp/auxv stack metadata, and hardware ring3 entry.
- [x] `done` Add syscall `copyin`/`copyout`/`copyinstr` helpers.
- [x] `done` Add built-in program fallback when ELF loading fails.
- [x] `done` Add hardware-enforced ring3 userspace entry/return for normal programs.
- [x] `done` Add native ELF execution with real user registers and shared page tables.

## M5: VFS and Devices

- [x] `done` Add file descriptors.
- [x] `done` Add VFS.
- [x] `done` Add devfs.
- [x] `done` Add tmpfs.
- [x] `done` Add tarfs/initramfs.
- [x] `done` Add virtio-blk as an early device, later replaced by the real driver.
- [x] `done` Add `/dev/tty` VFS device.
- [x] `done` Add `/dev` integration for terminal-backed stdin/stdout/stderr.
- [x] `done` Add VFS mount table and mountpoint dispatch.
- [x] `done` Add ACL fields and permission metadata in VFS nodes.
- [x] `done` Add symlinks, hard links, and `readlink`.
- [x] `done` Add `lstat()` and symlink-aware `stat` mode reporting.
- [x] `done` Add dot, dot-dot, duplicate-slash, symlink-loop (iterative resolver, 16-hop limit, ELOOP), mount-crossing via `..`, and POSIX errno normalization at syscall/VFS boundaries.
- [x] `done` Add POSIX-style open-file description mode checks for read-only, write-only, append, exclusive create, and directory-only opens.
- [x] `done` Enforce existing parent directories for create/mkdir and normalize chmod/chown paths.
- [x] `done` Add full permissions and mount-aware path normalization.

## M6: Network

- [x] `done` Add virtio-net probe/demo device, later replaced by the real driver.
- [x] `done` Add Ethernet frame parsing.
- [x] `done` Add ARP.
- [x] `done` Add IPv4.
- [x] `done` Add ICMP echo.
- [x] `done` Add UDP.
- [x] `done` Add DHCP client.
- [x] `done` Add DNS client.
- [x] `done` Add socket-driven TCP/UDP server behavior.

## M7: Graphics

- [x] `done` Add boot framebuffer path.
- [x] `done` Add graphical console.
- [x] `done` Add input.
- [x] `done` Add basic compositor.
- [x] `done` Add real VirtIO GPU mode-setting/rendering path (legacy + modern PCI transport, dirty-rect transfer/flush, cursor queue path with fallback).
- [x] `done` Add compositor optimizations: double-buffering, dirty-rect coalescing, adaptive full-redraw threshold, and event-driven wakeups.
- [x] `done` Remove framebuffer MMIO readback from console scrolling via RAM shadow buffer.

## M8: Advanced VFS and Filesystems

- [x] `done` Add hierarchical directory structure (`/bin`, `/dev`, `/etc`, `/home`, `/tmp`, `/var`).
- [x] `done` Add FAT32 or ext2 filesystem driver.
- [x] `done` Add initramfs fallback tree.
- [x] `done` Add FAT32 read/import path with limited feature support.
- [x] `done` Add ext1 read/write support (inode alloc, block alloc, single/double indirect, symlinks, VFS population; missing timestamps and fsck-friendly metadata).
- [x] `done` Add ext2 read/write support (bitmaps, sparse writes, and single/double-indirect blocks).
- [x] `done` Add ext3 read/write driver with JBD journaling (inode/block alloc, dir entries, journal mount/recover, rename, unlink, rmdir).
- [x] `done` Add ext4 read/write driver with extent tree, 64-bit BGD, flex_bg, JBD journaling, rename, unlink, rmdir.
- [x] `done` Add ext2 timestamps, durable directory updates, reboot persistence tests, and fsck-friendly metadata.
- [x] `done` Harden JBD recovery semantics: replay only committed descriptor transactions and clear `RECOVER` incompat bit after successful replay.
- [x] `done` Add ext3 metadata ordering hardening for `unlink`/`rmdir`/`rename` path with journal transaction grouping and inode timestamp updates.
- [x] `done` Formalize VFS node refcounting rules: enforce that all functions returning a VFS node MUST increment its refcount, and callers are strictly responsible for decref to prevent memory leaks during lookup paths.
- [x] `done` Implement a Unified Page Cache bridging VM pages and VFS file operations (enabling shared coherent memory-mapped files via `MAP_SHARED`).
- [x] `done` Add a dedicated `icache` (inode cache) to cache file system specific inodes and optimize `dcache` size/lookup performance.
- [x] `done` Transition VFS synchronization from global spinlocks to fine-grained per-directory lock hierarchies to prepare for SMP scaling (parent directory inode write-lock held across create/unlink/mkdir/rmdir/rename; per-inode rw_lock with reader/writer semantics for file data).
- [x] `done` Replace the global fixed-size descriptor table (`MAX_VFS_HANDLES`) with per-process dynamic descriptor tables to improve resource limits (slab-allocated refcounted `vfs_handle *`, per-process growth to 1024 FDs).
- [x] `done` Support asynchronous I/O interfaces (AIO / completion queues) at the VFS layer for non-blocking file read/write operations via io_setup/io_submit/io_getevents, per-task completion queues, a lazy kernel worker, VFS handle retention, and bounce-buffered read/write smoke coverage.

## M9: Hardware Drivers

- [x] `done` Add virtio-blk real block read/write implementation.
- [x] `done` Add virtio-net real PCI/VirtIO initialization path.
- [x] `done` Connect PS/2 keyboard input to shell and TTY paths.
- [x] `done` Add PCI device enumeration with full bus/slot/function scanning.
- [x] `done` Add block-device abstraction and cache (256-entry LRU, write-back, dirty flush on eviction).
- [x] `done` Add MBR/GPT partition discovery in the block-device layer.
- [x] `done` Add AHCI driver support.
- [x] `done` Add NVMe driver support.
- [x] `done` Add VirtIO GPU driver (legacy + modern transport, controlq/cursorq setup, scanout and present path).

## M10: Full Network Stack

- [x] `done` Ethernet frame parsing and sending.
- [x] `done` ARP resolution.
- [x] `done` IPv4 routing.
- [x] `done` ICMP ping responses.
- [x] `done` UDP protocol.
- [x] `done` DHCP client.
- [x] `done` DNS client.
- [x] `done` Add socket ABI integration for UDP/TCP-style descriptors, including POSIX-style error returns and non-blocking connect (`EINPROGRESS`/`EALREADY`) behavior.
- [x] `partial` Add minimal TCP client path for terminal tools.
- [x] `done` Add `listen`, `accept`, TCP lifecycle, socket options, and `select`/`poll` integration.
- [x] `done` Harden network buffer ownership: TX buffer pool is pre-allocated at device init; `net_send_ethernet()` uses pool buffers instead of calling `pmm_alloc_frame()` in the data path; completed buffers are returned to the pool in `net_poll()`, fully decoupling packet buffer allocation from the send interrupt path.

## M11: Shell and Utilities

- [x] `done` Shell built-in commands: `ps`, `mem`, `ping`, `reboot`.
- [x] `done` Pipes and redirection (real `pipe()`/`dup2()`, `<`, `>`, `>>`, `2>`, `2>&1`; deterministic EOF/nonblocking/broken-pipe smoke coverage).
- [x] `done` Environment variables.
- [x] `done` Job control.
- [x] `done` PATH lookup against VFS.
- [x] `done` Descriptor redirection for `<`, `>`, `>>`, `2>`, and descriptor duplication.
- [x] `done` Pipeline execution through real `pipe()` and `dup2()`.
- [x] `done` Add `selfhost` status command.
- [x] `done` Add full background-job tracking and POSIX terminal job control.

## M12: Syscalls and Process Management

- [x] `done` Task priorities in scheduler.
- [x] `done` `exit` syscall.
- [x] `done` `exec` syscall.
- [x] `done` `wait` syscall.
- [x] `done` `mmap` syscall.
- [x] `done` `sleep` syscall.
- [x] `done` `kill`, `signal`, and `getpid`.
- [x] `done` UID/GID syscalls.
- [x] `done` `getcwd`, `uname`, `time`, and `dmesg` syscalls.
- [x] `done` `fork`, `execve`, and `waitpid` syscalls.
- [x] `done` `brk`, `munmap`, `ioctl`, and termios syscalls.
- [x] `done` `setsid`, `getpgrp`, `setpgrp`, `setpriority`, `getpriority` syscalls.
- [x] `done` `statfs`, `fstatfs`, `fchmod`, `fchown`, `umask`, `syncfs`, and `link` syscalls.
- [x] `done` Add POSIX signal ABI: kernel-side `sigaction` table with pending/block masks, userspace restorer trampoline, validated `sigreturn` frame restore, and `sigprocmask` semantics with smoke coverage.

## M13: Userspace ABI, libc, and POSIX Runtime Hardening

- [x] `done` Add dedicated `/bin/m13-smoke` runtime hardening program and deterministic `M13-SMOKE:*` boot-log markers.
- [x] `done` Verify userspace process ABI baseline: `argc`, `argv[0]`, initial stack alignment sanity, and multi-argument native-ELF `execve` argv/envp semantics.
- [x] `done` Verify libc/syscall wrapper baseline in smoke paths: `read`, `write`, `open`, `close`, `lseek`, `fork`, `execve`, `waitpid`, `getpid`, `getuid`, `getgid`, `brk`, `mmap`, `munmap`.
- [x] `done` Verify stdio/basic libc baseline used by userland programs: `printf`, `snprintf`, `puts`, and file stdio lifecycle (`fopen`/`fwrite`/`fread`/`fclose`).
- [x] `done` Verify exec/fd runtime behavior through real exec boundaries: deterministic failed-`execve` status, parent integrity, fd inheritance, `dup2`, and close-on-exec behavior.
- [x] `done` Verify shell/userland ABI integration baseline: `/bin/sh -c` argv and status semantics.
- [x] `done` Complete full POSIX userspace signal baseline: `sigaction` handler delivery, mask/unmask via `sigprocmask`, robust `sigreturn` path, and red-zone-safe signal frame layout.
- [x] `done` Enforce strict 16-byte stack alignment validation at Ring 3 entry using explicit architectural assertions; reject non-compliant frames immediately.
- [x] `done` Audit libc syscall wrappers so unknown or negative kernel return values are never leaked directly, mapping them to strict POSIX `errno` values.
- [x] `done` Implement POSIX-compliant TTY Background Input/Output enforcement via `SIGTTIN` and `SIGTTOU` tracking.

## M14: Advanced Storage, Swap & File Systems

- [x] `done` Block device abstraction layer and caching.
- [x] `done` SATA/AHCI driver support.
- [x] `done` NVMe driver support.
- [x] `done` Ext2 filesystem driver with read/write support.
- [x] `done` Ext1 full read/write driver (inode/block alloc, indirect blocks, symlinks; missing timestamps and fsck metadata).
- [x] `done` Page swapping support.
- [x] `done` Demand paging optimization and OOM fallback hooks.
- [x] `done` Journaling abstraction for VFS.
- [x] `done` Advanced file locking (`flock`/`fcntl`).
- [x] `done` `sync()` and `fsync()` flush block cache paths.
- [x] `done` Add write-back dirty block-cache behavior with explicit flush on eviction, `sync()`, and `fsync()`.
- [x] `done` Persistent ext2 root-image creation and boot overlay.
- [x] `done` Add complete AHCI/NVMe baseline storage paths.
- [x] `done` Enforce strict block cache locking topology: block cache metadata is spinlock-protected and VFS inode lock acquisition now asserts/panics if attempted while the block cache lock is held, preventing layer-inversion deadlocks.

## M15: IPC, Security & Standard OS Features

- [x] `done` Process signals (`SIGINT`, `SIGKILL`, `SIGSEGV`, etc.): default actions, `SIG_IGN`, userspace handlers, mask semantics, and `sigreturn` context restoration are implemented and smoke-verified.
- [x] `done` Inter-process communication message queue baseline (`mq_open/send/receive/close/unlink`) with smoke verification.
- [x] `done` POSIX-style shared memory (`shmget`, `shmat`, `shmdt`, `shmctl`) baseline with smoke verification.
- [x] `done` User and group ID management (`uid`, `euid`, `gid`, `egid`, `setuid`, `setgid`, `su`) baseline.
- [x] `done` File permissions/capabilities metadata plus smoke-verified non-root access denial baseline.
- [x] `done` Standard C library profile for B1NIX userspace.
- [x] `done` BusyBox-style standard utilities (`su`, `ls -l`, `chown`, `chmod`).
- [x] `done` Add UNIX domain sockets.
- [x] `done` Enforce permissions/capabilities consistently through VFS and process credentials.
- [x] `done` Complete userspace signal semantics baseline (`sigaction` masks, nested-delivery-safe frame setup, robust `sigreturn` ABI behavior) and remove libc stubs for `sigprocmask`.
- [x] `done` Harden IPC interfaces (`mq_*`, `shm*`): enforce strict user-space pointer verification using `copyin`/`copyout` wrappers; direct raw pointer dereferencing inside the kernel is strictly prohibited.

## M16: User Space Applications & TUI

- [x] `done` Mini File Manager (TUI-based, Midnight Commander style).
- [x] `done` File tracking and build automation utility (`make` clone).
- [x] `done` Text editor (`vi`/`nano`-style clone).
- [x] `done` Shared TUI input/rendering helpers.
- [x] `done` Deterministic smoke coverage for the file explorer and editor through the normal boot path.
- [x] `done` Real hotkey dispatch coverage, including shared key decoding and terminal raw-mode restore.
- [x] `done` File manager copy/move clipboard actions: F5 copies selected file/dir to the other panel's directory; F6 moves via rename (or copy+delete cross-filesystem); F7 creates a directory; F8 deletes the selected file/dir.
- [x] `partial` Richer GUI/compositor-backed app surfaces and a longer-lived event loop remain deferred.
- [x] `done` Add richer editor persistence/workflow tests: smoke test creates a file, saves, reloads, and verifies content integrity.

## M17: POSIX Syscall Compliance & Self-Hosting

- [x] `done` POSIX Process Management: `fork()`, `execve()`, and `waitpid()` initial ABI.
- [x] `done` POSIX File I/O: `stat()`, `lseek()`, `unlink()`, `mkdir()`, `rmdir()`, `rename()`, `symlink()`, `readlink()`, `chdir()`, `getdents()`, `statfs()`, `fsync()`, `sync()`, `fcntl()`, `chmod()`, `chown()`, `umask()`.
- [x] `done` POSIX Pipes & FDs: `pipe()`, `dup2()`, `fcntl()`.
- [x] `done` POSIX Memory: user-space `mmap()`, `munmap()`, `brk()` initial heap ABI.
- [x] `done` POSIX Sockets: `socket()`, `bind()`, `connect()`, `send()`, `recv()`, `listen()`, `accept()` FD ABI.
- [x] `done` POSIX Terminal: `ioctl()`, `termios`, and `poll()` support.
- [x] `done` Add syscall ABI constants and userspace syscall header mirrors.
- [x] `done` Add `docs/abi.md` for the userspace ELF ABI and calling convention.
- [x] `done` Add `SYS_SELFHOST_STATUS` and `/bin/selfhost` status reporting.
- [ ] `planned` Cross-compile and port GCC specifically for `x86_64-b1nix`.
- [ ] `planned` Port GNU Binutils (`as`, `ld`, `objcopy`, `ar`) and GNU Make.
- [ ] `planned` Achieve self-hosting: compile the B1NIX kernel inside B1NIX using ported GCC.
- [x] `done` Restructure syscall layers with formalized refcount tracking: documented REFCOUNT RULES, atomic refcount on all VFS nodes/inodes, vfs_handle_retain/close lifecycle, and per-inode read-write locks.
- [x] `done` Formalize expected `errno` matrices for failed file operations, explicitly validating ELOOP (symlink depth), ENAMETOOLONG (path component limit), ENOTDIR (file-as-dir), EISDIR (write to dir), EROFS, and errno isolation across syscalls via /bin/m17-smoke smoke coverage.

## M18: Real Userspace and ELF Loader

- [x] `done` Load ELF64 executables from VFS with per-process page mapping via VMM (PT_LOAD segments mapped page-by-page into user address space).
- [x] `done` Build a user address-space record per process.
- [x] `done` Add syscall `copyin`/`copyout`/`copyinstr` helpers for user pointers.
- [x] `done` Create a user stack with `argc`, `argv`, `envp`, and auxiliary vector basics (AT_NULL, AT_ENTRY, AT_PHDR present).
- [x] `done` Implement `execve()` as image replacement with `vfs_close_on_exec()` and full ELF segment loading.
- [x] `done` Add process exit status propagation and zombie reaping semantics.
- [x] `done` Add QEMU tests that boot, launch `/bin/init`, and execute a VFS-loaded program.
- [x] `done` Add external clang-backed `b1nix-cc` wrapper for early ELF builds.
- [x] `done` Add hardware-enforced ring3 entry and return.
- [x] `done` Implement strict boundary verification for ELF `PT_LOAD` segments to guarantee that malformed binaries can never corrupt kernel memory limits.
- [x] `done` Enforce that all string operations during environment and stack construction utilize bounded `copyinstr` tracking with exact destination limit assertions.

## M19: Process Model and FD Tables

- [x] `done` Implement `fork()` with copied process metadata, FD view, COW-backed address-space isolation, callee-saved register save/restore, and assembly trampolines for proper child return.
- [x] `done` Add per-process file descriptor tables instead of a single global FD table.
- [x] `done` Inherit and close FDs according to POSIX rules, including close-on-exec.
- [x] `done` Make `stdin`, `stdout`, and `stderr` real descriptors `0`, `1`, and `2`.
- [x] `done` Store per-process cwd, environment, umask, process group, and session metadata.
- [x] `done` Implement `waitpid()` options (WNOHANG, WUNTRACED) and zombie lifecycle.
- [x] `done` Add process groups and terminal foreground job ownership for the core controlling-terminal paths.
- [x] `done` Add refcounted VFS handles/open-file descriptions.
- [x] `done` Add MMU-aware fork with copied metadata/FD state and COW-backed address-space isolation.
- [x] `done` Add exact POSIX child/parent register-return semantics: child RAX=0, parent returns PID, callee-saved registers preserved, assembly trampolines for both user and kernel fork paths.
- [x] `partial` Enforce strict `O_CLOEXEC` validation to prevent descriptor leaks across exec boundaries; FD table locking formalization deferred until multi-thread support is active.

## M20: Terminal, TTY, and Interactive Shell

- [x] `done` Add a real TTY device with line discipline.
- [x] `done` Support canonical and raw terminal modes through `termios`.
- [x] `done` Handle Ctrl-C, Ctrl-D, Ctrl-Z, backspace, arrows, and EOF behavior.
- [x] `done` Route keyboard input through `/dev/tty` and FD `0`.
- [x] `done` Replace temporary-file shell pipes with real `pipe()` and `dup2()` wiring.
- [x] `done` Add shell redirection `<`, `>`, `>>`, `2>`, and descriptor duplication.
- [x] `done` Implement `PATH` command lookup against the VFS.
- [x] `done` Improve shell errors and exit statuses.
- [x] `done` Route terminal control characters into the signal/process metadata path.
- [x] `done` Add controlling-terminal/process-group signal behavior (SIGTTIN/SIGTTOU enforcement with mask/handler awareness, ERESTARTSYS restart semantics).

## M21: Persistent Root Filesystem

- [x] `done` Boot with a persistent root filesystem from a disk image.
- [x] `done` Add mountpoints and a real `mount`/`umount` VFS model.
- [x] `done` Stabilize writable ext2 as the first reliable root filesystem target.
- [x] `done` Add `rename()`, `rmdir()`, `fstat()`, `fsync()`, and open flags (`O_CREAT`, `O_TRUNC`, `O_APPEND`, `O_DIRECTORY`, `O_EXCL`).
- [x] `done` Add safer VFS unlink/rmdir split, directory rename self-move prevention, and same-tree destination replacement.
- [x] `done` Flush block cache on shutdown and reboot: `vfs_sync()` called in `SYS_REBOOT` handler before `arch_halt()`; `blk_sync_all()` flushes dirty block-cache entries.
- [x] `done` Add `/etc`, `/bin`, `/dev`, `/home`, `/tmp`, and `/var` layout.
- [x] `done` Add an image creation/install script (`tools/create-rootfs.sh`) for local development.
- [x] `done` Add `make root-image` and `make run-root` workflows in the top-level Makefile.
- [x] `done` Overlay attached ext2 root over initramfs fallback files.
- [x] `done` Add mount listing for active VFS mount table entries.
- [x] `done` Add Btrfs probing/listing metadata without treating Btrfs as a usable POSIX filesystem.
- [x] `done` Add mount option handling baseline: `MS_RDONLY` enforced in `vfs_write()`, `vfs_mkdir()`, `vfs_unlink()`, `vfs_rmdir()`, `vfs_rename()`; mount flags stored in mount entry and passed to filesystem callbacks.

## M22: Core Terminal Utilities

- [x] `done` Add pwd, ls, cp, mv, rm, mkdir, rmdir, chmod, chown, and ln.
- [x] `done` Add ps, kill, sleep, date, uname, id, and whoami.
- [x] `done` Add text tools: cat, head, tail, grep, find, wc, sort, and uniq.
- [x] `done` Add filesystem tools: mount, df, sync, and hexdump.
- [x] `done` Keep BusyBox-style multi-call dispatch for small binaries.
- [x] `done` Add clear, mem, dmesg, ifconfig, ping, nc, and wget utilities.
- [x] `done` Implement ln through VFS hard-link aliases and ln -s symlinks.
- [x] `done` Add readlink utility and init-path symlink smoke coverage.
- [x] `done` Implement mount command mounting path and active mount listing.
- [x] `done` Add init-path utility smoke tests through /bin/m22-smoke and QEMU serial checks.
- [x] `done` Add interactive shell-driven utility smoke tests that execute through /bin/sh.
- [x] `done` Add option-compatible behavior for common utility flags (ls -la, cp -r, rm -rf, mkdir -p, grep -q, grep -n, head -n NUM, tail -n NUM).
- [x] `done` Restore all utility dispatch targets used by M22 smoke after the BusyBox table cleanup.

## M23: Networking for Terminal Use

- [x] `done` Turn the socket ABI into UDP-capable socket descriptors.
- [x] `partial` Add minimal TCP client support.
- [x] `initial` Add DNS resolver integration through libc-style calls and shell commands.
- [x] `done` Add `ifconfig`-style interface status.
- [x] `done` Add `ping`, `nc`, and a tiny `wget`/HTTP client.
- [x] `done` Handle missing network devices gracefully in user-facing network paths.
- [x] `done` Keep non-x86 network portability concerns out of the active terminal networking target.
- [x] `done` Add TCP server support: `listen`, `accept`, close states, retransmission, and timeout handling.
- [x] `done` Add socket options and `select`/`poll` readiness.

## M24: Reliability and Diagnostics

- [x] `done` Add syscall argument validation and error returns.
- [x] `done` Add kernel backtraces or symbolized panic locations: `arch_backtrace()` in `kernel/arch/x86/interrupts.c` implements RBP-based frame pointer unwinding (up to 32 frames), stack scanning fallback, and kernel text range validation; `panic_backtrace()` in `kernel/lib/klog.c` wraps it with symbol lookup via binary search.
- [x] `done` Replace avoidable panics with recoverable errors: only 2 lock-ordering safety panics remain in VFS (infrastructure assertions), zero panics in network paths.
- [x] `done` Add regression/smoke tests for VFS, scheduler, pipes, terminal, and sockets: comprehensive boot-log markers across M12-M16, M22, M24, M11 for all subsystems.
- [x] `done` Add QEMU smoke tests for the active x86_64 target.
- [x] `done` Track implemented, initial, stub, and planned features explicitly in docs.
- [x] `done` Add kernel log levels and a ring buffer readable from userspace.
- [x] `done` Add `/bin/dmesg` backed by `SYS_DMESG`.
- [x] `done` Add scheduler stress coverage for repeated `spawn`/`wait` short-lived utility tasks.
- [x] `done` Add shell-driven utility smoke coverage.
- [x] `done` Add CI-grade interactive shell utility tests.
- [x] `done` Make syscall errors consistently map to userspace `errno` for the covered libc wrappers and smoke paths.
- [x] `done` Eliminate non-critical kernel panics from VFS lookup and network packet ingestion paths: zero panics in net/ layer, only 2 lock-ordering safety assertions remain in VFS.
- [x] `done` Integrate automated static analysis checks into the top-level Makefile: `make analyze` target runs clang `--analyze` on all kernel sources with plist output.
## M24b: Symmetric Multiprocessing (SMP) & Multithreading

- [x] `done` Boot Application Processors (APs) via INIT-SIPI-SIPI sequence.
- [x] `done` Configure Local APIC timer, MMIO maps, and per-CPU data areas (via GS segment).
- [x] `done` Implement spinlock primitives for kernel-wide synchronization and thread safety.
- [x] `done` Activate per-CPU scheduler runqueues and idle task loops on all online processors.
- [ ] `planned` Implement cross-CPU task stealing (load balancing) in `sched_steal_task()` to enable task migration across cores.
- [ ] `planned` Formalize file descriptor table locking to ensure thread-safe FD allocation under multi-threaded execution.

## M25: Minimal Native C Toolchain

- [x] `done` Define the B1NIX userspace ELF ABI and calling convention.
- [x] `done` Add `crt0.o` startup code for B1NIX userspace programs.
- [x] `done` Add a userspace linker script for B1NIX ELF binaries.
- [x] `partial` Build a minimal libc profile with syscall wrappers, `string`, `stdio`, `stdlib`, and improved `malloc`; `qsort` (O(N log N) quicksort), `strtol`, and `strtod` are now complete; several POSIX-facing APIs are still stubs/incomplete.
- [x] `done` Add an external `b1nix-cc` wrapper backed by clang for early userland builds.
- [x] `done` Build and run a VFS-loaded native smoke ELF program.
- [x] `done` Build and run a VFS-loaded `hello.c` ELF program.
- [x] `done` Import TinyCC/TCC source and wire an early `/bin/tcc` build target.
- [x] `done` Prepare installed userspace headers, `libb1nix.a`, and `crt0.o` in the rootfs image.
- [x] `done` Compile and run `hello.c` inside B1NIX using `/bin/tcc`.
- [x] `done` Compile and run a simple utility inside B1NIX, including argv/output verification.
- [x] `done` Smoke-test native TCC stderr handling and non-zero exit status propagation.
- [x] `done` Document the current external cross-build to in-guest compilation boundary in `docs/abi.md`.
- [x] `done` Align libc POSIX-facing APIs and signatures: implemented the `strtod` family (`strtod`, `strtof`, `strtold`) and `ldexp`/`ldexpl` scaling routines, resolving the floating-point parsing stubs for the native compiler toolchain.
- [x] `done` Align remaining libc POSIX-facing APIs: signal set helpers (`sigfillset`, `sigdelset`, `sigismember`) implemented with EINVAL validation; `dlopen`/`dlsym`/`dlclose`/`dlerror` stubs hardened to follow POSIX error-reporting contract (error buffer, consume-and-clear semantics, RTLD_DEFAULT sentinel handle); `dlfcn.h` expanded with full RTLD_* constants including `RTLD_LOCAL`, `RTLD_NOLOAD`, `RTLD_NEXT`.
- [x] `done` Harden the internal kernel `malloc`/`free` heap implementations: magic number validation (KHEAP_MAGIC/KHEAP_FREED_MAGIC) and header canary (KHEAP_CANARY) validated on alloc and free; `kheap_validate()` walks all blocks checking canary integrity.

## M26: Full Toolchain and Self-Hosting

- [x] `done` Define the `x86_64-b1nix` target ABI document in `docs/abi.md`.
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
