# Audit of Crutches, Stubs, and Disabled Features in b1nix

## Architectural Note on System Assumptions and Design Trade-offs
This document classifies and registers temporary workarounds ("crutches"), stubs, bypassed code, and architectural design choices in the b1nix codebase.

It is critical to distinguish between **Legitimate Architectural Decisions** (standard OS design patterns, emulation/QEMU integration hooks, uniprocessor optimizations) and **Technical Debt / Implementation Gaps** (unimplemented POSIX calls, spin-loops, hardcoded limitations).

- **[Arch Design]**: A conscious design decision optimized for standard guest VM targets (QEMU/Bochs) or uniprocessor (UP) builds. Fixing them is unnecessary or counterproductive.
- **[Technical Debt]**: A simplified stub or workaround introduced to make software link or boot temporarily. These should eventually be refactored into fully conforming implementations.
- **[Resolved]**: A previously identified technical debt or stub that has now been fully refactored, implemented, or corrected.

## 1. Disabling Command Line Parameters (Boot Flags) `[Arch Design / Debugging]`
The kernel uses `bootinfo_has_flag()` to disable or bypass hardware/test initialization at runtime. Similar to Linux command-line overrides (`nomodeset`, `initcall_blacklist`), these serve as emergency fallback mechanisms for testing and diagnostics.

| Flag | File | Line | Description / Purpose |
|---|---|---|---|
| `b1nix.skip-m25` | [kernel/user/programs.c](file:///home/dmytrom/Documents/GitHub/b1nix/kernel/user/programs.c#L1328) | 1328 | Skips TinyCC (M25) compiler tests. Added due to sensitivity of TCC's fixed load address (0x2000000) to the kernel .text section size. |
| `b1nix.skip-xhci` | [kernel/dev/usb_xhci.c](file:///home/dmytrom/Documents/GitHub/b1nix/kernel/dev/usb_xhci.c#L1400) | 1400 | Bypasses initialization of the USB xHCI controller. |
| `b1nix.skip-e1000` | [kernel/dev/e1000.c](file:///home/dmytrom/Documents/GitHub/b1nix/kernel/dev/e1000.c#L450) | 450 | Bypasses initialization of the Intel e1000 network card. |
| `b1nix.skip-hda` | [kernel/dev/hda.c](file:///home/dmytrom/Documents/GitHub/b1nix/kernel/dev/hda.c#L638) | 638 | Bypasses initialization of the Intel HDA audio controller. |
| `b1nix.skip-r8169` | [kernel/dev/r8169.c](file:///home/dmytrom/Documents/GitHub/b1nix/kernel/dev/r8169.c#L312) | 312 | Bypasses initialization of the Realtek r8169 network card. |
| `b1nix.net=off / b1nix.nonet` | [kernel/net/net.c](file:///home/dmytrom/Documents/GitHub/b1nix/kernel/net/net.c#L400) | 400 | Completely disables the network subsystem. |
| `b1nix.nographics / nographics` | [kernel/user/programs.c](file:///home/dmytrom/Documents/GitHub/b1nix/kernel/user/programs.c#L2446) | 2446 | Disables the graphical user interface. |

## 2. Disabled Capabilities in Third-Party Software Ports `[Technical Debt / Resolved]`
These features were originally disabled in the build configurations (`tools/build-*.sh`) because b1nix libc or the kernel lacked the necessary APIs. Many have since been resolved by improving the system's POSIX conformance:

### BusyBox (Build Script: [tools/build-busybox.sh](file:///home/dmytrom/Documents/GitHub/b1nix/tools/build-busybox.sh))
**Disabled options / modifications:**
- BusyBox's builtin crypt (CONFIG_USE_BB_CRYPT) crashes with 'bad salt' on b1nix's custom password hashing scheme `"$b1$"`. The build script patches `libbb/pw_encrypt.c` to defer `"$b1$"` hashes to the libc `crypt()` function.
- **[Resolved]** CPU affinity syscall `sched_getaffinity` is now implemented. The upstream BusyBox affinity code is used directly.
- **[Resolved]** Functions `scandir()` and `alphasort()` are now implemented in libc, though BusyBox `tree.c` still uses a custom lightweight sorting implementation for simplicity.

### Dropbear SSH (Build Script: [tools/build-dropbear.sh](file:///home/dmytrom/Documents/GitHub/b1nix/tools/build-dropbear.sh))
**Disabled options / modifications:**
- **[Resolved]** zlib (traffic compression) has been enabled.
- **[Resolved]** PAM (pluggable authentication modules) has been enabled via libc PAM authentication shim.
- **[Resolved]** syslog (logging) has been enabled via socket-backed syslog client to /dev/log.
- **[Resolved]** lastlog, utmp/utmpx, wtmp/wtmpx (user login accounting) have been enabled via libc utmp/wtmp implementation.
- **[Resolved]** DO_HOST_LOOKUP (host resolution) has been enabled via libc `getaddrinfo`.
- harden (security hardening flags)
**Conditions to re-enable remaining:** Can be enabled now that PAM, syslog, and utmp/wtmp support are implemented in libc.

### GNU Bash (Build Script: [tools/build-bash.sh](file:///home/dmytrom/Documents/GitHub/b1nix/tools/build-bash.sh))
**Disabled options / modifications:**
- HANDLE_MULTIBYTE (multibyte encoding/wide character support)
- bash-malloc (uses kernel standard allocator instead)
- nls (localization/national language support)
- **[Resolved]** net-redirections (redirections via /dev/tcp/ and /dev/udp/) has been enabled.
- Patch on `parse.y` / `y.tab.c`: disabling HANDLE_MULTIBYTE caused an undeclared reference to `shell_input_line_property[]`, requiring a custom `#if defined(HANDLE_MULTIBYTE)` guard.
**Conditions to re-enable remaining:** Multibyte characters require full wchar/locale support in libc.

### GNU Wget (Build Script: [tools/build-wget.sh](file:///home/dmytrom/Documents/GitHub/b1nix/tools/build-wget.sh))
**Disabled options / modifications:**
- **[Resolved]** zlib (gzip compression) has been enabled.
- **[Resolved]** threads (multithreading) has been enabled.
- nls (localization)
- pcre (legacy PCRE1 is replaced with PCRE2)

### Mesa (OpenGL) (Build Script: [tools/build-mesa.sh](file:///home/dmytrom/Documents/GitHub/b1nix/tools/build-mesa.sh))
**Disabled options / modifications:**
- llvm (compiles shaders into machine code via LLVM; falls back to slow software softpipe)
- glx, egl, gbm (windowing interfaces and buffers)
- shared-glapi (shared OpenGL API; falls back to static linking only)
- gles1, gles2 (OpenGL ES)
- zstd, libunwind, valgrind, shader-cache
**Conditions to re-enable:** LLVM is disabled due to toolchain/build complexity under b1nix. Windowing interfaces are disabled as OSMesa renders directly into framebuffers in memory.

### NetSurf (Build Script: [tools/build-netsurf-fb.sh](file:///home/dmytrom/Documents/GitHub/b1nix/tools/build-netsurf-fb.sh))
**Disabled options / modifications:**
- **[Resolved]** JavaScript (Duktape engine) has been enabled.
- SVG (vector graphics)
- openssl (uses mbedTLS instead)
- libpsl, utf8proc, libiconv
- **[Resolved]** Codecs: JPEG and WebP have been enabled (JPEGXL remains disabled).
- rosprite, video, pdf
- HAVE_MMAP is explicitly undefined (`#undef HAVE_MMAP`) for file:// fetcher, as b1nix cannot mmap files located on the initramfs.

### curl (Build Script: [tools/build-curl.sh](file:///home/dmytrom/Documents/GitHub/b1nix/tools/build-curl.sh))
**Disabled options / modifications:**
- brotli, zstd (compression algorithms)
- **[Resolved]** libpsl (Public Suffix List) has been enabled.
- **[Resolved]** libidn2 (internationalized domain names) has been enabled.
- nghttp2, nghttp3, ngtcp2 (HTTP/2, HTTP/3, QUIC protocols)
- Protocols: **[Resolved]** `file://` protocol has been enabled. (ldap, ldaps, ftp, gopher, imap, mqtt, pop3, rtsp, smb, smtp, telnet, tftp, dict remain disabled).
- **[Resolved]** threaded-resolver (asynchronous DNS resolution via threads) has been enabled.
- **[Resolved]** unix-sockets (Unix domain sockets support in curl) has been enabled.
- **[Resolved]** alt-svc, hsts, websockets, headers-api, dateparse have been enabled.

### mbedTLS (Build Script: [tools/build-mbedtls.sh](file:///home/dmytrom/Documents/GitHub/b1nix/tools/build-mbedtls.sh))
**Disabled options / modifications:**
- **[Resolved]** MBEDTLS_HAVE_TIME, MBEDTLS_HAVE_TIME_DATE (time operations) have been enabled.
- MBEDTLS_TIMING_C (timing module)
- MBEDTLS_NET_C (native mbedTLS socket support)

## 3. Hardcoded Hardware & Virtualization Assumptions `[Arch Design / CI-CD hooks]`
These are integration hooks designed to run efficiently in virtual machines (QEMU/Bochs) and automated CI environments. For instance, exiting QEMU via port writes is the standard method for a guest kernel to pass test results back to the host system.

| Assumption / Component | File | Line | Classification | Description |
|---|---|---|---|---|
| **QEMU Exit on Shutdown (QEMU isa-debug-exit)** | [kernel/arch/x86_64/arch.c](file:///home/dmytrom/Documents/GitHub/b1nix/kernel/arch/x86_64/arch.c#L180) | 180 | **[Arch Design]** | Instead of entering an infinite wait loop or executing a complex ACPI shutdown sequence, the kernel writes `0` to port `0xf4` (QEMU's isa-debug-exit device) when `arch_halt()` is called. This instantly terminates the QEMU emulator with a return code, which is required by the host smoke test harness. |
| **Simplified ACPI Poweroff via QEMU/Bochs Ports** | [kernel/syscall/syscall.c](file:///home/dmytrom/Documents/GitHub/b1nix/kernel/syscall/syscall.c#L2944) | 2944 | **[Arch Design]** | The `SYS_REBOOT` syscall for poweroff (`B1NIX_REBOOT_POWEROFF`) writes fixed command values directly to ports `0x604`, `0xB004`, and `0x4004` to shut down QEMU/Bochs VMs. This bypasses parsing full ACPI tables and executing AML bytecode. |
| **Assumption of Contiguous APIC IDs** | [kernel/arch/x86_64/lapic.c](file:///home/dmytrom/Documents/GitHub/b1nix/kernel/arch/x86_64/lapic.c#L560) | 560 | **[Arch Design]** | When ACPI is unavailable, the SMP subsystem CPID fallback path assumes that Local APIC IDs of processors are contiguous integers from `0` to `cpu_count-1` (true on QEMU and simple hardware). When ACPI is available, the preferred MADT discovery path is used to get real firmware IDs. |
| **Hardcoded VRAM Sizes for Display Adapters** | [kernel/dev/pci.c](file:///home/dmytrom/Documents/GitHub/b1nix/kernel/dev/pci.c#L194) | 194 | **[Arch Design]** | Since virtual graphics devices (`virtio-gpu`, `virtio-vga`) allocate framebuffers dynamically from guest RAM and don't report VRAM size standardly, the kernel hardcodes the reported video memory: 32 MB for VirtIO, and 16 MB for QEMU VGA, VMware SVGA II, and VirtualBox Graphics Adapter. |
| **Hardcoded Network Parameters for QEMU SLIRP** | [kernel/net/dns.c](file:///home/dmytrom/Documents/GitHub/b1nix/kernel/net/dns.c#L25) | 25 | **[Arch Design]** | The default DNS server is hardcoded to IP `10.0.2.3` (QEMU SLIRP gateway DNS). Similarly, initramfs setup ([kernel/fs/initramfs.c#L327](file:///home/dmytrom/Documents/GitHub/b1nix/kernel/fs/initramfs.c#L327)) generates `/etc/resolv.conf` referencing this fixed IP. |
| **Simulated subnet mask /24 in /proc/net/route** | [kernel/fs/procfs.c](file:///home/dmytrom/Documents/GitHub/b1nix/kernel/fs/procfs.c#L704) | 704 | **[Arch Design]** | In `/proc/net/route`, the subnet mask is hardcoded to `/24` (`00FFFFFF` in hex) which is the default in QEMU SLIRP networks. Real setups might use different masks, but the output is stubbed to satisfy test scripts. |
| **Zeroed network counters in /proc/net/dev** | [kernel/fs/procfs.c](file:///home/dmytrom/Documents/GitHub/b1nix/kernel/fs/procfs.c#L719) | 719 | **[Arch Design]** | As the b1nix kernel does not track packet/byte counts per interface, `/proc/net/dev` reports hardcoded `0` for all interface statistics on `lo` and `eth0`. |
| **Forced ARP Requests in Test Mode** | [kernel/net/arp.c](file:///home/dmytrom/Documents/GitHub/b1nix/kernel/net/arp.c#L86) | 86 | **[Arch Design]** | When booted in test mode (`b1nix.test=1`), `arp_resolve` forces sending an ARP request even if the MAC is already in the local cache. This ensures the host capture scripts can trace ARP traffic and verify the network stack works. |
| **Disabled Session Checks in TTY (TIOCSPGRP)** | [kernel/fs/vfs.c](file:///home/dmytrom/Documents/GitHub/b1nix/kernel/fs/vfs.c#L4317) | 4317 | **[Arch Design]** | To simplify process group tracking, the POSIX check `current_task->session_id == console.session_id` is bypassed during `TIOCSPGRP` ioctl calls, as the boot console is allocated before userspace sessions are spawned. |
| **Test Skipping in Headless Environments** | [tests/smoke.sh](file:///home/dmytrom/Documents/GitHub/b1nix/tests/smoke.sh#L1356) | 1356 | **[Arch Design]** | The smoke tests capture missing VM features (like missing virglrenderer or HDA audio device) and convert potential failures into 'skips' to keep the general test suite green. |

## 4. Workarounds, Emulations, and Limitations in Libc / POSIX API
This section represents POSIX compatibility workarounds in the userspace library. Some are architectural shims (e.g. generating synthetic inodes), while others are severe implementation shortcuts (e.g. spin-yielding semaphores):

| Workaround / Mechanism | File | Line | Classification | Description and Consequences |
|---|---|---|---|---|
| **Alias remove() to unlink()** | [userspace/libc/stdio.c](file:///home/dmytrom/Documents/GitHub/b1nix/userspace/libc/stdio.c#L373) | 373 | **[Resolved]** | POSIX `remove()` is now correctly implemented, calling `unlink()` for files and falling back to `rmdir()` if the target is a directory. |
| **Alias vfork() to fork()** | [userspace/libc/unistd.c](file:///home/dmytrom/Documents/GitHub/b1nix/userspace/libc/unistd.c#L1448) | 1448 | **[Technical Debt]** | `vfork()` is implemented as a simple call to `fork()`. While semantically valid, this degrades process launch performance (especially under TCC) because it forces copying page tables. |
| **Best-Effort tcgetsid() Approximation** | [userspace/libc/unistd.c](file:///home/dmytrom/Documents/GitHub/b1nix/userspace/libc/unistd.c#L1031) | 1031 | **[Arch Design]** | Because the kernel does not support `TIOCGSID` ioctls on terminals, `tcgetsid()` is implemented by returning the current process's session ID (`getsid(0)`). This is only correct if the terminal is indeed the process's controlling tty. |
| **Synthetic Inode Numbers in readdir()** | [userspace/libc/dirent.c](file:///home/dmytrom/Documents/GitHub/b1nix/userspace/libc/dirent.c#L69) | 69 | **[Arch Design]** | The kernel `SYS_GETDENTS` syscall does not return file inode numbers. To prevent POSIX tools (such as `find`, `df` or `ls`) from failing due to zero/corrupted inodes, `readdir()` synthesizes monotonically increasing inode values (`d_ino`) in userspace using `++dirp->ino_seq`. |
| **pthread_cond_timedwait() Ignores Timeout** | [userspace/libc/pthread.c](file:///home/dmytrom/Documents/GitHub/b1nix/userspace/libc/pthread.c#L366) | 366 | **[Resolved]** | The `pthread_cond_timedwait()` function now properly implements timed waiting using a timed futex wait (`FUTEX_WAIT` with relative timeout). |
| **Spin-Wait pthread_mutex_timedlock() via nanosleep** | [userspace/libc/pthread.c](file:///home/dmytrom/Documents/GitHub/b1nix/userspace/libc/pthread.c#L532) | 532 | **[Resolved]** | Timed mutex locking is now futex-backed and blocks in the kernel, eliminating the busy-spin nanosleep loop. |
| **Spin-Yield sem_wait() via SYS_YIELD (Non-Atomic!)** | [userspace/libc/stdlib.c](file:///home/dmytrom/Documents/GitHub/b1nix/userspace/libc/stdlib.c#L1036) | 1036 | **[Resolved]** | Semaphore waiting is now atomic and futex-backed, closing the post/park race condition and eliminating SYS_YIELD spin-wait. |
| **Hardcoded root User Fallback in pwd** | [userspace/libc/pwd.c](file:///home/dmytrom/Documents/GitHub/b1nix/userspace/libc/pwd.c#L7) | 7 | **[Arch Design]** | `getpwuid()` and `getpwnam()` contain a hardcoded struct fallback for the `root` user (UID 0) if `/etc/passwd` is absent. This prevents tilde (~root) path expansions from crashing during GNU Make execution. |
| **Hardcoded root/users Group Fallbacks in grp.c** | [userspace/libc/grp.c](file:///home/dmytrom/Documents/GitHub/b1nix/userspace/libc/grp.c#L111) | 111 | **[Arch Design]** | `getgrgid()` and `getgrnam()` contain hardcoded group structure fallbacks for the `root` (GID 0) and `users` (GID 1000) groups if `/etc/group` is missing. This mirrors the root user fallback to keep GNU Make glob parsing running smoothly. |
| **Manual DWARF Exception Frame Registration in crt0.S** | [userspace/crt/crt0.S](file:///home/dmytrom/Documents/GitHub/b1nix/userspace/crt/crt0.S#L74) | 74 | **[Arch Design]** | As the b1nix build target lacks `crti.o`/`crtn.o` (so there is no callable `_init` routine), libgcc's DWARF exception frame registration (`__register_frame_info`) is orphaned. To prevent C++ dynamic exceptions (e.g. throws inside GCC's `cc1`) from triggering `ud2` (SIGILL) in the unwinder, `crt0.S` manually runs this registration before invoking `main()`. |
| **dlfcn Dynamic Linking Stubs** | [userspace/libc/stdlib.c](file:///home/dmytrom/Documents/GitHub/b1nix/userspace/libc/stdlib.c#L754) | 754 | **[Arch Design]** | The `dlopen()`, `dlsym()`, `dlclose()`, and `dlerror()` functions are intentional stubs, as b1nix only supports static binaries or boot-time linking. `dlopen(NULL, ...)` returns a non-NULL sentinel `(void*)1` so programs searching symbols in their own address space work correctly. All other dlopen calls return NULL with a descriptive dlerror message, following POSIX error-reporting contract. |
| **Memory Leak of Detached Thread Stacks** | [userspace/libc/pthread.c](file:///home/dmytrom/Documents/GitHub/b1nix/userspace/libc/pthread.c#L84) | 84 | **[Resolved]** | Stacks and state structures of detached threads are now safely reclaimed after thread exit by registering them on a dead thread tracking list, which is cleaned up during subsequent calls to `pthread_create()` or when detaching late. |
| **Flat Table Limit for Thread-Specific Data (TSD)** | [userspace/libc/pthread.c](file:///home/dmytrom/Documents/GitHub/b1nix/userspace/libc/pthread.c#L404) | 404 | **[Resolved]** | Refactored from a static flat table of 64 threads to a dynamically allocated linked list, removing any hard limit on concurrent threads using TSD. |
| **POSIX Compat Shims in NetSurf's nscompat.c** | [tools/build-netsurf-fb.sh](file:///home/dmytrom/Documents/GitHub/b1nix/tools/build-netsurf-fb.sh#L98) | 98 | **[Resolved]** | The compatibility implementations of `fstatat`, `unlinkat`, and `isascii` have been moved natively to libc, and `scandir` is already natively present. Only math/fenv stubs remain in the build-script shim. |
| **NetSurf Test Pump nanosleep Workaround** | [tools/build-netsurf-fb.sh](file:///home/dmytrom/Documents/GitHub/b1nix/tools/build-netsurf-fb.sh#L321) | 321 | **[Technical Debt]** | Rapidly invoking `gettimeofday` in NetSurf's event loop caused intermittent crashes on x86/i686 targets. The build script patched the loop to sleep via `nanosleep(10ms)` to stabilize test runs. |

## 5. Hardcoded System Limits and Capacities `[Arch Design / Resource Trade-offs]`
Sizing kernel tables statically is a standard microkernel design pattern. It prevents dynamic memory fragmentation and complex allocations in early boot code, at the expense of a hard limit on system capacity:

| Limit / Macro | Source Reference | Description |
|---|---|---|
| `MAX_CPUS 64` | kernel/sched/scheduler.c | Maximum number of SMP processors supported by the kernel GDT/TSS structures. |
| `MAX_TASKS 64` | kernel/sched/scheduler.c | Hard ceiling on the total number of active processes (recently converted from a static array to growable heap slots, but still constrained by indexing). |
| `MAX_BLK_DEVICES 32` | kernel/dev/blk.c | Maximum number of block devices registered in the kernel. |
| `MAX_BLK_PARTITIONS 32` | kernel/dev/blk.c | Maximum partitions tracked per disk. |
| `MAX_MOUNTS 16` | kernel/include/b1nix/vfs.h | Maximum active VFS mounts in the system. |
| `MAX_USERS 16 / MAX_GROUPS 8` | kernel/sched/uidgid.c | Hardcoded limits on credentials and group tracking. |
| `MAX_TCP_CONNS 16` | kernel/net/tcp.c | Maximum simultaneous TCP connections supported by the network stack. |
| `MAX_UDP_BINDINGS 64` | kernel/net/udp.c | Maximum active UDP socket bindings. |
| `ARP_TABLE_SIZE 16` | kernel/net/arp.c | Maximum size of the ARP cache. |
| `MAX_FILE_LOCKS 64` | kernel/fs/filelock.c | Maximum system-wide file locks. |
| `MAX_VFS_PIPES 64` | kernel/fs/pipe.c | Maximum concurrent anonymous/named pipes. |
| `MAX_VIRTIO_BLK 8` | kernel/dev/virtio_blk.c | Maximum VirtIO block devices supported. |
| `MAX_TRACKED_BLOCKS 1024` | kernel/mm/kheap.c | Limit on heap memory leak debug tracker entries. |
| `MAX_PROC_ATTACH 16` | kernel/ipc/shm.c | Max SysV shared memory segments attached per process. |
| `MAX_SYMBOLS 128` | kernel/lib/klog.c | Limit on parsed kernel symbols for backtraces. |
| `BOOTINFO_MAX_MEMORY_REGIONS 32` | kernel/include/b1nix/bootinfo.h | Maximum physical memory segments parsed from Multiboot2/FDT. |
| `ACL_MAX_ENTRIES 8` | kernel/fs/vfs.c | Limit on access control list records. |
| `MAX_EXEC_ARGS 256` | kernel/syscall/syscall.c | Maximum arguments passed to execve(). |
| `MAX_EXEC_ARG_LEN 4096` | kernel/syscall/syscall.c | Maximum individual string size for execve arguments. |

## 6. Stub Functions and Error Returns (ENOSYS / EPERM) `[Mixed]`
Many uniprocessor (UP) lock/IPI vectors are correctly optimized as compile-time no-ops. Others are missing POSIX syscall stubs (like `chroot` or `sigsuspend` on certain architectures) returning `ENOSYS`:

### kernel/arch/x86/interrupts.c
- Line 101: **[Arch Design]** `extern void isr255(void);  /* LAPIC spurious — no-EOI no-op */`

### kernel/arch/x86_64/gdbstub.c
- Line 1: **[Technical Debt]** `/* gdbstub — serial-port GDB remote stub (M36).`
- Line 15: **[Technical Debt]** `* int3 (#BP) exception routes here when the stub is active.`
- Line 155: **[Technical Debt]** `* stub. Mirrors the kernel-text range check used by the backtrace. */`
- Line 172: **[Technical Debt]** `* stub loop and continue execution). */`
- Line 261: **[Technical Debt]** `* halts again. For the stub's single-shot use we ack with S05. */`
- Line 350: **[Technical Debt]** `/* Enter the interactive stub on an exception/breakpoint. Only reached when the`

### kernel/arch/x86_64/interrupts.c
- Line 105: **[Arch Design]** `extern void isr255(void);  /* LAPIC spurious — no-EOI no-op */`
- Line 211: **[Arch Design]** `* each idle AP. Handler is a no-op (just EOI). */`
- Line 434: **[Arch Design]** `* is a pure no-op wake-up, so taking BKL would just add unnecessary`
- Line 464: **[Technical Debt]** `* GDB serial stub when the kernel was booted with b1nix.gdb. Off by default`

### kernel/arch/x86_64/isr.S
- Line 94: **[Arch Design]** `/* Reschedule IPI — vector 0x42 = 66. No-op handler whose only effect is to`
- Line 102: **[Arch Design]** `* triple-fault, so route it to a no-op stub. */`

### kernel/arch/x86_64/paging.c
- Line 399: **[Arch Design]** `* a no-op when g_max_cpus <= 1 so single-CPU boots pay nothing. Issued`

### kernel/dev/blk.c
- Line 64: **[Arch Design]** `* (a no-op if not currently linked, i.e. invalid/uninitialized entry). */`

### kernel/dev/ps2_kbd.c
- Line 82: **[Arch Design]** `* and IRQ1 already on, so the old no-op "worked" there; bare metal (e.g. Acer`

### kernel/dev/virtio_gpu.c
- Line 652: **[Arch Design]** `* virtio-gpu-pci host this is a no-op and emits no markers. */`
- Line 1356: **[Arch Design]** `/* Exercise the accelerated 3D path when the host offers VirGL (no-op on a`

### kernel/fs/vfs.c
- Line 3054: **[Arch Design]** `res = -ENOSYS;` (Standard statfs_cb callback check fallback)
- Line 4324: **[Arch Design]** `/* Detach-from-controlling-tty: accepted as a no-op on the boot console`
- Line 4735: **[Arch Design]** `return -ENOSYS;` (Standard fstatfs_cb callback check fallback)

### kernel/include/b1nix/errno.h
- Line 41: **[Technical Debt]** `#define ENOSYS          38  /* Function not implemented */`

### kernel/include/b1nix/gdbstub.h
- Line 7: **[Technical Debt]** `/* GDB Remote Serial Protocol stub (M36). See kernel/arch/x86_64/gdbstub.c. */`
- Line 29: **[Technical Debt]** `/* Interactive serial stub entry (only when booted with b1nix.gdb). */`

### kernel/include/b1nix/ipi.h
- Line 10: **[Arch Design]** `* single-CPU build short-circuits to a no-op. */`

### kernel/include/b1nix/lapic.h
- Line 95: **[Arch Design]** `/* Reschedule IPI (M28 #6). A no-op handler whose sole purpose is to wake a`

### kernel/include/b1nix/lockdep.h
- Line 9: **[Arch Design]** `* pay zero cost: every entry/exit point becomes a no-op the optimiser drops.`

### kernel/include/b1nix/netdev.h
- Line 77: **[Arch Design]** `* NIC's protocol stack). Emits M37-E1000 markers. No-op if no e1000 device`

### kernel/include/b1nix/sched.h
- Line 303: **[Arch Design]** `/* SMP work-stealing self-test (M24b). No-op unless >1 CPU is online and`

### kernel/include/b1nix/spinlock.h
- Line 7: **[Arch Design]** `* For UP (single-core) builds the lock is a no-op.`

### kernel/include/b1nix/tlb.h
- Line 17: **[Arch Design]** `* Single-CPU builds and pre-SMP boot skip the IPI entirely — no-op.`

### kernel/include/b1nix/usb.h
- Line 18: **[Arch Design]** `* the shared keyboard input ring (via ps2_kbd_handle_byte). Cheap no-op if no`
- Line 30: **[Arch Design]** `* report. No-op if no controller was initialised. */`

### kernel/lib/m36_diag.c
- Line 1: **[Technical Debt]** `/* M36 diagnostic self-test: exercises the GDB serial stub's protocol engine`

### kernel/main.c
- Line 245: **[Arch Design]** `* ACPI. No-op (PIC stays in charge) when no IOAPIC was reported. */`
- Line 539: **[Arch Design]** `/* M24b: verify cross-CPU work-stealing (no-op outside test mode / single CPU) */`
- Line 544: **[Arch Design]** `* stealable-worker window as the self-test; no-op outside test mode / single CPU. */`
- Line 553: **[Technical Debt]** `/* M36: verify the GDB serial-stub protocol engine and ftrace tracer. */`

### kernel/mm/pmm.c
- Line 280: **[Arch Design]** `* the identity-mapped 32-bit port, so this is a no-op there. */`
- Line 587: **[Arch Design]** `* cleared); the inner cli is a no-op. */`

### kernel/net/net.c
- Line 399: **[Arch Design]** `* b1nix.net=dhcp is still accepted as an explicit no-op for back-compat. */`

### kernel/sched/lockdep.c
- Line 3: **[Arch Design]** `* Translation-unit-level no-op when KERNEL_LOCKDEP is not defined — the`
- Line 151: **[Arch Design]** `/* Intentionally no-op: any work here would either contend on a`

### kernel/sched/scheduler.c
- Line 20: **[Arch Design]** `* builds). Defined in kernel/arch/x86_64/tlb.c; fast no-op when nothing pending. */`
- Line 388: **[Arch Design]** `* our own publish would self-deadlock. Resuming ourselves is a no-op`
- Line 542: **[Arch Design]** `* forget no-op handler — safe regardless. */`
- Line 2029: **[Arch Design]** `* reproducible. Treat SLEEPING-at-entry as a no-op-sleep recovery: we're`
- Line 2471: **[Arch Design]** `/* Normally a no-op: the child's exit path already closed and`

### kernel/syscall/syscall.c
- Line 2893: **[Arch Design]** `return (u64)-ENOSYS;` (Standard fallback for network/etc on unsupported architectures)
- Line 3287: **[Arch Design]** `ret = (u64)-ENOSYS;` (Standard fallback for unknown syscall numbers)

### userspace/include/EGL/egl.h
- Line 3: **[Technical Debt]** `* uses are declared; this is a real implementation in b1egl.c, not a stub. */`

### userspace/include/errno.h
- Line 54: **[Technical Debt]** `#define ENOSYS          38      /* Function not implemented */`

### userspace/include/fenv.h
- Line 7: **[Technical Debt]** `* fenv but tolerate a stub. */`

### userspace/include/iconv.h
- Line 10: **[Technical Debt]** `/* iconv stub for B1NIX — character encoding conversion is not supported.`

### userspace/include/net/route.h
- Line 9: **[Arch Design]** `* is accepted by the kernel socket ioctl handler (currently a no-op stub). */`

### userspace/include/pthread.h
- Line 140: **[Technical Debt]** `/* ── Attributes (placeholder, no real knobs honored on b1nix) ── */`

### userspace/include/sys/ioctl.h
- Line 36: **[Arch Design]** `* route mutation (SIOCADDRT/DELRT) is accepted as a no-op. */`

### userspace/include/sys/klog.h
- Line 7: **[Arch Design]** `* and CONSOLE_LEVEL (accepted but a no-op — b1nix has no console loglevel). */`

### userspace/libc/pthread.c
- Line 78: **[Technical Debt]** `* placeholder that exercises the SYS_SET_TLS path end-to-end. */`
- Line 391: **[Arch Design]** `/* ── Attributes (no-op) ── */`
- Line 623: **[Technical Debt]** `return ENOSYS;`
- Line 649: **[Arch Design]** `* clocks — these map to the process equivalents or no-op. */`

### userspace/libc/stdio.c
- Line 357: **[Arch Design]** `* fd streams are unbuffered so flush is a no-op. */`

### userspace/libc/stdlib.c
- Line 38: **[Arch Design]** `* replaces an earlier 16 MB static bump pool whose free() was a no-op: large`

### userspace/libc/unistd.c
- Line 1003: **[Technical Debt]** `* a pty — unlike the old "fd <= 2" placeholder, which mis-reported every`
- Line 1199: **[Resolved]** `int clock_settime(int clk_id, const struct timespec *tp) { ... }` (Now implemented via `SYS_SETTIMEOFDAY`)
- Line 1338: **[Arch Design]** `/* mknod: no SYS_MKNOD syscall — returns ENOSYS; standard device-node creation requires kernel support. */`
- Line 1346: **[Arch Design]** `/* mknod variant: same architectural limitation as above. */`
- Line 1395: **[Arch Design]** `/* lchown on symlinks: b1nix kernel has no lchown(2) syscall variant; returns EPERM (operation blocked by kernel policy). */`
- Line 1458: **[Resolved]** `int lchown(const char *path, uid_t owner, gid_t group) { ... }` (Returns EPERM with clear comment on symlinks instead of misleading ENOSYS; non-symlinks delegate to chown as before.)
- Line 1465: **[Resolved]** `int settimeofday(const struct timeval *tv, const struct timezone *tz) { ... }` (Now implemented via `SYS_SETTIMEOFDAY`)
- Line 1570: **[Resolved]** `int futimens(int fd, const struct timespec times[2]) { ... }` (Now implemented by resolving fd via `/proc/self/fd/<N>` readlink and delegating to `utimensat`.)

## 7. Hacks, Workarounds, and Temporary Code Markers `[Technical Debt]`
Explicit code notes highlighting temporary workarounds or shortcuts inside the implementation:

### kernel/arch/x86/ap_trampoline.S
- Line 15: `/* Load temporary GDT using physical address */`

### kernel/arch/x86_64/interrupts.c
- Line 433: `/* Reschedule IPI (M28 #6): same BKL bypass as TLB shootdown — the handler`

### kernel/dev/e1000.c
- Line 172: `/* Coarse ~1 µs-per-iteration delay via the classic port-0x80 dummy read. */`

### kernel/dev/ps2_kbd.c
- Line 249: `/* Fake Shift used by some set-1 keypad/PrintScreen sequences. */`
- Line 264: `case 0x2A: /* Fake Shift in keypad/PrintScreen sequence. */`

### kernel/fs/initramfs.c
- Line 357: `* actually drops root->user and the switched uid is read back. No fake markers. */`

### kernel/fs/ntfs.c
- Line 190: `* bootstrap read of record 0 a temporary single run is used by the caller. */`
- Line 491: `/* Bootstrap: read $MFT record 0 from a temporary run anchored at mft_lcn,`

### kernel/mm/kheap.c
- Line 152: `* normally (so coalescing still reclaims it). Large allocations bypass the`

### kernel/mm/page_cache.c
- Line 156: `memset(&dummy, 0, sizeof(dummy));`
- Line 157: `dummy.inode = page->inode;`
- Line 171: `page->inode->write_cb(&dummy, page->offset, virt_addr, size, 0);`

### kernel/sched/lockdep.c
- Line 33: `* LOCKDEP_*_GLOBAL helpers bypass the per-CPU acquisition stack and do only`

### kernel/sched/scheduler.c
- Line 1313: `* stack: a fake return address at [esp] and the void* arg at [esp+4]. */`

### kernel/syscall/syscall.c
- Line 574: `* and link a real /tmp/kernel.elf (no fake pass — proven by an actual`

### userspace/libc/netdb.c
- Line 251: `case EAI_AGAIN:    return "Temporary failure in name resolution";`
- Line 264: `case TRY_AGAIN:      return "Temporary name resolution failure";`

## 8. Code Commented Out via `#if 0`
No active `#if 0` blocks were found in the b1nix kernel or userspace source files (excluding third-party code in `userspace/tcc/`). All matches inside `userspace/tcc/` belong to the original, upstream TinyCC codebase and do not constitute b1nix-specific stubs.