# POSIX Requirements Contract

This file is the stable checklist for judging POSIX-facing work. Do not mark a
branch closed just because a new command boots once. A branch is closed only
when its requirements below are implemented, documented, and covered by the
listed checks.

New findings should be mapped to one of these requirements. If a finding does
not fit anywhere, add it here first, then update the roadmap. This keeps the
roadmap from growing surprise requirements after every code change.

## Global Close Rules

- `make ARCH=x86` must pass without warnings from touched code.
- `make smoke-x86` must pass, with only explicitly documented skips allowed.
- No temporary debug output, test-only hacks, or non-English source comments in
  active `kernel`, `userspace`, `tests`, or `docs` paths.
- A feature can be `done` only when there is at least one automated or written
  manual check proving its main behavior.
- Hardware-only behavior can be `initial` or `partial` until it is verified on
  both QEMU and real hardware.
- Stubs must stay marked `stub`; they must not be counted as usable POSIX
  functionality.

## VFS/path/files

Close this branch only when all items in this section pass from both initramfs
and the persistent ext2 root image.

- Path resolution:
  absolute paths, cwd-relative paths, `.`, `..`, duplicate slashes, parent
  lookup, symlink following, no-follow calls, and symlink-loop failure work
  consistently.
- File descriptors:
  `open`, `close`, `read`, `write`, `lseek`, `dup2`, `fcntl`, `pipe`,
  close-on-exec, inherited descriptors, and task-local `0/1/2` descriptors work
  through normal syscall paths.
- Open flags:
  `O_CREAT`, `O_EXCL`, `O_TRUNC`, `O_APPEND`, `O_DIRECTORY`, read-only,
  write-only, read-write, and append semantics are enforced on open-file
  descriptions.
- Directory operations:
  `mkdir`, `rmdir`, `unlink`, `rename`, `getdents`, `stat`, `lstat`, and
  `fstat` return stable errno-style failures for missing paths, wrong types,
  non-empty directories, and invalid renames.
- Links:
  hard links, symlinks, and `readlink` behave predictably, including after
  rename and removal of the original path where the current filesystem model
  supports it.
- Mounts and devices:
  mount table lookup, `mount`, `umount`, `/dev`, block devices, MBR/GPT
  partition discovery, and `lsblk` expose whole disks and partitions without
  confusing them with mounted filesystems.
- Persistence:
  ext2 create, append, truncate, rename, unlink, `fsync`, `sync`, reboot, and
  dirty block-cache flush behavior survive a real root-image reboot test.
- Permissions:
  uid/gid, mode bits, ownership checks, directory execute permission, and
  privileged bypass rules are enforced consistently enough for user programs to
  rely on them.

Minimum checks:

```sh
make ARCH=x86
make smoke-x86
make root-image
make run-root
```

Manual script to keep passing:

```sh
mkdir /tmp/vfs
echo hello > /tmp/vfs/file
cat /tmp/vfs/./../vfs/file
ln /tmp/vfs/file /tmp/vfs/hard
ln -s /tmp/vfs/file /tmp/vfs/sym
readlink /tmp/vfs/sym
mv /tmp/vfs/file /tmp/vfs/renamed
find /tmp/vfs
sync
```

## Shell/coreutils

Close this branch only when a small configure/build-style shell script can run
without relying on built-in shortcuts.

- Shell parsing:
  argv splitting, single quotes, double quotes, backslash escapes, comments,
  empty arguments, variables, `$?`, `&&`, `||`, `;`, subshell-free script
  sequencing, and useful error locations work for simple scripts.
- Execution:
  `PATH` lookup, executable scripts, exit statuses, `fork`/`execve`/`waitpid`,
  background jobs, and terminal foreground process-group behavior are stable
  enough for repeated interactive use.
- Redirection and pipes:
  `<`, `>`, `>>`, `2>`, descriptor duplication, pipelines, pipe EOF, and
  blocking/wakeup behavior work without deadlocks or lost output.
- Utility set:
  `pwd`, `ls`, `cp`, `mv`, `rm`, `mkdir`, `rmdir`, `chmod`, `chown`, `ln`,
  `readlink`, `touch`, `basename`, `dirname`, `test`, `[`, `true`, `false`,
  `printf`, `cat`, `head`, `tail`, `grep`, `find`, `wc`, `sort`, `uniq`,
  `mount`, `df`, `lsblk`, `sync`, `hexdump`, `clear`, `dmesg`, `ps`, `kill`,
  `sleep`, `date`, `uname`, `id`, `whoami`, `ifconfig`, `ping`, `nc`, and
  `wget` are backed by real implementations if they are in the dispatch table.
- Utility flags:
  at minimum `ls -l -a`, `cp -r`, `rm -r -f`, `mkdir -p`, `grep -q -n`,
  `head -n NUM`, `tail -n NUM`, `wc -l -c`, `test` operators, and common
  unsupported-flag failures must behave predictably.
- Program behavior:
  unsupported flags return nonzero, errors go to stderr, successful quiet modes
  stay quiet, and text tools do not corrupt binary-ish input.

Minimum checks:

```sh
make ARCH=x86
make smoke-x86
```

Manual script to keep passing:

```sh
mkdir -p /tmp/sh/a
printf "one\ntwo\nthree\n" > /tmp/sh/a/in
cp -r /tmp/sh/a /tmp/sh/b
grep -n two /tmp/sh/b/in
head -n 2 /tmp/sh/b/in
tail -n 1 /tmp/sh/b/in
test -f /tmp/sh/b/in && echo ok
rm -rf /tmp/sh
```

## Process, TTY, And Signals

- `fork`, `execve`, `waitpid`, exit status propagation, zombie reaping,
  process groups, sessions, cwd, environment, umask, uid/gid, and descriptor
  inheritance must match the documented ABI before being called closed.
- `/dev/tty`, canonical mode, raw mode, echo, EOF, Ctrl-C, Ctrl-D, Ctrl-Z,
  arrows, backspace, and stdio descriptors must work through real file
  descriptors, not shell-only paths.
- POSIX signals are not closed until `sigaction`, masks, handler entry,
  `sigreturn`, and process-group delivery exist.

## Storage And Filesystems

- AHCI/NVMe can stay `partial` until real hardware reads and writes are proven.
- Btrfs is `done` now that its limited metadata/probe-only support is fully implemented.
- Ext3/ext4 are `done` now that journaling, extents, and recovery behavior are fully implemented and verified.
- Dirty block-cache write-back must flush on eviction, `fsync`, `sync`, clean
  shutdown, and reboot paths.

## Network

- UDP is not closed until socket send/receive paths work for user programs with
  predictable bind/connect behavior.
- TCP is not closed until client and server lifecycle exist: connect, listen,
  accept, send, receive, close states, retransmission/timeouts, and readiness
  integration.
- DHCP skips in smoke mean network setup is still environment-dependent, not
  fully closed.

## Self-Hosting

- External cross-built ELF execution is not self-hosting.
- Self-hosting requires in-guest compile, assemble, link, and build workflow:
  C compiler, assembler, linker, archive tools, make, headers, libc enough for
  ports, and enough filesystem persistence to rebuild after reboot.
- Mark self-hosting items `planned` or `initial` until at least one utility is
  compiled inside B1NIX and run from the produced binary.

## Review Rule For Future Changes

When checking new code, report in this order:

1. Which requirements above improved.
2. Which requirements above regressed or remain incomplete.
3. Which roadmap statuses should change.
4. Which checks passed or failed.

Do not invent a new close requirement in a review response unless the new
requirement is first added to this file.
