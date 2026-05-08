#ifndef B1NIX_U_FCNTL_H
#define B1NIX_U_FCNTL_H

#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_CREAT     0x0040
#define O_EXCL      0x0080
#define O_TRUNC     0x0200
#define O_APPEND    0x0400
#define O_CLOEXEC   0x0800
#define O_DIRECTORY 0x10000
#define O_BINARY    0

#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define F_GETLK 5
#define F_SETLK 6
#define F_SETLKW 7

struct flock {
	short l_type;
	short l_whence;
	long  l_start;
	long  l_len;
	int   l_pid;
};

int fcntl(int fd, int cmd, ...);

#endif
