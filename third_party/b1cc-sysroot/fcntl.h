#ifndef B1NIX_U_FCNTL_H
#define B1NIX_U_FCNTL_H

#include <sys/types.h>

#define O_RDONLY    0x0000
#ifdef __cplusplus
extern "C" {
#endif
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_ACCMODE   0x0003  /* mask for O_RDONLY/O_WRONLY/O_RDWR */
#define O_TMPFILE   0x410000  /* __O_TMPFILE | O_DIRECTORY (unnamed temp file) */
#define O_CREAT     0x0040
#define O_EXCL      0x0080
#define O_TRUNC     0x0200
#define O_APPEND    0x0400
#define O_CLOEXEC   0x0800
#define O_NONBLOCK  0x4000
#define O_NDELAY    O_NONBLOCK
#define O_DIRECTORY 0x10000
#define O_NOFOLLOW  0x20000
#define O_NOCTTY    0x0100
#define O_BINARY    0
#define O_TEXT      0
/* Extended open() flags. b1nix's VFS ignores bits it doesn't implement, so these
 * are accepted-and-ignored today (real per-flag semantics tracked in the roadmap).
 * Values use b1nix-free bits, NOT Linux's — several of Linux's collide with b1nix
 * flags (e.g. Linux O_DIRECT 0x4000 == b1nix O_NONBLOCK). O_LARGEFILE is a no-op:
 * b1nix off_t is always 64-bit. O_PATH opens a reference fd (degrades to O_RDONLY
 * until path-only fds exist). */
#define O_DIRECT    0x1000
#define O_SYNC      0x2000
#define O_DSYNC     0x8000
#define O_NOATIME   0x40000
#define O_ASYNC     0x80000
#define O_PATH      0x200000
#define O_LARGEFILE 0

#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define F_GETLK 5
#define F_SETLK 6
#define F_SETLKW 7
#define F_DUPFD_CLOEXEC 1030

#define F_RDLCK 0
#define F_WRLCK 1
#define F_UNLCK 2

#define FD_CLOEXEC 1


struct flock {
	short l_type;
	short l_whence;
	long  l_start;
	long  l_len;
	int   l_pid;
};

int fcntl(int fd, int cmd, ...);

/* creat(2): equivalent to open(path, O_CREAT|O_WRONLY|O_TRUNC, mode).
 * posix_fadvise: file access-pattern hint (advisory; b1nix treats it as a
 * no-op and returns 0). M73: fallocate is now real (mode 0 extends the file
 * via SYS_FALLOCATE; hole-punch/collapse report EOPNOTSUPP). splice moves data
 * to/from a pipe over SYS_SPLICE. */
int creat(const char *path, mode_t mode);
int posix_fadvise(int fd, off_t offset, off_t len, int advice);
int posix_fallocate(int fd, off_t offset, off_t len);
int fallocate(int fd, int mode, off_t offset, off_t len);
ssize_t splice(int fd_in, off_t *off_in, int fd_out, off_t *off_out,
               size_t len, unsigned int flags);
/* splice flags (Linux ABI; advisory on b1nix). */
#define SPLICE_F_MOVE     1
#define SPLICE_F_NONBLOCK 2
#define SPLICE_F_MORE     4
#define SPLICE_F_GIFT     8

/* posix_fadvise advice values (Linux ABI). */
#define POSIX_FADV_NORMAL     0
#define POSIX_FADV_RANDOM     1
#define POSIX_FADV_SEQUENTIAL 2
#define POSIX_FADV_WILLNEED   3
#define POSIX_FADV_DONTNEED   4
#define POSIX_FADV_NOREUSE    5

/* fallocate mode flags (Linux ABI). */
#define FALLOC_FL_KEEP_SIZE      0x01
#define FALLOC_FL_PUNCH_HOLE     0x02
#define FALLOC_FL_COLLAPSE_RANGE 0x08
#define FALLOC_FL_ZERO_RANGE     0x10
#define FALLOC_FL_INSERT_RANGE   0x20

#ifdef __cplusplus
}
#endif

#endif

/* memfd file sealing (fcntl). Constants for ports that seal memfds; b1nix may
 * reject the operation at runtime, which callers like Mesa handle gracefully. */
#define F_ADD_SEALS   1033
#define F_GET_SEALS   1034
#define F_SEAL_SEAL   0x0001
#define F_SEAL_SHRINK 0x0002
#define F_SEAL_GROW   0x0004
#define F_SEAL_WRITE  0x0008
