# Archived: the native b1nix syscall ABI

Not built. Parked here until it is decided whether b1nix gets its own
userspace again (roadmap M121).

## What changed in the kernel

- Every user image is a Linux image. The ELF loader no longer looks at
  `EI_OSABI`, `PT_INTERP` or the GNU ABI note, and `enum user_personality` is
  gone. Linux does not read `EI_OSABI` either: a static musl binary leaves it at
  SYSV with no note, and b1nix used to run such a binary on the native ABI.
- Linux syscall numbers are translated for every task that has a user image.
  Kernel threads keep calling `syscall_dispatch` with b1nix numbers. The
  `SYS_*` enum in `kernel/include/b1nix/syscall.h` is still the kernel's
  internal routing key.
- These native-only paths were removed:
  - the native `siginfo_t` and signal numbering, on both arches;
  - the 16-byte evdev record for user readers;
  - the native `socklen_t` width;
  - the native `wait` status;
  - the native sigreturn trampoline number.

## What is here

| File | Came from |
|---|---|
| `syscall_native_handlers.c` | `sys_*` handlers only native numbers reached: `kernel/syscall/syscall.c` |
| `syscall_native_cases.c` | their `case SYS_*` arms in `syscall_dispatch_impl_inner` |
| `helpers.c` | helpers whose only callers were those handlers (file named per entry) |
| `lib/` | the in-kernel libc wrappers that issued native syscalls, with no caller left |
| `../../tests/native-abi/` | `/bin/m67-rust`, a static `x86_64-unknown-b1nix` Rust program, and its checks |

## Restoring

1. Put the handlers back above `syscall_dispatch_impl_inner`, and the cases
   into its `switch (number)`.
2. Put the helpers back into the files named in `helpers.c`, and `lib/unistd.c`
   back into the `Makefile` source list.
3. Reintroduce a personality in `struct user_loaded_image`, set it in
   `user_load_elf64`, and gate the Linux prelude in `syscall_dispatch_impl_inner`
   on it again.

Everything else is still in `git log` (see the commit that added this
directory).
