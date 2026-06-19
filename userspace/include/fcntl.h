#ifndef B1NIX_U_FCNTL_H
#define B1NIX_U_FCNTL_H

#include <sys/types.h>

#define O_RDONLY    0x0000
#ifdef __cplusplus
extern "C" {
#endif
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_CREAT     0x0040
#define O_EXCL      0x0080
#define O_TRUNC     0x0200
#define O_APPEND    0x0400
#define O_CLOEXEC   0x0800
#define O_NONBLOCK  0x4000
#define O_DIRECTORY 0x10000
#define O_NOCTTY    0x0100
#define O_BINARY    0
#define O_TEXT      0

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
