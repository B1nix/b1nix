#ifndef B1NIX_U_SYSCALL_H
#define B1NIX_U_SYSCALL_H

/* Syscall numbers (mirrored from kernel) */
enum {
	SYS_WRITE    = 1,
	SYS_EXIT     = 2,
	SYS_OPEN     = 7,
	SYS_READ     = 8,
	SYS_CLOSE    = 9,
	SYS_CREATE   = 10,
	SYS_CLEAR    = 14,
	SYS_MMAP     = 22,
	SYS_SLEEP    = 23,
	SYS_STAT     = 48,
	SYS_LSEEK    = 49,
	SYS_UNLINK   = 50,
	SYS_MKDIR    = 51,
	SYS_CHDIR    = 52,
	SYS_MUNMAP   = 57,
	SYS_GETCWD   = 75,
	SYS_UNAME    = 76,
	SYS_TIME     = 77,
	SYS_DMESG    = 78,
	SYS_MOUNTS   = 79,
	SYS_SETPRIORITY = 84,
	SYS_GETPRIORITY = 85,
	SYS_SETSID   = 86,
	SYS_GETPGRP  = 87,
	SYS_SETPGRP  = 88,
};

/* Raw syscall — uses int $0x80 convention for kernel threads */
static inline long syscall(long num, long a0, long a1, long a2, long a3)
{
	long ret;
	register long r10 __asm__("r10") = a3;
	register long r8  __asm__("r8")  = a2;
	register long rdx __asm__("rdx") = a1;
	register long rsi __asm__("rsi") = a0;
	register long rdi __asm__("rdi") = num;

	__asm__ volatile(
		"syscall"
		: "=a"(ret)
		: "r"(rdi), "r"(rsi), "r"(rdx), "r"(r8), "r"(r10)
		: "rcx", "r11", "memory"
	);
	return ret;
}

#endif
