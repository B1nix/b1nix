# Imported Linux filesystems: btrfs, ext4, jbd2

btrfs, ext4 and jbd2 are compiled **unmodified** from Linux 6.18.51 on top of
b1nix's linuxkpi (`kernel/include/linux`, `kernel/lkpi/`). There is no patch
directory: **a patch to anything under the staged tree is a bug in the shim.**
All three are GPL-2.0-only like b1nix (see `THIRD_PARTY_NOTICES.md`).

## Staging and build

- `tools/fs/fetch-linux-fs.sh` stages the release `LKPI_LINUX_VERSION` names in
  the Makefile (pinned by version + SHA-256; the DRM core and i915 come from the
  same one) into `build/src/fs-<ver>` (untracked): `fs/{btrfs,ext4,jbd2,iomap,quota}`,
  `fs/mbcache.c`, `lib/{maple_tree,xarray,radix-tree,idr}.c`,
  `lib/{zlib_*,lzo,zstd,xxhash.c}`, and the headers belonging to those
  subsystems. The xarray, radix tree and idr are linked for the whole kernel:
  6.x btrfs walks its extent-buffer tree by mark with `xa_state` cursors, and
  the DRM core uses the same three. Nothing else from `include/linux` — that is
  what the shim reimplements.
- Object lists come from upstream's Makefiles (unconditional `-y` sets only;
  ACLs, fs-verity, fscrypt, check-integrity, ref-verify are not built).
- `tools/fs/gen-shim-headers.sh` generates into `build/src/fs-<ver>-gen`: no-op
  `trace_*` macros (from the text, plus token-pasted ones the compiler reports;
  `*_enabled` → `0`) and forward declarations for structs first named inside a
  prototype.
- `tools/fs/probe-headers.sh [--syntax]` reports preprocess-clean files and the
  missing-header/error queue.

`B1NIX_FS_IMPORT` selects what links in:

| Value | Links | Default |
|---|---|---|
| `btrfs` | btrfs + iomap/maple_tree/compression libs | — |
| `1` | additionally ext4, jbd2, mbcache (`-DB1NIX_FS_IMPORT_EXT4=1`) | when `build/src/fs-<ver>/B1NIX-OBJECTS` exists |
| `0` | nothing | otherwise |

Both non-zero values also build `kernel/fs/lkpifs.c`. The imported code gets
two pointer diagnostics demoted (signedness of `u64 *`/`loff_t *`, and
prototype-scoped structs) because upstream relies on GCC treating them as
warnings; shim files keep full `-Wall -Wextra`. b1nix's `u64` is
`unsigned long` (DRM uapi needs it), which is why the first one fires.

## Mounting: the bridge

`kernel/fs/lkpifs.c` registers imported filesystems with b1nix's VFS:

- `btrfs` — the imported btrfs **is** b1nix's btrfs; there is no native driver.
- `ext4`, `ext3`, `ext2` — the imported ext4 under each name (only with
  `B1NIX_FS_IMPORT=1`); there is no native driver either.

The bridge is split because the two VFS models cannot share a translation unit:
`kernel/lkpi/fs_bridge.c` (Linux side) performs operations and returns opaque
`struct dentry *` handles; `kernel/fs/lkpifs.c` (b1nix side) is the
`struct vfs_fs`; `kernel/lkpi/fs_bridge.h` is the plain-C interface between
them. Nodes are materialised lazily via `lookup_cb`. Supported: open, read,
write, readdir, create, mkdir, rename, link, symlink, unlink, truncate, fsync,
xattrs, chattr flags, statfs, chmod/chown/utime.

## As the root filesystem

The root image is btrfs (`tools/images/mk-root-image.sh`, `ROOT_FS=ext4` for
the old one); the kernel mounts root by the probed type. Rules the bridge and
shim follow because a root exercised them:

- The VFS page cache holds writes until writeback, so getattr, setattr and
  rename flush the file first; otherwise btrfs's older size or mtime wins.
- Namespace ops take the directory locks the VFS would (`inode_lock`, shared
  for readdir — btrfs readdir upgrades and downgrades it).
- lkpi spinlocks save the IRQ state per CPU at the outermost acquire: Linux
  code releases locks out of order.
- `set_current_state(); schedule()` sleeps (bounded, woken by
  `wake_up_process`); it is not a yield.
- `iput` keeps an inode with links cached at zero references; unmount evicts.
- Lookup nodes inherit the mount's `fs_id` (the page-cache key) and are
  attached only if no node of that name exists.
- Kernel threads started inside a syscall are not the caller's children and
  hold no descriptors.

## Tests

| Cmdline flag | What it runs |
|---|---|
| `b1nix.lkpi-btrfs-test=<dev>` | `kernel/lkpi/fs_mount_test.c`: mount, read, RW ops, unmount directly through the Linux API |
| `b1nix.lkpi-bridge-test=<dev>` | `lkpifs_selftest()`: the same through b1nix paths under `/mnt/lkpi` |
| `b1nix.lkpi-ext4-test=<dev>` | bridge self-test over the imported `ext4` |

Images: `tools/fs/make-lkpi-image.sh btrfs|ext4`
(ext4 with the full default feature set). The judge is the host:
`btrfs check` / `btrfs restore`, `e2fsck -fn`. The smoke `blk` lane mounts a
`mkfs.btrfs` image through the imported driver (`M119-BTRFS:` markers).

```sh
B1NIX_FS_IMPORT=1 make KERNEL_CMDLINE="b1nix.test=1 b1nix.lkpi-bridge-test=sda" iso
```

## Shim layout

`<lkpi/env.h>` defines the boundary: b1nix-side files may include b1nix headers,
Linux-side files only `linux/*`.

| b1nix side | |
|---|---|
| `bio.c` | bio API; `submit_bio` into b1nix's block cache |
| `filemap.c` | page lock, dirty/uptodate/writeback state |
| `iov_iter.c` | I/O iterators |
| `crypto_shash.c` | crc32c, xxhash64 (sha256/blake2b refused by name) |
| `crc32.c` | crc32c, crc32_le/be, crc16 |
| `rwsem.c` | writer-preferring sleeping rwsem, counting semaphore |
| `fs_util.c`, `fs_misc.c` | strings, time, identity, UUIDs, mempools |

| Linux side | |
|---|---|
| `fs_filemap.c` | page-cache index over the mapping's xarray |
| `fs_inode.c`, `fs_dentry.c`, `fs_super.c`, `fs_file.c` | VFS object model, `sget`, generic file ops |
| `fs_bdev.c` | Linux block device paired with b1nix's, with its page cache |
| `fs_buffer.c` | buffer heads (ext4/jbd2) |
| `fs_xattr.c`, `fs_quota.c`, `fs_params.c` | xattrs, quota glue, mount options |
| `fs_support.c` | read-ahead, kobjects, kthreads, writeback |
| `fs_abi_check.c` | compiled against real Linux headers; asserts mirrored struct layouts |

Shim rules worth knowing:

- `linux/fs.h` mirrors `super_block`/`inode`/`dentry`/`file`/`address_space`
  member for member; `struct folio` mirrors `struct page` with offsets asserted.
- Range walks (`truncate_inode_pages_range`, `filemap_get_folios_tag`) iterate
  the xarray's entries, never the index space.
- Block geometry is answered from b1nix's block layer; there is no request
  queue model, so code needing `struct request` fails to compile by design.
- "Feature absent" stubs (`fscrypt`, `fsverity`, `security`, `dax`, `freezer`,
  `unicode`) return upstream's `!CONFIG_*` values exactly — those values are the
  contract.
- `CONFIG_PRINTK` is defined and `KERN_*` uses the SOH form, or btrfs errors
  vanish.
- `current->pid` is b1nix task id + 1 (btrfs treats pid 0 as corruption).
- Per-CPU variables are one shared instance; use `percpu_counter` for sums.

## Open

- `lib/raid6` is not imported: `linux/raid/{pq,xor}.h` are declarations only,
  so btrfs raid5/raid6 profiles do not work (single/dup/raid0/1/10 do).
- btrfs sha256/blake2b checksums are refused.
- CRC is table-driven; no SSE4.2 `crc32` fast path.
- ACLs, fs-verity, fscrypt not built.
