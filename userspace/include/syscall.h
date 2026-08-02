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

  /* --- M32: networking & multiplexing --- */
  SYS_SELECT       = 111,

  /* --- M25 libc completeness --- */
  SYS_UTIME        = 112,
  SYS_GETRANDOM    = 113,

  /* --- M32b: socket option / address API hardening --- */
  SYS_SETSOCKOPT   = 114,
  SYS_GETSOCKOPT   = 115,
  SYS_GETSOCKNAME  = 116,
  SYS_GETPEERNAME  = 117,
  SYS_SHUTDOWN     = 118,
  SYS_SETEUID      = 119,
  SYS_SETEGID      = 120,
  SYS_GETGROUPS    = 121,
  SYS_SETGROUPS    = 122,
  SYS_GETSID       = 123,
  SYS_SYSINFO      = 124,
  SYS_GETPPID      = 125,
  SYS_SIGSUSPEND   = 126,
  SYS_ALARM        = 127,
  SYS_FCHDIR       = 128,
  SYS_ACCESS       = 129,
  SYS_FTRUNCATE    = 130,
  SYS_DUP          = 131,
  SYS_GETRLIMIT    = 132,
  SYS_SETRLIMIT    = 133,
  SYS_SETXATTR     = 134,
  SYS_GETXATTR     = 135,
  SYS_LISTXATTR    = 136,
  SYS_REMOVEXATTR  = 137,
  SYS_GETPGID      = 138,
  SYS_SETREUID     = 139,
  SYS_SETREGID     = 140,
  SYS_SETRESUID    = 141,
  SYS_SETRESGID    = 142,
  SYS_WAITID       = 143,
  SYS_TIMES        = 144,
  SYS_GETRUSAGE    = 145,
  SYS_SENDMSG      = 146,
  SYS_RECVMSG      = 147,
  SYS_MEMFD_CREATE = 148,
  SYS_SETTIMEOFDAY = 149,
  SYS_SCHED_GETAFFINITY = 150,

  /* --- M56: event-loop & IPC primitives --- */
  SYS_EVENTFD2        = 151,
  SYS_EPOLL_CREATE1   = 152,
  SYS_EPOLL_CTL       = 153,
  SYS_EPOLL_WAIT      = 154,
  SYS_TIMERFD_CREATE  = 155,
  SYS_TIMERFD_SETTIME = 156,
  SYS_SIGNALFD4       = 157,
  /* --- M57: multiprocess broker primitives --- */
  SYS_SOCKETPAIR      = 158,
  /* --- POSIX memory / signal completeness --- */
  SYS_MADVISE         = 159,
  SYS_SIGALTSTACK     = 160,
  SYS_GET_TLS_INFO    = 161,
  SYS_RT_TGSIGQUEUEINFO = 162,
  SYS_GETDENTS64      = 163,  /* Linux getdents64 byte layout (Chromium port) */
  SYS_MINCORE         = 164,  /* page residency bitmap (Chromium memory-dump) */
  /* --- M73: modern I/O & introspection syscalls --- */
  SYS_SENDFILE         = 165,
  SYS_COPY_FILE_RANGE  = 166,
  SYS_SPLICE           = 167,
  SYS_FALLOCATE        = 168,
  SYS_STATX            = 169,
  SYS_INOTIFY_INIT1    = 170,
  SYS_INOTIFY_ADD_WATCH = 171,
  SYS_INOTIFY_RM_WATCH = 172,
  SYS_MSYNC            = 173,
  SYS_SECCOMP          = 174,
  SYS_PRCTL            = 175,
  SYS_SIGQUEUE         = 176,
  SYS_TIMER_CREATE     = 177,
  SYS_TIMER_SETTIME    = 178,
  SYS_TIMER_GETTIME    = 179,
  SYS_TIMER_DELETE     = 180,
  SYS_DL_PHDR_INFO     = 181,
  SYS_FD_PATH          = 182,
  /* --- M92: thread/process exit separation --- */
  SYS_EXIT_GROUP       = 231, /* exit_group: terminate all threads (Linux 231) */
  SYS_SET_TID_ADDRESS  = 232,
  SYS_READV             = 233,
  SYS_WRITEV            = 234,
  SYS_NANOSLEEP         = 235,
  SYS_EXECVEAT          = 236,
  SYS_SENDTO            = 237,
  SYS_RECVFROM          = 238,
  SYS_MKNOD             = 239, /* mknod(path, mode, dev) — mkfifo's backing call */
  SYS_FLOCK             = 240, /* flock(fd, operation) */
  SYS_CLOCK_GETRES      = 241, /* clock_getres(clk_id, res) */
  SYS_SIGTIMEDWAIT      = 242, /* sigtimedwait(set, info, timeout) */
  /* --- M95: loadable kernel modules --- */
  SYS_INIT_MODULE       = 243, /* init_module(image, len, params) */
  SYS_DELETE_MODULE     = 244, /* delete_module(name, flags) */
  SYS_FINIT_MODULE      = 245, /* finit_module(fd, params, flags) */
};

/* PT_TLS template for the running image, returned by SYS_GET_TLS_INFO so the
 * libc can build a per-thread ELF TLS block in pthread_create (the kernel sets
 * up only the main thread's TLS at exec). memsz==0 => the binary has no TLS. */
/* Fixed-width 64-bit fields to match the kernel's `u64 memsz, filesz, align`
 * layout exactly. `unsigned long` is 4 bytes on i686, which mis-paired the
 * fields against the kernel's 8-byte fields (userspace filesz read the high
 * half of the kernel's memsz = 0) and overflowed this buffer by 12 bytes —
 * breaking per-thread TLS init on the 32-bit port (thread-locals read 0
 * instead of their .tdata image). 8 bytes on both i686 and x86_64. */
struct b1nix_tls_info {
  unsigned long long memsz;  /* total TLS size (.tdata + .tbss) */
  unsigned long long filesz; /* initialised-image size (.tdata) */
  unsigned long long align;  /* TLS segment alignment (>=1) */
};

/* Linux-compatible CLONE_* flag bits (subset honored by b1nix). */
#define CLONE_VM       0x00000100
#define CLONE_FS       0x00000200
#define CLONE_FILES    0x00000400
#define CLONE_SIGHAND  0x00000800
#define CLONE_THREAD   0x00010000
#define CLONE_SETTLS   0x00080000
#define CLONE_PARENT   0x00008000
#define CLONE_NEWNS    0x00020000
#define CLONE_SYSVSEM  0x00040000
#define CLONE_PARENT_SETTID  0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_DETACHED 0x00400000
#define CLONE_CHILD_SETTID   0x01000000
#define CLONE_NEWUTS   0x04000000
#define CLONE_NEWIPC   0x08000000
#define CLONE_NEWUSER  0x10000000
#define CLONE_NEWPID   0x20000000
#define CLONE_NEWNET   0x40000000
#define CLONE_IO       0x80000000

/* Linux-compatible FUTEX_* op codes (subset). */
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1

/* SYS_REBOOT commands (arg0). */
#define B1NIX_REBOOT_RESTART  0
#define B1NIX_REBOOT_POWEROFF 1
#define B1NIX_REBOOT_HALT     2

/* Raw syscall for the x86_64 B1NIX syscall ABI. */
static inline long _syscall_raw(long num, long a0, long a1, long a2, long a3, long a4, long a5) {
#ifdef __x86_64__
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
#else
  long ret;
  register long ebx __asm__("ebx") = a0;
  register long ecx __asm__("ecx") = a1;
  register long edx __asm__("edx") = a2;
  register long esi __asm__("esi") = a3;
  register long edi __asm__("edi") = a4;
  register long eax __asm__("eax") = num;

  /* arg5 travels in EBP, which is also the compiler's frame-pointer register, so
   * touching it is dangerous. The kernel only reads EBP for genuine 6-argument
   * syscalls; for everything else arg5 is the macro's filler 0 and the handler
   * ignores it. So:
   *
   *  - When arg5 is a compile-time 0 (the overwhelming majority — every <6-arg
   *    call, including fork/clone-less paths), DON'T touch EBP at all. The trap
   *    frame then records the caller's real frame pointer, which matters because
   *    fork() copies that frame into the child: if EBP were 0 there, the child
   *    would resume with a null frame pointer and crash at the next function
   *    epilogue (`lea -k(%ebp),%esp`). The kernel harmlessly sees the real EBP
   *    as an unused arg5.
   *  - When arg5 is non-zero/non-constant (true 6-arg syscalls like mmap with a
   *    real offset), save/restore EBP around the trap and load arg5 into it.
   */
  if (__builtin_constant_p(a5) && (a5) == 0) {
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "r"(eax), "r"(ebx), "r"(ecx), "r"(edx), "r"(esi),
                       "r"(edi)
                     : "memory");
  } else {
    __asm__ volatile(
        "pushl %%ebp\n\t"
        "movl %[a5v], %%ebp\n\t"
        "int $0x80\n\t"
        "popl %%ebp\n\t"
        : "=a"(ret)
        : "r"(eax), "r"(ebx), "r"(ecx), "r"(edx), "r"(esi), "r"(edi),
          [a5v] "g"(a5)
        : "memory", "cc");
  }
  return ret;
#endif
}

#define syscall(num, ...) _syscall_route(num, ##__VA_ARGS__, 0, 0, 0, 0, 0, 0, 0)
#define _syscall_route(num, a0, a1, a2, a3, a4, a5, ...) _syscall_raw(num, (long)(a0), (long)(a1), (long)(a2), (long)(a3), (long)(a4), (long)(a5))

/* Linux __NR_ aliases for ports that call syscall(__NR_*) directly. */
#define __NR_gettid SYS_GETTID
#define __NR_getrandom SYS_GETRANDOM
#define __NR_futex SYS_FUTEX
#define __NR_getppid SYS_GETPPID
#define __NR_getsid SYS_GETSID
#define __NR_getpriority SYS_GETPRIORITY
#define __NR_rt_sigreturn SYS_SIGRETURN
#define __NR_getitimer 1023  /* b1nix lacks it; raw syscall -> ENOSYS */
#ifndef __NR_accept
#define __NR_accept SYS_ACCEPT
#endif
#ifndef __NR_access
#define __NR_access SYS_ACCESS
#endif
#ifndef __NR_alarm
#define __NR_alarm SYS_ALARM
#endif
#ifndef __NR_bind
#define __NR_bind SYS_BIND
#endif
#ifndef __NR_brk
#define __NR_brk SYS_BRK
#endif
#ifndef __NR_chdir
#define __NR_chdir SYS_CHDIR
#endif
#ifndef __NR_chmod
#define __NR_chmod SYS_CHMOD
#endif
#ifndef __NR_chown
#define __NR_chown SYS_CHOWN
#endif
#ifndef __NR_clear
#define __NR_clear SYS_CLEAR
#endif
#ifndef __NR_clock_gettime
#define __NR_clock_gettime SYS_CLOCK_GETTIME
#endif
#ifndef __NR_clone
#define __NR_clone SYS_CLONE
#endif
#ifndef __NR_close
#define __NR_close SYS_CLOSE
#endif
#ifndef __NR_connect
#define __NR_connect SYS_CONNECT
#endif
#ifndef __NR_create
#define __NR_create SYS_CREATE
#endif
#ifndef __NR_dmesg
#define __NR_dmesg SYS_DMESG
#endif
#ifndef __NR_dup
#define __NR_dup SYS_DUP
#endif
#ifndef __NR_dup2
#define __NR_dup2 SYS_DUP2
#endif
#ifndef __NR_epoll_create1
#define __NR_epoll_create1 SYS_EPOLL_CREATE1
#endif
#ifndef __NR_epoll_ctl
#define __NR_epoll_ctl SYS_EPOLL_CTL
#endif
#ifndef __NR_epoll_wait
#define __NR_epoll_wait SYS_EPOLL_WAIT
#endif
#ifndef __NR_eventfd2
#define __NR_eventfd2 SYS_EVENTFD2
#endif
#ifndef __NR_exec
#define __NR_exec SYS_EXEC
#endif
#ifndef __NR_execve
#define __NR_execve SYS_EXECVE
#endif
#ifndef __NR_exit
#define __NR_exit SYS_EXIT
#endif
#ifndef __NR_exit_thread
#define __NR_exit_thread SYS_EXIT_THREAD
#endif
#ifndef __NR_exit_group
#define __NR_exit_group SYS_EXIT_GROUP
#endif
#ifndef __NR_fchdir
#define __NR_fchdir SYS_FCHDIR
#endif
#ifndef __NR_fchmod
#define __NR_fchmod SYS_FCHMOD
#endif
#ifndef __NR_fchown
#define __NR_fchown SYS_FCHOWN
#endif
#ifndef __NR_fcntl
#define __NR_fcntl SYS_FCNTL
#endif
#ifndef __NR_fork
#define __NR_fork SYS_FORK
#endif
#ifndef __NR_fstat
#define __NR_fstat SYS_FSTAT
#endif
#ifndef __NR_fstatfs
#define __NR_fstatfs SYS_FSTATFS
#endif
#ifndef __NR_fsync
#define __NR_fsync SYS_FSYNC
#endif
#ifndef __NR_ftruncate
#define __NR_ftruncate SYS_FTRUNCATE
#endif
#ifndef __NR_futex
#define __NR_futex SYS_FUTEX
#endif
#ifndef __NR_getcpu
#define __NR_getcpu SYS_GETCPU
#endif
#ifndef __NR_getcwd
#define __NR_getcwd SYS_GETCWD
#endif
#ifndef __NR_getdents
#define __NR_getdents SYS_GETDENTS
#endif
#ifndef __NR_getdents64
#define __NR_getdents64 SYS_GETDENTS64
#endif
#ifndef __NR_getegid
#define __NR_getegid SYS_GETEGID
#endif
#ifndef __NR_geteuid
#define __NR_geteuid SYS_GETEUID
#endif
#ifndef __NR_getgid
#define __NR_getgid SYS_GETGID
#endif
#ifndef __NR_getgroups
#define __NR_getgroups SYS_GETGROUPS
#endif
#ifndef __NR_getpeername
#define __NR_getpeername SYS_GETPEERNAME
#endif
#ifndef __NR_getpgid
#define __NR_getpgid SYS_GETPGID
#endif
#ifndef __NR_getpgrp
#define __NR_getpgrp SYS_GETPGRP
#endif
#ifndef __NR_getpid
#define __NR_getpid SYS_GETPID
#endif
#ifndef __NR_getppid
#define __NR_getppid SYS_GETPPID
#endif
#ifndef __NR_getpriority
#define __NR_getpriority SYS_GETPRIORITY
#endif
#ifndef __NR_getrandom
#define __NR_getrandom SYS_GETRANDOM
#endif
#ifndef __NR_getrlimit
#define __NR_getrlimit SYS_GETRLIMIT
#endif
#ifndef __NR_getrusage
#define __NR_getrusage SYS_GETRUSAGE
#endif
#ifndef __NR_getsid
#define __NR_getsid SYS_GETSID
#endif
#ifndef __NR_getsockname
#define __NR_getsockname SYS_GETSOCKNAME
#endif
#ifndef __NR_getsockopt
#define __NR_getsockopt SYS_GETSOCKOPT
#endif
#ifndef __NR_gettid
#define __NR_gettid SYS_GETTID
#endif
#ifndef __NR_get_tls_info
#define __NR_get_tls_info SYS_GET_TLS_INFO
#endif
#ifndef __NR_getuid
#define __NR_getuid SYS_GETUID
#endif
#ifndef __NR_getxattr
#define __NR_getxattr SYS_GETXATTR
#endif
#ifndef __NR_ioctl
#define __NR_ioctl SYS_IOCTL
#endif
#ifndef __NR_io_getevents
#define __NR_io_getevents SYS_IO_GETEVENTS
#endif
#ifndef __NR_io_setup
#define __NR_io_setup SYS_IO_SETUP
#endif
#ifndef __NR_io_submit
#define __NR_io_submit SYS_IO_SUBMIT
#endif
#ifndef __NR_kill
#define __NR_kill SYS_KILL
#endif
#ifndef __NR_link
#define __NR_link SYS_LINK
#endif
#ifndef __NR_list
#define __NR_list SYS_LIST
#endif
#ifndef __NR_listen
#define __NR_listen SYS_LISTEN
#endif
#ifndef __NR_listxattr
#define __NR_listxattr SYS_LISTXATTR
#endif
#ifndef __NR_lseek
#define __NR_lseek SYS_LSEEK
#endif
#ifndef __NR_lstat
#define __NR_lstat SYS_LSTAT
#endif
#ifndef __NR_madvise
#define __NR_madvise SYS_MADVISE
#endif
#ifndef __NR_mem
#define __NR_mem SYS_MEM
#endif
#ifndef __NR_memfd_create
#define __NR_memfd_create SYS_MEMFD_CREATE
#endif
#ifndef __NR_mkdir
#define __NR_mkdir SYS_MKDIR
#endif
#ifndef __NR_mmap
#define __NR_mmap SYS_MMAP
#endif
#ifndef __NR_mount
#define __NR_mount SYS_MOUNT
#endif
#ifndef __NR_mounts
#define __NR_mounts SYS_MOUNTS
#endif
#ifndef __NR_mprotect
#define __NR_mprotect SYS_MPROTECT
#endif
#ifndef __NR_mq_close
#define __NR_mq_close SYS_MQ_CLOSE
#endif
#ifndef __NR_mq_open
#define __NR_mq_open SYS_MQ_OPEN
#endif
#ifndef __NR_mq_receive
#define __NR_mq_receive SYS_MQ_RECEIVE
#endif
#ifndef __NR_mq_send
#define __NR_mq_send SYS_MQ_SEND
#endif
#ifndef __NR_mq_unlink
#define __NR_mq_unlink SYS_MQ_UNLINK
#endif
#ifndef __NR_munmap
#define __NR_munmap SYS_MUNMAP
#endif
#ifndef __NR_net_dns
#define __NR_net_dns SYS_NET_DNS
#endif
#ifndef __NR_net_info
#define __NR_net_info SYS_NET_INFO
#endif
#ifndef __NR_net_ping
#define __NR_net_ping SYS_NET_PING
#endif
#ifndef __NR_open
#define __NR_open SYS_OPEN
#endif
#ifndef __NR_pipe
#define __NR_pipe SYS_PIPE
#endif
#ifndef __NR_poll
#define __NR_poll SYS_POLL
#endif
#ifndef __NR_ps
#define __NR_ps SYS_PS
#endif
#ifndef __NR_read
#define __NR_read SYS_READ
#endif
#ifndef __NR_readdir
#define __NR_readdir SYS_READDIR
#endif
#ifndef __NR_read_file
#define __NR_read_file SYS_READ_FILE
#endif
#ifndef __NR_read_kbd
#define __NR_read_kbd SYS_READ_KBD
#endif
#ifndef __NR_readlink
#define __NR_readlink SYS_READLINK
#endif
#ifndef __NR_reboot
#define __NR_reboot SYS_REBOOT
#endif
#ifndef __NR_recv
#define __NR_recv SYS_RECV
#endif
#ifndef __NR_recvmsg
#define __NR_recvmsg SYS_RECVMSG
#endif
#ifndef __NR_removexattr
#define __NR_removexattr SYS_REMOVEXATTR
#endif
#ifndef __NR_rename
#define __NR_rename SYS_RENAME
#endif
#ifndef __NR_rmdir
#define __NR_rmdir SYS_RMDIR
#endif
#ifndef __NR_rt_tgsigqueueinfo
#define __NR_rt_tgsigqueueinfo SYS_RT_TGSIGQUEUEINFO
#endif
#ifndef __NR_sched_getaffinity
#define __NR_sched_getaffinity SYS_SCHED_GETAFFINITY
#endif
#ifndef __NR_select
#define __NR_select SYS_SELECT
#endif
#ifndef __NR_selfhost_status
#define __NR_selfhost_status SYS_SELFHOST_STATUS
#endif
#ifndef __NR_send
#define __NR_send SYS_SEND
#endif
#ifndef __NR_sendmsg
#define __NR_sendmsg SYS_SENDMSG
#endif
#ifndef __NR_setegid
#define __NR_setegid SYS_SETEGID
#endif
#ifndef __NR_seteuid
#define __NR_seteuid SYS_SETEUID
#endif
#ifndef __NR_setgid
#define __NR_setgid SYS_SETGID
#endif
#ifndef __NR_setgroups
#define __NR_setgroups SYS_SETGROUPS
#endif
#ifndef __NR_setpgrp
#define __NR_setpgrp SYS_SETPGRP
#endif
#ifndef __NR_setpriority
#define __NR_setpriority SYS_SETPRIORITY
#endif
#ifndef __NR_setregid
#define __NR_setregid SYS_SETREGID
#endif
#ifndef __NR_setresgid
#define __NR_setresgid SYS_SETRESGID
#endif
#ifndef __NR_setresuid
#define __NR_setresuid SYS_SETRESUID
#endif
#ifndef __NR_setreuid
#define __NR_setreuid SYS_SETREUID
#endif
#ifndef __NR_setrlimit
#define __NR_setrlimit SYS_SETRLIMIT
#endif
#ifndef __NR_setsid
#define __NR_setsid SYS_SETSID
#endif
#ifndef __NR_setsockopt
#define __NR_setsockopt SYS_SETSOCKOPT
#endif
#ifndef __NR_set_stdout
#define __NR_set_stdout SYS_SET_STDOUT
#endif
#ifndef __NR_settimeofday
#define __NR_settimeofday SYS_SETTIMEOFDAY
#endif
#ifndef __NR_set_tls
#define __NR_set_tls SYS_SET_TLS
#endif
#ifndef __NR_setuid
#define __NR_setuid SYS_SETUID
#endif
#ifndef __NR_setxattr
#define __NR_setxattr SYS_SETXATTR
#endif
#ifndef __NR_shmat
#define __NR_shmat SYS_SHMAT
#endif
#ifndef __NR_shmctl
#define __NR_shmctl SYS_SHMCTL
#endif
#ifndef __NR_shmdt
#define __NR_shmdt SYS_SHMDT
#endif
#ifndef __NR_shmget
#define __NR_shmget SYS_SHMGET
#endif
#ifndef __NR_shutdown
#define __NR_shutdown SYS_SHUTDOWN
#endif
#ifndef __NR_sigaltstack
#define __NR_sigaltstack SYS_SIGALTSTACK
#endif
#ifndef __NR_signal
#define __NR_signal SYS_SIGNAL
#endif
#ifndef __NR_signalfd4
#define __NR_signalfd4 SYS_SIGNALFD4
#endif
#ifndef __NR_sigprocmask
#define __NR_sigprocmask SYS_SIGPROCMASK
#endif
#ifndef __NR_sigreturn
#define __NR_sigreturn SYS_SIGRETURN
#endif
#ifndef __NR_sigsuspend
#define __NR_sigsuspend SYS_SIGSUSPEND
#endif
#ifndef __NR_sleep
#define __NR_sleep SYS_SLEEP
#endif
#ifndef __NR_socket
#define __NR_socket SYS_SOCKET
#endif
#ifndef __NR_socketpair
#define __NR_socketpair SYS_SOCKETPAIR
#endif
#ifndef __NR_spawn
#define __NR_spawn SYS_SPAWN
#endif
#ifndef __NR_stat
#define __NR_stat SYS_STAT
#endif
#ifndef __NR_statfs
#define __NR_statfs SYS_STATFS
#endif
#ifndef __NR_symlink
#define __NR_symlink SYS_SYMLINK
#endif
#ifndef __NR_sync
#define __NR_sync SYS_SYNC
#endif
#ifndef __NR_syncfs
#define __NR_syncfs SYS_SYNCFS
#endif
#ifndef __NR_sysinfo
#define __NR_sysinfo SYS_SYSINFO
#endif
#ifndef __NR_termios_get
#define __NR_termios_get SYS_TERMIOS_GET
#endif
#ifndef __NR_termios_set
#define __NR_termios_set SYS_TERMIOS_SET
#endif
#ifndef __NR_time
#define __NR_time SYS_TIME
#endif
#ifndef __NR_timerfd_create
#define __NR_timerfd_create SYS_TIMERFD_CREATE
#endif
#ifndef __NR_timerfd_settime
#define __NR_timerfd_settime SYS_TIMERFD_SETTIME
#endif
#ifndef __NR_times
#define __NR_times SYS_TIMES
#endif
#ifndef __NR_umask
#define __NR_umask SYS_UMASK
#endif
#ifndef __NR_umount
#define __NR_umount SYS_UMOUNT
#endif
#ifndef __NR_uname
#define __NR_uname SYS_UNAME
#endif
#ifndef __NR_unlink
#define __NR_unlink SYS_UNLINK
#endif
#ifndef __NR_utime
#define __NR_utime SYS_UTIME
#endif
#ifndef __NR_wait
#define __NR_wait SYS_WAIT
#endif
#ifndef __NR_waitid
#define __NR_waitid SYS_WAITID
#endif
#ifndef __NR_waitpid
#define __NR_waitpid SYS_WAITPID
#endif
#ifndef __NR_write
#define __NR_write SYS_WRITE
#endif
#ifndef __NR_yield
#define __NR_yield SYS_YIELD
#endif

/* Lowercase Linux SYS_<name> aliases (glibc <sys/syscall.h> style) for ports
 * that call syscall(SYS_<name>) directly — Chromium, abseil, partition_alloc.
 * One alias per b1nix enum entry; they map onto the matching b1nix syscall so
 * the raw syscall() hits the correct entry. Kept complete so this whole error
 * category never recurs — regenerate from the enum if you add a syscall. */
#define SYS_accept SYS_ACCEPT
#define SYS_access SYS_ACCESS
#define SYS_alarm SYS_ALARM
#define SYS_bind SYS_BIND
#define SYS_brk SYS_BRK
#define SYS_chdir SYS_CHDIR
#define SYS_chmod SYS_CHMOD
#define SYS_chown SYS_CHOWN
#define SYS_clear SYS_CLEAR
#define SYS_clock_gettime SYS_CLOCK_GETTIME
#define SYS_clone SYS_CLONE
#define SYS_close SYS_CLOSE
#define SYS_connect SYS_CONNECT
#define SYS_create SYS_CREATE
#define SYS_dmesg SYS_DMESG
#define SYS_dup SYS_DUP
#define SYS_dup2 SYS_DUP2
#define SYS_epoll_create1 SYS_EPOLL_CREATE1
#define SYS_epoll_ctl SYS_EPOLL_CTL
#define SYS_epoll_wait SYS_EPOLL_WAIT
#define SYS_eventfd2 SYS_EVENTFD2
#define SYS_exec SYS_EXEC
#define SYS_execve SYS_EXECVE
#define SYS_exit SYS_EXIT
#define SYS_exit_thread SYS_EXIT_THREAD
#define SYS_fchdir SYS_FCHDIR
#define SYS_fchmod SYS_FCHMOD
#define SYS_fchown SYS_FCHOWN
#define SYS_fcntl SYS_FCNTL
#define SYS_fork SYS_FORK
#define SYS_fstat SYS_FSTAT
#define SYS_fstatfs SYS_FSTATFS
#define SYS_fsync SYS_FSYNC
#define SYS_ftruncate SYS_FTRUNCATE
#define SYS_futex SYS_FUTEX
#define SYS_getcpu SYS_GETCPU
#define SYS_getcwd SYS_GETCWD
#define SYS_getdents SYS_GETDENTS
#define SYS_getdents64 SYS_GETDENTS64
#define SYS_mincore SYS_MINCORE
#define SYS_getegid SYS_GETEGID
#define SYS_geteuid SYS_GETEUID
#define SYS_getgid SYS_GETGID
#define SYS_getgroups SYS_GETGROUPS
#define SYS_getpeername SYS_GETPEERNAME
#define SYS_getpgid SYS_GETPGID
#define SYS_getpgrp SYS_GETPGRP
#define SYS_getpid SYS_GETPID
#define SYS_getppid SYS_GETPPID
#define SYS_getpriority SYS_GETPRIORITY
#define SYS_getrandom SYS_GETRANDOM
#define SYS_getrlimit SYS_GETRLIMIT
#define SYS_getrusage SYS_GETRUSAGE
#define SYS_getsid SYS_GETSID
#define SYS_getsockname SYS_GETSOCKNAME
#define SYS_getsockopt SYS_GETSOCKOPT
#define SYS_gettid SYS_GETTID
#define SYS_get_tls_info SYS_GET_TLS_INFO
#define SYS_getuid SYS_GETUID
#define SYS_getxattr SYS_GETXATTR
#define SYS_ioctl SYS_IOCTL
#define SYS_io_getevents SYS_IO_GETEVENTS
#define SYS_io_setup SYS_IO_SETUP
#define SYS_io_submit SYS_IO_SUBMIT
#define SYS_kill SYS_KILL
#define SYS_link SYS_LINK
#define SYS_list SYS_LIST
#define SYS_listen SYS_LISTEN
#define SYS_listxattr SYS_LISTXATTR
#define SYS_lseek SYS_LSEEK
#define SYS_lstat SYS_LSTAT
#define SYS_madvise SYS_MADVISE
#define SYS_mem SYS_MEM
#define SYS_memfd_create SYS_MEMFD_CREATE
#define SYS_mkdir SYS_MKDIR
#define SYS_mmap SYS_MMAP
#define SYS_mount SYS_MOUNT
#define SYS_mounts SYS_MOUNTS
#define SYS_mprotect SYS_MPROTECT
#define SYS_mq_close SYS_MQ_CLOSE
#define SYS_mq_open SYS_MQ_OPEN
#define SYS_mq_receive SYS_MQ_RECEIVE
#define SYS_mq_send SYS_MQ_SEND
#define SYS_mq_unlink SYS_MQ_UNLINK
#define SYS_munmap SYS_MUNMAP
#define SYS_net_dns SYS_NET_DNS
#define SYS_net_info SYS_NET_INFO
#define SYS_net_ping SYS_NET_PING
#define SYS_open SYS_OPEN
#define SYS_pipe SYS_PIPE
#define SYS_poll SYS_POLL
#define SYS_ps SYS_PS
#define SYS_read SYS_READ
#define SYS_readdir SYS_READDIR
#define SYS_read_file SYS_READ_FILE
#define SYS_read_kbd SYS_READ_KBD
#define SYS_readlink SYS_READLINK
#define SYS_reboot SYS_REBOOT
#define SYS_recv SYS_RECV
#define SYS_recvmsg SYS_RECVMSG
#define SYS_removexattr SYS_REMOVEXATTR
#define SYS_rename SYS_RENAME
#define SYS_rmdir SYS_RMDIR
#define SYS_rt_tgsigqueueinfo SYS_RT_TGSIGQUEUEINFO
#define SYS_sched_getaffinity SYS_SCHED_GETAFFINITY
#define SYS_select SYS_SELECT
#define SYS_selfhost_status SYS_SELFHOST_STATUS
#define SYS_send SYS_SEND
#define SYS_sendmsg SYS_SENDMSG
#define SYS_setegid SYS_SETEGID
#define SYS_seteuid SYS_SETEUID
#define SYS_setgid SYS_SETGID
#define SYS_setgroups SYS_SETGROUPS
#define SYS_setpgrp SYS_SETPGRP
#define SYS_setpriority SYS_SETPRIORITY
#define SYS_setregid SYS_SETREGID
#define SYS_setresgid SYS_SETRESGID
#define SYS_setresuid SYS_SETRESUID
#define SYS_setreuid SYS_SETREUID
#define SYS_setrlimit SYS_SETRLIMIT
#define SYS_setsid SYS_SETSID
#define SYS_setsockopt SYS_SETSOCKOPT
#define SYS_set_stdout SYS_SET_STDOUT
#define SYS_settimeofday SYS_SETTIMEOFDAY
#define SYS_set_tls SYS_SET_TLS
#define SYS_setuid SYS_SETUID
#define SYS_setxattr SYS_SETXATTR
#define SYS_shmat SYS_SHMAT
#define SYS_shmctl SYS_SHMCTL
#define SYS_shmdt SYS_SHMDT
#define SYS_shmget SYS_SHMGET
#define SYS_shutdown SYS_SHUTDOWN
#define SYS_sigaltstack SYS_SIGALTSTACK
#define SYS_signalfd4 SYS_SIGNALFD4
#define SYS_sigprocmask SYS_SIGPROCMASK
#define SYS_sigreturn SYS_SIGRETURN
#define SYS_sigsuspend SYS_SIGSUSPEND
#define SYS_sleep SYS_SLEEP
#define SYS_nanosleep SYS_NANOSLEEP
#define SYS_socket SYS_SOCKET
#define SYS_socketpair SYS_SOCKETPAIR
#define SYS_spawn SYS_SPAWN
#define SYS_stat SYS_STAT
#define SYS_statfs SYS_STATFS
#define SYS_symlink SYS_SYMLINK
#define SYS_sync SYS_SYNC
#define SYS_syncfs SYS_SYNCFS
#define SYS_sysinfo SYS_SYSINFO
#define SYS_termios_get SYS_TERMIOS_GET
#define SYS_termios_set SYS_TERMIOS_SET
#define SYS_time SYS_TIME
#define SYS_timerfd_create SYS_TIMERFD_CREATE
#define SYS_timerfd_settime SYS_TIMERFD_SETTIME
#define SYS_times SYS_TIMES
#define SYS_umask SYS_UMASK
#define SYS_umount SYS_UMOUNT
#define SYS_uname SYS_UNAME
#define SYS_unlink SYS_UNLINK
#define SYS_utime SYS_UTIME
#define SYS_wait SYS_WAIT
#define SYS_waitid SYS_WAITID
#define SYS_waitpid SYS_WAITPID
#define SYS_write SYS_WRITE
#define SYS_yield SYS_YIELD

/* Linux-only syscall names that b1nix lacks: map to the closest b1nix call, or
 * to an out-of-range sentinel that the kernel rejects with -ENOSYS so the
 * caller takes its fallback path. */
#define SYS_rt_sigprocmask  SYS_SIGPROCMASK
/* rt_sigaction: 3-arg sig/act/oact (b1nix ignores the 4th sigsetsize arg).
 * NOTE: callers passing a Linux-layout struct (e.g. base/process/launch_posix's
 * kernel_sigaction) differ from b1nix `struct sigaction` — the between-fork/exec
 * handler reset is therefore best-effort, not bit-exact. */
#define SYS_rt_sigaction    SYS_SIGNAL
#define SYS_tgkill          SYS_KILL   /* drops tgid; b1nix tids are unique */
#define SYS_exit_group      SYS_EXIT_GROUP
#define SYS_clock_nanosleep 1024  /* -ENOSYS -> caller falls back to nanosleep */
#define SYS_pkey_mprotect   1025  /* b1nix has no protection keys -> -ENOSYS */
#define SYS_pkey_alloc      1026
#define SYS_pkey_free       1027

#endif
