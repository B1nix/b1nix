#ifndef B1NIX_U_UNISTD_H
#define B1NIX_U_UNISTD_H

#include <stddef.h>
#include <sys/types.h>
#include <limits.h>
#include <time.h>

/* NB: _POSIX_VERSION is intentionally NOT defined globally. Advertising it here
 * flips feature paths in already-ported software (e.g. OpenSSL's secure-memory
 * code starts calling mlock/madvise we do not provide). bash needs it (to pick
 * `int` over BSD `union wait` for process status), so it is defined per-build
 * via CFLAGS in tools/ports/build-bash.sh instead of for every port. */

/* b1nix has clock_gettime(CLOCK_MONOTONIC); advertise just that feature (narrow,
 * unlike the broad _POSIX_VERSION above) so monotonic-clock code paths compile. */
#define _POSIX_MONOTONIC_CLOCK 200809L

#define _PC_NAME_MAX 3
#define _PC_PATH_MAX 4
static inline long pathconf(const char *path, int name) {
    (void)path;
    if (name == _PC_PATH_MAX) return 4096;
    if (name == _PC_NAME_MAX) return 255;
    return -1;
}

#ifdef __cplusplus
extern "C" {
#endif

extern char **environ;

#ifndef STDIN_FILENO
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#endif

ssize_t write(int fd, const void *buf, size_t n);
ssize_t read(int fd, void *buf, size_t n);
ssize_t pread(int fd, void *buf, size_t n, off_t offset);
ssize_t pwrite(int fd, const void *buf, size_t n, off_t offset);
/* off_t is already 64-bit on b1nix, so the LFS *64 variants are identical. */
ssize_t pread64(int fd, void *buf, size_t n, off_t offset);
ssize_t pwrite64(int fd, const void *buf, size_t n, off_t offset);
/* Real syscall() function (Linux-compat). Declared BEFORE the function-like
 * syscall() macro in <syscall.h> below: direct `syscall(nr, ...)` calls still
 * hit the fast macro, but using `syscall` as a bare name (e.g. passing it to a
 * wrapper, as partition_alloc does) resolves to this symbol. The parens around
 * the name stop the function-like syscall() macro from expanding this
 * declaration regardless of whether <syscall.h> was included first. */
long (syscall)(long number, ...);
int gethostname(char *name, size_t len);
int sethostname(const char *name, size_t len);
int chown(const char *path, uid_t owner, gid_t group);
int fchown(int fd, uid_t owner, gid_t group);
char *ttyname(int fd);
int ttyname_r(int fd, char *buf, size_t buflen);
char *getpass(const char *prompt);
int close(int fd);
void _exit(int status) __attribute__((noreturn));
int sleep(unsigned int seconds);
int open(const char *path, int flags, ...);
int openat(int dirfd, const char *path, int flags, ...);
int unlink(const char *pathname);
int unlinkat(int dirfd, const char *pathname, int flags);
int link(const char *oldpath, const char *newpath);
int rmdir(const char *pathname);
long lseek(int fd, long offset, int whence);
long lseek64(int fd, long offset, int whence);
int execvp(const char *file, char *const argv[]);
int execlp(const char *file, const char *arg, ...);
int execv(const char *pathname, char *const argv[]);
int fork(void);
pid_t vfork(void);
int pipe(int pipefd[2]);
int pipe2(int pipefd[2], int flags);
int dup2(int oldfd, int newfd);
int mkdir(const char *path, unsigned int mode);
int chdir(const char *path);
int fchdir(int fd);
int chroot(const char *path);
char *getcwd(char *buf, size_t size);
int fsync(int fd);
int fdatasync(int fd);  /* Chromium port: b1nix has no data-only sync; = fsync */
void sync(void);
unsigned int alarm(unsigned int seconds);


/* stat structure for userspace */
struct stat {
  unsigned long long st_dev;
  unsigned long long st_ino;
  unsigned int st_mode;
  unsigned int st_nlink;
  unsigned int st_uid;
  unsigned int st_gid;
  unsigned long long st_rdev;
  off_t st_size;
  unsigned long long st_blksize;
  unsigned long long st_blocks;
  struct timespec st_atim;
  struct timespec st_mtim;
  struct timespec st_ctim;
};
#define st_atime st_atim.tv_sec
#define st_mtime st_mtim.tv_sec
#define st_ctime st_ctim.tv_sec
int stat(const char *path, struct stat *st);

/* Must mirror the kernel `struct b1nix_statfs` (all u64) byte-for-byte: the
 * SYS_STATFS handler copies sizeof(b1nix_statfs) bytes into this buffer. Using
 * `unsigned long` would be 32-bit on i686 and the kernel's 64-bit fields would
 * overflow the struct (observed: BusyBox `df` crashed on i686). */
struct statfs {
  unsigned long long f_type;
  unsigned long long f_bsize;
  unsigned long long f_blocks;
  unsigned long long f_bfree;
  unsigned long long f_bavail;
  unsigned long long f_files;
  unsigned long long f_ffree;
  unsigned long long f_fsid;
  unsigned long long f_namelen;
  unsigned long long f_frsize;
  unsigned long long f_flags;
  unsigned long long f_spare[4];
};
int statfs(const char *path, struct statfs *buf);
int fstatfs(int fd, struct statfs *buf);

int setuid(unsigned short uid);
int setgid(unsigned short gid);
int seteuid(uid_t uid);
int setegid(gid_t gid);
uid_t getuid(void);
uid_t geteuid(void);
gid_t getgid(void);
gid_t getegid(void);
int setpgid(pid_t pid, pid_t pgid);
pid_t getpgrp(void);
pid_t getpgid(pid_t pid);
int setreuid(uid_t ruid, uid_t euid);
int setregid(gid_t rgid, gid_t egid);
int setresuid(uid_t ruid, uid_t euid, uid_t suid);
int setresgid(gid_t rgid, gid_t egid, gid_t sgid);
int nice(int incr);
pid_t tcgetpgrp(int fd);
int tcsetpgrp(int fd, pid_t pgrp);
pid_t setsid(void);
pid_t getsid(pid_t pid);
int execve(const char *pathname, char *const argv[], char *const envp[]);
int getgroups(int size, gid_t list[]);
#include <sys/select.h>

#ifndef HAVE_SETEUID
#define HAVE_SETEUID 1
#endif
#ifndef HAVE_SETEGID
#define HAVE_SETEGID 1
#endif

#include <syscall.h>

static inline unsigned int umask(unsigned int mask) {
    return syscall(SYS_UMASK, mask);
}

int chmod(const char *path, unsigned int mode);

int fchmod(int fd, unsigned int mode);
int ftruncate(int fd, off_t length);
int truncate(const char *path, off_t length);
#define ftruncate64 ftruncate

#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

int dup(int oldfd);
int access(const char *path, int mode);
int fchdir(int fd);

/* lstat/fstat are real out-of-line functions (not static inline): code such as
 * BusyBox's recursive_action takes their address via `(follow ? stat : lstat)`,
 * and a static-inline syscall wrapper emitted per-TU for an address-of misbuilds
 * the local-register-variable asm (observed: tar saw st_size/st_mode as 0). A
 * single extern definition is taken by address correctly. */
int lstat(const char *path, struct stat *st);
int fstat(int fd, struct stat *st);

/* Real exported symbols (not static inline) so foreign-function callers — e.g.
 * Rust std's `libc` FFI — resolve them at link time. Defined in posix_compat.c. */
int getpid(void);
int getppid(void);

/* POSIX/Linux compat surface (implemented in posix_compat.c over b1nix
 * syscalls). pipe2/accept4 honour O_CLOEXEC/O_NONBLOCK; daemon/pwrite/preadv/
 * pwritev/fdatasync/linkat are real; M73 added real sendfile/splice/
 * copy_file_range over the kernel; sigwait reports ENOSYS (no signal-wait). */
int pipe2(int pipefd[2], int flags);
int daemon(int nochdir, int noclose);
int fdatasync(int fd);
ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset);
int linkat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath,
           int flags);
/* M73: fd→fd range copy (over SYS_COPY_FILE_RANGE). splice is declared in
 * <fcntl.h>; sendfile in <sys/sendfile.h>. */
ssize_t copy_file_range(int fd_in, off_t *off_in, int fd_out, off_t *off_out,
                        size_t len, unsigned int flags);

int usleep(unsigned int usec);

/* Suspend the calling thread until a signal is delivered. Always returns -1
 * with errno set to EINTR. */
int pause(void);

/* Index of the CPU currently executing this thread (0 = BSP, 1+ = AP). */
static inline int getcpu(void) {
    return syscall(SYS_GETCPU);
}

int isatty(int fd);

char *getlogin(void);

int symlink(const char *target, const char *linkpath);
ssize_t readlink(const char *pathname, char *buf, size_t bufsiz);
ssize_t readlinkat(int dirfd, const char *pathname, char *buf, size_t bufsiz);
int lchown(const char *path, uid_t owner, gid_t group);

#define _SC_CLK_TCK 2
#define _SC_PAGESIZE 30
#define _SC_PAGE_SIZE _SC_PAGESIZE
#define _SC_NPROCESSORS_CONF 83
#define _SC_NPROCESSORS_ONLN 84
#define _SC_PHYS_PAGES 85
#define _SC_AVPHYS_PAGES 86
#define _SC_GETPW_R_SIZE_MAX 70  /* suggested getpw*_r buffer size */
#define _SC_ARG_MAX 0  /* max bytes of arg+env to exec */
#define _SC_OPEN_MAX 4 /* max open fds (== OPEN_MAX) */
long sysconf(int name);
int getpagesize(void);
int getdtablesize(void);

extern char *optarg;
extern int optind, opterr, optopt;
int getopt(int argc, char *const argv[], const char *optstring);

#ifdef __cplusplus
}
#endif

#endif
