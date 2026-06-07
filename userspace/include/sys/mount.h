#ifndef B1NIX_U_SYS_MOUNT_H
#define B1NIX_U_SYS_MOUNT_H

#include <stddef.h>    /* size_t */
#include <sys/ioctl.h> /* _IOR etc. for the BLK* numbers below */

/* Block-device ioctls (Linux linux/fs.h ABI), used by BusyBox blkid/fdisk. */
#define BLKRRPART    _IO(0x12, 95)
#define BLKGETSIZE   _IO(0x12, 96)  /* size in 512-byte sectors (unsigned long) */
#define BLKFLSBUF    _IO(0x12, 97)
#define BLKSSZGET    _IO(0x12, 104) /* logical sector size (int) */
#define BLKBSZGET    _IOR(0x12, 112, size_t)
#define BLKGETSIZE64 _IOR(0x12, 114, size_t) /* size in bytes (u64) */

/* mount(2) flag bits. These mirror the Linux MS_* ABI values, which is also
 * what the b1nix kernel's B1NIX_MS_* constants use, so a flag word passed
 * straight to SYS_MOUNT is interpreted identically. b1nix only acts on a
 * subset (RDONLY/NOSUID/NOEXEC/SYNCHRONOUS/REMOUNT/BIND); the rest are
 * accepted and ignored. */
#define MS_RDONLY       1
#define MS_NOSUID       2
#define MS_NODEV        4
#define MS_NOEXEC       8
#define MS_SYNCHRONOUS  16
#define MS_REMOUNT      32
#define MS_MANDLOCK     64
#define MS_DIRSYNC      128
#define MS_NOSYMFOLLOW  256
#define MS_NOATIME      1024
#define MS_NODIRATIME   2048
#define MS_BIND         4096
#define MS_MOVE         8192
#define MS_REC          16384
#define MS_SILENT       32768
#define MS_POSIXACL     (1 << 16)
#define MS_UNBINDABLE   (1 << 17)
#define MS_PRIVATE      (1 << 18)
#define MS_SLAVE        (1 << 19)
#define MS_SHARED       (1 << 20)
#define MS_RELATIME     (1 << 21)
#define MS_KERNMOUNT    (1 << 22)
#define MS_I_VERSION    (1 << 23)
#define MS_STRICTATIME  (1 << 24)
#define MS_LAZYTIME     (1 << 25)

/* umount2(2) flags. */
#define MNT_FORCE       1
#define MNT_DETACH      2
#define MNT_EXPIRE      4
#define UMOUNT_NOFOLLOW 8

int mount(const char *source, const char *target, const char *filesystemtype,
          unsigned long mountflags, const void *data);
int umount(const char *target);
int umount2(const char *target, int flags);

#endif
