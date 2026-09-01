#ifndef B1NIX_VIRTIO_9P_H
#define B1NIX_VIRTIO_9P_H

#include <b1nix/types.h>
#include <b1nix/virtio.h>

#define VIRTIO_9P_DEVICE_ID_LEGACY 0x1009
#define VIRTIO_9P_DEVICE_ID_MODERN 0x1049
#define VIRTIO_9P_MAX_TAG_LEN      64
#define VIRTIO_9P_DEFAULT_MSIZE    65536

#define P9_NOTAG  ((u16)~0)
#define P9_NOFID  ((u32)~0)

/* 9P2000.L Message Types (Opcodes) */
enum {
    P9_RLERROR = 7,
    P9_TSTATFS = 8,
    P9_RSTATFS = 9,
    P9_TLOPEN = 12,
    P9_RLOPEN = 13,
    P9_TLCREATE = 14,
    P9_RLCREATE = 15,
    P9_TSYMLINK = 16,
    P9_RSYMLINK = 17,
    P9_TMKNOD = 18,
    P9_RMKNOD = 19,
    P9_TRENAME = 20,
    P9_RRENAME = 21,
    P9_TREADLINK = 22,
    P9_RREADLINK = 23,
    P9_TGETATTR = 24,
    P9_RGETATTR = 25,
    P9_TSETATTR = 26,
    P9_RSETATTR = 27,
    P9_TXATTRWALK = 30,
    P9_RXATTRWALK = 31,
    P9_TXATTRCREATE = 32,
    P9_RXATTRCREATE = 33,
    P9_TREADDIR = 40,
    P9_RREADDIR = 41,
    P9_TFSYNC = 50,
    P9_RFSYNC = 51,
    P9_TLOCK = 52,
    P9_RLOCK = 53,
    P9_TGETLOCK = 54,
    P9_RGETLOCK = 55,
    P9_TLINK = 70,
    P9_RLINK = 71,
    P9_TMKDIR = 72,
    P9_RMKDIR = 73,
    P9_TRENAMEAT = 74,
    P9_RRENAMEAT = 75,
    P9_TUNLINKAT = 76,
    P9_RUNLINKAT = 77,
    P9_TVERSION = 100,
    P9_RVERSION = 101,
    P9_TAUTH = 102,
    P9_RAUTH = 103,
    P9_TATTACH = 104,
    P9_RATTACH = 105,
    P9_TFLUSH = 108,
    P9_RFLUSH = 109,
    P9_TWALK = 110,
    P9_RWALK = 111,
    P9_TREAD = 116,
    P9_RREAD = 117,
    P9_TWRITE = 118,
    P9_RWRITE = 119,
    P9_TCLUNK = 120,
    P9_RCLUNK = 121,
    P9_TREMOVE = 122,
    P9_RREMOVE = 123,
};

/* QID structure (13 bytes) */
struct p9_qid {
    u8  type;
    u32 version;
    u64 path;
} __attribute__((packed));

/* QID Types */
#define P9_QTDIR     0x80
#define P9_QTAPPEND  0x40
#define P9_QTEXCL    0x20
#define P9_QTMOUNT   0x10
#define P9_QTAUTH    0x08
#define P9_QTTMP     0x04
#define P9_QTSYMLINK 0x02
#define P9_QTFILE    0x00

/* 9P Message Header (7 bytes) */
struct p9_header {
    u32 size;
    u8  type;
    u16 tag;
} __attribute__((packed));

/* 9P Statfs response structure */
struct p9_rstatfs {
    u32 type;
    u32 bsize;
    u64 blocks;
    u64 bfree;
    u64 bavail;
    u64 files;
    u64 ffree;
    u64 fsid;
    u32 namelen;
} __attribute__((packed));

/* 9P Getattr request mask & response valid bits */
#define P9_GETATTR_MODE         0x00000001ULL
#define P9_GETATTR_NLINK        0x00000002ULL
#define P9_GETATTR_UID          0x00000004ULL
#define P9_GETATTR_GID          0x00000008ULL
#define P9_GETATTR_RDEV         0x00000010ULL
#define P9_GETATTR_ATIME        0x00000020ULL
#define P9_GETATTR_MTIME        0x00000040ULL
#define P9_GETATTR_CTIME        0x00000080ULL
#define P9_GETATTR_INO          0x00000100ULL
#define P9_GETATTR_SIZE         0x00000200ULL
#define P9_GETATTR_BLOCKS       0x00000400ULL
#define P9_GETATTR_BTIME        0x00000800ULL
#define P9_GETATTR_GEN          0x00001000ULL
#define P9_GETATTR_DATA_VERSION 0x00002000ULL
#define P9_GETATTR_BASIC        0x000007FFULL /* mode..blocks */
#define P9_GETATTR_ALL          0x00003FFFULL

struct p9_rgetattr {
    u64 valid;
    struct p9_qid qid;
    u32 mode;
    u32 uid;
    u32 gid;
    u64 nlink;
    u64 rdev;
    u64 size;
    u64 blksize;
    u64 blocks;
    u64 atime_sec;
    u64 atime_nsec;
    u64 mtime_sec;
    u64 mtime_nsec;
    u64 ctime_sec;
    u64 ctime_nsec;
    u64 btime_sec;
    u64 btime_nsec;
    u64 gen;
    u64 data_version;
} __attribute__((packed));

/* Setattr valid bits */
#define P9_SETATTR_MODE        0x00000001U
#define P9_SETATTR_UID         0x00000002U
#define P9_SETATTR_GID         0x00000004U
#define P9_SETATTR_SIZE        0x00000008U
#define P9_SETATTR_ATIME       0x00000010U
#define P9_SETATTR_MTIME       0x00000020U
#define P9_SETATTR_CTIME       0x00000040U
#define P9_SETATTR_ATIME_SET   0x00000080U
#define P9_SETATTR_MTIME_SET   0x00000100U

/* VirtIO 9P instance structure */
struct virtio_9p_dev {
    struct virtio_device dev;
    struct virtqueue vq;
    char tag[VIRTIO_9P_MAX_TAG_LEN];
    u32 msize;
    u8 *req_buf;
    u8 *resp_buf;
    u64 req_buf_phys;
    u64 resp_buf_phys;
    volatile int busy;
};

/* Serialization / Deserialization buffer helper */
struct p9_buffer {
    u8 *data;
    usize capacity;
    usize offset;
};

void virtio_9p_init(void);
struct virtio_9p_dev *virtio_9p_find_by_tag(const char *tag);
int virtio_9p_transact(struct virtio_9p_dev *p9dev, usize req_len, usize max_resp_len, usize *actual_resp_len);

void p9_fs_init(void);

#endif
