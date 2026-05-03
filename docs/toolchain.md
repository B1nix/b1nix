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
