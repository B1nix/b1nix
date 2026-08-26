# userspace/include

Two different things live here, and telling them apart matters.

**This kernel's own interfaces** — `b1nix/`, `syscall.h`, `sys/syscall.h`,
`mojo/`, `linux/soundcard.h`, `tui.h`. These describe ioctls, structures and
syscall numbers that exist only on b1nix, the way Linux's `uapi` headers
describe Linux's. Nothing outside this tree can supply them.

**b1cc's C library headers** — everything else: `stdio.h`, `unistd.h`,
`sys/*.h` and the rest of the standard set.

They are NOT a second libc that b1nix maintains, and they are not duplicates
of musl's. Every binary built here compiles against musl (see `CFLAGS` in the
Makefile, which puts musl's include directory first). This set exists because
`b1cc`, our own C compiler, cannot parse musl's headers — its parser stops on
their typedefs — and `B1CC_SYSROOT_INCLUDE` points at this directory.

Deleting them as duplicates breaks `b1cc` on its own corpus. That was tried,
and the failure is not obvious: an incremental build rebuilds nothing that
included them and reports success. Only a build from scratch shows it.
