#ifndef UNISTD_H
#define UNISTD_H

#include <stddef.h>
#include <b1nix/dirent.h>
#include <b1nix/posix.h>

/* Syscall wrappers for userspace programs */
int write(int fd, const void *buf, size_t count);
int read(int fd, void *buf, size_t count);
int close(int fd);
void _exit(int status);
int sleep(unsigned int seconds);
int fork(void);
int execve(const char *path, char *const argv[], char *const envp[]);
int waitpid(int pid, int *status, int options);
int stat(const char *path, struct b1nix_stat *st);
int fstat(int fd, struct b1nix_stat *st);
long lseek(int fd, long offset, int whence);
int unlink(const char *path);
int rename(const char *old_path, const char *new_path);
int rmdir(const char *path);
int fsync(int fd);
int mount(const char *source, const char *target, const char *fstype, unsigned long flags);
int umount(const char *target);
void sync(void);
int mkdir(const char *path, unsigned int mode);
int chdir(const char *path);
int getdents(int fd, struct dirent *buf, size_t max_entries);
int pipe(int pipefd[2]);
int dup2(int oldfd, int newfd);
int fcntl(int fd, int cmd, unsigned long arg);
void *mmap(void *addr, size_t length, int prot, int flags, int fd, long offset);
int munmap(void *addr, size_t length);
void *sbrk(long increment);
int socket(int domain, int type, int protocol);
int bind(int fd, const void *addr, size_t addrlen);
int connect(int fd, const void *addr, size_t addrlen);
long send(int fd, const void *buf, size_t len, int flags);
long recv(int fd, void *buf, size_t len, int flags);
int ioctl(int fd, unsigned long request, void *arg);
int tcgetattr(int fd, struct b1nix_termios *termios);
int tcsetattr(int fd, int optional_actions, const struct b1nix_termios *termios);
int selfhost_status(struct b1nix_selfhost_status *status);
int getcwd(char *buf, size_t size);
int uname(struct b1nix_utsname *buf);
long time(void);
int dmesg(char *buf, size_t size);

#endif
