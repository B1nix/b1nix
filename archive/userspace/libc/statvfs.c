/* statvfs.c — POSIX statvfs(2)/fstatvfs(2) over the b1nix statfs syscalls.
 *
 * Provided for upstream BusyBox `df` (migration wave 2b), which queries free
 * space via statvfs(). The b1nix kernel exposes a Linux-style statfs; this
 * shim maps its fields onto the POSIX statvfs layout. */
#include <sys/statvfs.h>
#include <unistd.h>
#include <string.h>

static void statfs_to_statvfs(struct statvfs *out, const struct statfs *sf) {
  memset(out, 0, sizeof(*out));
  out->f_bsize = sf->f_bsize;
  out->f_frsize = sf->f_frsize ? sf->f_frsize : sf->f_bsize;
  out->f_blocks = sf->f_blocks;
  out->f_bfree = sf->f_bfree;
  out->f_bavail = sf->f_bavail;
  out->f_files = sf->f_files;
  out->f_ffree = sf->f_ffree;
  out->f_favail = sf->f_ffree;
  out->f_fsid = sf->f_fsid;
  out->f_flag = sf->f_flags;
  out->f_namemax = sf->f_namelen;
}

int statvfs(const char *path, struct statvfs *buf) {
  struct statfs sf;
  if (statfs(path, &sf) != 0)
    return -1;
  statfs_to_statvfs(buf, &sf);
  return 0;
}

int fstatvfs(int fd, struct statvfs *buf) {
  struct statfs sf;
  if (fstatfs(fd, &sf) != 0)
    return -1;
  statfs_to_statvfs(buf, &sf);
  return 0;
}
