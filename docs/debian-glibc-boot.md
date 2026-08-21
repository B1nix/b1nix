# Booting a Debian (glibc) userspace on b1nix

b1nix already ran Alpine binaries, but Alpine's libc is musl — the libc b1nix's
Linux ABI layer was developed against, and therefore a weak test of it. Debian
is built on glibc, which exercises far more of the interface: a different
dynamic loader, `AT_*` auxv entries musl never reads, `clone3`, `prlimit64`,
`rseq`, `AT_EMPTY_PATH`, and the kernel `struct termios` rather than musl's.

Everything below runs unmodified Debian binaries. Nothing in the guest image is
patched to work around a kernel defect; where the guest failed, the kernel was
fixed.

## Running it

```sh
make debian-image     # downloads debian:bookworm-slim once, builds debian.ext4
make debian-smoke     # boots it and checks the markers
```

`tools/images/mk-debian-image.sh` pulls the official `debian:bookworm-slim`
amd64 image layer straight from the registry with `curl`, adds `procps`,
`sysvinit-core` and their dependencies as `.deb` files, and hands the tree to
`mke2fs -d`. It needs the network the first time only, caches everything under
`build/x86_64/debian/`, and is not reachable from `make iso` or `make smoke`.

`tests/debian-smoke.sh` boots the normal b1nix ISO with

```
root=LABEL=b1nix-debian init=/sbin/init
```

and attaches the image as a virtio-blk disk. `DEBIAN_INIT=/b1nix-stage.sh` runs
the harness directly as PID 1 instead of under sysvinit.

`SMOKE_DEBIAN=1 sh tests/smoke.sh` folds the result into the main suite.

## Where it got to

| Stage | Result |
|---|---|
| 1. A single glibc dynamic binary (`/bin/dash`) | runs |
| 2. Distro coreutils: `ls -l`, `cat`, `mount`, `ps`, `id`, `dmesg`, `uname -a` | run |
| 3. An init: Debian's **sysvinit 3.06** as PID 1, driving `/etc/inittab` | runs |

`uname -a` inside the guest:

```
Linux b1nix 6.6.0-b1nix-0.111.0 #1 SMP b1nix x86_64 GNU/Linux
```

systemd was not attempted. It needs cgroup v2 with the full controller set,
`pidfd_open`/`pidfd_send_signal`, `CLONE_PIDFD`, `mount` propagation flags
(`MS_SHARED`/`MS_SLAVE`) and a working `/dev/kmsg` writer per unit — none of
which this kernel has; the shape of the work is a milestone, not a fix.

## The kernel gaps this found, and what they were

1. **`PT_INTERP` recognition was hardcoded to musl.** `elf64_is_linux_binary()`
   matched the literal `/lib/ld-musl-x86_64.so.1`, so a Debian binary naming
   `/lib64/ld-linux-x86-64.so.2` ran with the *native* personality, where
   `arch_prctl(ARCH_SET_FS)` is a different syscall number. Any dynamic loader
   now implies the Linux ABI, which is the truth: b1nix has no interpreter of
   its own.

2. **A script could not be PID 1.** Shebang handling lived only in
   `user_execve_current`; `user_spawn_env` went straight to the ELF loader, so
   `init=` pointing at a shell script died at the ELF magic check (`bad magic
   23 21`). The spawn path now honours `#!` with the same one-hop rule.

3. **`uname` did not report a Linux version.** glibc's loader compares the
   release string against the minimum in a binary's `NT_GNU_ABI_TAG` note and
   aborts with "FATAL: kernel too old" if it loses. The release is now
   `6.6.0-b1nix-<version>` — a string that parses as a Linux version and still
   names the kernel that is running. It is one string (`B1NIX_RELEASE_STR`) for
   `uname`, `/proc/version`, `/proc/sys/kernel/osrelease`, `/sys/kernel/osrelease`,
   `/lib/modules/<release>` and a module's vermagic.

4. **`AT_EMPTY_PATH` was not implemented.** glibc's `fstat()` is
   `newfstatat(fd, "", &st, AT_EMPTY_PATH)`. The empty path was appended to the
   descriptor's own path, producing `…/libc.so.6/` and `-ENOTDIR`: every library
   the loader opened failed with "cannot stat shared object". `fstat` on a
   descriptor is now answered from the descriptor.

5. **`waitpid(-1)` never matched anything.** glibc emits `mov $-1, %edi`, so the
   register holds `0x00000000ffffffff`; the kernel read it as 64 bits and looked
   for pid 4294967295. Every command a Debian shell ran reported status 255.
   The pid argument of `wait4`, `waitid` and `kill` is now read as an `int`, as
   Linux's `SYSCALL_DEFINE` does. musl widens to `long`, which is why this was
   invisible for years.

6. **`fork` failed with EFAULT.** glibc's fork is
   `clone(CLONE_CHILD_SETTID|CLONE_CHILD_CLEARTID|SIGCHLD, NULL, …)`, and
   `scheduler_clone_thread` — which only ever made threads — rejected the NULL
   stack. A `clone` without `CLONE_VM` and without a stack is now a fork, and
   the child's tid is written into the child's copy of `*child_tid`
   (`CLONE_CHILD_SETTID`): staged in the parent before the address space is
   copied, then restored afterwards, which takes the COW fault and separates the
   two copies. Without it a forked child keeps its parent's cached tid, and
   `raise()` in the child signals the parent.

7. **`TCGETS` smashed the caller's stack.** The kernel wrote its own 48-byte
   `struct b1nix_termios` into the buffer glibc passes, which is a 36-byte
   `struct __kernel_termios` — 12 bytes past the end, reported as
   `*** stack smashing detected ***` by every coreutil that checked whether
   stdout was a terminal. Every tty driver now converts through one wire layout
   (`kernel/include/b1nix/termios_abi.h`), which is Linux's, so there is a
   single answer to "what does a termios look like to userspace".

8. **`fstat` and `lseek` did not work on pipes.** A handle with no `vfs_node`
   fell through to a node lookup that could only fail, so `cmd | tail` reported
   "cannot fstat 'standard input': Bad file descriptor" and `head` took `EBADF`
   from `lseek` as a broken descriptor. Pipes, sockets and the anonymous objects
   (eventfd, timerfd, signalfd, epoll, inotify) now `fstat` as what they are,
   and a seek on one is `ESPIPE`.

9. **`/dev/console` discarded everything written to it.** The node was created
   with no read/write callbacks at all. Debian's sysvinit, whose entire output
   goes to `/dev/console`, booted the machine in complete silence. It now gets
   the same callbacks as `/dev/tty`, at boot and after a root switch.

10. **`clone3`, `prlimit64` (set) and `epoll_pwait2`** returned `-ENOSYS` or
    were half-implemented. glibc falls back for all three, so none of them was
    load-bearing, but each cost a failed syscall on every start-up and left the
    newer interfaces untested.

11. **`AT_PLATFORM`, `AT_HWCAP2`, `AT_FLAGS`, `AT_MINSIGSTKSZ`** were missing
    from the auxv. `getauxval` returning 0 for `AT_MINSIGSTKSZ` is a
    legal-looking wrong answer, so it is now stated explicitly.

## What is still missing

- No vDSO. glibc falls back to real syscalls for `clock_gettime`/`getpid`, so
  everything works and is slower than it needs to be.
- `getdents64` on a directory being modified concurrently is not tested here.
- `mount(8)` from util-linux reports the root twice (once as ext2, once as
  ext4): the kernel's mount table keeps both entries for the same device.
- No getty and no login: the harness owns the console.
