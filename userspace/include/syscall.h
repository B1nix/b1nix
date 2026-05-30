#ifndef B1NIX_U_SYSCALL_H
#define B1NIX_U_SYSCALL_H

/* Syscall numbers (mirrored from kernel) */
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

  /* --- M29: Threads, Futex, TLS --- */
  SYS_CLONE        = 106,
  SYS_FUTEX        = 107,
  SYS_SET_TLS      = 108,
  SYS_GETTID       = 109,
  SYS_EXIT_THREAD  = 110,
};

/* Linux-compatible CLONE_* flag bits (subset honored by b1nix). */
#define CLONE_VM       0x00000100
#define CLONE_FS       0x00000200
#define CLONE_FILES    0x00000400
#define CLONE_SIGHAND  0x00000800
#define CLONE_THREAD   0x00010000
#define CLONE_SETTLS   0x00080000
#define CLONE_CHILD_CLEARTID 0x00200000

/* Linux-compatible FUTEX_* op codes (subset). */
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1

/* SYS_REBOOT commands (arg0). */
#define B1NIX_REBOOT_RESTART  0
#define B1NIX_REBOOT_POWEROFF 1
#define B1NIX_REBOOT_HALT     2

/* Raw syscall for the x86_64 B1NIX syscall ABI. */
static inline long _syscall_raw(long num, long a0, long a1, long a2, long a3, long a4, long a5) {
  long ret;
  register long rdi __asm__("rdi") = a0;
  register long rsi __asm__("rsi") = a1;
  register long rdx __asm__("rdx") = a2;
  register long r10 __asm__("r10") = a3;
  register long r8 __asm__("r8") = a4;
  register long r9 __asm__("r9") = a5;
  register long rax __asm__("rax") = num;

  __asm__ volatile("syscall"
                   : "=a"(ret)
                   : "r"(rax), "r"(rdi), "r"(rsi), "r"(rdx), "r"(r10), "r"(r8), "r"(r9)
                   : "rcx", "r11", "memory");
  return ret;
}

#define syscall(num, ...) _syscall_route(num, ##__VA_ARGS__, 0, 0, 0, 0, 0, 0, 0)
#define _syscall_route(num, a0, a1, a2, a3, a4, a5, ...) _syscall_raw(num, (long)(a0), (long)(a1), (long)(a2), (long)(a3), (long)(a4), (long)(a5))

#endif
