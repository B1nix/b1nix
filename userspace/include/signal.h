#ifndef B1NIX_U_SIGNAL_H
#define B1NIX_U_SIGNAL_H

/* Expose ucontext_t/greg_t/REG_* here: SA_SIGINFO handlers reinterpret their
 * third (void*) argument as ucontext_t, and glibc makes these visible via
 * <signal.h> too (e.g. base/debug/stack_trace_posix.cc relies on it). */
#include <sys/ucontext.h>

#ifdef __cplusplus
extern "C" {
#endif
/* Signal numbers: Linux x86_64 standard layout.
 * These values match the Linux ABI so that binaries that hard-code signal
 * numbers (bash "kill -9", procps, etc.) work without translation.
 * MUST stay in sync with kernel/include/b1nix/sched.h. */
#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGTRAP   5
#define SIGABRT   6
#define SIGBUS    7
#define SIGFPE    8
#define SIGKILL   9
#define SIGUSR1   10
#define SIGSEGV   11
#define SIGUSR2   12
#define SIGPIPE   13
#define SIGALRM   14
#define SIGTERM   15
#define SIGSTKFLT 16  /* Linux 387 FPU stack fault — no b1nix handler, SIG_DFL=ignore */
#define SIGCHLD   17
#define SIGCONT   18
#define SIGSTOP   19
#define SIGTSTP   20
#define SIGTTIN   21
#define SIGTTOU   22
#define SIGURG    23
#define SIGXCPU   24
#define SIGXFSZ   25
#define SIGVTALRM 26
#define SIGPROF   27
#define SIGWINCH  28
#define SIGIO     29
#define SIGPOLL   SIGIO
#define SIGPWR    30
#define SIGSYS    31
#ifndef SI_KERNEL
#define SI_KERNEL 0x80  /* siginfo si_code: sent by the kernel */
#endif
/* siginfo si_code origin values (POSIX/glibc). */
#define SI_USER     0
#define SI_QUEUE   -1
#define SI_TIMER   -2
#define SI_MESGQ   -3
#define SI_ASYNCIO -4
#define SI_SIGIO   -5
#define SI_TKILL   -6
#define NSIG    31

/* M74 POSIX real-time signals: SIGRTMIN..SIGRTMAX (queued, payload-carrying).
 * Must match the kernel (kernel/include/b1nix/sched.h). */
#define SIGRTMIN 32
#define SIGRTMAX 63

/* siginfo si_code values (POSIX). Used by crash reporters to describe faults. */
#define FPE_INTDIV 1
#define FPE_FLTDIV 2
#define FPE_INTOVF 3
#define FPE_FLTOVF 4
#define FPE_FLTUND 5
#define FPE_FLTRES 6
#define FPE_FLTINV 7
#define FPE_FLTSUB 8
#define SEGV_MAPERR 1
#define SEGV_ACCERR 2
#define BUS_ADRALN 1
#define BUS_ADRERR 2
#define BUS_OBJERR 3
#define ILL_ILLOPC 1
#define ILL_ILLOPN 2
#define ILL_ILLADR 3
#define ILL_ILLTRP 4
#define ILL_PRVOPC 5
#define ILL_PRVREG 6
#define ILL_COPROC 7
#define ILL_BADSTK 8
#define TRAP_BRKPT 1
#define TRAP_TRACE 2
#define CLD_EXITED 1
#define CLD_KILLED 2
#define CLD_DUMPED 3
#define CLD_TRAPPED 4
#define CLD_STOPPED 5
#define CLD_CONTINUED 6
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

/* M74: POSIX real-time signal payload, carried by sigqueue(3) and a
 * SIGEV_SIGNAL timer's sigev_value, delivered to an SA_SIGINFO handler as
 * siginfo->si_value. */
union sigval {
    int sival_int;
    void *sival_ptr;
};

typedef struct {
    int si_signo;
    int si_code;
    int si_errno;
    int si_pid;
    int si_uid;
    int si_status;
    void *si_addr;   /* faulting address for SIGSEGV/SIGBUS/SIGFPE/SIGILL */
    /* si_value is appended last so existing field offsets are unchanged. RT
     * signals (sigqueue / POSIX timers) deliver their payload here. */
    union sigval si_value;
} siginfo_t;

/* sigqueue(3): send signal `sig` to `pid` with the RT payload `value`. */
int sigqueue(int pid, int sig, const union sigval value);

/* sigevent notification methods (POSIX). b1nix implements SIGEV_SIGNAL. */
#define SIGEV_SIGNAL 0
#define SIGEV_NONE   1
#define SIGEV_THREAD 2

/* struct sigevent — how a POSIX timer notifies on expiry. Native b1nix layout
 * (kept minimal: notify method, target signal, and the payload). */
struct sigevent {
    int sigev_notify;          /* SIGEV_SIGNAL / SIGEV_NONE */
    int sigev_signo;           /* signal to raise */
    union sigval sigev_value;  /* delivered as siginfo->si_value */
};

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

/* Alternate signal stack (sigaltstack). ss_flags bits and the minimum stack
 * size must match the kernel (kernel/include/b1nix/sched.h). */
#define SS_ONSTACK 1
#define SS_DISABLE 2
#define MINSIGSTKSZ 2048
#define SIGSTKSZ    8192

typedef struct {
    void  *ss_sp;
    int    ss_flags;
    unsigned long ss_size;
} stack_t;

sighandler_t signal(int signum, sighandler_t handler);
int sigemptyset(sigset_t *set);
int sigaddset(sigset_t *set, int signum);
int sigfillset(sigset_t *set);
int sigdelset(sigset_t *set, int signum);
int sigismember(const sigset_t *set, int signum);
int sigprocmask(int how, const sigset_t *set, sigset_t *oldset);
int pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset);
int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact);
int sigsuspend(const sigset_t *mask);
int sigaltstack(const stack_t *ss, stack_t *old_ss);
int sigwait(const sigset_t *set, int *sig);
#ifdef __cplusplus
}
#endif

#endif
