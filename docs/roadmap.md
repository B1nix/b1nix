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

- Overall practical POSIX compatibility: roughly 75-82%.
- `VFS/path/files`: roughly 90-95%.
- `Shell/coreutils`: roughly 90-94% (M33 closed the shell-grammar gap:
  globbing, `$((…))`, here-docs, `$(…)`/backticks, subshells, functions,
  `case`, arrays, full job control, `for`/`while`/`until` loops, `${x:-y}`-style
  parameter expansion, and bare `VAR=value` assignment).

These percentages mean "can run small real workflows", not "passes a POSIX
conformance suite". The biggest remaining shell gaps are substring/pattern
parameter expansion (`${x:off:len}`, `${x%pat}`) and the env-prefix command
form (`VAR=x cmd`). Pipeline execution is now concurrent (forked producer
stages, see M33), so large-data pipes no longer risk the old full-pipe
deadlock. Broader remaining blockers are permission edge cases.

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
- [x] `done` Add minimal TCP client path for terminal tools. Active-open
  handshake (`tcp_connect`), data send/recv, and close lifecycle are
  implemented and the connect/listen/accept/send/recv path is smoke-verified
  end-to-end (`TCP-SMOKE: path-exercised`, `kernel/user/programs.c`).
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
- [x] `done` Cross-compile and port GCC specifically for `x86_64-b1nix`.
- [x] `done` Port GNU Binutils (`as`, `ld`, `objcopy`, `ar`).
- [x] `done` Achieve self-hosting: compile the B1NIX kernel inside B1NIX using ported GCC (all 76 TUs compiled + linked in-guest into a real `kernel.elf`). **Fully closed 2026-05-29: the self-built kernel boots AND passes the entire smoke suite (218/0).** The "residual codegen-class crash in the M11 coreutils path" was NOT GCC codegen — it was a 16 KB kernel-stack overflow (busybox builtins run in-kernel on the per-task stack, a kheap block; `uniq_main`'s ~12 KB frame overran it and corrupted the adjacent node). Fixed by `KERNEL_STACK_SIZE` 16→32 KB; see [`docs/m26-selfhost.md`](m26-selfhost.md) UPDATE (o)/(p).
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
- [x] `done` Enforce strict `O_CLOEXEC` validation to prevent descriptor leaks across exec boundaries. The deferred FD-table locking is now in place: each task carries a `spinlock_t fd_lock` held by every fd-table mutator (M24b), and multi-thread support is live (M29 `clone()`/pthreads), so the formalization that was waiting on it is complete.

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
- [x] `done` Add minimal TCP client support (see M10 — handshake/send/recv/close, smoke-verified).
- [x] `done` Add DNS resolver integration through libc-style calls and shell commands.
  Kernel DNS client is now synchronous (`dns_resolve_sync`, `kernel/net/dns.c`)
  with A-record capture, parses `/etc/resolv.conf` for the nameserver, and is
  exposed via `SYS_NET_DNS(host, &ip4)`. libc gains `gethostbyname`,
  `getaddrinfo`/`freeaddrinfo`/`gai_strerror`, and `inet_pton`/`inet_ntop`/
  `inet_aton`/`inet_ntoa`/`htons` family (`userspace/libc/netdb.c`, `netdb.h`,
  `arpa/inet.h`); `ping`/`nc`/`wget` resolve hostnames (numeric fast-path, DNS
  fallback). Smoke-verified offline: deterministic A-record parse + resolv.conf
  (`DNS-SMOKE: ok parse-a-record`/`resolv-conf`) and libc numeric paths
  (`M32-NET: ok inet-pton-ntop`/`gethostbyname-numeric`/`getaddrinfo`). Live
  name resolution depends on a reachable nameserver (QEMU user-mode resolver).
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

- [x] `done` Boot Application Processors (APs) via INIT-SIPI-SIPI sequence. **The trampoline path was never actually exercised before (the AP idle loop used a no-op self context-switch), and hid two fatal bugs now fixed: (1) the SIPI start-up vector was `0x80` (start at `0x80000`) while the trampoline is copied to physical `0x8000` — the vector must be `0x08` (`0x8000 >> 12`); (2) the trampoline GDT had only a 64-bit code segment (`L=1, D=0`) used for the 32-bit protected-mode phase, so `ap_32` was decoded as 16-bit and faulted — added a proper 32-bit code/data pair (`0x08`/`0x10`) and use the 64-bit segment (`0x18`) only for long mode. APs also now load the *active* kernel PML4 (from `CR3`), not the boot `pml4` (which lacks the kheap where AP kernel stacks live). CPU count is read from CPUID leaf `0x0B` by iterating subleaves (highest level's logical-processor count), with a CPUID.1 fallback. Verified in-guest at `-smp 4`: 3 APs reach `ap_main`.**
- [x] `done` Configure Local APIC timer, MMIO maps, and per-CPU data areas (via GS segment). Also fixed `lapic_send_ipi` to write the destination APIC ID into ICR_HIGH bits `[31:24]` (`apic_id << 24`, xAPIC layout) instead of the x2APIC `<< 32` form, which truncated through the `u32` register to 0 and sent every IPI to the BSP (an INIT-to-self triple-faulted the boot CPU).
- [x] `done` Implement spinlock primitives for kernel-wide synchronization and thread safety.
- [x] `done` Activate per-CPU scheduler runqueues and idle task loops on all online processors.
- [x] `done` Implement cross-CPU task stealing (load balancing) in `sched_steal_task()` to enable task migration across cores. `sched_steal_task` migrates only tasks explicitly marked `stealable` (self-contained CPU-bound kernel workers); ordinary userspace tasks are never stolen because the kernel's syscall/VFS paths are not yet SMP-safe for parallel kernel-mode execution. An idle AP (`ap_main`, `kernel/arch/x86/lapic.c`) genuinely context-switches into a stolen worker via its own per-CPU idle context and parks back to it on completion (`ap_worker_trampoline`), with the worker reaped on the AP afterward. **Verified at `-smp 4`: 12 workers created on the BSP, all stolen and executed on non-BSP cores (`M24B-SMP: completed=12 migrated=12 ok work-stealing`); see `kernel/sched/smp_test.c` / `smoke_run/qrun-smp.sh`.**
- [x] `done` Formalize file descriptor table locking to ensure thread-safe FD allocation under multi-threaded execution. Each task carries a `spinlock_t fd_lock` (`struct task`) and every fd-table mutator (`scheduler_fd_alloc`/`_set`/`_close`/`_flags_set`) holds it. b1nix's model is strictly per-task — `fork`/`kthread_create` allocate fresh fd arrays (handles refcounted, arrays never shared) — so a per-task lock fully serialises concurrent access; plain (non-IRQ-saving) `spin_lock` is correct since fd tables are only touched from task/syscall context, never ISRs.
- [x] `done` Make `tasks[]` slot allocation/free SMP-safe with a leaf `g_tasks_lock` (`kernel/sched/scheduler.c`): `find_unused_task` claims+zeroes+ids a slot atomically, every free goes through `free_task_slot`, and `next_task_id` is assigned only under the lock. Previously the table relied on `interrupts_disable()`, which only fences the local CPU.
- [x] `done` Make `current_task` per-CPU (`#define current_task (get_percpu()->cur_task)` over the `cur_task` slot in `struct percpu`; commit `ace00c4`) — the prerequisite for running tasks on more than one core. `syscall_entry.S` reads it as `%gs:0x10`; the kernel keeps its per-CPU base in GS with no SWAPGS, so `user_jump.S` must not reload `%gs` when entering ring 3. Smoke stays 222/0 (single-CPU userspace + `-smp 4`).
- [x] `done` Run ordinary userspace processes on Application Processors via a **Big Kernel Lock** (recursive, serialises kernel-mode execution across cores; userspace runs lock-free in parallel). APs get per-CPU TSS + their own arch init and run the cooperative scheduler off a shared global runqueue; only ELF userspace migrates to APs. Verified at `-smp 4`: userspace ran on all 4 cores (`M24B-BKL: ok userspace-on-ap`, 180 ok / 0 fail); single-CPU unchanged (178 ok / 0 fail). **Details: [docs/m24b-bkl.md](m24b-bkl.md).**
- [x] `done` Preemptive scheduling — **delivered in M28**: each core ticks itself off the LAPIC timer at 100 Hz and `scheduler_on_timer_tick` now calls `scheduler_yield()` (the long-deferred "re-enable once VFS locking is audited" step), after the BKL/VFS paths were made preemption-safe. See M28-A / M28 #8.
- [x] `done` POSIX threads — **delivered in M29**: `clone()` with `CLONE_VM`/`CLONE_FILES`/`CLONE_THREAD`/`CLONE_SETTLS`/…, `SYS_futex`, `%fs`-based TLS, and a full in-libc `libpthread` (mutex/condvar/join/detach). See M29.

## M25: Minimal Native C Toolchain

- [x] `done` Define the B1NIX userspace ELF ABI and calling convention.
- [x] `done` Add `crt0.o` startup code for B1NIX userspace programs.
- [x] `done` Add a userspace linker script for B1NIX ELF binaries.
- [x] `done` Build a minimal libc profile with syscall wrappers, `string`, `stdio`, `stdlib`, and improved `malloc`; `qsort` (O(N log N) quicksort), `strtol`, and `strtod` are complete. The previously-stubbed POSIX-facing APIs are now real: a full `scanf`/`fscanf`/`sscanf`/`vscanf`/`vfscanf`/`vsscanf` format engine (`%d %i %u %o %x %c %s %f %p %n %%`, width/suppress/length modifiers), `frexp`, `tmpfile`, a real `fchmod` (SYS_FCHMOD), `utime` (new `SYS_UTIME` → `vfs_utime`), and `gettimeofday` with sub-second precision via `clock_gettime`. Smoke-verified (`M25-SMOKE: ok scanf`/`frexp`/`fileops`). (`alarm` remains a no-op stub — it needs a per-process SIGALRM timer, deferred.)
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
- [x] `done` Port Binutils (`as`, `ld`, `objcopy`, `ar`) for `x86_64-b1nix`.
- [x] `done` Port GCC after the minimal C toolchain and filesystem are stable.
- [x] `done` Build larger user programs with the external cross toolchain.
- [x] `done` Build a target `libstdc++` for `x86_64-b1nix` (prerequisite for the native GCC port).
- [x] `done` Run the native GCC inside B1NIX to compile C to an object file (`cc1` + `as`: `gcc -c` works in-guest). See [`docs/m26-selfhost.md`](m26-selfhost.md).
- [x] `done` Build the B1NIX kernel with the ported GCC on the host (`make TOOLCHAIN=gcc`); the GCC-built kernel boots and passes the suite. (The earlier "passes M12…M22" omitted M11 — that gap was the 16 KB kernel-stack overflow below, not a GCC issue; the in-guest GCC-built kernel is now verified 218/0 including M11, and the host build shares the same source.)
- [x] `done` Build the B1NIX kernel inside B1NIX. The in-guest native GCC+ld compile all 76 kernel translation units and link a real `kernel.elf` (verified 2026-05-28: 76 cc1 + 1 ld spawns, 0 errors, 1,577,080-byte ELF; `smoke_run/_fullbuild_8gb4.log`). Required lifting the 4GB direct-map cap to 8GB and clamping the pmm to it so frames stay reachable (`DIRECT_MAP_SIZE`, `kernel/mm/pmm.c`, `kernel/arch/x86/paging.c`). **The self-built (GCC) kernel now boots AND passes the full smoke suite (218/0) — verified 2026-05-29 by building `kernel.elf` in-guest at 256MB from the stack-fixed source (1,582,336-byte ELF), then booting that exact artifact in test mode (`smoke_run/_smoke_selfbuilt.sh`).** The earlier "M11 codegen-class panic" was a misdiagnosis — it was the 16 KB kernel-stack overflow below, which GCC's frame layout exposed before clang did. Full handoff in [`docs/m26-selfhost.md`](m26-selfhost.md) UPDATE (o)/(p).
- [x] `done` Fix the 16 KB kernel-stack overflow that corrupted the kheap (`kernel/sched/scheduler.c`, `KERNEL_STACK_SIZE` 16→32 KB). Per-task kernel stacks are `kmalloc`'d kheap blocks, so an overflow silently overwrites the adjacent heap block (e.g. a `vfs_node`) instead of faulting — it surfaced as a deterministic `#GP` in `find_child` during M11 (and as the self-built/GCC-kernel "M11 panic"). The busybox coreutils run in-kernel on this stack and some have large on-stack buffers (`uniq_main` ~12 KB: `buf[8192]`+`lines[512]`) which, with the ~6.6 KB syscall-dispatch frame + VFS/ext4 chain, exceed 16 KB. Root-caused by ruling out every allocator/VFS theory with instrumentation, then confirmed by the stack bump (smoke 159→218/0). See [`docs/m26-selfhost.md`](m26-selfhost.md) UPDATE (o).
- [x] `done` Add native `make`/assembler/linker workflow usable from the B1NIX shell. In-guest `as`+`ld` work, and **`gcc`-driven linking now works** — `crtbegin.o`/`crtend.o`/`libgcc.a` are staged into the sysroot by `install-native-toolchain` (verified staged in `root.ext4`), and the driver relocates its prefix from `argv[0]` (no `-B`/sysroot hack needed). **UPDATE (2026-05-29): real GNU Make ported; the toy `nmake` dropped.** The in-kernel `nmake` built-in (a ~320-line toy: no variables, pattern rules, or automatic vars) is removed entirely (`kernel/user/nmake.c` + its registration/Makefile/rsp entries gone). In its place, **GNU Make 3.82 is cross-built for `x86_64-b1nix`** by `tools/build-make.sh` (bundled glob/fnmatch → no libc `<glob.h>` needed; b1nix-aware `config.sub` dropped in from the toolchain). It compiles + links cleanly against `libb1nix.a` into a 251 KB static b1nix ELF (entry `0x2000100`), installed to `/persist/bin/make` via `install-native-toolchain`. Porting it required a handful of small libc additions, all verified by host smoke (`M26-SMOKE: ok readdir`, 179 ok): real `opendir`/`readdir`/`closedir` over `SYS_GETDENTS` (were NULL stubs) + `dirfd`, plus `<limits.h>`, `<pwd.h>`+`getpwnam`/`getpwuid`, `<ar.h>`, `L_tmpnam`/`tmpnam`, and `getlogin`. A real Makefile (`tools/inguest/Makefile`: variables, `.c`/`.S` pattern rules, automatic vars, prerequisite tracking) drives the full 75-TU kernel build + link — validated by a host GNU-Make dry-run (69 cc + 6 as + link). **VERIFIED IN-GUEST (2026-05-29, TCG on the macOS host):** the ported `/persist/bin/make` runs inside b1nix — `make --version` prints `GNU Make 3.82 ... Built for x86_64-pc-b1nix`, and `make -C tools/inguest/maketest` drives a real Makefile end-to-end (variables, automatic var `$<`, prereq chaining `all→dep`, recipe spawn via `/bin/sh`, clean exit). Driver: `smoke_run/inguest_make_test.py` (fixture `tools/inguest/maketest/`). This exposed and fixed a real POSIX bug: userspace `wait(&status)`/`waitpid(-1, ...)` never worked (the libc `wait()` hit the in-kernel `SYS_WAIT (pid,status)` ABI, and `scheduler_waitpid` ignored `pid==-1`); now libc `wait()` routes through `SYS_WAITPID(-1,…)`, `scheduler_waitpid` treats `pid==-1` as "any child", and "no children" returns `ECHILD` (not the bogus `EPERM`). Host smoke stays 179 ok. Driving the *full kernel build* via `tools/inguest/Makefile` in-guest is the only nice-to-have left (the flat `tools/inguest/build-kernel.sh` remains the proven full-build driver; the Makefile is host-dry-run-validated to issue the same compiles + link).
- [x] `done` Make pmm single-frame allocation O(1) (`kernel/mm/pmm.c`, commit `90b0b94`). Replaced the O(total_frames) per-alloc bitmap scan with an intrusive LIFO free-list (next-pointer stored in each free frame via the direct map, seeded at the direct-map switch) plus a per-frame on-list bitmap that keeps push idempotent (no double-link cycles); the used-bitmap stays authoritative via a word-skip scan for contiguous/fallback allocs. Removes the per-cc1-instantiate allocation grind at low RAM (2048MB 76-step sequence: ~halt → ~112s). Host smoke 218/0; 8GB self-host unchanged (identical 1,577,080-byte `kernel.elf`).
- [x] `done` Reduce kheap internal fragmentation (`kernel/mm/kheap.c`). **Closed 2026-06-01:** beyond the split-on-reuse below, bidirectional boundary-tag coalescing is implemented and **on by default** (`KHEAP_ENABLE_COALESCE 1`), verified heap-consistent end-to-end under the strict `KHEAP_VALIDATE` walker (zero trips, smoke 314/0). The free-list reuse was first-fit with no splitting, so a small alloc consumed a whole large freed block; in-guest live kheap was ~1.99GB with only ~57MB useful (`page_cache_add_page` held 974MB across 10,953 ~64-byte entries). Splitting the remainder on reuse drops live kheap to ~57MB and advances the 2048MB build from file 27→58. Host smoke 218/0; 8GB unaffected.
- [x] `done` Return large-allocation pages to the pmm (`kernel/mm/kheap.c`). **Closed 2026-06-01:** the large-allocation arena (`klarge_*`) returns every ≥256 KB allocation's pages to the pmm on `kfree` (the item's stated goal), and general-heap tail page-return (`KHEAP_ENABLE_PAGE_RETURN 1`, `KHEAP_SHRINK_MIN` 512 KB) is also on by default so a coalesced top-of-heap tail is unmapped back to the pmm. (Mid-heap small-block page-return is intentionally not done — fragmentation there is handled by coalescing, not unmapping.) Adds a separate page-granular **large-allocation arena** (`klarge_*`): every `kmalloc`/`kzalloc` ≥ `KLARGE_THRESHOLD` (256KB) is mapped from fresh pmm frames into the upper part of the kheap's `PML4[384]` slot and its whole pages are **unmapped + returned to the pmm on `kfree`** (with vaddr-span reuse). This caps the high-water growth from the big transient cc1/as/ld ELF-staging buffers (the ~516KB and ~33MB allocations that drove the in-guest kheap to ~2GB), so it tracks live large-buffer use instead of growing monotonically. The general bump heap below was originally kept identical to the known-good baseline (no coalescing, no boundary tags) — chosen deliberately after a coalescing experiment turned what was thought to be a "pre-existing latent heap UAF" (active during M25/TCC) from silent into fatal (see [`docs/m26-selfhost.md`](m26-selfhost.md) UPDATE l). **UPDATE (2026-05-29): that "UAF" was the 16 KB kernel-stack overflow (since fixed); general-heap coalescing + tail page-return have since been re-enabled — see the heap-corruption item (Step b) below.** Host smoke reaches `B1NIX-TEST: done` (all modules, M25/TCC + M16/mc pass), and `[KLARGE]` traces confirm `free_frames` recovers on every large free.
- [x] `done` Low-RAM (≤2048MB) in-guest self-host **achieved**. With the large-allocation arena above plus the block-DMA-lifetime fixes (see [`docs/m26-selfhost.md`](m26-selfhost.md) UPDATE m) and swap activation, the in-guest GCC+ld build completes at **256MB and 512MB (both pass-verified)** on the Fedora/KVM rig — well within the ≤2048MB target (8GB already passed). The honest floor is now **128MB: fail** — below the practical working set of GCC/cc1, and not relieved by swap (see next item).
- [x] `done` Make swap reclaim actually run under memory pressure (`kheap.c`, `swap.c`, `eviction.c`; commit `bc586ac`). **Closed 2026-06-01:** swap reclaim is proven to run in-guest — `swap_evict_page` fires 1026× at 128 MB (was 0 before the IRQ-state fix) and the full 76-TU self-host build completes at 256 MB. (Residual: general-heap *growth* for small allocations still holds `heap_lock` IRQs-off so it can't itself swap; impact is negligible because the large transient buffers that drive pressure go through the lock-released klarge path.) The pmm OOM path only swaps when `interrupts_enabled()`, but `klarge_alloc`/`heap_grow` allocated under `heap_lock` (IRQs off), so `swap_evict_page` was **never called** (zero `[M26DIAG] swap_evict` lines before a 128MB OOM). Fixed: klarge reserves its vaddr span under the lock then maps frames lock-released at caller IRQ state; `MAX_SWAP_SLOTS` 1024→65536 (was a 4MB cap) clamped to device; `MAX_USER_PAGES` 4096→65536. Verified in-guest: `swap_evict` now fires 1026× at 128MB (was 0); 256MB unaffected — and now completes the **full 76-TU build + link** (verified 2026-05-29, was only "KBUILD 64/76" before), no regression.
- [x] `wontfix` Push the self-host floor below 256MB — **not pursued by decision**. 128MB is below the practical working set of GCC itself: the official GCC docs state **no minimum RAM** (it is workload-dependent), and community guidance puts real builds at ~512MB-with-no-headroom; b1nix at **256MB** already beats that (small kernel TUs at `-O0`). So 128MB failing is GCC's genuine footprint, not a b1nix defect. For the record, the technical levers if ever wanted: with swap reclaim now functional, 128MB still OOMs at `KBUILD 1` but **swap is 98% free** — the limiter is the **non-swappable kernel peak** (general kheap ~32MB + klarge ELF-staging for cc1 + page tables), not swap; a bigger swap would not help. The only way lower is shrinking that peak (stream/segment the ELF load instead of a full-file klarge buffer; reduce kheap residency). See [`docs/m26-selfhost.md`](m26-selfhost.md) UPDATE (n).
- [x] `done` The "pre-existing latent heap UAF surfaced during M25/TCC" — **resolved; it was the 16 KB kernel-stack overflow, not a DMA/heap UAF.** Original theory: a consumer writes into an allocated block's header asynchronously of the owner; the aggressive per-op `kheap_validate` walk panicked `kheap magic/prev_size corrupt` deep in M25 even with coalescing + page-return disabled, and a driver DMA audit suspected AHCI/NVMe completion-wait timeouts (`ahci.c`, `nvme.c`) and `virtio_blk.c` missing read barriers. **Re-verified 2026-05-29 on the 32 KB stack:** a reconstructed full-heap `kheap_validate` (periodic, every-64-ops full bump-region walk) runs **clean through M25/TCC + M16/mc to `B1NIX-TEST: done`, 218/0, zero corruption** — strongly indicating the corruption was the same stack overflow that the M11 `#GP` was (UPDATE o), since the kernel stack is itself a kheap block. **Step (a) DONE (2026-05-29):** the strict per-op validator was **recovered from the local `m26-coalesce-wip` stash (not lost)** into `kernel/mm/kheap.c` behind `#define KHEAP_VALIDATE` (default 0). When enabled it walks the entire general-heap bump region on **every** `kmalloc`/`kfree` (stricter than the periodic every-64-ops pass) and panics on the first bad header. Run on the 32KB stack it stayed **clean to `B1NIX-TEST: done`, 178 ok markers, zero trips** — confirming no residual heap/DMA UAF; the corruption was the stack overflow. **Step (b) DONE (2026-05-29):** general-heap coalescing re-attempted and landed in `kernel/mm/kheap.c`. A boundary-tag `prev_size` was added to the block header (header stride stays 32B) along with `heap.last_block`; `kfree` now **bidirectionally coalesces** a freed block with its physical neighbours when they are also free (`KHEAP_ENABLE_COALESCE`), and **returns a coalesced tail's pages to the pmm** when that block is the topmost one and ≥ `KHEAP_SHRINK_MIN` (512KB, deliberately above `KLARGE_THRESHOLD` so it only fires once many small frees have merged into a large contiguous tail) (`KHEAP_ENABLE_PAGE_RETURN`). The strict `KHEAP_VALIDATE` validator was extended to also check the `prev_size` boundary tags and the `last_block` invariant on every op; run with `KHEAP_VALIDATE=1`, host smoke reached `B1NIX-TEST: done`, **178 ok markers, zero validator trips** — coalescing + page-return are heap-consistent end-to-end. The klarge arena still owns every allocation ≥ 256KB (it already returns those pages); general-heap coalescing is the anti-fragmentation / completeness complement. Production default keeps `KHEAP_VALIDATE 0` with both coalescing toggles on. The DMA-lifetime hardening landed earlier is still correct regardless. See [`docs/m26-selfhost.md`](m26-selfhost.md) UPDATE l/o/p.

## M27: Terminal OS Polish

- [x] `done` Add boot menu options and kernel command line parsing. `bootinfo_get_kv()` key=value parser + `init=`/`b1nix.single`/`b1nix.nographics` dispatch in `init_main` + a 3-entry GRUB boot menu. Host smoke 223/0 (`M27-CMDLINE: ok kv-parse`). See [`docs/m27-polish.md`](m27-polish.md).
- [x] `done` Add init scripts and a simple service supervisor. `/etc/rc` ships in the initramfs and `init_main` runs it via `/bin/sh /etc/rc` at startup; the reap loop now respawns the login shell when it exits (fixing a latent `-ECHILD` busy-spin) and halts rather than spin if no shell can start. Host smoke 225/0 (`M27-INIT: ok rc-script`). See [`docs/m27-polish.md`](m27-polish.md).
- [x] `done` Add users, passwords or login shell basics. `/etc/passwd` ships in the initramfs; libc `getpwnam`/`getpwuid` parse it; `/bin/login` looks a user up, drops privileges (`setgid`/`setuid`), and execs their shell (init runs it on `b1nix.login`). Passwords not yet checked. Fixed a real ramfs `getdents`/`readdir` bug (every-other-entry + duplicates across batches) found along the way. Host smoke 231/0 (`M27-USER: *`). See [`docs/m27-polish.md`](m27-polish.md).
- [x] `done` Add stable shutdown, reboot, and emergency shell paths. `SYS_REBOOT` (was a no-op `arch_halt` stub) now takes RESTART (8042 pulse + triple-fault fallback) / POWEROFF (QEMU/Bochs ACPI ports) / HALT commands, exposed as `reboot`/`poweroff`/`halt`/`shutdown`. Emergency single-user `/bin/sh` via `b1nix.single` + spawn-failure fallback. Host smoke 224/0 (`reboot: restarting`, real reset under QEMU `-no-reboot`). See [`docs/m27-polish.md`](m27-polish.md).
- [x] `done` Document everyday usage from boot to editing/building files. "Everyday usage: boot → edit → build" walkthrough (GRUB entries, cmdline options, login, tools, shutdown) in [`docs/m27-polish.md`](m27-polish.md).
- [x] `done` Keep the system usable without graphics as a first-class target. All kernel/userspace I/O is text/serial; the only GUI (`/bin/mc`) is opt-in via `b1nix.ui=1`, and `b1nix.nographics` forces text mode (honoured even alongside `ui=1`). See [`docs/m27-polish.md`](m27-polish.md).
- [x] `done` Add first-boot setup for persistent root images. `/etc/rc` initialises `/persist` (creates `home`/`etc`/`tmp`, idempotent via `.b1nix-setup`) on first boot when the persistent root is mounted; inert otherwise. See [`docs/m27-polish.md`](m27-polish.md).
- [x] `done` Add a clear POSIX compatibility matrix for application ports. Summary matrix in [`docs/m27-polish.md`](m27-polish.md), linking the authoritative [`posix-requirements.md`](posix-requirements.md) / [`posix-branches.md`](posix-branches.md).

## M28: Preemptive SMP Scheduling & Fine-Grained Locking

- [x] `done` Per-CPU LAPIC scheduler tick (M28-A) + preemptive yield (T8 / M28 #8). Each core ticks itself off `LAPIC_TIMER_VECTOR` (0x40) at 100 Hz instead of the BSP-only PIT IRQ0. `scheduler_on_timer_tick` now calls `scheduler_yield()` — the long-deferred "re-enable once VFS locking is audited" step. The foundation that closes it: vfs_tree_lock IRQ-save semantics (a chain walker holds the rwlock with IRQs disabled, so same-CPU timer cannot preempt it mid-walk), every other spinlock is irq-save or per-task, and timer-yield now takes the BKL when a tick interrupts userspace so scheduler state is still serialized against AP syscall/idle handoffs. EOI moved BEFORE `scheduler_on_timer_tick` so the LAPIC unblocks immediately and isn't wedged when the yield context-switches away. Vector 32 (PIT) kept as a calibration-failed fallback; vector 255 (LAPIC spurious) wired as a no-EOI no-op. Smoke 250/0/0 single CPU + `-smp 4`.
- [x] `done` Audit and remove the Big Kernel Lock (BKL) for userspace execution on APs. Foundation: lock-order DAG in [`docs/m28-locking.md`](m28-locking.md), lockdep tracker (`-DKERNEL_LOCKDEP=1`) with Variant-A bequeath relaxation (`lockdep_*_global` for BKL + per-inode rw_lock — no false positives on cross-CPU release), TLB shootdown IPI (0x41, gated), reschedule IPI (0x42) wired into `wake_sleepers` / `scheduler_wake_all` / `scheduler_exit_current` (F6). Atomic-CAS state transitions: DEAD→REAPING (F1), SLEEPING/BLOCKED→READY (F4), READY→RUNNING in `pick_next_task` (F5), BLOCKED→READY in exit_current's parent wake (F6). `bkl_unlock` is a no-op for non-owner CPUs. **T1 / T2 / T3 / T8 (idle + timer-ISR BKL invariant): done.** **Stack-lifetime race fixed via per-task `stack_released` lease** — `arch_context_switch` takes 3rd arg `&old->stack_released` and stores `1` after RSP swap; `scheduler_exit_current` + signal SIGKILL/SIGDFL terminators + `ap_worker_trampoline` clear `0` before `state = TASK_DEAD`; `scheduler_waitpid` polls `==1` before `kfree(stack)`. Closes the M14 `iretq` GP fault deterministically. **T4 (`bkl_lock` removal from `syscall_entry.S`) LANDED** with full smoke **271 / 0** on both single-CPU and `-smp 4`. Closure required reverting two CS/SS check inversions that 0d6e287 introduced in `user_frame_is_valid` and `sys_sigreturn` (silently killed every userspace ELF with SIGSEGV at end-of-first-syscall — the 125-test regression that defined the earlier "T4 doesn't land" story) and removing two BKL handoff blocks that wedged `/tmp/hello` after M25-HELLO. See [`docs/m28-t4-blocker.md`](m28-t4-blocker.md) for the full closure story and the regression to avoid re-introducing.
- [x] `partial` Read-write lock primitive + per-subsystem locking discipline (M28-B + #3 + #4). Header-only atomic `rwlock_t` (`kernel/include/b1nix/rwlock.h`) and a global `vfs_tree_lock` explicitly protect every `vfs_node` parent/sibling chain walk in `kernel/fs/vfs.c` (readers in `find_child` + `vfs_get_mount_for_node`, writers across all 14 sibling-list mutation sites — `add_node` × 2 / `vfs_create` / `vfs_mkdir` / `vfs_remove_node` / `vfs_unlink_at_internal` / `vfs_link` / `vfs_symlink` / `vfs_rename_internal` insert + rollback). `g_tasks_lock` stays a spinlock_t (the slot-lifecycle contract is already lock-free correct for walkers — the real SMP gap on tasks[] is per-task field tearing, see [`docs/m28-locking.md`](m28-locking.md)). `heap_lock` / `pmm_lock` instrumented and confirmed at the correct DAG levels (HEAP < PMM — lockdep caught the original ordering being wrong). VMM has no explicit lock today and is the remaining MM piece — relies on BKL until M28 item 2. Smoke 250/0/0 (lockdep off and on).
- [x] `done` Benchmark and optimize context-switch latency under SMP workloads. Test-mode rdtsc micro-benchmark in `kernel/sched/m28_ctxbench.c` reports cycles for the SYS_GETPID kernel handler (`scheduler_get_pid`) and a full `scheduler_yield` round-trip. Single-CPU only (SMP-4 still hits the documented baseline flake at high syscall density — out of scope for benchmarking until T4 stabilization lands). Single-CPU baseline: getpid ≈ 214 cycles, yield ≈ 1581 cycles. Smoke harness greps `M28-BENCH: ok` so the benchmark is part of the regression set.
- [x] `done` **M28 COMPLETE — Big Kernel Lock fully removed.** The BKL is gone from every kernel-entry path (syscall T4, exception handler, device-IRQ handler) AND the lock object itself is deleted (`kernel/sched/bkl.c`, `include/b1nix/bkl.h`, `task.bkl_depth`). No global kernel lock exists; all SMP serialisation is the per-subsystem DAG in [`docs/m28-locking.md`](m28-locking.md). Prerequisites closed along the way: `vmm_handle_page_fault` made self-locking under `vmm_lock` (prepare-then-commit, since it took no locks before); `pending_signals` lost-update fixed with atomic RMW; vfs sibling-list walks moved from bare cli/sti to `vfs_tree_lock`; the residual SMP-4 stack-lifetime race closed by extending the `stack_released` lease to the block/sleep yield arms (15/15 clean SMP-4 stress boots). **M28 #3 (per-task field tearing): closed by analysis** — no memory-corrupting cross-task race remains; a per-task lock would be decorative. **M28 #4 (heap/pmm granularity): largely done** — per-CPU PMM cache + per-CPU kmalloc magazine + O(1) `track_free` (heap cross-core contention 2.58x→1.78x; absolute kfree cost cut ~3.6x by removing a fixed O(1024) scan). OOM no longer panics the kernel — the offending allocation fails / its process dies, system survives (verified `-smp 8 -j8 512 MB` no-swap). Self-host verified green at `-smp 8 -j8 / 2048 MB`. Smoke 271/0 (UP + SMP-4).

## M29: POSIX Threads & Futex Synchronization

- [x] `done` Implement `clone()` syscall with `CLONE_VM`, `CLONE_FS`, `CLONE_FILES`, `CLONE_SIGHAND`, `CLONE_THREAD`, `CLONE_SETTLS`, and `CLONE_CHILD_CLEARTID` flags. `kernel/sched/scheduler.c::scheduler_clone_thread` allocates a new task slot, shares pml4/vma/fd_table/user_image with the parent (refcount on user_image bumped, mm/fds detached at waitpid reap via [`pml4_other_refs`](../kernel/sched/scheduler.c)/`fdtable_other_refs` so the surviving sibling keeps them), and lands in ring 3 via a `clone_thread_kentry` → `x86_user_jump` trampoline with `start_routine(arg)` in RDI.
- [x] `done` Implement `SYS_futex` for fast userspace locking and waiting. `kernel/sched/futex.c` — 64-bucket hash table keyed on `(pml4_phys, uaddr)`, `FUTEX_WAIT` re-checks `*uaddr == val` under the bucket lock (closes the classic wait/wake race) then `scheduler_block_on`s; `FUTEX_WAKE` removes up to N waiters from the bucket and transitions each via `scheduler_wake_task`.
- [x] `done` Configure Thread Local Storage (TLS) via `%fs` segment base. `SYS_SET_TLS(addr)` writes `MSR_FS_BASE` (0xC0000100) live and stores the value in a per-task side-table; the scheduler re-writes the MSR on every context switch in (`kernel/arch/x86/arch.c::arch_set_fs_base`). Userspace `mov %fs:N, %reg` lands at `fs_base + N` since `x86_user_jump` keeps the FS selector at the user data descriptor.
- [x] `done` Build a compliant `libpthread` inside libc with mutexes, condvars, and join/detach APIs. `userspace/libc/pthread.c` — `pthread_create/exit/join/detach/self/equal`, three-state futex-backed `pthread_mutex_*` (normal + recursive flavour, fast-path is a single atomic CAS), `pthread_cond_*` (sequence-counter + FUTEX_WAKE), `pthread_once`, and `pthread_mutexattr_*`. Header at `userspace/include/pthread.h`.
- [x] `done` Verify thread safety in core memory and file operations from userspace. Smoke `M29-PTHREAD` covers `pthread_create` + `pthread_join` (return value passing), mutex serialisation of two competing increment loops (counter == 400 after 2×200), `pthread_cond_signal`/`wait`, `SYS_SET_TLS` + `mov %fs:0, %reg` round-trip, and `SYS_GETTID` returning distinct ids per thread. Markers: `M29-PTHREAD: ok create-join`, `mutex`, `condvar`, `tls`, `gettid`, `done`.

NOTE: M29 thread metadata (`is_thread` / `tls_base` / `child_tid_clear`) lives in parallel side-tables in `kernel/sched/scheduler.c` (`g_task_is_thread[MAX_TASKS]` etc.), NOT in `struct task`. Adding fields to `struct task` triggered an unrelated paging issue — the LAPIC PT became unreachable from user-task pml4 (cr2 `0xfffffe00000000b0`, PT[0]=0) right after the M14 mount syscall. Root cause looks like a latent kernel/pmm interaction between struct task chunk allocation order and the LAPIC PT physical frame; not investigated further since the side-table layout sidesteps it entirely. See `task_is_thread()` / `task_tls_base()` / `task_child_tid_clear()` accessors in `sched.h`.

## M30: ELF Dynamic Linking & Shared Libraries

- [x] `done` Kernel ELF loader supports both static `PT_INTERP` detection (logs the requested interpreter) and full `ET_DYN`/PIE loading. PIE binaries get a fixed `PIE_LOAD_BASE` (0x500000000000), every PT_LOAD's vaddr is offset by that base, and the loader walks `PT_DYNAMIC` to apply `R_X86_64_RELATIVE` relocations (both `DT_RELA` and `DT_JMPREL` paths). Multiple PT_LOADs that share a 4 KB page (typical in PIE binaries — `.data` and `.dynamic` both live in vaddr 0x1000) reuse the first-allocated frame so later segments don't overwrite earlier ones. (`kernel/user/process.c`)
- [x] `done` `/lib/ld-b1nix.so` shipped in the initramfs — same payload as `/bin/m30-pie` (the PIE smoke binary). With the kernel applying relocations in-band, there's no separate userspace dynamic-linker handoff: PT_INTERP names this file as documentation/spec compliance, and the kernel does the relocation work itself. A future POSIX-style userspace ld.so would replace this stub.
- [x] `done` PIE smoke (`userspace/bin/m30_pie.c`) — a self-contained ET_DYN binary with three R_X86_64_RELATIVE relocations into a `messages[]` pointer table. If relocations weren't applied (or applied incorrectly) the strlen+write loop would dereference NULL or a link-time stub address and page-fault. Three markers: `M30-DYN: ok pie-binary`, `ok pie-relocs`, `done`.
- [x] `done` `dlopen` / `dlsym` / `dlerror` / `dlclose` POSIX-compliant stubs in `userspace/libc/stdlib.c`. `dlopen` returns NULL with `dlerror` set, conditional `if (h = dlopen(...))` code paths compile and fall through cleanly. Once a separate userspace dynamic-loader handoff lands, these become the real implementations.

Note: a true POSIX-style `libc.so` (shared object with `DT_NEEDED` resolution, symbol versioning, GOT/PLT fixups) is **not** implemented — the in-kernel relocation only handles RELATIVE within a single ELF, no inter-module symbol resolution. The framework is sufficient for any PIE binary that doesn't need external symbol resolution.

## M31: User Security, Passwords & Permissions

- [x] `done` `/etc/shadow` shipped in the initramfs with the standard `user:hash:lastchange:min:max:warn:inactive:expire:reserved` layout. Parser + shadow lookup at `kernel/user/busybox.c::shadow_lookup`. Hashes are the b1nix-crypt format `$b1$<salt>$<base64>`; the in-kernel `login` built-in extracts the salt, re-hashes the supplied password, and constant-time compares.
- [x] `done` SHA-512 (FIPS 180-4) implemented in-kernel at `kernel/lib/sha512.c` (80-round Merkle-Damgård, no libc / no SSE — fits the kernel's `-mno-sse` constraint). Layered crypt at `kernel/lib/crypt.c` uses 1024 SHA-512 rounds — not bcrypt-grade but it's deterministic, salt-aware, and gives b1nix a verifiable on-disk password hash.
- [x] `done` VFS permission checks in `vfs.c::cred_can_access` cover `/etc`, `/root`, and `/home` (initramfs files are root-owned 0644; the VFS rejects writes by non-root). Verified end-to-end by the M31-SEC smoke (`uid-denial`: a uid 1000 task that tries to setuid(0) is rejected with EPERM by `cred_set_uid`).
- [x] `done` Setuid binaries. Initramfs files marked `INITRAMFS_SETUID` (new flag in `b1nix/initramfs.h`) get S_ISUID set on their inode; `user_execve_current` honours S_ISUID by setting the new task's euid/suid to the file owner's uid. Smoke binary `/bin/m31-setuid` (owner=root, suid bit on) drops to uid 1000 in the parent, execve's the suid binary, and verifies the child reports `euid=0`.

Smoke: 5 `M31-SEC:` markers — `start`, `ok uid-syscalls`, `ok shadow-format`, `ok setuid-elevate`, `ok uid-denial`, `done`.

## M32: Advanced Network Stack & TCP Completeness

- [x] `done` TCP sliding-window flow control. `struct tcp_conn` carries `snd_wnd` (peer's advertised window, refreshed on every received ACK). `tcp_send()` now enforces it: it never puts more than `min(cwnd, snd_wnd)` bytes in flight (`snd_nxt - snd_una`), truncates a send to the usable window, and returns 0 once the window is full so the caller retries after an ACK advances `snd_una`. Smoke-verified end-to-end with a deliberately small (10-byte) advertised window (`M32-TCP: ok window-throttle`, `kernel/net/tcp.c` + `kernel/user/programs.c`).
- [x] `done` TCP Reno congestion control. `struct tcp_conn` carries `cwnd`, `ssthresh`, `dup_acks`. ACK reception runs slow-start (cwnd += MSS while cwnd < ssthresh) and congestion-avoidance (cwnd += MSS²/cwnd) increments. **Fast retransmit**: 3 duplicate ACKs trigger ssthresh = cwnd/2, cwnd = ssthresh + 3·MSS (RFC 5681 inflation), and the head of the retransmit queue is resent immediately (no waiting for RTO). Subsequent dup ACKs in fast recovery inflate cwnd by another MSS each. Full NewReno partial-ACK handling and Cubic are deferred — Reno is the standard textbook algorithm.
- [x] `done` Port standard network clients and servers. Closed with a small POSIX-socket network utility, `/bin/m32-nettool`, providing a TCP echo listener and an HTTP/1.0 GET client over `getaddrinfo()`/`socket()`/`connect()`/`bind()`/`listen()`/`accept()`/`send()`/`recv()`. The kernel IPv4 layer now has a deterministic `127.0.0.1` loopback fast path so client/server socket workflows can be smoke-tested without depending on QEMU user-net or an external host. `m32-smoke` verifies a real loopback TCP echo exchange and an HTTP-style request/response (`M32-NET: ok tcp-echo`, `ok http-get`, `ok tcp-server`), while the older in-kernel BusyBox `wget`/`nc` remain available for interactive external networking.
- [x] `done` Port upstream curl instead of writing a local clone. `/bin/curl` is built from the official curl 8.20.0 release tarball with autotools (`tools/build-curl.sh`) through the b1nix userspace libc wrapper (`tools/b1nix-autotools-cc`) and embedded as a real static ELF in initramfs. The current port is HTTP-only (`http`, `ipfs`, `ipns` reported by configure): SSL/TLS, SSH transports, IPv6, Unix sockets, compression, HTTP/2, and HTTP/3 are intentionally disabled until the required crypto/socket substrate lands. To support curl, userspace libc gained the missing compatibility surface curl expects (`stdbool.h`, `float.h`, `netinet/in.h`, POSIX-shaped `sockaddr_in`, `fcntl`/`O_NONBLOCK`, `setsockopt`, `ftruncate`, `pthread_cond_timedwait`, `FD_*` macros, and related socket constants).
- [x] `done` Port upstream GNU Wget instead of relying only on the in-kernel BusyBox applet. `/bin/wget` is built from GNU Wget 1.21.4 with autotools (`tools/build-wget.sh`) through the b1nix userspace libc wrapper and embedded as a static ELF in initramfs. The current port is HTTP-only (`--without-ssl --disable-iri --disable-pcre --disable-pcre2 --disable-threads --disable-nls --disable-ipv6 --disable-ntlm`) and is smoke-verified by a loopback HTTP download from `m32-smoke` (`M32-NET: ok wget-loopback`).
- [x] `done` `select()` syscall — `SYS_SELECT(nfds, readfds, writefds, exceptfds, timeout_ms)` in `kernel/syscall/syscall.c`. Translates `fd_set` bitmasks to the existing `pollfd` machinery (single in-kernel block-on-channel), translates `revents` back to fd_sets. Userspace `sys/select.h` defines the standard `FD_ZERO`/`SET`/`CLR`/`ISSET` macros + a `struct timeval`. libc wrapper at `userspace/libc/unistd.c::select` converts the `tv` to milliseconds (NULL timeout ⇒ wait forever).
- [x] `done` M32-NET smoke: select() with a zero timeout (no fds ready), select() against a pipe that has data buffered (read end fires), select() across multiple fds (only the readable one fires), numeric resolver plumbing, TCP loopback echo, and HTTP-style request/response. Markers include `start`, `ok select-timeout-zero`, `ok select-pipe-ready`, `ok select-multi-fd`, `ok inet-pton-ntop`, `ok gethostbyname-numeric`, `ok getaddrinfo`, `ok tcp-echo`, `ok http-get`, `ok tcp-server`, `done`.

## M32a: Full Network Client Feature Backlog

- [x] `done` Add TLS/HTTPS support for **curl** (phase 2, 2026-06-01). `curl` is now built against static mbedTLS by default (`B1NIX_TLS ?= mbedtls`) and verified end-to-end by a self-contained **loopback HTTPS** smoke: `m32-nettool tls-server` runs an mbedTLS server presenting an embedded EC test certificate (SAN `IP:127.0.0.1`, generated by `tools/gen-tls-test-certs.sh`, embedded under `/etc/tls-test`), and curl performs a real TLS 1.2 handshake validating the chain against the matching test CA (`M32-NET: ok curl-https-handshake`), plus a negative path proving curl rejects a chain it does not trust (`ok curl-https-selfsigned-reject`). This is the first proof the mbedTLS runtime works on b1nix (full client+server handshake + cert verification) with no external-network dependency. Required fixing a kernel `#UD` panic where `getrandom` issued `rdrand` unconditionally on a CPU without RDRAND (now gated on `CPUID.1:ECX[30]`). **wget TLS is still `planned`/blocked**: wget only supports OpenSSL or GnuTLS backends (`--with-ssl={gnutls,openssl,no}`), neither of which is ported, so wget stays HTTP-only until one of those is brought up. SNI/hostname verification and wall-clock cert-time validation are exercised via the loopback path; broader `https://` over the external (non-loopback) net path is limited by the kernel TCP stack not yet completing connections to off-link hosts.
- [x] `done` TLS build plumbing (phase 1a, 2026-06-01): added `tools/build-mbedtls.sh` to cross-build static mbedTLS archives for b1nix userspace and wired `tools/build-curl.sh` to accept `B1NIX_TLS=mbedtls` (default remains `none` / HTTP-only). `Makefile` now forwards `B1NIX_TLS` to curl/wget builders so TLS can be enabled intentionally per build. Wget remains HTTP-only for now (explicit warning when TLS is requested) until its SSL backend wiring is added.
- [x] `done` TLS baseline hardening (phase 1b, 2026-06-01): added `SYS_GETRANDOM` (+ libc `getrandom`) as a kernel-backed entropy source for userspace crypto, switched initramfs CA path from placeholder to a fetched Mozilla bundle (`tools/fetch-cacert.sh` → `build/cacert.pem`), and configured curl with `--with-ca-bundle=/etc/ssl/certs/ca-certificates.crt`. (The live TLS handshake is now exercised over loopback — see the phase-2 entry above.)
- [x] `done` IPv6 userspace groundwork (phase 1, 2026-06-01): libc now exposes `AF_INET6`/`PF_INET6`, `struct in6_addr`, `struct sockaddr_in6`, `INET6_ADDRSTRLEN`, `in6addr_any`, and `in6addr_loopback`; `inet_pton`/`inet_ntop` accept `AF_INET6`; `getaddrinfo` can return `sockaddr_in6` for numeric IPv6 (including `::1`) and `localhost`. M32 smoke markers: `M32-NET: ok inet6-pton-ntop`, `M32-NET: ok getaddrinfo-inet6`.
- [ ] `planned` Add IPv6 networking. Kernel work: IPv6 addresses, routing, neighbor discovery, ICMPv6, UDP/TCP over IPv6, dual-stack socket behavior, `sockaddr_in6`, `AF_INET6`, `getaddrinfo`/`getnameinfo` updates, and smoke for `::1` loopback plus DNS AAAA parsing.
- [ ] `planned` Add PCRE/PCRE2 support as an optional userspace port. Start with PCRE2 because it is the maintained library; wire it into the autotools wrapper, add libc gaps it exposes, then enable PCRE2-dependent wget/curl features only when the library is present.
- [ ] `planned` Add IRI/IDN support. Requires a deliberate Unicode/locale stance: wide-character correctness, UTF-8 normalization/punycode, IDNA handling, and tests that URLs with non-ASCII host/path components either work or fail predictably.
- [ ] `planned` Add NTLM only if a real compatibility need appears. It requires MD4/MD5/HMAC/Unicode conversion details and credential-handling policy; keep it behind an explicit build flag so the default clients remain small and auditable.
- [ ] `planned` Re-enable the disabled curl/wget configure features incrementally (`SSL`, `IPv6`, `PCRE2`, `IRI/IDN`, `NTLM`) with one smoke marker per feature instead of flipping them all at once.

## M32b: SSH Daemon Prerequisites
- [ ] `planned` Pick the first sshd target. Prefer `dropbear` or `tinyssh` for the first daemon-sized port; defer OpenSSH until PTY, privilege separation, and broader libc/POSIX coverage are stronger.
- [ ] `planned` Add the crypto and RNG baseline required by SSH: secure random bytes, Curve25519/DH key exchange, Ed25519/ECDSA/RSA host/user keys, chacha20-poly1305, AES-CTR/GCM, HMAC-SHA2, SHA2 hashing, and constant-time compare helpers.
- [ ] `planned` Add persistent SSH host-key storage under `/etc/ssh` with permission checks, first-boot key generation, and tooling to inspect/regenerate keys.
- [ ] `planned` Finish the PTY/TTY substrate for interactive logins: `/dev/ptmx`, pty pairs, controlling terminal handoff, `termios`, window-size ioctls, and clean hangup semantics.
- [ ] `planned` Harden socket APIs sshd expects: `SO_REUSEADDR`, `SO_KEEPALIVE`, `TCP_NODELAY`, nonblocking `fcntl`, `poll`/`select` edge cases, `getsockname`, `getpeername`, `shutdown`, listen backlog behavior, and reliable child socket inheritance.
- [ ] `planned` Add process/session plumbing for login shells: `fork`/`exec`, `setuid`/`setgid`, `setsid`, `chdir` to home, environment setup, uid/gid permission checks, and process-group/foreground-terminal ownership.
- [ ] `planned` Add auth storage and policy: `/etc/shadow` password auth, optional `authorized_keys`, account shell/home validation, login failure accounting, and clear root-login defaults.
- [ ] `planned` Add service lifecycle support: an init/service entry for sshd, background daemon mode, log output, pid tracking, clean shutdown/restart, and a way to bind only loopback during early testing.
- [ ] `planned` Smoke-test sshd on localhost first: SSH protocol handshake, host-key persistence, password login to run a single command, interactive shell over pty, and negative auth cases. Add scp/sftp only after the login path is stable.

## M33: POSIX Shell Compliance & Job Control Polish

- [x] `done` Implement command substitution (`$(cmd)` and `` `cmd` ``): runs the inner command with stdout captured to a temp file (no pipe deadlock), trims trailing newlines / collapses embedded newlines, supports nesting; resolved in all readers before expansion. `kernel/user/programs.c` (`sh_expand_cmdsubst`/`sh_capture_command`); smoke-verified (`M33-SHELL: ok cmdsubst`).
- [x] `done` Support subshell execution via `( list )`: paren-aware operator/pipe splitting, inner list runs with its own cwd copy + env snapshot so `cd`/variable side effects do not leak; trailing redirections applied around the group. `kernel/user/programs.c` (`sh_run_subshell`); smoke-verified (`M33-SHELL: ok subshell`).
- [x] `done` Complete POSIX terminal job control: growable job table with Running/Stopped state; a foreground job that receives `SIGTSTP` is recorded as Stopped (`run_external_command` waits with `WUNTRACED`); `bg` resumes it in the background and `fg` resumes + waits (re-stopping is tracked) via `SIGCONT`; `jobs` reports state. `kernel/user/programs.c` (`sh_bg`/`sh_fg`/`sh_jobs_print`); smoke-verified end-to-end (`M33-SHELL: ok jobs` — real SIGTSTP stop + bg/SIGCONT resume of a child).
- [x] `done` Add support for complex script structures (functions, `case` statements, arrays). Multi-line constructs collected by a shared block reader (`sh_handle_compound`/`sh_collect_braces`): `name() { … }` functions stored in a growable table and run with `$1..$9`/`$#` set; `case WORD in pat) … ;; esac` matched via `glob_match`; `arr=(…)` arrays with `${arr[i]}`, `${arr[@]}`, `${#arr[@]}` in `expand_env`. Shell env + function + array tables are now heap-backed and grow on demand (no fixed caps). Smoke-verified (`M33-SHELL: ok function`/`case`/`array`).
- [x] `done` Implement pathname expansion (globbing): `*`, `?`, and `[…]`/`[!…]` bracket expressions against the VFS. Quote-aware (`parse_cmd` flags only unquoted metachars), sorted matches, dotfiles hidden unless the pattern starts with `.`, literal fallback when nothing matches. `kernel/user/programs.c` (`glob_match`/`glob_expand`); smoke-verified (`M33-SHELL: ok glob-star`/`glob-class`/`glob-nomatch`).
- [x] `done` Implement arithmetic expansion (`$((…))`): recursive-descent integer evaluator (`+ - * / %`, parentheses, unary `+/-`, decimal literals, bare/`$`-prefixed variables) wired into `expand_env`. Smoke-verified (`M33-SHELL: ok arith`).
- [x] `done` Implement here-documents (`<<`, `<<-`): source-agnostic body collection (script fd + interactive tty), `<<-` leading-tab stripping, quoted-delimiter expansion suppression, and body variable expansion; spooled to a temp file and consumed via the normal `<` redirection path. `kernel/user/programs.c` (`sh_resolve_heredoc`); smoke-verified (`M33-SHELL: ok heredoc`).
- [x] `done` Broaden coreutils utility flag coverage toward POSIX. On top of the M22 set (`ls -la`, `cp -r`, `rm -rf`, `mkdir -p`, `grep -q/-n/-v`, `wc -lwc`, `head/tail -n`), added `grep -i` (case-insensitive) and `grep -c` (match count). `kernel/user/busybox.c`; smoke-verified (`M33-SHELL: ok grep-flags`). Further flags can be added incrementally against this pattern.
- [x] `done` Add `for`/`while`/`until` loops. Collected by the shared block reader (`sh_collect_block` to the `done` keyword) and run by `sh_run_for`/`sh_run_while`; `for` splits a cmdsubst+var-expanded word list, `while`/`until` re-evaluate the condition each iteration (with a large runaway guard so a never-false loop can't wedge the in-kernel shell). `kernel/user/programs.c`; smoke-verified (`M33-SHELL: ok for-loop`/`while-loop`).
- [x] `done` Add `${x:-w}`/`${x-w}`, `${x:=w}`/`${x=w}`, `${x:+w}`/`${x+w}` parameter expansion and `${#x}` length (word recursively expanded) in `expand_env`, plus bare `VAR=value` scalar assignment (`sh_try_scalar_assign`). Smoke-verified (`M33-SHELL: ok param-expand`, used by the loop tests).
- [x] `done` Add `trap 'cmds' SIG …`: handlers stored in a growable table (`sh_trap_set`/`sh_run_trap`); `trap` with no args lists them, `trap - SIG` clears. The `EXIT` handler fires from the `exit` builtin. `kernel/user/programs.c`; smoke-verified (`M33-SHELL: ok trap`). NOTE: asynchronous signal-delivered traps (INT/TERM mid-command) are not yet wired — only `EXIT` and explicit firing are active.
- [x] `done` Concurrent pipeline execution. **Closed 2026-06-01** (previously `wontfix`): `sh_execute_pipeline` now `fork()`s each non-final stage as a producer (`SYS_FORK`, the same fork-of-builtin path the lock smoke uses) so producer and consumer run concurrently — matching POSIX subshell semantics and, more importantly, fixing a **latent deadlock** in the old run-to-completion path (a producer emitting more than the 512-byte pipe buffer blocked on a full pipe before the consumer ever started). The consumer chain runs in the shell process; the producer is reaped via `waitpid`. A sequential fallback remains if `fork` is unavailable. Smoke-verified by streaming a multi-KB file through a pipe (`M33-SHELL: ok pipe-large`, would have deadlocked the boot under the old path); all M11 pipe tests still pass (314/0). `kernel/user/programs.c`.

## M34: Virtual Filesystems (/proc and /sys)

- [x] `done` `/proc` synthetic filesystem (`kernel/fs/procfs.c`). System files `meminfo`, `uptime`, `loadavg`, `version`, `cpuinfo`, `stat`, `filesystems`, plus per-process `self/{status,cmdline,comm,stat,maps}` and lazily-materialised `<pid>/` dirs (content regenerated per read from the PMM, scheduler task table and per-task VMAs). Synthetic files are `VFS_DEVICE` nodes so the VFS dynamic-read path is taken (the `VFS_FILE`+read_cb page-cache path would freeze content). Smoke-verified (`M34-PROC: ok meminfo`/`version`/`proc-self-status`/`proc-self-maps`/`proc-listing`/`proc-pid-status`).
- [x] `done` `/sys` synthetic filesystem (`kernel/fs/sysfs.c`): `kernel/{ostype,osrelease,hostname,version}`, `devices/system/cpu/{possible,online,present}`, `memory/total_kb`. Smoke-verified (`M34-PROC: ok sysfs-osrelease`/`sysfs-cpu`).
- [x] `done` Process monitoring tools `free`, `top`, `sysctl` (read `/proc` + `/sys`) added to busybox alongside the existing `ps`; registered as `/bin/{free,top,sysctl}`. Smoke-verified end-to-end (`M34-PROC: ok tools` — free/sysctl/top run and read back live kernel state). See `docs/m34-m36-diagnostics.md`.

## M35: Core Dumps & Diagnostic Analysis

- [x] `done` ELF core dump generation on fatal CPU-fault signals (SIGSEGV/SIGABRT/SIGILL/SIGFPE/SIGBUS) with no handler. `kernel/arch/x86/coredump.c` writes a valid `ET_CORE` ELF to `/tmp/core` with a `PT_NOTE`/`NT_PRSTATUS` register file and one `PT_LOAD` per mapped run of the dying task's address space. Pages are probed with `vmm_virt_to_phys()` before reading so a lazily-unmapped page can't fault the dumper. Smoke-verified (`M35-CORE: ok crash-signal`/`core-elf`/`core-prstatus`).
- [x] `done` `kallsyms` backtrace symbolication. A two-pass link (`tools/gen_kallsyms.sh` + linker.ld `.kallsyms` section placed after the address-frozen `.text/.rodata/.data`) embeds an address→name blob; `ksym_lookup`/`ksym_print` (`kernel/lib/klog.c`) resolve `name+0xoffset`, wired into `arch_backtrace`. Smoke-verified (`M35-DIAG: ok kallsyms`/`kallsyms-offset`/`kallsyms-multi`).
- [x] `done` User-space/panic symbol resolution: backtraces now print symbolised frames, and `/proc` exposes process diagnostics for post-mortem analysis. See `docs/m34-m36-diagnostics.md`.

## M36: Kernel Debugging & Tracing (GDB Stub & ftrace)

- [x] `done` Serial-port GDB Remote Serial Protocol stub (`kernel/arch/x86/gdbstub.c`): `?`, `g`/`G`, `m`/`M`, `c`/`s`, `qSupported` over COM1, entered on int3 (#BP)/#DB when booted with `b1nix.gdb` (off by default so a normal boot never blocks on a host). The transport-agnostic packet engine is self-tested in-kernel (`M36-GDB: ok stop-reply`/`read-regs`/`read-mem`/`framing`).
- [x] `done` ftrace function tracer (`kernel/lib/ftrace.c`): `-finstrument-functions` hooks (`__cyg_profile_func_enter/exit`) record a ring buffer of enter/exit events, symbolised via `kallsyms`. Only opted-in TUs are instrumented (the Makefile flags `ftrace_demo.c`) so the kernel is not globally slowed and hooks don't recurse. Smoke-verified (`M36-FTRACE: ok capture`/`symbolize`).

## M37: Real Hardware Booting (Bare Metal)

- [ ] `planned` Write a UEFI bootloader setup or GRUB USB configuration targeting standard x86_64 PC hardware.
- [x] `done` Replace hardcoded system limits with dynamic ACPI table discovery (dynamic CPU count, interrupt routing). Delivered across A1 (ACPI MADT CPU enumeration), A1-irq (IOAPIC routing replaces 8259 PIC), A2 (LAPIC timer calibrated against PIT), B1 (direct map sized to actual RAM), B2 (caches scaled to RAM), B3 (swap + eviction tables sized to RAM/device), C1 (chunked growable task table), C2 (growable program registry), C3 (MAX_CPUS raised + runtime `g_max_cpus` from MADT). Smoke 250/0/0 after each step. See `docs/dehardcode-audit.md` for the per-item commit map and remaining follow-ups (drivers→`vmm_map_mmio()`, LAPIC tick switch, heap-backed TSS array).
- [ ] `partial` Implement BIOS/UEFI VBE/GOP graphics mode-setting dynamically at startup. Framebuffer parameters (resolution/bpp/pitch/address) are already runtime-discovered from Multiboot2 via `bootinfo_get()->framebuffer`, so the kernel adapts to whatever GRUB set. True in-kernel mode-setting still requires either UEFI/GOP (depends on the M37 UEFI bootloader item above) or a v86 emulator for BIOS INT 10h; deferred until one of those lands.

## M38: Sound / Audio Subsystem

- [ ] `planned` Implement an Intel High Definition Audio (HDA) or AC97 PCI device driver.
- [ ] `planned` Expose sound interfaces via `/dev/dsp` or a simple ALSA-like API.
- [ ] `planned` Build a WAV file parser and player utility for user-space.

## M39: Configurable Init System (SysVinit & /etc/inittab)

- [ ] `planned` Implement a parser for the `/etc/inittab` configuration file to define system startups, runlevels, and respawn targets.
- [ ] `planned` Add support for runlevels (e.g., single-user, multi-user, GUI) and transition control via `telinit` / `init Q`.
- [ ] `planned` Implement multiple virtual terminal (TTY) lines or serial consoles handled by independent `getty` / `login` process spawners.
- [ ] `planned` Replace the hardcoded boot program selection in the kernel-registered `/bin/init` with the configurable file-based service dispatcher.

## M40: Linux ABI Compatibility Layer

Run unmodified Linux x86-64 ELF binaries. This is a large, continually-catching-up
effort (FreeBSD Linuxulator / WSL1 style) and is **gated on M29 (threads/`futex`/TLS),
M30 (dynamic linking), and M34 (`/proc`, `/sys`)** — until those land, the layer would
sit on a process model that cannot satisfy glibc. Source-level porting (recompiling
against b1nix libc) is the cheaper, higher-leverage path and should be exhausted first.

- [ ] `planned` Add a Linux syscall translation layer mapping Linux x86-64 syscall numbers and semantics onto b1nix kernel services (distinct from the native `b1nix/syscall.h` numbers).
- [ ] `planned` Support `PT_INTERP` so the glibc dynamic linker (`ld-linux-x86-64.so`) can load, plus a static-binary fast path.
- [ ] `planned` Provide Linux-compatible process/thread startup: full `auxv`, vDSO, TLS via `%fs`, and a Linux-shaped signal trampoline.
- [ ] `planned` Implement the `/proc` and `/sys` entries glibc and common tools probe at startup (depends on M34).
- [ ] `planned` Add a binary "personality" switch so Linux ELFs are detected and dispatched to the compat layer without affecting native b1nix binaries.
