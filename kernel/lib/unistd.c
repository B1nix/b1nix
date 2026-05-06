#include <unistd.h>
#include <b1nix/syscall.h>

int write(int fd, const void *buf, size_t count)
{
	return (int)syscall_dispatch(SYS_WRITE, (u64)(usize)buf, (u64)count, (u64)fd, 1);
}

int read(int fd, void *buf, size_t count)
{
	return (int)syscall_dispatch(SYS_READ, (u64)fd, (u64)(usize)buf, (u64)count, 0);
}

int close(int fd)
{
	syscall_dispatch(SYS_CLOSE, (u64)fd, 0, 0, 0);
	return 0;
}

void _exit(int status)
{
	syscall_dispatch(SYS_EXIT, (u64)status, 0, 0, 0);
	while (1);
}

int sleep(unsigned int seconds)
{
	/* Assuming 100 ticks per second */
	syscall_dispatch(SYS_SLEEP, (u64)seconds * 100, 0, 0, 0);
	return 0;
}

int fork(void)
{
	return (int)syscall_dispatch(SYS_FORK, 0, 0, 0, 0);
}

int execve(const char *path, char *const argv[], char *const envp[])
{
	return (int)syscall_dispatch(SYS_EXECVE, (u64)(usize)path, (u64)(usize)argv, (u64)(usize)envp, 0);
}

int waitpid(int pid, int *status, int options)
{
	(void)options;
	return (int)syscall_dispatch(SYS_WAITPID, (u64)pid, (u64)(usize)status, 0, 0);
}

int stat(const char *path, struct b1nix_stat *st)
{
	return (int)syscall_dispatch(SYS_STAT, (u64)(usize)path, (u64)(usize)st, 0, 0);
}

int lstat(const char *path, struct b1nix_stat *st)
{
	return (int)syscall_dispatch(SYS_LSTAT, (u64)(usize)path, (u64)(usize)st, 0, 0);
}

int fstat(int fd, struct b1nix_stat *st)
{
	return (int)syscall_dispatch(SYS_FSTAT, (u64)fd, (u64)(usize)st, 0, 0);
}

long lseek(int fd, long offset, int whence)
{
	return (long)syscall_dispatch(SYS_LSEEK, (u64)fd, (u64)offset, (u64)whence, 0);
}

int unlink(const char *path)
{
	return (int)syscall_dispatch(SYS_UNLINK, (u64)(usize)path, 0, 0, 0);
}

int rename(const char *old_path, const char *new_path)
{
	return (int)syscall_dispatch(SYS_RENAME, (u64)(usize)old_path, (u64)(usize)new_path, 0, 0);
}

int rmdir(const char *path)
{
	return (int)syscall_dispatch(SYS_RMDIR, (u64)(usize)path, 0, 0, 0);
}

int fsync(int fd)
{
	return (int)syscall_dispatch(SYS_FSYNC, (u64)fd, 0, 0, 0);
}

int mount(const char *source, const char *target, const char *fstype, unsigned long flags)
{
	return (int)syscall_dispatch(SYS_MOUNT, (u64)(usize)source, (u64)(usize)target, (u64)(usize)fstype, (u64)flags);
}

int umount(const char *target)
{
	return (int)syscall_dispatch(SYS_UMOUNT, (u64)(usize)target, 0, 0, 0);
}

void sync(void)
{
	syscall_dispatch(SYS_SYNC, 0, 0, 0, 0);
}

int mkdir(const char *path, unsigned int mode)
{
	return (int)syscall_dispatch(SYS_MKDIR, (u64)(usize)path, (u64)mode, 0, 0);
}

int chdir(const char *path)
{
	return (int)syscall_dispatch(SYS_CHDIR, (u64)(usize)path, 0, 0, 0);
}

int getdents(int fd, struct dirent *buf, size_t max_entries)
{
	return (int)syscall_dispatch(SYS_GETDENTS, (u64)fd, (u64)(usize)buf, (u64)max_entries, 0);
}

int pipe(int pipefd[2])
{
	return (int)syscall_dispatch(SYS_PIPE, (u64)(usize)pipefd, 0, 0, 0);
}

int dup2(int oldfd, int newfd)
{
	return (int)syscall_dispatch(SYS_DUP2, (u64)oldfd, (u64)newfd, 0, 0);
}

int fcntl(int fd, int cmd, unsigned long arg)
{
	return (int)syscall_dispatch(SYS_FCNTL, (u64)fd, (u64)cmd, (u64)arg, 0);
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, long offset)
{
	(void)addr;
	(void)prot;
	(void)flags;
	(void)fd;
	(void)offset;
	return (void *)(usize)syscall_dispatch(SYS_MMAP, (u64)length, 0, 0, 0);
}

int munmap(void *addr, size_t length)
{
	return (int)syscall_dispatch(SYS_MUNMAP, (u64)(usize)addr, (u64)length, 0, 0);
}

void *sbrk(long increment)
{
	u64 old_brk = syscall_dispatch(SYS_BRK, 0, 0, 0, 0);
	u64 new_brk = old_brk + (u64)increment;
	syscall_dispatch(SYS_BRK, new_brk, 0, 0, 0);
	return (void *)(usize)old_brk;
}

int socket(int domain, int type, int protocol)
{
	return (int)syscall_dispatch(SYS_SOCKET, (u64)domain, (u64)type, (u64)protocol, 0);
}

int bind(int fd, const void *addr, size_t addrlen)
{
	return (int)syscall_dispatch(SYS_BIND, (u64)fd, (u64)(usize)addr, (u64)addrlen, 0);
}

int connect(int fd, const void *addr, size_t addrlen)
{
	return (int)syscall_dispatch(SYS_CONNECT, (u64)fd, (u64)(usize)addr, (u64)addrlen, 0);
}

long send(int fd, const void *buf, size_t len, int flags)
{
	return (long)syscall_dispatch(SYS_SEND, (u64)fd, (u64)(usize)buf, (u64)len, (u64)flags);
}

long recv(int fd, void *buf, size_t len, int flags)
{
	return (long)syscall_dispatch(SYS_RECV, (u64)fd, (u64)(usize)buf, (u64)len, (u64)flags);
}

int ioctl(int fd, unsigned long request, void *arg)
{
	return (int)syscall_dispatch(SYS_IOCTL, (u64)fd, (u64)request, (u64)(usize)arg, 0);
}

int tcgetattr(int fd, struct b1nix_termios *termios)
{
	return (int)syscall_dispatch(SYS_TERMIOS_GET, (u64)fd, (u64)(usize)termios, 0, 0);
}

int tcsetattr(int fd, int optional_actions, const struct b1nix_termios *termios)
{
	(void)optional_actions;
	return (int)syscall_dispatch(SYS_TERMIOS_SET, (u64)fd, (u64)(usize)termios, 0, 0);
}

int selfhost_status(struct b1nix_selfhost_status *status)
{
	return (int)syscall_dispatch(SYS_SELFHOST_STATUS, (u64)(usize)status, 0, 0, 0);
}

int getcwd(char *buf, size_t size)
{
	return (int)syscall_dispatch(SYS_GETCWD, (u64)(usize)buf, (u64)size, 0, 0);
}

int uname(struct b1nix_utsname *buf)
{
	return (int)syscall_dispatch(SYS_UNAME, (u64)(usize)buf, 0, 0, 0);
}

long time(void)
{
	return (long)syscall_dispatch(SYS_TIME, 0, 0, 0, 0);
}

int dmesg(char *buf, size_t size)
{
	return (int)syscall_dispatch(SYS_DMESG, (u64)(usize)buf, (u64)size, 0, 0);
}
