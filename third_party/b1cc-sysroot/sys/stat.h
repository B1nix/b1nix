#ifndef B1NIX_SYS_STAT_H
#define B1NIX_SYS_STAT_H

#include <sys/types.h>
#include <unistd.h>

#define S_IFMT   0170000
#define S_IFSOCK 0140000
#define S_IFLNK  0120000
#define S_IFREG  0100000
#define S_IFBLK  0060000
#define S_IFDIR  0040000
#define S_IFCHR  0020000
#define S_IFIFO  0010000

#define S_ISUID  0004000
#define S_ISGID  0002000
#define S_ISVTX  0001000

#define S_IRWXU  00700
#define S_IRUSR  00400
#define S_IWUSR  00200
#define S_IXUSR  00100

#define S_IRWXG  00070
#define S_IRGRP  00040
#define S_IWGRP  00020
#define S_IXGRP  00010

#define S_IRWXO  00007
#define S_IROTH  00004
#define S_IWOTH  00002
#define S_IXOTH  00001

#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)

#define UTIME_NOW  ((1l << 30) - 1l)
#define UTIME_OMIT ((1l << 30) - 2l)
#define AT_FDCWD -100
#define AT_SYMLINK_NOFOLLOW 0x100

#define AT_REMOVEDIR 0x200

#ifdef __cplusplus
extern "C" {
#endif

int mknod(const char *pathname, mode_t mode, dev_t dev);
int mkfifo(const char *pathname, mode_t mode);
int fchmodat(int dirfd, const char *path, mode_t mode, int flags);
int fstatat(int dirfd, const char *pathname, struct stat *statbuf, int flags);

struct timespec;
int utimensat(int dirfd, const char *pathname, const struct timespec times[2], int flags);
int futimens(int fd, const struct timespec times[2]);

/* M73: statx (Linux struct statx layout). */
#ifndef AT_EMPTY_PATH
#define AT_EMPTY_PATH 0x1000
#endif
#ifndef AT_SYMLINK_NOFOLLOW
#define AT_SYMLINK_NOFOLLOW 0x100
#endif
#define STATX_TYPE   0x0001U
#define STATX_MODE   0x0002U
#define STATX_NLINK  0x0004U
#define STATX_UID    0x0008U
#define STATX_GID    0x0010U
#define STATX_ATIME  0x0020U
#define STATX_MTIME  0x0040U
#define STATX_CTIME  0x0080U
#define STATX_INO    0x0100U
#define STATX_SIZE   0x0200U
#define STATX_BLOCKS 0x0400U
#define STATX_BASIC_STATS 0x07ffU
#define STATX_BTIME  0x0800U

struct statx_timestamp {
  long long tv_sec;
  unsigned int tv_nsec;
  int __reserved;
};

struct statx {
  unsigned int stx_mask;
  unsigned int stx_blksize;
  unsigned long long stx_attributes;
  unsigned int stx_nlink;
  unsigned int stx_uid;
  unsigned int stx_gid;
  unsigned short stx_mode;
  unsigned short __spare0[1];
  unsigned long long stx_ino;
  unsigned long long stx_size;
  unsigned long long stx_blocks;
  unsigned long long stx_attributes_mask;
  struct statx_timestamp stx_atime;
  struct statx_timestamp stx_btime;
  struct statx_timestamp stx_ctime;
  struct statx_timestamp stx_mtime;
  unsigned int stx_rdev_major;
  unsigned int stx_rdev_minor;
  unsigned int stx_dev_major;
  unsigned int stx_dev_minor;
  unsigned long long __spare2[14];
};

int statx(int dirfd, const char *pathname, int flags, unsigned int mask,
          struct statx *statxbuf);

#ifdef __cplusplus
}
#endif

#endif
