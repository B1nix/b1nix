#ifndef B1NIX_SYSCALL_H
#define B1NIX_SYSCALL_H

#include <b1nix/types.h>

enum {
	/* --- Core Control & I/O --- */
	SYS_WRITE    = 1,
	SYS_MEM      = 2,
	SYS_SPAWN    = 3,
	SYS_LIST     = 4,
	SYS_READ_FILE = 5,
	SYS_YIELD    = 6,
	SYS_OPEN     = 7,
	SYS_READ     = 8,
	SYS_CLOSE    = 9,
	SYS_LSEEK    = 10,
	SYS_STAT     = 11,
	SYS_FSTAT    = 12,
	SYS_LSTAT    = 13,
	SYS_IOCTL    = 14,
	SYS_FCNTL    = 15,
	SYS_DUP2     = 16,
	SYS_PIPE     = 17,
	SYS_FSYNC    = 18,

	/* --- Directory & FS Ops --- */
	SYS_CREATE   = 19,
	SYS_UNLINK   = 20,
	SYS_MKDIR    = 21,
	SYS_RMDIR    = 22,
	SYS_RENAME   = 23,
	SYS_SYMLINK  = 24,
	SYS_READLINK = 25,
	SYS_GETDENTS = 26,
	SYS_READDIR  = 27,
	SYS_STATFS   = 28,
	SYS_FSTATFS  = 29,
	SYS_SYNC     = 30,
	SYS_SYNCFS   = 31,
	SYS_UMASK    = 32,
	SYS_CHMOD    = 33,
	SYS_FCHMOD   = 34,
	SYS_CHOWN    = 35,
	SYS_FCHOWN   = 36,

	/* --- Process Management --- */
	SYS_FORK     = 37,
	SYS_EXEC     = 38,
	SYS_EXECVE   = 39,
	SYS_WAIT     = 40,
	SYS_WAITPID  = 41,
	SYS_GETPID   = 42,
	SYS_GETUID   = 43,
	SYS_GETEUID  = 44,
	SYS_GETGID   = 45,
	SYS_GETEGID  = 46,
	SYS_SETUID   = 47,
	SYS_SETGID   = 48,
	SYS_SLEEP    = 49,
	SYS_KILL     = 50,
	SYS_SIGNAL   = 51,
	SYS_SETSID   = 52,
	SYS_GETPGRP  = 53,
	SYS_SETPGRP  = 54,
	SYS_SETPRIORITY = 55,
	SYS_GETPRIORITY = 56,

	/* --- Memory Management --- */
	SYS_BRK      = 57,
	SYS_MMAP     = 58,
	SYS_MUNMAP   = 59,
	SYS_EXIT     = 60,

	/* --- IPC (MQ & SHM) --- */
	SYS_MQ_OPEN    = 61,
	SYS_MQ_SEND    = 62,
	SYS_MQ_RECEIVE = 63,
	SYS_MQ_CLOSE   = 64,
	SYS_MQ_UNLINK  = 65,
	SYS_SHMGET     = 66,
	SYS_SHMAT      = 67,
	SYS_SHMDT      = 68,
	SYS_SHMCTL     = 69,

	/* --- Networking --- */
	SYS_SOCKET   = 70,
	SYS_BIND     = 71,
	SYS_CONNECT  = 72,
	SYS_SEND     = 73,
	SYS_RECV     = 74,
	SYS_NET_INFO = 75,
	SYS_NET_PING = 76,
	SYS_NET_DNS  = 77,
	SYS_LISTEN   = 78,
	SYS_ACCEPT   = 79,

	/* --- System Info & Misc --- */
	SYS_TIME     = 80,
	SYS_UNAME    = 81,
	SYS_GETCWD   = 82,
	SYS_CHDIR    = 83,
	SYS_REBOOT   = 84,
	SYS_DMESG    = 85,
	SYS_MOUNT    = 86,
	SYS_UMOUNT   = 87,
	SYS_MOUNTS   = 88,
	SYS_PS       = 89,
	SYS_CLEAR    = 90,
	SYS_SET_STDOUT = 91,
	SYS_READ_KBD = 92,
	SYS_TERMIOS_GET = 93,
	SYS_TERMIOS_SET = 94,
	SYS_SELFHOST_STATUS = 95,
	SYS_LINK     = 96,
	SYS_POLL     = 97,
	SYS_MPROTECT = 98,
	SYS_SIGRETURN = 99,
	SYS_CLOCK_GETTIME = 100,
	SYS_IO_SETUP = 101,
	SYS_IO_SUBMIT = 102,
	SYS_IO_GETEVENTS = 103,
	SYS_SIGPROCMASK = 104,
	SYS_GETCPU   = 105,
};

#include <b1nix/arch_x86.h>
u64 syscall_dispatch_impl(u64 number, u64 arg0, u64 arg1, u64 arg2, u64 arg3, u64 arg4, u64 arg5, struct interrupt_frame *frame);

#define SYSCALL_DISPATCH_X(x, n, a0, a1, a2, a3, a4, a5, f, FUNC, ...) FUNC
#define syscall_dispatch(...) SYSCALL_DISPATCH_X(, ##__VA_ARGS__, syscall_dispatch_impl(__VA_ARGS__), syscall_dispatch_impl(__VA_ARGS__, 0))
int syscall_copyin(void *dst, const void *user_src, usize size);
int syscall_copyout(void *user_dst, const void *src, usize size);
int syscall_copyinstr(char *dst, usize dst_size, const char *user_src);
void free_kernel_array(char **k_array);

#endif
