#ifndef B1NIX_U_SIGNAL_H
#define B1NIX_U_SIGNAL_H

#ifdef __cplusplus
extern "C" {
#endif
/* Signal numbers MUST match the kernel's table (kernel/include/b1nix/sched.h):
 * kernel-generated signals (segfault, the tty/job-control signals) carry these
 * numbers, so userspace handlers/masks must agree or they silently miss them.
 * (The historic POSIX-Linux numbering here did not match the kernel.) */
#define SIGABRT 1
#define SIGALRM 2
#define SIGBUS  3
#define SIGCHLD 4
#define SIGCONT 5
#define SIGFPE  6
#define SIGHUP  7
#define SIGILL  8
#define SIGINT  9
#define SIGKILL 10
#define SIGPIPE 11
#define SIGQUIT 12
#define SIGSEGV 13
#define SIGSTOP 14
#define SIGTERM 15
#define SIGTSTP 16
#define SIGTTIN 17
#define SIGTTOU 18
#define SIGUSR1 19
#define SIGUSR2 20
#define SIGSYS  21
#define SIGTRAP 22
#define SIGXCPU 24
#define SIGXFSZ 25
#define SIGVTALRM 26
#define SIGWINCH 28
#define NSIG    31

#define FPE_INTDIV 1
#define FPE_FLTDIV 2
#define SIG_UNBLOCK 2
#define SIG_BLOCK   0
#define SIG_SETMASK 1
#define SA_NOCLDSTOP 0x00000001
#define SA_NOCLDWAIT 0x00000002
#define SA_SIGINFO 4
#define SA_ONSTACK  0x08000000
#define SA_RESTART  0x10000000
#define SA_RESTORER 0x04000000
#define SA_NODEFER  0x40000000
#define SA_RESETHAND 0x80000000

typedef void (*sighandler_t)(int);
typedef int sig_atomic_t;

int kill(int pid, int sig);
int killpg(int pgrp, int sig);
char *strsignal(int sig);

int raise(int sig);

#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)
#define SIG_ERR ((void (*)(int))-1)

typedef struct {
    int si_signo;
    int si_code;
} siginfo_t;

/* 64-bit to match the kernel ABI (struct sigaction uses u64 sa_flags/sa_mask,
 * sigset_t is a u64 bitmask). `unsigned long` is 8 bytes on x86_64 but only 4 on
 * the 32-bit port, which shifted sa_restorer/sa_mask to the wrong offsets — the
 * kernel then read a garbage sa_restorer and killed any process that installed a
 * real signal handler (M15). Use a fixed 64-bit type so the layout matches on
 * both architectures. */
typedef unsigned long long sigset_t;

struct sigaction {
    union {
        sighandler_t sa_handler;
        void (*sa_sigaction)(int, siginfo_t *, void *);
    };
    unsigned long long sa_flags;
    void (*sa_restorer)(void);
    sigset_t sa_mask;
};

sighandler_t signal(int signum, sighandler_t handler);
int sigemptyset(sigset_t *set);
int sigaddset(sigset_t *set, int signum);
int sigfillset(sigset_t *set);
int sigdelset(sigset_t *set, int signum);
int sigismember(const sigset_t *set, int signum);
int sigprocmask(int how, const sigset_t *set, sigset_t *oldset);
int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact);
int sigsuspend(const sigset_t *mask);
#ifdef __cplusplus
}
#endif

#endif
