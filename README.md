# B1NIX

B1NIX is an experimental Unix-like monolithic operating system written mostly
in C. It boots through Multiboot2, runs native ELF programs in ring 3, provides
its own kernel, libc, shell, filesystems, network stack, drivers, and native
development toolchain, and can rebuild its kernel from inside B1NIX.

The primary target is `x86_64`. A separate 32-bit `i686` port is also actively
built and tested. The old AArch64 experiment is archived and is not part of the
current build.

> B1NIX is a research and hobby operating system, not a production system.
> Interfaces, disk formats, security behavior, and build workflows may change.

## Current Capabilities

- Multiboot2 boot through GRUB on BIOS and UEFI systems.
- 64-bit and 32-bit x86 kernels selected with `ARCH=x86_64` or `ARCH=x86`.
- Preemptive SMP scheduling, per-CPU state, process groups, job control,
  signals, futexes, pthreads, and copy-on-write `fork()`.
- Native ELF32/ELF64 userspace with isolated page tables, `mmap`, shared file
  mappings, PIE loading, core dumps, and a POSIX-oriented syscall ABI.
- VFS with dynamic descriptor tables, pipes, PTYs, file locking, AIO,
  `/proc`, `/sys`, initramfs, a page cache, and persistent root filesystems.
- Read/write ext2, ext3, and ext4; FAT32 and ext1 support; read-only ISO9660,
  exFAT, NTFS, and Btrfs metadata probing.
- VirtIO block, network, and GPU; AHCI, NVMe, Intel e1000/e1000e, Realtek
  r8169, PS/2 input, xHCI USB keyboard and mass-storage paths.
- IPv4 and IPv6, ARP, NDP, ICMP, UDP, TCP, Unix sockets, DHCP, DNS, NTP,
  `select`/`poll`, and passive TCP services.
- Framebuffer console, a small compositor, a text editor, and a two-panel file
  manager.
- A shell with pipelines, redirection, scripts, globbing, command and
  arithmetic substitution, here-documents, functions, loops, `case`, arrays,
  traps, and foreground/background job control.
- Native utilities plus curl, GNU Wget, Dropbear SSH, TinyCC, and an optional
  upstream BusyBox 1.38.0 port.
- Ported binutils, GCC, libstdc++, and GNU Make. The x86_64 kernel can be
  compiled and linked from inside B1NIX.
- Automated QEMU coverage for boot, SMP, memory, storage, filesystems,
  networking, SSH, graphics, libc, shell, and userspace behavior.

See [docs/roadmap.md](docs/roadmap.md) for the detailed implementation status.

## Quick Start

### Requirements

The basic build expects:

- GNU Make
- Clang
- LLVM `ld.lld`, `llvm-ar`, and related tools
- `xxd`
- GRUB `grub-mkrescue` or `grub2-mkrescue`
- `xorriso`
- QEMU `qemu-system-x86_64`
- `mke2fs` from e2fsprogs
- curl or Wget for downloading third-party source archives

On macOS with Homebrew:

```sh
brew install llvm lld qemu grub xorriso e2fsprogs
export PATH="/opt/homebrew/opt/llvm/bin:$PATH"
```

Check the detected tools:

```sh
make check-tools
```

### Build And Run

Build the default x86_64 kernel:

```sh
make
```

The result is:

```text
build/x86_64/kernel.elf
```

Build an ISO:

```sh
make iso
```

Boot it in QEMU with a persistent ext4 root disk and user-mode networking:

```sh
make run-x86_64
```

The first complete build downloads and cross-builds several userspace
components, so it is substantially slower than an incremental kernel build.

For the 32-bit port:

```sh
make ARCH=x86 iso
make ARCH=x86 run-x86_64
```

Build output is architecture-qualified under `build/x86_64/` or `build/x86/`.

## Image Types

| Target | Output | Purpose |
| --- | --- | --- |
| `make iso` | `build/<arch>/b1nix.iso` | Kernel and built-in initramfs |
| `make root-image` | `build/<arch>/root.ext4` | 512 MiB persistent root image |
| `make iso-live` | `build/<arch>/b1nix-live.iso` | ISO with a RAM-backed ext4 root image |
| `make iso-test` | `build/<arch>/b1nix-test.iso` | Live image with the test mode enabled |
| `make iso-full` | `build/<arch>/b1nix-live.iso` | Full live-image workflow |

Override the root image size when needed:

```sh
make ROOT_IMAGE_SIZE=1024 root-image
```

## Using B1NIX

A normal boot starts `/bin/init`, runs `/etc/rc`, starts networking when a NIC
is present, launches Dropbear bound to loopback, and opens the B1NIX shell.

Useful commands include:

```text
help        b1fetch     uname       dmesg      meminfo
ls          cat         grep        find       mount
ps          top         free        sysctl     ifconfig
ping        nc          curl        wget       tcc
mc          ne          shutdown    reboot
```

TinyCC is installed as `/bin/tcc`. When a persistent root image is attached,
headers, `crt0.o`, and `libb1nix.a` are installed under `/include` and `/lib`.
Work under `/persist` to retain files across boots.

Common kernel command-line options:

| Option | Effect |
| --- | --- |
| `b1nix.single` | Start an emergency root shell |
| `b1nix.login` | Start the login prompt instead of a direct shell |
| `b1nix.ui=1` | Start the two-panel UI |
| `b1nix.nographics` | Force the text console |
| `init=/path` | Override the program launched by init |
| `b1nix.net=off` | Disable networking |
| `b1nix.ssh-external` | Bind SSH to all interfaces instead of loopback |
| `b1nix.ssh-no-root` | Disable SSH root login |
| `b1nix.ssh-pubkey-only` | Disable SSH password authentication |
| `b1nix.gdb` | Wait for the serial GDB remote stub on a breakpoint |
| `root=LABEL=name` | Select a root block device by filesystem label |

The development image currently includes `root/root` and `user/user`
credentials for login and SSH testing. Do not expose it to an untrusted
network without changing the credentials and SSH policy.

## SSH From The Host

The automated host-to-guest test builds an externally reachable image,
forwards host port 2222, logs in, and executes a command:

```sh
sh tests/ssh-hostfwd.sh x86_64
```

It requires the host OpenSSH client, `nc`, and `expect`. For a manual run:

```sh
make ARCH=x86_64 KERNEL_CMDLINE="b1nix.ssh-external=1" iso

qemu-system-x86_64 \
  -cdrom build/x86_64/b1nix.iso \
  -serial stdio -display none \
  -netdev user,id=n0,hostfwd=tcp:127.0.0.1:2222-:22 \
  -device virtio-net-pci,netdev=n0
```

Then connect from another terminal:

```sh
ssh -p 2222 root@127.0.0.1
```

## Testing

Run the main boot and subsystem suite:

```sh
make smoke
```

Other useful targets:

```sh
make smoke-x86_64
make smoke-x86
make graphics-smoke
make memory-smoke
make analyze
make smoke
```

Smoke logs, packet captures, and temporary disk images are written to
`smoke_run/`. The default output follows milestone markers live while QEMU is
running. Set `SMOKE_VERBOSE=1` to also print every post-run assertion:

```sh
SMOKE_VERBOSE=1 make smoke
```

## Native Toolchain And Self-Hosting

Build the cross and in-guest toolchains:

```sh
tools/build-toolchain.sh
tools/build-native-toolchain.sh
```

For i686, set `B1NIX_ARCH=x86` for both commands. The toolchain build is large
and is cached under `build/toolchain_build/`; `make clean` preserves it, while
`make distclean` removes it.

The root-image workflow installs the matching native toolchain when available
and stages the B1NIX source tree at:

```text
/usr/src/b1nix
```

Detailed ABI and self-hosting notes are in [docs/abi.md](docs/abi.md) and
[docs/m26-selfhost.md](docs/m26-selfhost.md).

## Real Hardware

`make ARCH=x86_64 iso` produces a hybrid BIOS/UEFI image that can be written
directly to a USB drive:

```sh
sudo dd if=build/x86_64/b1nix.iso of=/dev/sdX bs=4M conv=fsync status=progress
```

This destroys the selected drive. Secure Boot must be disabled.

The boot path, framebuffer, e1000-family NIC, AHCI/NVMe, and xHCI keyboard
drivers are designed for physical x86_64 machines, but hardware coverage is
still limited and some controller-specific quirks remain unverified. Read
[docs/m37-real-hardware.md](docs/m37-real-hardware.md) before trying it.

## Project Layout

```text
kernel/             kernel core, architecture code, drivers, VFS, networking
userspace/          libc, headers, crt, native programs, and TinyCC
boot/               GRUB configuration
tools/              build, porting, packaging, and self-hosting tools
tests/              QEMU smoke and persistence tests
docs/               roadmap, ABI, porting notes, and subsystem documentation
archive/            inactive architecture experiments
graphify-out/       generated project knowledge graph
build/              generated artifacts and downloaded upstream sources
smoke_run/          generated test logs, captures, and temporary images
```

## Known Limitations

- B1NIX is not fully POSIX conformant and does not run Linux binaries.
- The shell and libc support substantial real workflows but remain incomplete.
- PIE/`ET_DYN` binaries and relative relocations work, but a full userspace
  dynamic linker with `DT_NEEDED`, GOT/PLT, and cross-module symbol resolution
  is not implemented. Linux ABI compatibility is also not implemented.
- The upstream BusyBox migration is optional and native `/bin` commands remain
  the default.
- The ext-family drivers cover the tested B1NIX workflows but are not complete
  replacements for Linux filesystem implementations. Generated development
  images use a conservative ext4 feature set; exFAT and NTFS are read-only.
- The 32-bit port currently uses at most 1 GiB of RAM; x86_64 has been tested
  with a 16 GiB QEMU memory map.
- Audio, a configurable SysV-style mode for the native B1NIX init, multiple
  virtual consoles, Wi-Fi, and general USB device support are not implemented.
- Security hardening has not reached production quality.

## Documentation

- [Roadmap and status](docs/roadmap.md)
- [Toolchain setup](docs/toolchain.md)
- [Userspace ABI](docs/abi.md)
- [Architecture porting guide](docs/porting-guide.md)
- [POSIX requirements](docs/posix-requirements.md)
- [BusyBox port](docs/busybox-port.md)
- [External SSH](docs/m32c-external-ssh.md)
- [Diagnostics and tracing](docs/m34-m36-diagnostics.md)
- [Real-hardware notes](docs/m37-real-hardware.md)

## License

Original B1NIX code is licensed under
[GPL-3.0-or-later](LICENSE). The original B1NIX userspace C library, headers,
and C runtime are licensed under
[LGPL-3.0-or-later](LICENSE.LGPL-3.0), allowing applications under other
licenses to use the library subject to the LGPL conditions.

See [LICENSING.md](LICENSING.md) for the exact scope. TinyCC and all other
third-party components retain their own licenses; see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
