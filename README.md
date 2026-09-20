# B1NIX

B1NIX is an experimental Unix-like monolithic kernel written in C11. It boots
through Limine (Multiboot2, BIOS and UEFI) and runs unmodified Linux userspace
through a Linux-compatible syscall ABI. The test images are assembled from
pinned Alpine packages (BusyBox, sway, foot, Mesa, Chromium, ...); a Debian
image checks the glibc side. Graphics and filesystem drivers are imported
unmodified from Linux through an in-tree linuxkpi layer.

> B1NIX is a research and hobby operating system, not a production system.
> Interfaces, disk formats, security behavior and build workflows may change.

Implementation status per milestone is tracked in
[docs/kernel/roadmap.md](docs/kernel/roadmap.md), and the distribution's in [docs/distro/roadmap.md](docs/distro/roadmap.md); [docs/README.md](docs/README.md) indexes both.

## Architectures

| `ARCH=` | Status |
| --- | --- |
| `x86_64` (default) | Primary target: QEMU (KVM) and real hardware |
| `aarch64` | Second target of the same kernel: QEMU `virt`, Raspberry Pi 4, Sony Xperia 5 ([tools/board/sony-xperia-5](tools/board/sony-xperia-5/README.md)); gaps in [docs/kernel/platforms.md](docs/kernel/platforms.md) |

## Host Requirements

Linux with KVM is the supported development host. The build uses a pure
LLVM toolchain; there is no GCC anywhere in it.

- GNU Make, `clang`, `ld.lld`, `llvm-ar`/`llvm-ranlib`, `xxd`, `curl`
- `limine` and `xorriso` for ISOs
- `qemu-system-x86_64` (and `qemu-system-aarch64` for `ARCH=aarch64`)
- `mke2fs` from e2fsprogs

`tests/smoke.sh` also runs on macOS (HVF acceleration, Homebrew's keg-only
e2fsprogs), but it is not the primary host and gets less coverage.

```sh
make check-tools                     # report missing tools
tools/toolchain/build-toolchain.sh   # one-time: musl sysroot, compiler-rt, libc++
```

The toolchain is cached under `build/<arch>/toolchain/`; `make clean` keeps it,
`make distclean` removes it.

## Build And Run

```sh
make                 # kernel only: build/x86_64/kernel.elf
make iso             # bootable ISO: build/x86_64/b1nix.iso
make run             # boot the ISO in QEMU with user-mode networking
make run-graphics    # boot to runlevel 5 on a virtio GPU
make run-root        # boot with the persistent ext4 root image attached
make ARCH=aarch64 run-aarch64
```

Other images:

| Target | Output |
| --- | --- |
| `make root-image` | `build/<arch>/root.img`, persistent root filesystem |
| `make iso-live` | `build/<arch>/b1nix-live.iso`, ISO with a RAM-backed root |
| `make iso-test` | `build/<arch>/b1nix-test.iso`, live image with test mode on |

Root images fetch prebuilt packages by default; `make PORTS_SOURCE=local
root-image` builds the ports locally instead. `ROOT_IMAGE_SIZE=<MiB>` overrides
the image size.

`make iso` produces a hybrid BIOS/UEFI image that can be written to a USB drive
with `dd` (Secure Boot must be off). Hardware coverage is limited.

The development image has `root/root` and `user/user` credentials; do not expose
it to an untrusted network.

## Testing

All testing is integration testing in QEMU. The kernel runs in-kernel self-tests
and smoke binaries when booted with `b1nix.test=1`, and the host scripts grep
the serial log for their markers.

```sh
make smoke            # full suite (sh tests/smoke.sh $(ARCH))
make smoke-quick      # reduced suite
make smoke-b1cc       # in-guest C compiler only
make graphics-smoke
make memory-smoke
make analyze          # clang static analyzer over the kernel
make debian-smoke     # Debian (glibc) userspace on the b1nix kernel
sh tests/ssh-hostfwd.sh x86_64   # SSH into the guest from the host
```

Logs and temporary disk images go to `smoke_run/`. `SMOKE_VERBOSE=1` prints
every assertion; `SMOKE_PCAP=1` captures network traffic.

## Layout

```text
kernel/      kernel core, arch code, drivers, VFS, networking, linuxkpi
tests/programs/   headers, rootfs overlay, b1cc, native programs and smoke tests
boot/        Limine configuration
tools/       toolchain, ports, packaging, image and device scripts
tests/       host-side QEMU test drivers
docs/        roadmap and subsystem notes
```

## License

Original b1nix code is licensed under the
[GNU General Public License, version 2 only](LICENSE). Third-party components
keep their own licenses; imported Linux DRM/i915 sources are taken under their
MIT option. See [docs/licensing.md](docs/licensing.md).
