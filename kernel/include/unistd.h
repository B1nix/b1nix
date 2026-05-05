#ifndef UNISTD_H
#define UNISTD_H

#include <stddef.h>

/* Syscall wrappers for userspace programs */
int write(int fd, const void *buf, size_t count);
int read(int fd, void *buf, size_t count);
int close(int fd);
void _exit(int status);
int sleep(unsigned int seconds);

#endif
