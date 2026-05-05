#ifndef B1NIX_U_UNISTD_H
#define B1NIX_U_UNISTD_H

#include <stddef.h>

int   write(int fd, const void *buf, size_t n);
int   read(int fd, void *buf, size_t n);
int   close(int fd);
void  _exit(int status) __attribute__((noreturn));
int   sleep(unsigned int seconds);
int   open(const char *path, int flags);
int   mkdir(const char *path, unsigned int mode);
int   chdir(const char *path);
int   getcwd(char *buf, size_t size);
long  time(void);

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
};
int stat(const char *path, struct stat *st);

#endif
