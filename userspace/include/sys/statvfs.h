#ifndef B1NIX_U_SYS_STATVFS_H
#define B1NIX_U_SYS_STATVFS_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* f_flag bits */
#define ST_RDONLY 0x0001
#define ST_NOSUID 0x0002

struct statvfs {
  unsigned long f_bsize;   /* file system block size */
  unsigned long f_frsize;  /* fragment size */
  fsblkcnt_t f_blocks;     /* size of fs in f_frsize units */
  fsblkcnt_t f_bfree;      /* free blocks */
  fsblkcnt_t f_bavail;     /* free blocks for unprivileged users */
  fsfilcnt_t f_files;      /* total inodes */
  fsfilcnt_t f_ffree;      /* free inodes */
  fsfilcnt_t f_favail;     /* free inodes for unprivileged users */
  unsigned long f_fsid;    /* file system ID */
  unsigned long f_flag;    /* mount flags */
  unsigned long f_namemax; /* maximum filename length */
  unsigned long f_spare[6];
};

int statvfs(const char *path, struct statvfs *buf);
int fstatvfs(int fd, struct statvfs *buf);

#ifdef __cplusplus
}
#endif

#endif /* B1NIX_U_SYS_STATVFS_H */
