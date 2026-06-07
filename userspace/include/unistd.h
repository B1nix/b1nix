#ifndef B1NIX_U_UNISTD_H
#define B1NIX_U_UNISTD_H

#include <stddef.h>
#include <sys/types.h>
#include <time.h>

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
int unlink(const char *pathname);
int link(const char *oldpath, const char *newpath);
int rmdir(const char *pathname);
long lseek(int fd, long offset, int whence);
int execvp(const char *file, char *const argv[]);
int execlp(const char *file, const char *arg, ...);
int execv(const char *pathname, char *const argv[]);
int fork(void);
pid_t vfork(void);
int pipe(int pipefd[2]);
int dup2(int oldfd, int newfd);
int mkdir(const char *path, unsigned int mode);
int chdir(const char *path);
int fchdir(int fd);
int chroot(const char *path);
char *getcwd(char *buf, size_t size);
int fsync(int fd);
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

static inline int chmod(const char *path, unsigned int mode) {
    return syscall(SYS_CHMOD, path, mode);
}

int fchmod(int fd, unsigned int mode);
int ftruncate(int fd, off_t length);

static inline int dup(int oldfd) {
    static int next_fd = 100;
    return syscall(SYS_DUP2, oldfd, next_fd++);
}

#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

static inline int access(const char *path, int mode) {
    (void)mode;
    int fd = open(path, 0); // O_RDONLY
    if (fd >= 0) {
        close(fd);
        return 0;
    }
    return -1;
}

/* lstat/fstat are real out-of-line functions (not static inline): code such as
 * BusyBox's recursive_action takes their address via `(follow ? stat : lstat)`,
 * and a static-inline syscall wrapper emitted per-TU for an address-of misbuilds
 * the local-register-variable asm (observed: tar saw st_size/st_mode as 0). A
 * single extern definition is taken by address correctly. */
int lstat(const char *path, struct stat *st);
int fstat(int fd, struct stat *st);

static inline int getpid(void) {
    return syscall(SYS_GETPID);
}

/* Index of the CPU currently executing this thread (0 = BSP, 1+ = AP). */
static inline int getcpu(void) {
    return syscall(SYS_GETCPU);
}

int isatty(int fd);

char *getlogin(void);

int symlink(const char *target, const char *linkpath);
ssize_t readlink(const char *pathname, char *buf, size_t bufsiz);
int lchown(const char *path, uid_t owner, gid_t group);

#define _SC_CLK_TCK 2
long sysconf(int name);

extern char *optarg;
extern int optind, opterr, optopt;
int getopt(int argc, char *const argv[], const char *optstring);

#ifdef __cplusplus
}
#endif

#endif
