#ifndef B1NIX_U_UNISTD_H
#define B1NIX_U_UNISTD_H

#include <stddef.h>
#include <sys/types.h>

#define _PC_PATH_MAX 4
static inline long pathconf(const char *path, int name) {
    (void)path;
    if (name == _PC_PATH_MAX) return 4096;
    return -1;
}

#ifdef __cplusplus
extern "C" {
#endif

extern char **environ;

int write(int fd, const void *buf, size_t n);
int read(int fd, void *buf, size_t n);
int close(int fd);
void _exit(int status) __attribute__((noreturn));
int sleep(unsigned int seconds);
int open(const char *path, int flags, ...);
int unlink(const char *pathname);
int rmdir(const char *pathname);
long lseek(int fd, long offset, int whence);
int execvp(const char *file, char *const argv[]);
int execv(const char *pathname, char *const argv[]);
int fork(void);
int pipe(int pipefd[2]);
int dup2(int oldfd, int newfd);
int mkdir(const char *path, unsigned int mode);
int chdir(const char *path);
char *getcwd(char *buf, size_t size);
int fsync(int fd);
void sync(void);
unsigned int alarm(unsigned int seconds);


/* stat structure for userspace */
struct stat {
  unsigned long st_dev;
  unsigned long st_ino;
  unsigned int st_mode;
  unsigned int st_nlink;
  unsigned int st_uid;
  unsigned int st_gid;
  unsigned long st_rdev;
  off_t st_size;
  unsigned long st_blksize;
  unsigned long st_blocks;
  unsigned int st_atime;
  unsigned int st_mtime;
  unsigned int st_ctime;
};
int stat(const char *path, struct stat *st);

struct statfs {
  unsigned long f_type;
  unsigned long f_bsize;
  unsigned long f_blocks;
  unsigned long f_bfree;
  unsigned long f_bavail;
  unsigned long f_files;
  unsigned long f_ffree;
  unsigned long f_fsid;
  unsigned long f_namelen;
  unsigned long f_frsize;
  unsigned long f_flags;
  unsigned long f_spare[4];
};
int statfs(const char *path, struct statfs *buf);

int setuid(unsigned short uid);
int setgid(unsigned short gid);

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

static inline int lstat(const char *path, struct stat *st) {
    return syscall(SYS_LSTAT, path, st);
}

static inline int fstat(int fd, struct stat *st) {
    return syscall(SYS_FSTAT, fd, st);
}

static inline int getpid(void) {
    return syscall(SYS_GETPID);
}

/* Index of the CPU currently executing this thread (0 = BSP, 1+ = AP). */
static inline int getcpu(void) {
    return syscall(SYS_GETCPU);
}

static inline int isatty(int fd) {
    return fd >= 0 && fd <= 2;
}

char *getlogin(void);

#ifdef __cplusplus
}
#endif

#endif
