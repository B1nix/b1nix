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
#define SA_SIGINFO 4

typedef void (*sighandler_t)(int);

typedef struct {
    int si_signo;
    int si_code;
} siginfo_t;

typedef unsigned long sigset_t;

struct sigaction {
    void (*sa_sigaction)(int, siginfo_t *, void *);
    sigset_t sa_mask;
    int sa_flags;
};

sighandler_t signal(int signum, sighandler_t handler);
void sigemptyset(sigset_t *set);
void sigaddset(sigset_t *set, int signum);
void sigprocmask(int how, const sigset_t *set, sigset_t *oldset);
void sigaction(int signum, const struct sigaction *act, struct sigaction *oldact);

#endif
