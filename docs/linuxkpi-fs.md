# Importing Linux's filesystems: btrfs, ext4, jbd2

The same bargain M101 struck for the DRM core, applied to storage: the
filesystem is compiled exactly as upstream wrote it, and everything it stands on
is b1nix's own linuxkpi. **A patch to anything under the staged tree is a bug in
the shim.** There is deliberately no patch directory and no place to put one.

## Why import rather than keep writing our own

b1nix already has an ext2/3/4 driver and a btrfs one with a copy-on-write write
path. Both are ours, both work, and both are a standing promise to reimplement —
correctly, forever — a format defined by somebody else's code. The btrfs writer
currently refuses free-space-tree filesystems, shared extents and compression,
and each of those is a subsystem's worth of work that upstream already has.

Importing changes what is maintained: not a filesystem, but the interface a
filesystem stands on. That interface is finite and shared — btrfs, ext4 and jbd2
all sit on the same VFS, page cache and block layer — so a fourth filesystem
after them costs a fetch-script entry.

## Licensing

b1nix is `GPL-2.0-only`, and so are `fs/btrfs`, `fs/ext4` and `fs/jbd2`. Unlike
the DRM core — which is MIT and had five GPL-only files excluded — there is no
question to answer here and no file left out on licence grounds. The staged uapi
headers carry `GPL-2.0 WITH Linux-syscall-note`. Recorded in
[`THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md).

## What is staged

`tools/fs/fetch-linux-fs.sh`, pinned to Linux 6.6 by version and SHA-256, into
`build/src/fs-6.6` (not tracked). 105 objects:

| Tree | Objects | Why |
|---|---|---|
| `fs/btrfs` | 58 | the filesystem |
| `fs/ext4` | 33 | the filesystem |
| `fs/jbd2` | 6 | ext4's journal |
| `fs/iomap` | 6 | both filesystems do direct and buffered I/O through it |
| `fs/mbcache.c` | 1 | ext4's xattr block deduplication |
| `lib/maple_tree.c` | 1 | ext4's extent status tree |

plus `lib/zlib_*`, `lib/lzo`, `lib/zstd` and `lib/xxhash.c` — btrfs compresses
with all three, and writing our own DEFLATE would be a second implementation of
a format whose output has to be byte-compatible with every other kernel's.

The object lists come from upstream's own `Makefile` variables rather than being
chosen here, so they cannot drift from the pinned source. Only the unconditional
`-y` sets: POSIX ACLs, fs-verity, fs-encryption, `check-integrity` and
`ref-verify` are each a subsystem the shim has not got, and each is added when it
does, deliberately.

Nothing under Linux's `include/linux` is staged — those headers are what
`kernel/include/linux` reimplements. The exceptions are the interfaces
*belonging to* imported subsystems (`jbd2.h`, `journal-head.h`, `iomap.h`,
`mbcache.h`, `maple_tree.h`, `zlib.h`, `lzo.h`, `zstd*.h`, `xxhash.h`), which are
imported with their code.

## Derived, not written

`tools/fs/gen-shim-headers.sh` generates what can be derived from the pinned
source, in three passes. Two of them ask the compiler rather than the text,
because the text does not know the answer:

1. **From the text.** 231 distinct `trace_*()` calls across the four trees, each
   turned into a no-op macro. Writing them by hand would be 231 chances to miss
   one — and a missed tracepoint is not a compile error at the call site, it is
   an implicit declaration that links against nothing.
2. **Tracepoints from the compiler.** Some are built by token pasting: btrfs's
   `DECLARE_SPACE_INFO_UPDATE` expands `trace_update_##name`, so
   `trace_update_bytes_pinned` exists in the preprocessed source and appears
   nowhere in the text. This pass compiles every file and adds whatever the
   compiler reports as an undeclared `trace_*`. A name ending in `_enabled` gets
   `0` rather than an empty statement — it is a predicate a filesystem tests,
   not a call it makes.
3. **Structs named before their own header.** A `struct X *` first seen inside a
   prototype declares a type local to it; when the real X arrives later in the
   same translation unit the two do not match, and clang calls it an error where
   GCC calls it a warning. It happens inside btrfs itself — `extent-tree.h`
   declares functions taking `struct btrfs_delayed_ref_head *` and the header
   defining it is included later. The names are collected from the compiler's
   own visibility warnings and forward-declared ahead of everything, which is
   what the C would have said and changes no code. Ten of them at the 6.6 pin.

## Measuring the gap

`tools/fs/probe-headers.sh` reports two numbers:

- **preprocess-clean** — files whose whole include closure resolves. The only
  measure that moves monotonically while headers are being written, because the
  preprocessor stops at the first missing include in a file, so a histogram of
  missing headers under-reports by design.
- **missing / errors** — the work queue, most-blocking first.

`--syntax` switches to `-fsyntax-only`, the harder question: every declaration
has to exist and match.

## Where it stands

**btrfs from unmodified Linux 6.6 mounts and unmounts inside b1nix.** It links
into the kernel (`B1NIX_FS_IMPORT=btrfs`), its module init runs at boot, and
given a real image made by `mkfs.btrfs` on the host it:

- opens the device through b1nix's block layer,
- reads its superblock through the device's page cache and verifies the
  checksum with the crc32c in `kernel/lkpi/crc32.c`,
- identifies the filesystem — `BTRFS: device label LKPITEST devid 1 transid 8
  sda scanned by lkpi-btrfs-test`,
- reads the chunk tree, the root tree and the free-space tree,
- publishes its sysfs interface, starts its cleaner and transaction threads,
- produces a root dentry — inode 256, block size 4096, magic `0x9123683e`,
- lists that directory, looks names up in it, reads an inline file, reads a
  file with real extents at three offsets, walks two directories down and
  follows a symlink — every byte checked against what `mkfs.btrfs --rootdir`
  put there,
- and then, mounted read-WRITE, creates a file and a directory, writes to them,
  fsyncs, overwrites a page in the middle of an extent, appends past the end,
  truncates, renames, hard-links, symlinks and unlinks — after which the host's
  own `btrfs check` reports **no error found** and `btrfs restore` reads back
  every byte b1nix wrote,
- and unmounts: `close_ctree` joins both kthreads, flushes fourteen workqueues
  and returns.

Run it with:

```sh
B1NIX_FS_IMPORT=btrfs make KERNEL_CMDLINE="b1nix.test=1 b1nix.lkpi-btrfs-test=sda" iso
qemu-system-x86_64 -m 2048 -smp 2 -cdrom build/x86_64/b1nix.iso -boot d \
  -drive file=<image>,format=raw,if=none,id=d -device ahci,id=ahci0 \
  -device ide-hd,drive=d,bus=ahci0.0 -serial stdio -display none
```

and the log carries `LKPI-FS: ok btrfs-mount root_ino=256 …` followed by
`LKPI-FS: ok btrfs-umount`. The import is off by default, so the smoke suite
does not build it; the run above is the proof.

All 105 imported translation units compile, and the whole set links.

### The defects the mount and the reads found

Each was in the shim, and each is worth recording because the symptom named
something else:

1. **Range walks over the index space.** `truncate_inode_pages_range` and its
   neighbours iterated every index from `start` to `end` instead of the entries
   the mapping actually holds. A whole-file range ends at `(pgoff_t)-1`, so the
   loop ran 2^64 times over a handful of folios. The mount looked wedged; the
   profiler put 96% of the kernel's samples in `xa_load`.
2. **btrfs's own messages were compiled out.** Without `CONFIG_PRINTK` every
   `btrfs_err()` becomes `btrfs_no_printk()`, so the filesystem reported each
   failure by returning a bare `-EINVAL` with nothing in the log. The level
   prefixes also have to be the SOH form imported code parses (`KERN_SOH "6"`),
   not a readable `"<6>"` — `printk_get_level()` recognises exactly one of those.
3. **A block device with no kobject.** `bdev_kobj()` returned NULL, and btrfs
   reads the device's name straight out of it before sysfs is involved.
4. **A bdi with no device.** `sb->s_bdi->dev` was NULL, and `&NULL->kobj` is not
   a NULL kobject — it is a pointer to offset zero, which the link call then
   refused. That was "failed to init sysfs interface: -22".
5. **`vfs_kern_mount` kept `s_umount`.** `sget()` returns the superblock with
   the lock held and upstream drops it at the end of the mount; leaving it held
   made the unmount wait on itself.
6. **Two SMP defects in b1nix's own sysfs**, exposed because a mount publishes
   attributes from whatever CPU it runs on: the registry's lists had no lock at
   all, and `kobject_create_and_add()` did not zero the kobject it allocated, so
   its `sysfs` pointer was heap garbage that the unmount then followed.

Reading through the mount found four more:

7. **`read_cache_folio` checked `uptodate` without waiting.** A `read_folio` may
   be asynchronous — btrfs submits the bio and returns, and the folio becomes
   uptodate when the completion runs — so the folio lock is the signal. Every
   read of a file with real extents returned `-EIO`; inline files, whose data is
   in the item and needs no bio, read fine.
8. **`file_accessed()` skipped `touch_atime()`'s refusals**, so a read on a
   read-only mount dirtied an inode, btrfs joined a transaction, and every block
   group on a read-only mount is read-only: "Transaction aborted (error -28)".
9. **`page_get_link()` was a stub** returning `-EOPNOTSUPP`. btrfs stores a
   symlink's target as file data and uses exactly that helper.
10. **`dput()` never left the inode's alias list**, so a freed dentry stayed
    linked from `inode->i_dentry` — which btrfs warns about when it destroys the
    inode.

### The bridge

`mount -t btrfs-lkpi /dev/sda /mnt` mounts a real btrfs through the imported
code and serves every path under it from there: open, read, write, readdir,
create, mkdir, rename, link, symlink, unlink, truncate and fsync. It is two
files, because b1nix's VFS and Linux's cannot meet in one translation unit:

| File | Side | What it is |
|---|---|---|
| `kernel/lkpi/fs_bridge.c` | Linux | performs the operations, hands back opaque handles |
| `kernel/fs/lkpifs.c` | b1nix | a `struct vfs_fs` whose callbacks call the above |

A handle is a `struct dentry *` that the b1nix side never looks inside, and
`kernel/lkpi/fs_bridge.h` — the only header both include — is plain C types.
Nodes are materialised lazily through `lookup_cb`, as the 9p client does, so a
mount costs one dentry rather than a walk of the whole filesystem.

The type is deliberately not called `btrfs`: b1nix's own driver keeps that name
and the two are useful side by side. `b1nix.lkpi-bridge-test=<device>` runs the
proof — mount, read by path, a nested path, readdir, an extent read at a high
offset, a write, mkdir/rename/unlink, umount — after which the host's
`btrfs check` is clean and `btrfs restore` shows what the bridge wrote.

**One defect it found, in the shim rather than the bridge:** `current->pid` was
b1nix's task id, and the boot task's id is 0. btrfs stores the locking task's
pid in a tree block and reports "already locked by pid=0, extent tree
corruption detected" when a later lock finds its own pid there — which, for a
task whose pid is 0, is every buffer that has just been unlocked. Linux has no
pid 0 for anything doing filesystem work either. The shim now reports the id
plus one.

### The defects the write path found

11. **`blkcg_punt_bio_submit()` was a no-op**, and btrfs submits every bio from
    an async helper through it (`REQ_BTRFS_CGROUP_PUNT`). The "punt" is a
    cgroup detour, not a discard — upstream's `!CONFIG_BLK_CGROUP` version just
    submits the bio. Ours **threw the write away**: the page stayed under
    writeback forever and the next write to the same file waited on it.
12. **`set_page_dirty()` was a no-op inline.** Dirtying is not a flag: the
    filesystem records the change when its `dirty_folio` runs. With a no-op,
    btrfs's metadata never reached writeback, so the transaction wrote a new
    superblock pointing at tree blocks that had never been written and the
    image failed `btrfs check` with "bad tree block … have=0".
13. **`security_inode_init_security()` returned `-EOPNOTSUPP`.** Upstream
    returns 0 when no LSM has an inode hook; btrfs aborts the transaction on
    any non-zero return, so every file creation failed with `-95`.
14. **`filemap_get_folios_tag()` walked the index space** — the same defect as
    (1), in the function writeback uses, where the range really is the whole
    file.
15. **`bio_split()` ignored the caller's bioset**, so a split bio had no
    `front_pad` and `btrfs_bio(bio)` pointed before the allocation; it also
    chained the halves, which raised an outstanding count nothing would lower.
16. **`alloc_ordered_workqueue()` dropped its format arguments**, so two of
    btrfs's queues came out with binary names — harmless in itself, and a
    pointer read from whatever followed on the stack.

`kthread_stop()` is also real now — it raises the thread's stop flag, wakes it
and waits for its function to return, because `close_ctree` frees the structures
the cleaner is still reading.

Written so far, all of it real rather than stubbed except where upstream itself
stubs:

- **The VFS object model** (`linux/fs.h`, ~1500 lines) — `super_block`,
  `inode`, `dentry`, `file`, `address_space` and the four operations vectors,
  member for member as upstream has them, because imported code reads and writes
  them directly. The file it replaced was 222 lines of anonymous-inode support
  written for DRM.
- **The page-cache surface** — `struct page` grew `mapping`, `index` and
  `flags`; `struct folio` is a real type whose layout mirrors `struct page` field
  for field with every offset asserted at compile time (upstream's
  `FOLIO_MATCH`), inside a union so that both `folio->mapping` and `&folio->page`
  work — imported code uses both spellings. `linux/pagemap.h` is the filemap
  interface rather than the four DRM stubs it was.
- **`rwsem`** (`kernel/lkpi/rwsem.c`) — a writer-preferring sleeping
  reader/writer semaphore. Not a mutex with two names: btrfs's backref walk
  takes the read side twice on one task, which deadlocks against an exclusive
  lock and does not against a shared one. Writers are preferred because the
  alternative starves a transaction commit under a steady read load.
- **`crc32c`, `crc32_le/be`, `crc16`** (`kernel/lkpi/crc32.c`) — three different
  polynomials, all three needed: btrfs checksums metadata with the first, ext4
  hashes directories with the second and checksums group descriptors without
  `metadata_csum` with the third. x86-64's SSE4.2 `crc32` instruction is
  deliberately not used yet — the correct slow version has to exist to check a
  fast one against.
- **`buffer_head`** — the state bits with their meanings, generated accessors,
  and the interface ext4 and jbd2 are built on.
- **`bio`, `blk_types`, `bvec`, `blkdev`** — the block device as a filesystem
  sees it. Every geometry question (size, logical block size, read-only,
  discard, zoned) is answered from b1nix's own block layer and none of them is
  allowed to be a guess. The request *queue* is deliberately not modelled: b1nix
  has one block cache with its own writeback, so imported code reaching for a
  `struct request` fails to compile, which is the correct outcome.
- **`writeback`, `percpu`, `percpu_counter`, `percpu-rwsem`, `mempool`,
  `cpumask`** — each with its simplification stated where it makes one. The
  per-CPU variables are one shared instance, which is right for the hints they
  carry and explicitly wrong for anything that must be summed — which is what
  `percpu_counter` is for.
- **`fscrypt`, `fsverity`, `security`, `quota`, `dax`, `freezer`, `unicode`** —
  upstream's own "feature absent" stubs, and their return values are the
  contract rather than a convenience. An LSM hook that refuses breaks a mount;
  `fscrypt_setup_filename` returning an empty name makes every directory lookup
  miss; `utf8_load` failing is what makes ext4 refuse a casefold filesystem
  rather than mounting it case-sensitively and letting two names collide.
- **`asm/barrier.h`** — per architecture, because this tree builds for aarch64
  too, where `rmb`/`wmb` are real instructions rather than compiler barriers.

## Two divergences worth knowing

**Two pointer diagnostics are demoted for the imported code, and only for it.**
Upstream builds these filesystems with GCC, where both are warnings, and relies
on that in two places: a `u64 *` handed a `loff_t *` — the same 64 bits with
different signedness, and on this tree a different `long` spelling as well — and
a struct first named inside a function-pointer parameter list, which becomes a
type local to that prototype and then refuses to match the real one. Both are
upstream's own code; neither can be fixed without editing it. b1nix's own shim
files keep `-Wall -Wextra` and a clean bill, so a pointer bug written *here* is
still a build failure.

**`u64` is `unsigned long`, not `unsigned long long`.** Upstream's is the
latter, which is why the first of those diagnostics fires at all. Changing it
was tried and reverted: the DRM uapi headers are written against `uint64_t`, and
one 64-bit spelling cannot satisfy both imports at once. This is the cost, and
it is contained to that one demoted diagnostic.

## What is implemented

The shim is about 6,000 lines across two sides of the boundary described in
`<lkpi/env.h>`:

| On the b1nix side | What it does |
|---|---|
| `bio.c` | the bio API, and `submit_bio` into b1nix's block cache |
| `filemap.c` | the page lock, dirty/uptodate/writeback state, copies |
| `iov_iter.c` | the I/O iterator over user, kernel and page buffers |
| `crypto_shash.c` | crc32c and xxhash64 |
| `crc32.c` | crc32c, crc32 and crc16 — three polynomials, all needed |
| `rwsem.c` | a writer-preferring sleeping rwsem, and a counting semaphore |
| `fs_util.c`, `fs_misc.c` | strings, numbers, time, identity, UUIDs, mempools |

| On the Linux side | What it does |
|---|---|
| `fs_filemap.c` | the page-cache index over the mapping's xarray |
| `fs_inode.c` | inode allocation, the hash, links, timestamps, permissions |
| `fs_dentry.c` | dentries, aliases, and the paths built from them |
| `fs_super.c` | superblocks, `sget`, mounting, freezing, the registry |
| `fs_file.c` | generic read, write, seek and the file-side plumbing |
| `fs_bdev.c` | the Linux block device paired with b1nix's, and its page cache |
| `fs_support.c` | read-ahead, option parsing, kobjects, kthreads, writeback |

`fs_abi_check.c` is compiled against the real Linux headers and asserts every
mirrored structure's layout against the original — it has already caught one
(`bool` where the mirror said `int`, which moved every field after it).

## What remains, in order

1. **A read through the mount.** The root dentry exists; nothing has yet asked
   btrfs for a directory listing or a file's bytes through it. That is the next
   thing the self-test should assert.
2. **The bridge**: a b1nix filesystem type that mounts an imported
   `file_system_type` and forwards b1nix VFS operations into it, so a path under
   `/mnt` is served by the imported code rather than by a self-test.
3. **Writing.** The mount above is read-only. A read-write mount needs the
   writeback path — `write_cache_pages` and the buffer-head write helpers
   (`__block_write_begin_int`, `block_write_end`, `block_commit_write`) are
   still `-EOPNOTSUPP`.
4. **ext4 and jbd2** (`B1NIX_FS_IMPORT=1`), which stand on that same buffer-head
   layer.
5. **`crypto_shash`** over the rest of btrfs's checksum algorithms: crc32c and
   xxhash64 are written, sha256 and blake2b are refused by name.
6. **Proof.** A `mkfs.btrfs` image written by the imported code and checked with
   `btrfs check` on the host, exactly as the native writer is graded today. Then
   the same for ext4 with `e2fsck -fn`.

## What is NOT imported, and what that costs

`lib/raid6` is not staged, so `linux/raid/pq.h` and `linux/raid/xor.h` declare
the entry points btrfs's `raid56.c` calls without an implementation behind them.
The consequence has a shape worth knowing: **btrfs raid5 and raid6 profiles will
not work**, and every other profile — single, dup, raid0, raid1, raid10, which is
what `mkfs.btrfs` uses by default — is unaffected.
