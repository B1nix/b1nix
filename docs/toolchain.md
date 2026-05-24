# Toolchain

The kernel is built as a freestanding ELF image. On macOS, Apple `ld` cannot
link this target, so install LLVM tools that include `ld.lld`.

## Required

- `clang`
- `ld.lld`
- `make`

## Optional for Running

- `grub-mkrescue`
- `xorriso`
- `qemu-system-x86_64`

## macOS Notes

With Homebrew, the usual packages are:

```sh
brew install llvm lld qemu grub xorriso
```

Depending on the shell environment, LLVM may live outside `PATH`. For example:

```sh
export PATH="/opt/homebrew/opt/llvm/bin:$PATH"
```

After that:

```sh
make check-tools
make
make run-x86
```

Homebrew GRUB may expose `i686-elf-grub-mkrescue` instead of
`grub-mkrescue`; the Makefile accepts either name.

## M17 Target ABI

B1NIX now exposes an initial POSIX-style syscall ABI for the future
`x86_64-b1nix` toolchain target:

- process: `fork`, `execve`, `waitpid`
- file I/O: `stat`, `lseek`, `unlink`, `mkdir`, `chdir`, `getdents`
- descriptors: `pipe`, `dup2`, `fcntl`
- memory: `mmap`, `munmap`, `brk`
- sockets: `socket`, `bind`, `connect`, `send`, `recv`
- terminal: `ioctl`, `tcgetattr`, `tcsetattr`

The in-kernel `/bin/selfhost` utility reports the target manifest through
`SYS_SELFHOST_STATUS`. The ABI and manifest are ready for a cross toolchain,
but a real GCC/binutils port and a full in-guest kernel rebuild are still
tracked as open M17 work.

## M18 Userspace Image ABI

B1NIX now accepts ELF64 executable images from VFS. The loader validates the ELF
header, copies PT_LOAD segments into per-process image state, allocates a
separate user address-space record, and builds an initial stack with `argc`,
`argv`, `envp`, plus basic auxiliary vector entries. `execve()` routes through
the same image loader and exits the replaced image with the loaded program's
status.

The current execution substrate is still the cooperative kernel-thread model,
so the first ELF payload format is a tiny `B1NXEXEC` trampoline used for boot
and smoke testing. This keeps `/bin/init` and `/bin/hello` loaded from VFS while
leaving hardware ring transitions and real address-space switching for the next
memory-management milestones.

Run the M18 boot smoke manually with:

```sh
make smoke-m18
```

The expected serial log contains `user: enter /bin/init` followed by
`Hello from VFS-loaded ELF`.

## M20 Terminal ABI

The kernel now exposes `/dev/tty` through VFS and initializes file descriptors
`0`, `1`, and `2` to that device during VFS setup. `read(0, ...)` and the legacy
keyboard syscall both pass through the TTY line discipline, while `ioctl`,
`tcgetattr`, and `tcsetattr` read and update the terminal's `termios` state.

The shell uses `PATH` lookup for external commands and wires simple pipelines
with real `pipe()` and `dup2()` descriptors. It also supports `<`, `>`, `>>`,
`2>`, and `2>&1` redirection forms.

## M19 Process Descriptor ABI

Each task now owns a descriptor table that maps fd numbers to VFS open-file
descriptions. The VFS handle layer keeps refcounts, so inherited descriptors,
`dup2()`, pipes, sockets, and TTY descriptors can be closed independently by
each process. `FD_CLOEXEC` is honored during `execve()`, and all remaining
descriptors are released when a task exits. The boot task initializes
`stdin`, `stdout`, and `stderr` to `/dev/tty`; child tasks inherit those
descriptors normally.

`waitpid()` supports `B1NIX_WNOHANG`, and exited children stay as zombies until
their parent reaps them. Process metadata now tracks cwd, environment storage,
umask, process group, and session ids. Forked tasks get separate page tables,
copy-on-write private mappings, and copied process metadata plus fd state.

## M21 Root Filesystem

The VFS now has a mount table plus `mount()`, `umount()`, and `sync()` syscalls.
The syscall ABI also includes `rename()`, `rmdir()`, `fstat()`, `fsync()`, and
the initial open flags `O_CREAT`, `O_TRUNC`, `O_APPEND`, and `O_DIRECTORY`.
`sync()` and `fsync()` flush the block cache.

Create a seeded ext2 root image for local testing with:

```sh
make ARCH=x86 root-image
```

This writes `build/x86/root.ext2` with `/bin`, `/etc`, `/dev`, `/home`, `/tmp`,
and `/var`. Boot it as the root filesystem with:

```sh
make ARCH=x86 run-root
```

When the image is attached as `virtio-blk0`, ext2 mounts at `/` and overlays the
initramfs fallback files. The fallback keeps the built-in `/bin/init` available
until a complete disk userland is installed.

## Smoke Tests & Test Artifacts

To prevent cluttering the repository root and build directories, all artifacts, logs, temporary files, and disk images generated during smoke tests MUST be placed in the dedicated `smoke_run/` directory at the project root.

The `smoke_run/` directory is git-ignored and contains:
- `sata-smoke-*.img`, `nvme-smoke-*.img`, `swap-smoke-*.img`, `disk.img` (temporary drive images used in testing)
- `b1nix-smoke-boot.log`, `b1nix-graphics-smoke.log` (execution logs capturing serial output)
- `net.pcap` (network packets dump generated during network smoke testing)
- `ext3-persist/` (directory containing crash/recovery iterations logs)

Any newly added test script or testing procedure must write all of its temporary files, output files, or logs strictly into the `smoke_run/` folder.
