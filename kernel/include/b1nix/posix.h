#ifndef B1NIX_POSIX_H
#define B1NIX_POSIX_H

#include <b1nix/types.h>

#define B1NIX_SEEK_SET 0
#define B1NIX_SEEK_CUR 1
#define B1NIX_SEEK_END 2

#define B1NIX_WNOHANG 1

#define B1NIX_S_IFREG 0100000
#define B1NIX_S_IFDIR 0040000
#define B1NIX_S_IFCHR 0020000
#define B1NIX_S_IFIFO 0010000
#define B1NIX_S_IFSOCK 0140000
#define B1NIX_S_IFLNK 0120000

#define B1NIX_F_GETFL 3
#define B1NIX_F_SETFL 4
#define B1NIX_F_GETFD 1
#define B1NIX_F_SETFD 2
#define B1NIX_F_GETLK 5
#define B1NIX_F_SETLK 6
#define B1NIX_F_SETLKW 7
#define B1NIX_FD_CLOEXEC 1

#define B1NIX_AF_UNIX 1
#define B1NIX_AF_LOCAL B1NIX_AF_UNIX
#define B1NIX_AF_INET 2

#define B1NIX_SOCK_STREAM 1
#define B1NIX_SOCK_DGRAM 2

#define B1NIX_O_RDONLY 0x0000
#define B1NIX_O_WRONLY 0x0001
#define B1NIX_O_RDWR 0x0002
#define B1NIX_O_CREAT 0x0040
#define B1NIX_O_EXCL 0x0080
#define B1NIX_O_TRUNC 0x0200
#define B1NIX_O_APPEND 0x0400
#define B1NIX_O_CLOEXEC 0x0800
#define B1NIX_O_NONBLOCK 0x4000
#define B1NIX_O_DIRECTORY 0x10000

#define B1NIX_TCGETS 0x5401
#define B1NIX_TCSETS 0x5402
#define B1NIX_TIOCGPGRP 0x540F
#define B1NIX_TIOCSPGRP 0x5410

#define B1NIX_ECHO 0x00000008
#define B1NIX_ICANON 0x00000002
#define B1NIX_ISIG 0x00000001
#define B1NIX_OPOST 0x00000001
#define B1NIX_TOSTOP 0x00000100

struct b1nix_stat {
  u64 st_dev;
  u64 st_ino;
  u32 st_mode;
  u32 st_nlink;
  u32 st_uid;
  u32 st_gid;
  u64 st_rdev;
  u64 st_size;
  u64 st_blksize;
  u64 st_blocks;
  u32 st_atime;
  u32 st_mtime;
  u32 st_ctime;
};

struct b1nix_termios {
  u32 c_iflag;
  u32 c_oflag;
  u32 c_cflag;
  u32 c_lflag;
  u8 c_cc[32];
};

struct b1nix_sockaddr_in {
  u16 sin_family;
  u16 sin_port;
  u32 sin_addr;
  u8 sin_zero[8];
};

struct b1nix_sockaddr_un {
  u16 sun_family;
  char sun_path[108];
};

struct b1nix_utsname {
  char sysname[32];
  char nodename[32];
  char release[32];
  char version[32];
  char machine[32];
};

struct b1nix_selfhost_status {
  u32 abi_version;
  u32 target_ready;
  u32 binutils_ready;
  u32 make_ready;
  u32 can_build_kernel_inside_b1nix;
  char target_triple[32];
  char compiler[32];
  char assembler[32];
  char linker[32];
  char make[32];
};

struct b1nix_mount_entry {
  char source[64];
  char target[64];
  char fstype[16];
  u64 flags;
};

struct b1nix_statfs {
  u64 f_type;
  u64 f_bsize;
  u64 f_blocks;
  u64 f_bfree;
  u64 f_bavail;
  u64 f_files;
  u64 f_ffree;
  u64 f_fsid;
  u64 f_namelen;
  u64 f_frsize;
  u64 f_flags;
  u64 f_spare[4];
};

#define B1NIX_POLLIN 0x001
#define B1NIX_POLLOUT 0x004
#define B1NIX_POLLERR 0x008
#define B1NIX_POLLHUP 0x010
#define B1NIX_POLLNVAL 0x020

struct b1nix_pollfd {
  int fd;
  short events;
  short revents;
};

struct timespec {
  long tv_sec;
  long tv_nsec;
};

#endif
