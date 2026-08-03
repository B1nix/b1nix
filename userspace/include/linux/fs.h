/* b1nix: uapi <linux/fs.h> — the block-device ioctls the kernel implements
 * (kernel/fs/vfs.c, the "type 0x12" block) plus the constants that come with
 * them. Everything here is answered for a node backed by a block_device;
 * anything else in Linux's fs.h that b1nix does not implement is deliberately
 * absent rather than declared and rejected at runtime.
 */
#ifndef _B1NIX_LINUX_FS_H
#define _B1NIX_LINUX_FS_H

#include <linux/types.h>

#define BLKROSET   _IO(0x12, 93)
#define BLKROGET   _IO(0x12, 94)
#define BLKRRPART  _IO(0x12, 95)
#define BLKGETSIZE _IO(0x12, 96)
#define BLKFLSBUF  _IO(0x12, 97)
#define BLKRASET   _IO(0x12, 98)
#define BLKRAGET   _IO(0x12, 99)
#define BLKSSZGET  _IO(0x12, 104)
#define BLKBSZGET  _IOR(0x12, 112, size_t)
#define BLKBSZSET  _IOW(0x12, 113, size_t)
#define BLKGETSIZE64 _IOR(0x12, 114, size_t)

/* Mount flags, as passed to mount(2). */
#define MS_RDONLY      1
#define MS_NOSUID      2
#define MS_NODEV       4
#define MS_NOEXEC      8
#define MS_SYNCHRONOUS 16
#define MS_REMOUNT     32
#define MS_MANDLOCK    64
#define MS_NOATIME     1024
#define MS_NODIRATIME  2048
#define MS_BIND        4096

/* Seek constants for sparse files (kernel/fs/vfs.c honours both). */
#ifndef SEEK_DATA
#define SEEK_DATA 3
#define SEEK_HOLE 4
#endif

#endif /* _B1NIX_LINUX_FS_H */
