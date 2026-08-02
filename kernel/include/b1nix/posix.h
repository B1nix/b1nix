#ifndef B1NIX_POSIX_H
#define B1NIX_POSIX_H

#include <b1nix/types.h>

#define B1NIX_SEEK_SET 0
#define B1NIX_SEEK_CUR 1
#define B1NIX_SEEK_END 2

#define B1NIX_WNOHANG 1
#define B1NIX_WUNTRACED 2
#define B1NIX_WCONTINUED 8

#define B1NIX_S_IFMT  0170000
#define B1NIX_S_IFREG 0100000
#define B1NIX_S_IFDIR 0040000
#define B1NIX_S_IFCHR 0020000
#define B1NIX_S_IFIFO 0010000
#define B1NIX_S_IFSOCK 0140000
#define B1NIX_S_IFLNK 0120000

#define B1NIX_S_ISUID 0004000
#define B1NIX_S_ISGID 0002000
#define B1NIX_S_ISVTX 0001000

#define B1NIX_F_DUPFD 0
#define B1NIX_F_GETFL 3
#define B1NIX_F_SETFL 4
#define B1NIX_F_GETFD 1
#define B1NIX_F_SETFD 2
#define B1NIX_F_GETLK 5
#define B1NIX_F_SETLK 6
#define B1NIX_F_SETLKW 7
/* F_DUPFD_CLOEXEC: duplicate to lowest fd >= arg with FD_CLOEXEC set. Linux
 * uses 1030; matched here so glibc/musl broker code (base/posix) works. */
#define B1NIX_F_DUPFD_CLOEXEC 1030
#define B1NIX_FD_CLOEXEC 1

/* M56 file sealing (memfd). Linux ABI values. */
#define B1NIX_F_ADD_SEALS 1033
#define B1NIX_F_GET_SEALS 1034
#define B1NIX_F_SEAL_SEAL   0x0001 /* prevent further seals from being set */
#define B1NIX_F_SEAL_SHRINK 0x0002 /* prevent file from shrinking */
#define B1NIX_F_SEAL_GROW   0x0004 /* prevent file from growing */
#define B1NIX_F_SEAL_WRITE  0x0008 /* prevent writes */

/* M56 memfd_create flags (Linux ABI). */
#define B1NIX_MFD_CLOEXEC       0x0001
#define B1NIX_MFD_ALLOW_SEALING 0x0002

#define B1NIX_AF_UNIX 1
#define B1NIX_AF_LOCAL B1NIX_AF_UNIX
#define B1NIX_AF_INET 2
#define B1NIX_AF_INET6 10
#define B1NIX_AF_NETLINK 16

#define B1NIX_SOCK_STREAM 1
#define B1NIX_SOCK_DGRAM 2
/* SOCK_SEQPACKET: connection-oriented like SOCK_STREAM, but message boundaries
 * are preserved like SOCK_DGRAM. Crashpad's handler protocol is built on it. */
#define B1NIX_SOCK_SEQPACKET 5
#define B1NIX_SOCK_RAW 3

/* Mount flags */
#define B1NIX_MS_RDONLY   0x0001ULL
#define B1NIX_MS_NOSUID   0x0002ULL
#define B1NIX_MS_NOEXEC   0x0008ULL
#define B1NIX_MS_SYNCHRONOUS 0x0010ULL
#define B1NIX_MS_REMOUNT  0x0020ULL
#define B1NIX_MS_BIND     0x1000ULL

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
/* tcsetattr(fd, act, ...) issues ioctl(fd, TCSETS+act): TCSADRAIN -> TCSETSW,
 * TCSAFLUSH -> TCSETSF. b1nix ttys have no output buffering, so the drain/flush
 * variants apply the termios identically to TCSETS. */
#define B1NIX_TCSETSW 0x5403
#define B1NIX_TCSETSF 0x5404
#define B1NIX_TIOCSCTTY 0x540E
#define B1NIX_TIOCGPGRP 0x540F
#define B1NIX_TIOCSPGRP 0x5410
#define B1NIX_TIOCSTI 0x5412
#define B1NIX_TIOCGWINSZ 0x5413
#define B1NIX_TIOCSWINSZ 0x5414
#define B1NIX_TIOCNOTTY 0x5422
#define B1NIX_TIOCGPTN 0x80045430   /* get pty number (master) */
#define B1NIX_TIOCSPTLCK 0x40045431 /* (un)lock slave (unlockpt) */

/* c_lflag */
#define B1NIX_ECHO 0x00000008
#define B1NIX_ICANON 0x00000002
#define B1NIX_ISIG 0x00000001
#define B1NIX_TOSTOP 0x00000100
/* c_oflag */
#define B1NIX_OPOST 0x00000001
#define B1NIX_ONLCR 0x00000004
/* c_iflag */
#define B1NIX_ICRNL 0x00000100

/* c_cc indices (Linux-compatible). */
#define B1NIX_VINTR  0
#define B1NIX_VQUIT  1
#define B1NIX_VERASE 2
#define B1NIX_VEOF   4
#define B1NIX_VTIME  5
#define B1NIX_VMIN   6
#define B1NIX_VSUSP  10

struct b1nix_winsize {
  u16 ws_row;
  u16 ws_col;
  u16 ws_xpixel;
  u16 ws_ypixel;
};

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
  struct {
    u64 tv_sec;
    u64 tv_nsec;
  } st_atim;
  struct {
    u64 tv_sec;
    u64 tv_nsec;
  } st_mtim;
  struct {
    u64 tv_sec;
    u64 tv_nsec;
  } st_ctim;
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

struct b1nix_in6_addr {
  u8 s6_addr[16];
};

struct b1nix_sockaddr_in6 {
  u16 sin6_family;
  u16 sin6_port;
  u32 sin6_flowinfo;
  struct b1nix_in6_addr sin6_addr;
  u32 sin6_scope_id;
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

#define B1NIX_MSG_PEEK 0x02
#define B1NIX_MSG_DONTWAIT 0x40

struct b1nix_pollfd {
  int fd;
  short events;
  short revents;
};

struct timespec {
  i64 tv_sec;
  i64 tv_nsec;
};

/* M56 eventfd flags (Linux ABI). EFD_SEMAPHORE changes read semantics to a
 * decrement-by-one. */
#define B1NIX_EFD_SEMAPHORE 0x00000001
#define B1NIX_EFD_CLOEXEC   0x00080000
#define B1NIX_EFD_NONBLOCK  0x00000800

/* M56 timerfd: clockids + flags (Linux ABI). Only relative arming is honored
 * (TFD_TIMER_ABSTIME is accepted but treated relative — b1nix has a single
 * monotonic tick base). */
#define B1NIX_CLOCK_REALTIME      0
#define B1NIX_CLOCK_MONOTONIC     1
#define B1NIX_TFD_CLOEXEC         0x00080000
#define B1NIX_TFD_NONBLOCK        0x00000800
#define B1NIX_TFD_TIMER_ABSTIME   0x00000001

/* M56 signalfd flags (Linux ABI). */
#define B1NIX_SFD_CLOEXEC  0x00080000
#define B1NIX_SFD_NONBLOCK 0x00000800

/* M56 epoll flags + ops (Linux ABI). */
#define B1NIX_EPOLL_CLOEXEC 0x00080000
#define B1NIX_EPOLL_CTL_ADD 1
#define B1NIX_EPOLL_CTL_DEL 2
#define B1NIX_EPOLL_CTL_MOD 3
/* epoll event masks reuse the poll bits plus the edge-triggered modifier. */
#define B1NIX_EPOLLIN  0x001
#define B1NIX_EPOLLOUT 0x004
#define B1NIX_EPOLLERR 0x008
#define B1NIX_EPOLLHUP 0x010
#define B1NIX_EPOLLPRI 0x002
#define B1NIX_EPOLLRDHUP 0x2000
#define B1NIX_EPOLLET  (1u << 31)
#define B1NIX_EPOLLONESHOT (1u << 30)

/* Packed to match the Linux x86_64 ABI (12 bytes: no padding between the
 * 4-byte events word and the 8-byte data union). The same layout is used on
 * i386 where the struct is naturally 12 bytes. */
struct b1nix_epoll_event {
  u32 events;
  union {
    u64 u64;
    void *ptr;
    int fd;
    u32 u32;
  } data;
} __attribute__((packed));

struct b1nix_itimerspec {
  struct timespec it_interval; /* period for repeating timers */
  struct timespec it_value;    /* initial expiration */
};

/* signalfd read record (Linux struct signalfd_siginfo is 128 bytes; b1nix
 * fills the fields it tracks and zero-pads the rest so the size matches). */
struct b1nix_signalfd_siginfo {
  u32 ssi_signo;
  i32 ssi_errno;
  i32 ssi_code;
  u32 ssi_pid;
  u32 ssi_uid;
  i32 ssi_fd;
  u32 ssi_tid;
  u32 ssi_band;
  u32 ssi_overrun;
  u32 ssi_trapno;
  i32 ssi_status;
  i32 ssi_int;
  u64 ssi_ptr;
  u64 ssi_utime;
  u64 ssi_stime;
  u64 ssi_addr;
  u16 ssi_addr_lsb;
  u8  pad[46];
};

struct timeval {
  i64 tv_sec;
  i64 tv_usec;
};

#define RUSAGE_SELF     0
#define RUSAGE_CHILDREN (-1)
#define RUSAGE_THREAD   1

struct rusage {
  struct timeval ru_utime;
  struct timeval ru_stime;
  long   ru_maxrss;
  long   ru_ixrss;
  long   ru_idrss;
  long   ru_isrss;
  long   ru_minflt;
  long   ru_majflt;
  long   ru_nswap;
  long   ru_inblock;
  long   ru_oublock;
  long   ru_msgsnd;
  long   ru_msgrcv;
  long   ru_nsignals;
  long   ru_nvcsw;
  long   ru_nivcsw;
};

typedef long clock_t;

struct tms {
  clock_t tms_utime;
  clock_t tms_stime;
  clock_t tms_cutime;
  clock_t tms_cstime;
};

#define B1NIX_WEXITED 4
#define B1NIX_WSTOPPED 2
#define B1NIX_WNOWAIT 0x01000000

typedef enum {
  P_ALL = 0,
  P_PID = 1,
  P_PGID = 2
} idtype_t;

typedef struct {
  int si_signo;
  int si_code;
  int si_errno;
  int si_pid;
  int si_uid;
  int si_status;
} siginfo_t;

/* M73: *at() resolution flags (Linux-compatible values). */
#define AT_FDCWD -100
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_EMPTY_PATH 0x1000

/* M73: fallocate mode (only KEEP_SIZE meaningfully handled). */
#ifndef FALLOC_FL_KEEP_SIZE
#define FALLOC_FL_KEEP_SIZE 0x01
#endif

/* M73: statx — Linux struct statx layout (so glibc/port binaries get real
 * values). STATX_BASIC_STATS is the set b1nix can fill from struct b1nix_stat. */
#define STATX_TYPE 0x0001U
#define STATX_MODE 0x0002U
#define STATX_NLINK 0x0004U
#define STATX_UID 0x0008U
#define STATX_GID 0x0010U
#define STATX_ATIME 0x0020U
#define STATX_MTIME 0x0040U
#define STATX_CTIME 0x0080U
#define STATX_INO 0x0100U
#define STATX_SIZE 0x0200U
#define STATX_BLOCKS 0x0400U
#define STATX_BASIC_STATS 0x07ffU
#define STATX_BTIME 0x0800U

struct statx_timestamp {
  i64 tv_sec;
  u32 tv_nsec;
  i32 __reserved;
};

struct statx {
  u32 stx_mask;
  u32 stx_blksize;
  u64 stx_attributes;
  u32 stx_nlink;
  u32 stx_uid;
  u32 stx_gid;
  u16 stx_mode;
  u16 __spare0[1];
  u64 stx_ino;
  u64 stx_size;
  u64 stx_blocks;
  u64 stx_attributes_mask;
  struct statx_timestamp stx_atime;
  struct statx_timestamp stx_btime;
  struct statx_timestamp stx_ctime;
  struct statx_timestamp stx_mtime;
  u32 stx_rdev_major;
  u32 stx_rdev_minor;
  u32 stx_dev_major;
  u32 stx_dev_minor;
  u64 __spare2[14];
};

/* System node/domain name, owned by the syscall layer (sethostname(2) /
 * setdomainname(2)) and read by uname(2), procfs and sysfs. */
void kernel_hostname_get(char *buf, usize len);
void kernel_domainname_get(char *buf, usize len);
int kernel_hostname_set(const char *name);
int kernel_domainname_set(const char *name);

#endif
