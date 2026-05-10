#ifndef B1NIX_U_UNISTD_H
#define B1NIX_U_UNISTD_H

#include <stddef.h>

extern char **environ;

int   write(int fd, const void *buf, size_t n);
int   read(int fd, void *buf, size_t n);
int   close(int fd);
void  _exit(int status) __attribute__((noreturn));
int   sleep(unsigned int seconds);
int   open(const char *path, int flags, ...);
int   unlink(const char *pathname);
long  lseek(int fd, long offset, int whence);
int   execvp(const char *file, char *const argv[]);
int   mkdir(const char *path, unsigned int mode);
int   chdir(const char *path);
int   getcwd(char *buf, size_t size);
int   fsync(int fd);
void  sync(void);

/* stat structure for userspace */
struct stat {
	unsigned long st_dev;
	unsigned long st_ino;
	unsigned int  st_mode;
	unsigned int  st_nlink;
	unsigned int  st_uid;
	unsigned int  st_gid;
	unsigned long st_rdev;
	unsigned long st_size;
	unsigned long st_blksize;
	unsigned long st_blocks;
	unsigned int  st_atime;
	unsigned int  st_mtime;
	unsigned int  st_ctime;
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

#endif
