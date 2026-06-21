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
#define LX_access          21
#define LX_pipe            22
#define LX_select          23
#define LX_sched_yield     24
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
#define LX_execve          59
#define LX_exit            60
#define LX_wait4           61
#define LX_kill            62
#define LX_uname           63
#define LX_fcntl           72
#define LX_fsync           74
#define LX_getcwd          79
#define LX_chdir           80
#define LX_rename          82
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
#define LX_ftruncate       77
#define LX_link            86
#define LX_fchmod          91
#define LX_fchown          93
#define LX_gettid          186
#define LX_getdents64      217
#define LX_clock_gettime   228
#define LX_exit_group      231
#define LX_getrandom       318

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
	{LX_access,         SYS_ACCESS,        "access"},
	{LX_pipe,           SYS_PIPE,          "pipe"},
	{LX_select,         SYS_SELECT,        "select"},
	{LX_sched_yield,    SYS_YIELD,         "sched_yield"},
	{LX_dup,            SYS_DUP,           "dup"},
	{LX_dup2,           SYS_DUP2,          "dup2"},
	{LX_nanosleep,      SYS_SLEEP,         "nanosleep"},
	{LX_getpid,         SYS_GETPID,        "getpid"},
	{LX_socket,         SYS_SOCKET,        "socket"},
	{LX_connect,        SYS_CONNECT,       "connect"},
	{LX_accept,         SYS_ACCEPT,        "accept"},
	{LX_sendto,         SYS_SEND,          "sendto"},
	{LX_recvfrom,       SYS_RECV,          "recvfrom"},
	{LX_shutdown,       SYS_SHUTDOWN,      "shutdown"},
	{LX_bind,           SYS_BIND,          "bind"},
	{LX_listen,         SYS_LISTEN,        "listen"},
	{LX_getsockname,    SYS_GETSOCKNAME,   "getsockname"},
	{LX_getpeername,    SYS_GETPEERNAME,   "getpeername"},
	{LX_setsockopt,     SYS_SETSOCKOPT,    "setsockopt"},
	{LX_getsockopt,     SYS_GETSOCKOPT,    "getsockopt"},
	{LX_clone,          SYS_CLONE,         "clone"},
	{LX_fork,           SYS_FORK,          "fork"},
	{LX_execve,         SYS_EXECVE,        "execve"},
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
	{LX_setpgid,        SYS_SETPGRP,       "setpgid"},
	{LX_getppid,        SYS_GETPPID,       "getppid"},
	{LX_getpgrp,        SYS_GETPGRP,       "getpgrp"},
	{LX_setsid,         SYS_SETSID,        "setsid"},
	{LX_getpgid,        SYS_GETPGID,       "getpgid"},
	{LX_getsid,         SYS_GETSID,        "getsid"},
	{LX_ftruncate,      SYS_FTRUNCATE,     "ftruncate"},
	{LX_link,           SYS_LINK,          "link"},
	{LX_fchmod,         SYS_FCHMOD,        "fchmod"},
	{LX_fchown,         SYS_FCHOWN,        "fchown"},
	{LX_gettid,         SYS_GETTID,        "gettid"},
	{LX_getdents64,     SYS_GETDENTS,      "getdents64"},
	{LX_clock_gettime,  SYS_CLOCK_GETTIME, "clock_gettime"},
	/* exit_group has no distinct native number; map it to SYS_EXIT, whose
	 * handler already calls scheduler_exit_group (terminates the whole thread
	 * group), matching Linux exit_group semantics. */
	{LX_exit_group,     SYS_EXIT,          "exit_group"},
	{LX_getrandom,      SYS_GETRANDOM,     "getrandom"},
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
	if (lx < 1 || lx > 31)
		return 0;
	return lx_signo_to_b1nix_tbl[lx];
}

int b1nix_signo_to_linux(int b) {
	for (int lx = 1; lx <= 31; lx++)
		if (lx_signo_to_b1nix_tbl[lx] == b)
			return lx;
	return 0;
}

/* Remap a sigset_t: bit (signo-1) is set for signo, and the signo numbering
 * differs between Linux and b1nix, so the bit positions must be translated. */
u64 linux_sigset_to_b1nix(u64 lx) {
	u64 b = 0;
	for (int l = 1; l <= 31; l++) {
		if (lx & (1ULL << (l - 1))) {
			int bs = linux_signo_to_b1nix(l);
			if (bs)
				b |= (1ULL << (bs - 1));
		}
	}
	return b;
}

u64 b1nix_sigset_to_linux(u64 b) {
	u64 lx = 0;
	for (int bs = 1; bs <= 30; bs++) {
		if (b & (1ULL << (bs - 1))) {
			int l = b1nix_signo_to_linux(bs);
			if (l)
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
