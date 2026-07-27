#ifndef B1NIX_FUSE_H
#define B1NIX_FUSE_H

#include <b1nix/types.h>
#include <b1nix/vfs.h>

#define FUSE_DEV_NAME "/dev/fuse"

/* FUSE Opcode definitions (matching Linux FUSE ABI v7) */
#define FUSE_LOOKUP       1
#define FUSE_FORGET       2
#define FUSE_GETATTR      3
#define FUSE_SETATTR      4
#define FUSE_READLINK     5
#define FUSE_SYMLINK      6
#define FUSE_MKNOD        8
#define FUSE_MKDIR        9
#define FUSE_UNLINK      10
#define FUSE_RMDIR       11
#define FUSE_RENAME      12
#define FUSE_LINK        13
#define FUSE_OPEN        14
#define FUSE_READ        15
#define FUSE_WRITE       16
#define FUSE_STATFS      17
#define FUSE_RELEASE     18
#define FUSE_FSYNC       20
#define FUSE_INIT        26
#define FUSE_OPENDIR     27
#define FUSE_READDIR     28
#define FUSE_RELEASEDIR  29

struct fuse_in_header {
  u32 len;
  u32 opcode;
  u64 unique;
  u64 nodeid;
  u32 uid;
  u32 gid;
  u32 pid;
  u32 padding;
};

struct fuse_out_header {
  u32 len;
  i32 error;
  u64 unique;
};

struct fuse_init_in {
  u32 major;
  u32 minor;
  u32 max_readahead;
  u32 flags;
};

struct fuse_init_out {
  u32 major;
  u32 minor;
  u32 max_readahead;
  u32 flags;
  u32 max_background;
  u32 congestion_threshold;
  u32 max_write;
  u32 time_gran;
  u32 unused[9];
};

void fuse_init(void);

#endif
