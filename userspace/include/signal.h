#ifndef B1NIX_U_SIGNAL_H
#define B1NIX_U_SIGNAL_H

#define SIGINT  2
#define SIGILL  4
#define SIGABRT 6
#define SIGBUS  7
#define SIGFPE  8
#define SIGSEGV 11
#define SIGTERM 15

#define FPE_INTDIV 1
#define FPE_FLTDIV 2
#define SIG_UNBLOCK 2
#define SIG_BLOCK   0
#define SIG_SETMASK 1
#define SA_SIGINFO 4
#define SA_RESTORER 0x04000000
#define SA_NODEFER  0x40000000

typedef void (*sighandler_t)(int);

#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)
#define SIG_ERR ((void (*)(int))-1)

typedef struct {
    int si_signo;
    int si_code;
} siginfo_t;

typedef unsigned long sigset_t;

struct sigaction {
    union {
        sighandler_t sa_handler;
        void (*sa_sigaction)(int, siginfo_t *, void *);
    };
    unsigned long sa_flags;
    void (*sa_restorer)(void);
    sigset_t sa_mask;
};

sighandler_t signal(int signum, sighandler_t handler);
int sigemptyset(sigset_t *set);
int sigaddset(sigset_t *set, int signum);
int sigprocmask(int how, const sigset_t *set, sigset_t *oldset);
int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact);

#endif
