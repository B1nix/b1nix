# Filesystems and storage

Milestones M5, M8, M14, M21, M34, M43, M72, M107, M110, M114 and M120.

## The VFS

Every path goes through one VFS with nodes, inodes, an inode cache and a page
cache (M5, M8). Permissions are checked there. Links, renames, extended
attributes, file locks and asynchronous I/O are implemented once for all
filesystems. Mounts form a table per mount namespace, and a walk records which
mount it is in, so a bind mount, a mount stacked on another and a
`pivot_root` all resolve the way Linux resolves them (M123; see
`isolation-and-security.md`). Paths are resolved relative to the calling
process's root after a `chroot`.

The kernel provides these filesystems itself: tmpfs and ramfs, the initramfs
tarfs, procfs and sysfs with dynamic per-process entries (M34), devtmpfs,
devpts, cgroup2, mqueue, debugfs, 9p for host shares, and the new mount
API (`fsopen`, `fsmount`, `open_tree`, `move_mount`). FAT32, exFAT, ISO 9660
and read-only NTFS are native drivers (M43). NTFS, isofs and a few other pieces
are loadable modules. `/dev/fuse` exists but is only a placeholder: FUSE
filesystems do not work yet.

9p's `readdir` is wrong and is the sys lane's `M110-9P: ok readdir` failure.
`p9_vfs_readdir` (`kernel/fs/9p.c`) passes the VFS's entry INDEX straight to
`Treaddir` as its offset, but 9P's offset is the opaque cookie the previous
entry carried (`next_off`, which that function reads and throws away). A second
call therefore asks the server to resume from a position that means nothing, and
the listing either loses entries or repeats them until the caller never
finishes — `fail readdir-find-file` on a loaded host, a hang on a quiet one. The
fix is to remember the cookie the last call ended on and resume from it. This
predates M125 and reproduces unchanged at its branch point; it is named here
because the milestone's full-suite run is what found it.

## Linux's own ext4 and btrfs (M120)

btrfs, ext4 and jbd2 are compiled unmodified from Linux 6.18.51 on top of the
linuxkpi shim in `kernel/include/linux` and `kernel/lkpi/`. There is no patch
directory: a patch to anything under the staged tree would be a bug in the shim.
`tools/import/fs/fetch-linux-fs.sh` stages the sources, pinned by version and hash, and
`B1NIX_FS_IMPORT` chooses btrfs alone or btrfs with ext4. ext2, ext3 and ext4
are all mounted by the imported ext4, and there are no native ext or btrfs
drivers. The root image is btrfs on both architectures.

The bridge has two halves because the two VFS models cannot share a translation
unit: `kernel/lkpi/fs_bridge.c` works in Linux's terms and `kernel/fs/lkpifs.c`
in b1nix's. Getting a root filesystem to hold its data taught these rules:

- Getattr, setattr and rename flush pending writes first.
- Namespace operations take the directory locks Linux would.
- lkpi spinlocks save IRQ state at the outermost acquire.
- `schedule()` really sleeps.
- Imported filesystems get 32-bit directory cookies, because bit 62 is the VFS
  cursor's phase bit.
- Truncate zeroes the tail of the page containing the new end of file instead
  of dropping it.

Struct layouts that the shim mirrors from Linux are asserted against real Linux
headers (`fs_abi_check.c`). A stub for a feature that is absent returns exactly
what upstream returns when that feature is configured out.

What is not there yet: the raid5/raid6 profiles of btrfs (`lib/raid6` is not
imported), sha256 and blake2b checksums, a hardware CRC fast path, ACLs,
fs-verity and fscrypt. The host is the judge in the tests: images made by
`tools/import/fs/make-lkpi-image.sh` are checked afterwards with `btrfs check` and
`e2fsck`.

## Disk quotas (M124)

Linux's quota core — `fs/quota/{dquot,quota_tree,quota_v2,kqid}.c` — is
imported, and so is the system call's own `fs/quota/quota.c`: b1nix has a
system-call layer of its own, so that file is compiled inside
`kernel/lkpi/fs_quotactl.c`, which names the superblock the way `quotactl_fd`
does and leaves every command to upstream's handlers. `quotactl(2)` finds its
filesystem by the block device it was mounted from and `quotactl_fd(2)` by any
descriptor on it; a filesystem with no quota operations answers `ENOSYS`, as
Linux does.

An ext4 made with `mke2fs -O quota` therefore behaves as it does on Linux:
usage is accounted from the mount, `Q_QUOTAON` turns enforcement on, a write
past a hard limit fails with `EDQUOT`, `quota-tools` (`setquota`, `repquota`,
`quotaon`) drive it, the limits survive a remount, and `e2fsck -fn` finds the
quota inodes consistent with the usage afterwards. Three things had to be true
for that:

- A `chown` transfers the file's usage. The shim's `struct iattr` carries each
  id under both names, and `dquot_transfer` reads the `vfs*` pair — filling
  only `ia_uid` moved every chowned file's usage to root.
- A write reaches the filesystem while the caller is still there to be told
  `EDQUOT`. b1nix's page cache takes writes and pushes them later, so a
  filesystem enforcing a quota asks for them directly
  (`inode->write_through_cb`) and the cache only mirrors what was written.
- The quota core's `/proc/sys/fs/quota` table is published
  (`kernel/lkpi/fs_sysctl.c`): `quota-tools` decides whether the kernel has
  quota support at all by whether that directory exists.

There is no quota on tmpfs, and mount options are not yet passed to the
imported filesystems, so `mount -o usrquota` does not turn enforcement on —
`quotaon` does.

## Block devices

The block layer has a cache shared by every filesystem (M14). A write that
spans several blocks claims all of them, writeback waits for I/O already in
flight, and invalidation never zeroes a block that is being read or written
(M122). Durable `MAP_SHARED` writes reach the disk through `msync` and writeback
(M72).

Disks are named the way Linux names them (`sda`, `vda`, `nvme0n1`) from the
order they are enumerated, and the root is chosen by bus, by content or by
`root=LABEL=` (M110). Partitions come from MBR and GPT. There are loop devices,
NBD, software RAID (md), ATAPI drives and CFI NOR flash through MTD, which is
enough for BusyBox's `losetup`, `nbd-client`, `readahead` and the other
applets that stood on those layers (M107, M114). Swap works on a file or a partition. The root is
writable and synced on shutdown (M21).

On aarch64 the disk is virtio-blk over the mmio transport, where a request has
a ten-second budget and a loaded TCG host does overrun it. A timeout is
recoverable rather than fatal to the queue: the descriptors and the buffer stay
the device's, the descriptor cursor rolls forward instead of restarting at zero,
and a completion is matched by the used element's id — so the late answer to an
abandoned request frees that request's buffer instead of being read as the
status of the one in flight. Before that, one timeout desynchronised the ring
and every later request failed.

Open: under a fully parallel aarch64 run the log tree has been seen to reach
`btrfs_cow_block` with a transaction handle whose `->transaction` is NULL. The
COW check then aborts the transaction and the abort itself faults writing
through that NULL pointer. Load-dependent, not reproduced on x86_64.
