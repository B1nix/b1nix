# POSIX Branch Closeout Plan

This document describes the next practical work for two important POSIX-facing
branches:

- `VFS/path/files`
- `Shell/coreutils`

The goal is not to chase every POSIX corner case at once. The goal is to make
each branch predictable enough that real programs can trust the filesystem,
path handling, shell execution, and common utilities.

## Status Legend

- `done`: usable and covered by a build or smoke path.
- `initial`: first implementation exists, but it needs compatibility work.
- `partial`: important behavior exists, but there are known semantic gaps.
- `planned`: not implemented yet.

## Branch: VFS/path/files

### Current Base

- `done` Per-process file descriptor tables exist.
- `done` `stdin`, `stdout`, and `stderr` are real task-local descriptors.
- `done` `open`, `read`, `write`, `close`, `lseek`, `stat`, `lstat`,
  `fstat`, `getdents`, `unlink`, `mkdir`, `rmdir`, `rename`, `fsync`, and
  `sync` are wired.
- `done` Path resolution handles cwd, dot, dot-dot, duplicate slashes,
  and common syscall entry paths centrally in VFS.
- `done` Symlinks and `readlink` exist and are resolved in VFS.
- `done` Hard-link aliases exist in the in-memory VFS model.
- `done` Mount table, `mount`, `umount`, and mount listing exist.
- `done` Permissions, ownership, ACL metadata, and credential checks are consistent.
- `done` Persistent ext2 read/write support is robust.
- `stub` Btrfs is metadata/probe-only and should not be treated as a POSIX
  filesystem yet.

### Target

Close this branch when path and file behavior is boring in the good way:
programs can create, open, rename, remove, list, seek, link, symlink, and stat
files through absolute and relative paths without depending on shell built-ins
or initramfs shortcuts.

### Implementation Order

1. Centralize path resolution in VFS.

   Files to touch:

   - `kernel/fs/vfs.c`
   - `kernel/include/b1nix/vfs.h`
   - `kernel/syscall/syscall.c`

   Work:

   - Move duplicated syscall-side path cleanup toward one VFS-owned resolver.
   - Resolve paths using `cwd` for relative paths.
   - Preserve a no-follow mode for `lstat`, `readlink`, and symlink creation.
   - Add a symlink-follow limit, for example 16 hops, and return an error on
     loops.
   - Normalize before parent lookup so `mkdir /tmp/a/../b`, `open ./x`, and
     `unlink /tmp//x` behave the same as their canonical path.

   Acceptance:

   - `cd /tmp; mkdir a; touch a/f; cat ./a/../a/f` works.
   - `ln -s /tmp/a/f /tmp/link; cat /tmp/link` works.
   - `readlink /tmp/link` returns the stored target, not the resolved target.
   - A symlink loop fails cleanly instead of hanging.

2. Make VFS errors consistent.

   Files to touch:

   - `kernel/fs/vfs.c`
   - `kernel/include/b1nix/errno.h`
   - `kernel/syscall/syscall.c`
   - `userspace/include/errno.h`

   Work:

   - Stop returning plain `-1` from VFS paths where the reason is known.
   - Use stable negative errno values: `-ENOENT`, `-EEXIST`, `-ENOTDIR`,
     `-EISDIR`, `-ENOTEMPTY`, `-ELOOP`, `-EINVAL`, `-EACCES`, `-EXDEV`.
   - Keep syscall ABI behavior consistent with userspace libc expectations.

   Acceptance:

   - `rm /missing` reports not found.
   - `rmdir /tmp/nonempty` fails with not-empty semantics.
   - `cat /tmp` fails as a directory, not as an unknown generic failure.
   - Utility messages can distinguish missing path, wrong type, and denied
     permission.

3. Harden directory operations.

   Files to touch:

   - `kernel/fs/vfs.c`
   - `kernel/fs/ext2.c`
   - `kernel/user/busybox.c`

   Work:

   - Reject unlinking directories through `unlink`.
   - Reject removing non-empty directories through `rmdir`.
   - Make `rename` handle same-directory and cross-directory moves.
   - Decide and document whether cross-mount rename returns `-EXDEV`.
   - Ensure `getdents` includes stable `.` and `..` behavior or document why it
     is intentionally omitted for now.

   Acceptance:

   - `mkdir /tmp/a; touch /tmp/a/f; rmdir /tmp/a` fails.
   - `rm /tmp/a` fails if `a` is a directory.
   - `mv /tmp/a/f /tmp/g` preserves file content.
   - `find /tmp` does not get confused by renamed directories.

4. Make open flags closer to POSIX.

   Files to touch:

   - `kernel/fs/vfs.c`
   - `kernel/include/b1nix/posix.h`
   - `userspace/include/fcntl.h`
   - `userspace/libc/unistd.c`

   Work:

   - Complete `O_CREAT`, `O_TRUNC`, `O_APPEND`, `O_DIRECTORY`, and add
     `O_EXCL` if missing.
   - Store readable/writable mode on open-file descriptions.
   - Reject reads from write-only handles and writes to read-only handles.
   - Make append writes ignore the current offset and always write at EOF.

   Acceptance:

   - `echo a > /tmp/f; echo b >> /tmp/f; cat /tmp/f` prints both lines.
   - `open(path, O_CREAT | O_EXCL)` fails when the file already exists.
   - Opening a normal file with `O_DIRECTORY` fails.

5. Make filesystem-backed metadata durable enough for real use.

   Files to touch:

   - `kernel/fs/ext2.c`
   - `kernel/dev/blk.c`
   - `kernel/fs/journal.c`

   Work:

   - Flush inode size, block bitmap, inode bitmap, and directory entry updates
     on `fsync` and `sync`.
   - Add focused ext2 tests for create, truncate, append, rename, unlink, and
     reboot persistence.
   - Treat Btrfs as detect/list/mount metadata-only until it has real tree
     parsing and file reads.

   Acceptance:

   - A file created on the ext2 root image remains after reboot.
   - A renamed file remains under the new name after reboot.
   - Deleted files do not reappear after `sync; reboot`.

### VFS Test Checklist

Run at least:

```sh
make ARCH=x86
make smoke-x86
```

Inside B1NIX, manually exercise:

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

Close `VFS/path/files` only after the same script works from initramfs and from
the persistent ext2 root image.

## Branch: Shell/coreutils

### Current Base

- `done` Shell command loop exists.
- `done` `PATH` lookup exists.
- `done` Pipes use real `pipe()` and `dup2()`.
- `done` Redirection supports `<`, `>`, `>>`, `2>`, and descriptor duplication.
- `done` Basic environment variables exist.
- `done` BusyBox-style dispatch exists for many utilities.
- `done` Existing utilities include `pwd`, `ls`, `cp`, `mv`, `rm`, `mkdir`,
  `rmdir`, `chmod`, `chown`, `ln`, `readlink`, `ps`, `kill`, `sleep`, `date`,
  `uname`, `id`, `whoami`, `cat`, `head`, `tail`, `grep`, `find`, `wc`,
  `sort`, `uniq`, `mount`, `df`, `lsblk`, `sync`, `hexdump`, `clear`, `dmesg`,
  `ifconfig`, `ping`, `nc`, and `wget`.
- `done` Utility flags are intentionally narrow, but core flags for ls, cp, rm, mkdir, and grep are supported.
- `done` Interactive shell-driven smoke tests runner exists.
- `done` Background jobs and basic job control are complete.

### Target

Close this branch when a small configure/build-style script can run without
special casing B1NIX: it should be able to use quoting, variables, redirection,
pipes, exit statuses, and common file/text utilities.

### Implementation Order

1. Split shell parsing from execution.

   Files to touch:

   - `kernel/user/programs.c`
   - optionally `kernel/user/shell.c` if the shell becomes large enough to
     deserve its own file.

   Work:

   - `done` Add a token structure for words, operators, quotes, and redirects.
   - `done` Preserve quoted spaces for single and double quotes.
   - `done` Implement backslash escaping for the common interactive cases.
   - `done` Keep execution as a second phase: argv, redirections, pipeline, status.

   Acceptance:

   - `echo "hello world"` prints one argument.
   - `echo 'hello world'` prints one argument.
   - `echo a\ b` prints one argument containing a space.
   - Syntax errors do not corrupt the next prompt.

2. Make exit status and conditionals useful.

   Files to touch:

   - `kernel/user/programs.c`
   - `kernel/user/process.c`
   - `kernel/sched/scheduler.c`

   Work:

   - `done` Track the last foreground command status as `$?`.
   - `done` Implement `cmd1 && cmd2` and `cmd1 || cmd2`.
   - `done` Make pipelines return a documented status, preferably the rightmost
     command status for now.
   - `done` Ensure failed `exec` returns `127` and redirection failure returns nonzero.

   Acceptance:

   - `false || echo ok` prints `ok`.
   - `true && echo ok` prints `ok`.
   - `missing-command; echo $?` reports `127`.

3. Finish expected shell built-ins.

   Files to touch:

   - `kernel/user/programs.c`

   Work:

   - `done` Keep these as shell built-ins: `cd`, `export`, `unset`, `pwd`, `exit`,
     `jobs`, `fg`, `bg`, `help`.
   - `done` Make `exit` work reliably from the interactive shell and from scripts.
   - `done` Make Ctrl-D at an empty prompt exit the shell cleanly.
   - `done` Keep Ctrl-C from killing the shell itself unless it is the foreground job
     target.

   Acceptance:

   - `export A=1; echo $A` works.
   - `unset A; echo $A` prints empty.
   - `exit` exits a child shell.
   - Ctrl-D at an empty prompt exits the shell.

4. Add script execution.

   Files to touch:

   - `kernel/user/programs.c`
   - `kernel/user/process.c`
   - `kernel/fs/initramfs.c`

   Work:

   - `done` Support `sh /path/script`.
   - `done` Support `#!/bin/sh` for executable text files if practical.
   - `done` Read scripts through VFS, not through hardcoded initramfs tables.
   - `done` Preserve line numbers for useful error messages.

   Acceptance:

   - `/bin/sh /etc/profile` or another small test script runs from VFS.
   - A script can create files, pipe text, check `$?`, and exit nonzero.

5. Make coreutils option behavior predictable.

   Files to touch:

   - `kernel/user/busybox.c`
   - `kernel/user/programs.c`
   - `tests/smoke.sh`

   Work:

   - `done` Keep utility implementations small, but document and test supported flags.
   - `done` Prioritize flags used by build scripts:
     `ls -l -a`, `cp -r`, `rm -r -f`, `mkdir -p`, `grep -q -n`, `head -n`,
     `tail -n`, `wc -l -c`, `sort`, `uniq`, `test`/`[`, `basename`,
     `dirname`, `touch`, `printf`, `true`, `false`.
   - `done` Return nonzero on unsupported flags instead of silently ignoring them.
   - `done` Prefer exact simple behavior over broad incomplete behavior.

   Acceptance:

   - `mkdir -p /tmp/a/b` works.
   - `rm -rf /tmp/a` removes a tree.
   - `grep -q pattern file; echo $?` works.
   - `test -f /tmp/file && echo yes` works.

6. Add shell-driven smoke coverage.

   Files to touch:

   - `tests/smoke.sh`
   - `kernel/user/programs.c`
   - `kernel/fs/initramfs.c`

   Work:

   - `done` Add a `/bin/shell-smoke` or `/etc/smoke.sh` path.
   - `done` Run the same utility workflow through the shell, not direct C calls only.
   - `done` Cover quoting, redirection, append, pipes, conditionals, variables,
     recursive remove, and symlinks.

   Acceptance:

   - `make smoke-x86` includes the shell/coreutils script path.
   - The smoke log prints one clear `PASS shell/coreutils` marker.
   - Failures print the command that failed.

### Shell/Coreutils Test Checklist

Run at least:

```sh
make ARCH=x86
make smoke-x86
```

Inside B1NIX, manually exercise:

```sh
echo "hello world" > /tmp/q
cat /tmp/q | grep hello
false || echo recovered
true && echo continued
mkdir -p /tmp/tree/a
echo x > /tmp/tree/a/file
cp -r /tmp/tree /tmp/tree2
find /tmp/tree2
rm -rf /tmp/tree /tmp/tree2
```

Close `Shell/coreutils` only after the same workflow runs as a script through
`/bin/sh`, not only by typing commands interactively.

## Suggested Next Small Pieces

If you want the lowest-risk implementation order, start here:

1. Add `touch`, `basename`, `dirname`, `test`/`[`, and `printf`.
2. Add `mkdir -p` and `rm -rf`.
3. Add shell quoting for single quotes, double quotes, and backslash escapes.
4. Add `$?`, `&&`, and `||`.
5. Add VFS symlink loop detection and errno cleanup.
6. Add shell-driven smoke tests.

That order gives visible user-facing wins while also forcing the VFS path layer
to become stricter.
