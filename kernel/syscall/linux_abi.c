/* M40 — Linux x86_64 syscall-number translation table.
 *
 * Maps Linux x86_64 syscall numbers (the canonical values from the kernel's
 * arch/x86/entry/syscalls/syscall_64.tbl, also exposed via <asm/unistd_64.h>)
 * to b1nix native numbers (<b1nix/syscall.h>). Only the subset that has a real
 * native handler is mapped; everything else falls through to LINUX_SYS_UNMAPPED
 * and the dispatcher returns -ENOSYS, exactly as Linux would for an
 * unimplemented call.
 *
 * This is deliberately a NUMBER map: a translated call dispatches into the same
 * sys_* handler a native b1nix task hits, so the semantics, copyin/copyout and
 * locking are shared and already tested.
 */

#include <b1nix/linux_abi.h>
#include <b1nix/posix.h>
#include <b1nix/syscall.h>

/* Linux x86_64 syscall numbers (subset). Named to mirror __NR_* so the table is
 * auditable against asm/unistd_64.h. */
#define LX_read            0
#define LX_write           1
#define LX_open            2
#define LX_close           3
#define LX_stat            4
#define LX_fstat           5
#define LX_lstat           6
#define LX_poll            7
#define LX_lseek           8
#define LX_mmap            9
#define LX_mprotect        10
#define LX_munmap          11
#define LX_brk             12
#define LX_rt_sigaction    13
#define LX_rt_sigprocmask  14
#define LX_rt_sigreturn    15
#define LX_ioctl           16
#define LX_readv           19
#define LX_writev          20
#define LX_access          21
#define LX_pipe            22
#define LX_select          23
#define LX_sched_yield     24
#define LX_madvise         28
#define LX_dup             32
#define LX_dup2            33
#define LX_nanosleep       35
#define LX_getpid          39
#define LX_socket          41
#define LX_connect         42
#define LX_accept          43
#define LX_sendto          44
#define LX_recvfrom        45
#define LX_shutdown        48
#define LX_bind            49
#define LX_listen          50
#define LX_getsockname     51
#define LX_getpeername     52
#define LX_setsockopt      54
#define LX_getsockopt      55
#define LX_clone           56
#define LX_fork            57
#define LX_vfork           58
#define LX_execve          59
#define LX_execveat        322
#define LX_exit            60
#define LX_wait4           61
#define LX_kill            62
#define LX_uname           63
#define LX_fcntl           72
#define LX_fsync           74
#define LX_getcwd          79
#define LX_chdir           80
#define LX_rename          82
#define LX_sysinfo         99
#define LX_times           100
#define LX_flock           73
#define LX_mknod           133
#define LX_sched_getaffinity 204
#define LX_clock_getres    229
#define LX_statx           332
#define LX_mkdir           83
#define LX_rmdir           84
#define LX_unlink          87
#define LX_symlink         88
#define LX_readlink        89
#define LX_chmod           90
#define LX_chown           92
#define LX_umask           95
#define LX_getuid          102
#define LX_getgid          104
#define LX_setuid          105
#define LX_setgid          106
#define LX_geteuid         107
#define LX_getegid         108
#define LX_setpgid         109
#define LX_getppid         110
#define LX_getpgrp         111
#define LX_setsid          112
#define LX_getpgid         121
#define LX_getsid          124
#define LX_alarm           37
#define LX_ftruncate       77
#define LX_fchdir          81
#define LX_link            86
#define LX_setpriority     141
#define LX_sync            162
#define LX_fchmod          91
#define LX_fchown          93
#define LX_gettid          186
#define LX_getdents64      217
#define LX_clock_gettime   228
#define LX_exit_group      231
/* --- M92: musl-required *at() and wrapper syscalls --- */
#define LX_openat          257
#define LX_newfstatat      262
#define LX_unlinkat        263
#define LX_mkdirat         258
#define LX_linkat          265
#define LX_symlinkat       266
#define LX_readlinkat      267
#define LX_fchmodat        268
#define LX_fchownat        260
#define LX_faccessat       269
#define LX_renameat2       316
#define LX_pipe2           293
#define LX_dup3            292
#define LX_ppoll           271
#define LX_pselect6        270
#define LX_accept4         288
#define LX_clock_nanosleep 230
#define LX_set_tid_address 218
#define LX_prlimit64       302
#define LX_set_robust_list 273
#define LX_get_robust_list 274
#define LX_prctl           157
#define LX_seccomp         317
#define LX_getrandom       318
#define LX_reboot          169
#define LX_io_setup        206
#define LX_io_submit       209
#define LX_io_getevents    208
#define LX_sendmsg         46
#define LX_recvmsg         47
#define LX_getgroups       115
#define LX_setgroups       116
#define LX_sigaltstack     131
#define LX_selfhost_status 511
/* musl event-loop syscalls */
#define LX_eventfd2        290
#define LX_eventfd         284
#define LX_epoll_create1   291
#define LX_epoll_ctl       233
#define LX_epoll_wait      232
#define LX_epoll_pwait     281
#define LX_timer_settime   223
#define LX_timer_gettime   224
#define LX_timer_delete    226
#define LX_timerfd_create  283
#define LX_timerfd_settime 286
#define LX_timerfd_gettime 287
#define LX_signalfd4       289
#define LX_signalfd        282
#define LX_memfd_create    319
/* M73 modern-I/O syscalls (b1nix handlers use the Linux argument layout) */
#define LX_msync           26
#define LX_sendfile        40
#define LX_splice          275
#define LX_fallocate       285
#define LX_inotify_init    253
#define LX_inotify_add_watch 254
#define LX_inotify_rm_watch  255
#define LX_inotify_init1   294
#define LX_copy_file_range 326
#define LX_socketpair      53
/* SysV shm: shmget(29) is a dispatcher special case (flag-bit remap). */
#define LX_shmat           30
#define LX_shmctl          31
#define LX_shmdt           67
/* Credential / resource syscalls with identical argument layouts */
#define LX_getrlimit       97
#define LX_getrusage       98
#define LX_setreuid        113
#define LX_setregid        114
#define LX_setresuid       117
#define LX_setresgid       119
#define LX_setrlimit       160
#define LX_mount           165
#define LX_umount2         166
#define LX_getcpu          309

/* The f* xattr variants take a descriptor and have no native handler, so they
 * stay unmapped (-ENOSYS). The follow/don't-follow argument these numbers imply
 * is supplied by the dispatcher — see LINUX_NR_*XATTR in <b1nix/linux_abi.h>. */
#define LX_setxattr        188
#define LX_lsetxattr       189
#define LX_getxattr        191
#define LX_lgetxattr       192
#define LX_listxattr       194
#define LX_llistxattr      195
#define LX_removexattr     197
#define LX_lremovexattr    198

struct lx_map {
	u64 linux_nr;
	u32 b1nix_nr;
	const char *name;
};

/* The table is small and lookups are linear; M40 favours clarity over speed
 * (syscall translation is not on a hot path for the supported workloads). */
static const struct lx_map lx_table[] = {
	{LX_read,           SYS_READ,          "read"},
	{LX_write,          SYS_WRITE,         "write"},
	{LX_open,           SYS_OPEN,          "open"},
	{LX_close,          SYS_CLOSE,         "close"},
	{LX_stat,           SYS_STAT,          "stat"},
	{LX_fstat,          SYS_FSTAT,         "fstat"},
	{LX_lstat,          SYS_LSTAT,         "lstat"},
	{LX_poll,           SYS_POLL,          "poll"},
	{LX_lseek,          SYS_LSEEK,         "lseek"},
	{LX_mmap,           SYS_MMAP,          "mmap"},
	{LX_mprotect,       SYS_MPROTECT,      "mprotect"},
	{LX_munmap,         SYS_MUNMAP,        "munmap"},
	{LX_brk,            SYS_BRK,           "brk"},
	{LX_rt_sigaction,   SYS_SIGNAL,        "rt_sigaction"},
	{LX_rt_sigprocmask, SYS_SIGPROCMASK,   "rt_sigprocmask"},
	{LX_rt_sigreturn,   SYS_SIGRETURN,     "rt_sigreturn"},
	{LX_ioctl,          SYS_IOCTL,         "ioctl"},
	{LX_readv,          SYS_READV,         "readv"},
	{LX_writev,         SYS_WRITEV,        "writev"},
	{LX_access,         SYS_ACCESS,        "access"},
	{LX_pipe,           SYS_PIPE,          "pipe"},
	{LX_select,         SYS_SELECT,        "select"},
	{LX_sched_yield,    SYS_YIELD,         "sched_yield"},
	{LX_madvise,        SYS_MADVISE,       "madvise"},
	{LX_dup,            SYS_DUP,           "dup"},
	{LX_dup2,           SYS_DUP2,          "dup2"},
	{LX_nanosleep,      SYS_NANOSLEEP,     "nanosleep"},
	{LX_getpid,         SYS_GETPID,        "getpid"},
	{LX_socket,         SYS_SOCKET,        "socket"},
	{LX_connect,        SYS_CONNECT,       "connect"},
	{LX_accept,         SYS_ACCEPT,        "accept"},
	{LX_sendto,         SYS_SENDTO,        "sendto"},
	{LX_recvfrom,       SYS_RECVFROM,      "recvfrom"},
	{LX_sendmsg,        SYS_SENDMSG,       "sendmsg"},
	{LX_recvmsg,        SYS_RECVMSG,       "recvmsg"},
	{LX_shutdown,       SYS_SHUTDOWN,      "shutdown"},
	{LX_bind,           SYS_BIND,          "bind"},
	{LX_listen,         SYS_LISTEN,        "listen"},
	{LX_getsockname,    SYS_GETSOCKNAME,   "getsockname"},
	{LX_getpeername,    SYS_GETPEERNAME,   "getpeername"},
	{LX_setsockopt,     SYS_SETSOCKOPT,    "setsockopt"},
	{LX_getsockopt,     SYS_GETSOCKOPT,    "getsockopt"},
	{LX_clone,          SYS_CLONE,         "clone"},
	{LX_fork,           SYS_FORK,          "fork"},
	/* vfork: implemented with plain fork semantics (POSIX permits this — the
	 * child simply doesn't share the parent's address space). */
	{LX_vfork,          SYS_FORK,          "vfork"},
	{LX_execve,         SYS_EXECVE,        "execve"},
	{LX_execveat,       SYS_EXECVEAT,      "execveat"},
	{LX_exit,           SYS_EXIT,          "exit"},
	{LX_wait4,          SYS_WAITPID,       "wait4"},
	{LX_kill,           SYS_KILL,          "kill"},
	{LX_uname,          SYS_UNAME,         "uname"},
	{LX_fcntl,          SYS_FCNTL,         "fcntl"},
	{LX_fsync,          SYS_FSYNC,         "fsync"},
	{LX_getcwd,         SYS_GETCWD,        "getcwd"},
	{LX_chdir,          SYS_CHDIR,         "chdir"},
	{LX_rename,         SYS_RENAME,        "rename"},
	{LX_mkdir,          SYS_MKDIR,         "mkdir"},
	{LX_mknod,          SYS_MKNOD,         "mknod"},
	{LX_flock,          SYS_FLOCK,         "flock"},
	{LX_sysinfo,        SYS_SYSINFO,       "sysinfo"},
	{LX_times,          SYS_TIMES,         "times"},
	{LX_sched_getaffinity, SYS_SCHED_GETAFFINITY, "sched_getaffinity"},
	{LX_clock_getres,   SYS_CLOCK_GETRES,  "clock_getres"},
	{LX_statx,          SYS_STATX,         "statx"},
	{LX_rmdir,          SYS_RMDIR,         "rmdir"},
	{LX_unlink,         SYS_UNLINK,        "unlink"},
	{LX_symlink,        SYS_SYMLINK,       "symlink"},
	{LX_readlink,       SYS_READLINK,      "readlink"},
	{LX_chmod,          SYS_CHMOD,         "chmod"},
	{LX_chown,          SYS_CHOWN,         "chown"},
	{LX_umask,          SYS_UMASK,         "umask"},
	{LX_getuid,         SYS_GETUID,        "getuid"},
	{LX_getgid,         SYS_GETGID,        "getgid"},
	{LX_setuid,         SYS_SETUID,        "setuid"},
	{LX_setgid,         SYS_SETGID,        "setgid"},
	{LX_geteuid,        SYS_GETEUID,       "geteuid"},
	{LX_getegid,        SYS_GETEGID,       "getegid"},
	{LX_getgroups,      SYS_GETGROUPS,     "getgroups"},
	{LX_setgroups,      SYS_SETGROUPS,     "setgroups"},
	{LX_sigaltstack,    SYS_SIGALTSTACK,   "sigaltstack"},
	{LX_setpgid,        SYS_SETPGRP,       "setpgid"},
	{LX_getppid,        SYS_GETPPID,       "getppid"},
	{LX_getpgrp,        SYS_GETPGRP,       "getpgrp"},
	{LX_setsid,         SYS_SETSID,        "setsid"},
	{LX_getpgid,        SYS_GETPGID,       "getpgid"},
	{LX_getsid,         SYS_GETSID,        "getsid"},
	{LX_alarm,          SYS_ALARM,         "alarm"},
	{LX_ftruncate,      SYS_FTRUNCATE,     "ftruncate"},
	{LX_fchdir,         SYS_FCHDIR,        "fchdir"},
	{LX_link,           SYS_LINK,          "link"},
	{LX_setpriority,    SYS_SETPRIORITY,   "setpriority"},
	{LX_sync,           SYS_SYNC,          "sync"},
	{LX_fchmod,         SYS_FCHMOD,        "fchmod"},
	{LX_fchown,         SYS_FCHOWN,        "fchown"},
	{LX_gettid,         SYS_GETTID,        "gettid"},
	{LX_getdents64,     SYS_GETDENTS,      "getdents64"},
	{LX_clock_gettime,  SYS_CLOCK_GETTIME, "clock_gettime"},
	/* exit_group: terminate the entire thread group (Linux 231). */
	{LX_exit_group,     SYS_EXIT_GROUP,    "exit_group"},
	{LX_getrandom,      SYS_GETRANDOM,     "getrandom"},
	{LX_prctl,          SYS_PRCTL,         "prctl"},
	{LX_seccomp,        SYS_SECCOMP,       "seccomp"},
	/* --- M92: musl-required syscalls --- */
	/* *at() syscalls are handled as special cases in the dispatcher (they need
	 * dirfd+path resolution), not in this table. Listed here for name lookup. */
	{LX_openat,         SYS_OPEN,          "openat"},
	{LX_newfstatat,     SYS_STAT,          "newfstatat"},
	{LX_unlinkat,       SYS_UNLINK,        "unlinkat"},
	{LX_mkdirat,        SYS_MKDIR,         "mkdirat"},
	{LX_linkat,         SYS_LINK,          "linkat"},
	{LX_symlinkat,      SYS_SYMLINK,       "symlinkat"},
	{LX_readlinkat,     SYS_READLINK,      "readlinkat"},
	{LX_fchmodat,       SYS_CHMOD,         "fchmodat"},
	{LX_fchownat,       SYS_CHOWN,         "fchownat"},
	{LX_faccessat,      SYS_ACCESS,        "faccessat"},
	{LX_renameat2,      SYS_RENAME,        "renameat2"},
	/* Wrapper syscalls: thin shims over existing b1nix handlers. */
	{LX_pipe2,          SYS_PIPE,          "pipe2"},
	{LX_dup3,           SYS_DUP2,          "dup3"},
	{LX_ppoll,          SYS_POLL,          "ppoll"},
	{LX_pselect6,       SYS_SELECT,        "pselect6"},
	{LX_accept4,        SYS_ACCEPT,        "accept4"},
	{LX_clock_nanosleep, SYS_SLEEP,        "clock_nanosleep"},
	{LX_reboot,         SYS_REBOOT,        "reboot"},
	{LX_setxattr,       SYS_SETXATTR,      "setxattr"},
	{LX_lsetxattr,      SYS_SETXATTR,      "lsetxattr"},
	{LX_getxattr,       SYS_GETXATTR,      "getxattr"},
	{LX_lgetxattr,      SYS_GETXATTR,      "lgetxattr"},
	{LX_listxattr,      SYS_LISTXATTR,     "listxattr"},
	{LX_llistxattr,     SYS_LISTXATTR,     "llistxattr"},
	{LX_removexattr,    SYS_REMOVEXATTR,   "removexattr"},
	{LX_lremovexattr,   SYS_REMOVEXATTR,   "lremovexattr"},
	/* prlimit64 is a dispatcher special case (get+set in one call). */
	{LX_set_robust_list, SYS_SYNC,         "set_robust_list"},
	{LX_get_robust_list, SYS_SYNC,         "get_robust_list"},
	{LX_io_setup,        SYS_IO_SETUP,     "io_setup"},
	{LX_io_submit,       SYS_IO_SUBMIT,    "io_submit"},
	{LX_io_getevents,    SYS_IO_GETEVENTS, "io_getevents"},
	{LX_selfhost_status, SYS_SELFHOST_STATUS, "selfhost_status"},
	/* musl set_tid_address: store clear_child_tid ptr, return tid */
	{LX_set_tid_address, SYS_SET_TID_ADDRESS, "set_tid_address"},
	{LX_getcpu,          SYS_GETCPU,       "getcpu"},
	/* musl event-loop syscalls */
	{LX_eventfd2,        SYS_EVENTFD,      "eventfd2"},
	{LX_eventfd,         SYS_EVENTFD,      "eventfd"},
	{LX_epoll_create1,   SYS_EPOLL_CREATE, "epoll_create1"},
	{LX_epoll_ctl,       SYS_EPOLL_CTL,    "epoll_ctl"},
	{LX_epoll_wait,      SYS_EPOLL_WAIT,   "epoll_wait"},
	{LX_epoll_pwait,     SYS_EPOLL_WAIT,   "epoll_pwait"},
	{LX_timer_settime,   SYS_TIMER_SETTIME,   "timer_settime"},
	{LX_timer_gettime,   SYS_TIMER_GETTIME,   "timer_gettime"},
	{LX_timer_delete,    SYS_TIMER_DELETE,    "timer_delete"},
	{LX_timerfd_create,  SYS_TIMERFD_CREATE,  "timerfd_create"},
	{LX_timerfd_settime, SYS_TIMERFD_SETTIME, "timerfd_settime"},
	{LX_timerfd_gettime, SYS_TIMERFD_GETTIME, "timerfd_gettime"},
	{LX_signalfd4,       SYS_SIGNALFD,     "signalfd4"},
	{LX_signalfd,        SYS_SIGNALFD,     "signalfd"},
	{LX_memfd_create,    SYS_MEMFD_CREATE, "memfd_create"},
	/* M73 modern-I/O: argument layouts already match Linux. */
	{LX_sendfile,        SYS_SENDFILE,        "sendfile"},
	{LX_splice,          SYS_SPLICE,          "splice"},
	{LX_fallocate,       SYS_FALLOCATE,       "fallocate"},
	{LX_copy_file_range, SYS_COPY_FILE_RANGE, "copy_file_range"},
	{LX_inotify_init,    SYS_INOTIFY_INIT1,   "inotify_init"},
	{LX_inotify_init1,   SYS_INOTIFY_INIT1,   "inotify_init1"},
	{LX_inotify_add_watch, SYS_INOTIFY_ADD_WATCH, "inotify_add_watch"},
	{LX_inotify_rm_watch,  SYS_INOTIFY_RM_WATCH,  "inotify_rm_watch"},
	/* Identical argument layouts and constant values on both ABIs. */
	{LX_socketpair,      SYS_SOCKETPAIR,    "socketpair"},
	{LX_shmat,           SYS_SHMAT,         "shmat"},
	{LX_shmctl,          SYS_SHMCTL,        "shmctl"},
	{LX_shmdt,           SYS_SHMDT,         "shmdt"},
	{LX_msync,           SYS_MSYNC,         "msync"},
	{LX_getrlimit,       SYS_GETRLIMIT,     "getrlimit"},
	{LX_setrlimit,       SYS_SETRLIMIT,     "setrlimit"},
	{LX_getrusage,       SYS_GETRUSAGE,     "getrusage"},
	{LX_setreuid,        SYS_SETREUID,      "setreuid"},
	{LX_setregid,        SYS_SETREGID,      "setregid"},
	{LX_setresuid,       SYS_SETRESUID,     "setresuid"},
	{LX_setresgid,       SYS_SETRESGID,     "setresgid"},
	/* mount takes (source, target, fstype, flags[, data-ignored]);
	 * umount2's flags argument is ignored by sys_umount. */
	{LX_mount,           SYS_MOUNT,         "mount"},
	{LX_umount2,         SYS_UMOUNT,        "umount2"},
};

#define LX_TABLE_LEN (sizeof(lx_table) / sizeof(lx_table[0]))

u32 linux_syscall_to_b1nix(u64 linux_nr) {
	for (usize i = 0; i < LX_TABLE_LEN; i++) {
		if (lx_table[i].linux_nr == linux_nr)
			return lx_table[i].b1nix_nr;
	}
	return LINUX_SYS_UNMAPPED;
}

const char *linux_syscall_name(u64 linux_nr) {
	for (usize i = 0; i < LX_TABLE_LEN; i++) {
		if (lx_table[i].linux_nr == linux_nr)
			return lx_table[i].name;
	}
	return "?";
}

void linux_stat_from_b1nix(struct linux_stat *out, const struct b1nix_stat *in) {
	for (usize i = 0; i < sizeof(*out); i++)
		((u8 *)out)[i] = 0;
	out->st_dev = in->st_dev;
	out->st_ino = in->st_ino;
	out->st_nlink = in->st_nlink;
	out->st_mode = in->st_mode;
	out->st_uid = in->st_uid;
	out->st_gid = in->st_gid;
	out->st_rdev = in->st_rdev;
	out->st_size = (i64)in->st_size;
	out->st_blksize = (i64)in->st_blksize;
	out->st_blocks = (i64)in->st_blocks;
	out->st_atime = in->st_atim.tv_sec;
	out->st_atime_nsec = in->st_atim.tv_nsec;
	out->st_mtime = in->st_mtim.tv_sec;
	out->st_mtime_nsec = in->st_mtim.tv_nsec;
	out->st_ctime = in->st_ctim.tv_sec;
	out->st_ctime_nsec = in->st_ctim.tv_nsec;
}

/* Copy a NUL-terminated string into a zeroed 65-byte Linux utsname field,
 * truncating at 64 chars (b1nix fields are 32 bytes, so truncation never bites). */
static void lx_field_copy(char dst[65], const char *src) {
	usize i = 0;
	for (; i < 64 && src[i]; i++)
		dst[i] = src[i];
	for (; i < 65; i++)
		dst[i] = '\0';
}

/* Linux x86_64 signal number -> b1nix signal number (b1nix uses a different
 * numbering, e.g. Linux SIGUSR1=10 but b1nix=19). Indexed by Linux signo; 0
 * means "no b1nix equivalent" (e.g. Linux SIGSTKFLT=16). Built by name. */
static const u8 lx_signo_to_b1nix_tbl[32] = {
	[1] = 7,   /* SIGHUP   */ [2] = 9,   /* SIGINT  */
	[3] = 12,  /* SIGQUIT  */ [4] = 8,   /* SIGILL  */
	[5] = 22,  /* SIGTRAP  */ [6] = 1,   /* SIGABRT */
	[7] = 3,   /* SIGBUS   */ [8] = 6,   /* SIGFPE  */
	[9] = 10,  /* SIGKILL  */ [10] = 19, /* SIGUSR1 */
	[11] = 13, /* SIGSEGV  */ [12] = 20, /* SIGUSR2 */
	[13] = 11, /* SIGPIPE  */ [14] = 2,  /* SIGALRM */
	[15] = 15, /* SIGTERM  */ [16] = 0,  /* SIGSTKFLT (none) */
	[17] = 4,  /* SIGCHLD  */ [18] = 5,  /* SIGCONT */
	[19] = 14, /* SIGSTOP  */ [20] = 16, /* SIGTSTP */
	[21] = 17, /* SIGTTIN  */ [22] = 18, /* SIGTTOU */
	[23] = 23, /* SIGURG   */ [24] = 24, /* SIGXCPU */
	[25] = 25, /* SIGXFSZ  */ [26] = 26, /* SIGVTALRM */
	[27] = 27, /* SIGPROF  */ [28] = 28, /* SIGWINCH */
	[29] = 29, /* SIGIO    */ [30] = 30, /* SIGPWR  */
	[31] = 21, /* SIGSYS   */
};

int linux_signo_to_b1nix(int lx) {
	if (lx >= 1 && lx <= 31)
		return lx_signo_to_b1nix_tbl[lx];
	/* Linux RT signals: __SIGRTMIN=34 .. 64  →  b1nix 32 .. 62.
	 * The offset is -2 (Linux reserves 32,33 for SIGCANCEL/SIGSETXID
	 * internally but those are Linux RT signals, not standard signals). */
	if (lx >= 34 && lx <= 64)
		return lx - 2;
	return 0;
}

int b1nix_signo_to_linux(int b) {
	for (int lx = 1; lx <= 31; lx++)
		if (lx_signo_to_b1nix_tbl[lx] == b)
			return lx;
	/* Reverse-map RT signals: b1nix 32..62  →  Linux 34..64 */
	if (b >= 32 && b <= 62)
		return b + 2;
	return 0;
}

/* Remap a sigset_t: bit (signo-1) is set for signo, and the signo numbering
 * differs between Linux and b1nix, so the bit positions must be translated. */
u64 linux_sigset_to_b1nix(u64 lx) {
	u64 b = 0;
	/* Standard signals 1..31 */
	for (int l = 1; l <= 31; l++) {
		if (lx & (1ULL << (l - 1))) {
			int bs = linux_signo_to_b1nix(l);
			if (bs)
				b |= (1ULL << (bs - 1));
		}
	}
	/* RT signals: Linux 34..63  →  b1nix 32..61 */
	for (int l = 34; l <= 63; l++) {
		if (lx & (1ULL << (l - 1))) {
			int bs = l - 2;
			b |= (1ULL << (bs - 1));
		}
	}
	return b;
}

u64 b1nix_sigset_to_linux(u64 b) {
	u64 lx = 0;
	/* Standard signals 1..31 */
	for (int bs = 1; bs <= 31; bs++) {
		if (b & (1ULL << (bs - 1))) {
			int l = b1nix_signo_to_linux(bs);
			if (l)
				lx |= (1ULL << (l - 1));
		}
	}
	/* RT signals: b1nix 32..62  →  Linux 34..64 */
	for (int bs = 32; bs <= 62; bs++) {
		if (b & (1ULL << (bs - 1))) {
			int l = bs + 2;
			lx |= (1ULL << (l - 1));
		}
	}
	return lx;
}

void linux_utsname_from_b1nix(struct linux_utsname *out,
                              const struct b1nix_utsname *in) {
	lx_field_copy(out->sysname, in->sysname);
	lx_field_copy(out->nodename, in->nodename);
	lx_field_copy(out->release, in->release);
	lx_field_copy(out->version, in->version);
	lx_field_copy(out->machine, in->machine);
	lx_field_copy(out->domainname, "(none)");
}
