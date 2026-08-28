/*
 * M42 wave-5 prerequisites smoke test.
 *
 * The roadmap (M42: Upstream BusyBox Port, "Migration wave 5") gates enabling
 * the upstream `ash` shell on these POSIX features being available: atomic
 * sigsuspend, alarm, real resource limits, dup/isatty/access/ftruncate, and
 * complete fnmatch/regex behaviour. This binary exercises that gate, plus
 * signal-interruptible waitpid and SIGSTOP/SIGCONT job control. Markers are
 * emitted as `M42-W5PRE: ...` and checked by tests/smoke.sh.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <time.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fnmatch.h>
#include <regex.h>

static void emit(const char *s) {
    write(1, s, strlen(s));
}

static void ok(const char *name) {
    char buf[128];
    snprintf(buf, sizeof(buf), "M42-W5PRE: ok %s\n", name);
    emit(buf);
}

static void fail(const char *name) {
    char buf[128];
    snprintf(buf, sizeof(buf), "M42-W5PRE: FAIL %s\n", name);
    emit(buf);
}

static volatile int g_sigchld_count = 0;
static volatile int g_sigalrm_count = 0;
static volatile int g_sigusr1_count = 0;

static void sigchld_handler(int sig) {
    (void)sig;
    g_sigchld_count++;
}

static void sigalrm_handler(int sig) {
    (void)sig;
    g_sigalrm_count++;
}

static void sigusr1_handler(int sig) {
    (void)sig;
    g_sigusr1_count++;
}

int main(void) {
    emit("M42-W5PRE: start\n");

    /* 1. RLIMIT_NOFILE resource limits */
    struct rlimit rlim;
    if (getrlimit(RLIMIT_NOFILE, &rlim) != 0) {
        fail("getrlimit");
        return 1;
    }
    rlim_t original_cur = rlim.rlim_cur;
    rlim.rlim_cur = 10;
    if (setrlimit(RLIMIT_NOFILE, &rlim) != 0) {
        fail("setrlimit-lower");
        return 1;
    }

    int fds[15];
    int open_count = 0;
    for (int i = 0; i < 15; i++) {
        int fd = open("/tmp/rlimit_test.txt", O_CREAT | O_RDWR, 0666);
        if (fd >= 0) {
            fds[i] = fd;
            open_count++;
        } else {
            if (errno != EMFILE) {
                fail("rlimit-incorrect-errno");
                return 1;
            }
            break;
        }
    }
    for (int i = 0; i < open_count; i++) {
        close(fds[i]);
    }
    // With cur limit = 10, we should not be able to open 15 files because stdin, stdout, stderr are already 3, leaving 7 slots.
    if (open_count > 7) {
        fail("rlimit-enforcement");
        return 1;
    }
    ok("rlimit-enforcement");

    rlim.rlim_cur = original_cur;
    if (setrlimit(RLIMIT_NOFILE, &rlim) != 0) {
        fail("setrlimit-restore");
        return 1;
    }
    ok("getrlimit-setrlimit");

    /* 2. dup(), access(), ftruncate() */
    int dup_fd = dup(1);
    if (dup_fd < 0) {
        fail("dup");
        return 1;
    }
    close(dup_fd);
    ok("dup");

    if (access("/tmp", F_OK) != 0 || access("/tmp", R_OK | W_OK) != 0) {
        fail("access-existing");
        return 1;
    }
    if (access("/nonexistent_path_xyz", F_OK) == 0) {
        fail("access-nonexistent");
        return 1;
    }
    ok("access");

    int trunc_fd = open("/tmp/trunc_test.txt", O_CREAT | O_RDWR | O_TRUNC, 0666);
    if (trunc_fd < 0) {
        fail("ftruncate-open");
        return 1;
    }
    if (ftruncate(trunc_fd, 50) != 0) {
        fail("ftruncate-grow");
        return 1;
    }
    struct stat st;
    if (fstat(trunc_fd, &st) != 0 || st.st_size != 50) {
        fail("ftruncate-grow-size");
        return 1;
    }
    char trunc_buf[50];
    lseek(trunc_fd, 0, SEEK_SET);
    if (read(trunc_fd, trunc_buf, 50) != 50) {
        fail("ftruncate-read");
        return 1;
    }
    for (int i = 0; i < 50; i++) {
        if (trunc_buf[i] != 0) {
            fail("ftruncate-zeroed");
            return 1;
        }
    }
    if (ftruncate(trunc_fd, 20) != 0) {
        fail("ftruncate-shrink");
        return 1;
    }
    if (fstat(trunc_fd, &st) != 0 || st.st_size != 20) {
        fail("ftruncate-shrink-size");
        return 1;
    }
    close(trunc_fd);
    ok("ftruncate");

    /* 3. fchdir() */
    int dir_fd = open("/tmp", O_RDONLY);
    if (dir_fd < 0) {
        fail("fchdir-open");
        return 1;
    }
    if (fchdir(dir_fd) != 0) {
        fail("fchdir");
        return 1;
    }
    char cwd[256];
    if (!getcwd(cwd, sizeof(cwd)) || strcmp(cwd, "/tmp") != 0) {
        fail("fchdir-cwd");
        return 1;
    }
    close(dir_fd);
    ok("fchdir");

    /* 4. fnmatch() */
    if (fnmatch("a*b", "axxxb", 0) != 0 ||
        fnmatch("a?b", "axb", 0) != 0 ||
        fnmatch("a[0-9]b", "a5b", 0) != 0 ||
        fnmatch("a[!0-9]b", "axb", 0) != 0 ||
        fnmatch("a[!0-9]b", "a5b", 0) == 0) {
        fail("fnmatch-basic");
        return 1;
    }
    if (fnmatch("a/b", "a/b", FNM_PATHNAME) != 0 ||
        fnmatch("a*b", "a/b", FNM_PATHNAME) == 0) {
        fail("fnmatch-pathname");
        return 1;
    }
    if (fnmatch("*a", ".a", FNM_PERIOD) == 0 ||
        fnmatch(".*a", ".a", FNM_PERIOD) != 0) {
        fail("fnmatch-period");
        return 1;
    }
    ok("fnmatch");

    /* 5. Regex bounds & classes */
    regex_t preg;
    if (regcomp(&preg, "a{2,4}", REG_EXTENDED) != 0) {
        fail("regcomp-interval");
        return 1;
    }
    if (regexec(&preg, "a", 0, NULL, 0) == 0 ||
        regexec(&preg, "aa", 0, NULL, 0) != 0 ||
        regexec(&preg, "aaa", 0, NULL, 0) != 0 ||
        regexec(&preg, "aaaa", 0, NULL, 0) != 0 ||
        regexec(&preg, "aaaaa", 0, NULL, 0) != 0) { // matches "aaaa" as substring
        fail("regexec-interval");
        return 1;
    }
    regfree(&preg);

    if (regcomp(&preg, "[[:punct:]]+", REG_EXTENDED) != 0) {
        fail("regcomp-class");
        return 1;
    }
    if (regexec(&preg, "abc", 0, NULL, 0) == 0 ||
        regexec(&preg, "a!b", 0, NULL, 0) != 0) {
        fail("regexec-class");
        return 1;
    }
    regfree(&preg);
    ok("regex");

    /* 6. Signals and Job Control */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sigaction(SIGCHLD, &sa, NULL);

    sa.sa_handler = sigalrm_handler;
    sigaction(SIGALRM, &sa, NULL);

    sa.sa_handler = sigusr1_handler;
    sigaction(SIGUSR1, &sa, NULL);

    // Test interrupted wait
    sigset_t block_mask, orig_mask;
    sigemptyset(&block_mask);
    sigaddset(&block_mask, SIGUSR1);
    sigprocmask(SIG_BLOCK, &block_mask, &orig_mask);

    int child = fork();
    if (child == 0) {
        // Child loops yielding
        while (1) {
            usleep(10000);
        }
        _exit(0);
    }

    // Alarm / sigsuspend test
    g_sigalrm_count = 0;
    alarm(1);
    sigset_t susp_mask;
    sigfillset(&susp_mask);
    sigdelset(&susp_mask, SIGALRM);
    sigdelset(&susp_mask, SIGINT);
    // sigsuspend should return -1 with EINTR when alarm fires
    int ss_rc = sigsuspend(&susp_mask);
    if (ss_rc != -1 || errno != EINTR || g_sigalrm_count != 1) {
        char err_buf[256];
        snprintf(err_buf, sizeof(err_buf), "sigsuspend-alarm: rc=%d, errno=%d, count=%d\n", ss_rc, errno, g_sigalrm_count);
        emit(err_buf);
        fail("sigsuspend-alarm");
        return 1;
    }
    ok("sigsuspend-alarm");

    /* A signal the caller keeps blocked, unblocked only by the wait mask.
     *
     * This is the case that distinguishes a correct sigsuspend from one that
     * restores the caller's mask too early. The handler has to run while the
     * wait mask is installed; a kernel that puts the original mask back before
     * delivery re-blocks the very signal that ended the wait, so no handler
     * runs, the signal stays pending, and a program that loops on sigsuspend —
     * every shell waiting for a child does — never makes progress again. */
    g_sigusr1_count = 0;
    raise(SIGUSR1);                       /* blocked: pends, does not run */
    if (g_sigusr1_count != 0) {
        fail("sigsuspend-blocked-signal");
        return 1;
    }
    sigset_t wait_mask;
    sigfillset(&wait_mask);
    sigdelset(&wait_mask, SIGUSR1);
    ss_rc = sigsuspend(&wait_mask);
    if (ss_rc != -1 || errno != EINTR || g_sigusr1_count != 1) {
        char sb[128];
        snprintf(sb, sizeof(sb), "sigsuspend-blocked-signal: rc=%d errno=%d count=%d\n",
                 ss_rc, errno, g_sigusr1_count);
        emit(sb);
        fail("sigsuspend-blocked-signal");
        return 1;
    }
    /* And the caller's own mask is back afterwards: SIGUSR1 blocked again. */
    sigset_t after;
    sigemptyset(&after);
    sigprocmask(SIG_BLOCK, NULL, &after);
    if (!sigismember(&after, SIGUSR1)) {
        fail("sigsuspend-mask-restored");
        return 1;
    }
    ok("sigsuspend-blocked-signal");

    /* ppoll's mask argument is the same promise sigsuspend makes, for a wait
     * on descriptors instead of on nothing: a signal blocked everywhere else
     * must be deliverable for the duration of this call. A kernel that drops
     * the argument leaves such a program unkillable by that signal — it stays
     * pending while the process runs on. */
    g_sigalrm_count = 0;
    sigset_t alrm_mask;
    sigemptyset(&alrm_mask);
    sigaddset(&alrm_mask, SIGALRM);
    sigprocmask(SIG_BLOCK, &alrm_mask, NULL);
    alarm(1);
    sigset_t poll_mask;
    sigfillset(&poll_mask);
    sigdelset(&poll_mask, SIGALRM);
    struct timespec pto = { 5, 0 };
    int pp = ppoll(NULL, 0, &pto, &poll_mask);
    if (pp != -1 || errno != EINTR || g_sigalrm_count != 1) {
        char pb[128];
        snprintf(pb, sizeof(pb), "pwait-sigmask: rc=%d errno=%d count=%d\n",
                 pp, errno, g_sigalrm_count);
        emit(pb);
        fail("pwait-sigmask");
        return 1;
    }
    sigprocmask(SIG_UNBLOCK, &alrm_mask, NULL);
    ok("pwait-sigmask");

    /* And the timeout it was given, not the millisecond it rounds to.
     *
     * ppoll and pselect take a timespec because sub-millisecond waits are the
     * point of them. Rounded to whole milliseconds, 200 us became either zero —
     * an event loop that spins at full speed — or a whole tick. Measure both
     * ends: the call must actually wait, and it must not wait ten times too
     * long. */
    struct timespec t0, t1, tiny = { 0, 200000 };  /* 200 us */
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int tp = ppoll(NULL, 0, &tiny, NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long long el_us = (long long)(t1.tv_sec - t0.tv_sec) * 1000000
                    + (t1.tv_nsec - t0.tv_nsec) / 1000;
    if (tp != 0 || el_us < 150 || el_us > 20000) {
        char tb[128];
        snprintf(tb, sizeof(tb), "ppoll-precision: rc=%d elapsed=%lldus\n", tp, el_us);
        emit(tb);
        fail("ppoll-precision");
        return 1;
    }
    ok("ppoll-precision");

    // Test interrupted waitpid
    // We unblock SIGUSR1 in parent, spawn thread or just waitpid, but waitpid is interrupted by signal.
    // Actually, parent blocks no signals, waits on child. We send SIGUSR1 to parent from another context,
    // but here we can just test if waitpid gets interrupted.
    // Wait, b1nix waitpid is interruptible. Let's verify.
    // If we set up alarm(1) again, and then call waitpid(child, &status, 0):
    // waitpid should return -1 with EINTR.
    alarm(1);
    int status = 0;
    int w_rc = waitpid(child, &status, 0);
    if (w_rc != -1 || errno != EINTR) {
        fail("interrupted-waitpid");
        return 1;
    }
    ok("interrupted-waitpid");

    // Test Job Control (SIGSTOP / SIGCONT)
    g_sigchld_count = 0;
    if (kill(child, SIGSTOP) != 0) {
        fail("kill-stop");
        return 1;
    }
    // Yield to let child stop and parent receive SIGCHLD
    for (int i = 0; i < 10; i++) {
        usleep(10000);
    }
    int stop_status = 0;
    int stop_pid = waitpid(child, &stop_status, WUNTRACED | WNOHANG);
    if (stop_pid != child || !WIFSTOPPED(stop_status)) {
        fail("job-control-stop");
        return 1;
    }

    if (kill(child, SIGCONT) != 0) {
        fail("kill-cont");
        return 1;
    }
    for (int i = 0; i < 10; i++) {
        usleep(10000);
    }
    int cont_status = 0;
    int cont_pid = waitpid(child, &cont_status, WCONTINUED | WNOHANG);
    if (cont_pid != child || !WIFCONTINUED(cont_status)) {
        fail("job-control-cont");
        return 1;
    }

    if (g_sigchld_count < 2) {
        fail("sigchld-delivery");
        return 1;
    }
    ok("job-control");

    // Terminate child
    kill(child, SIGKILL);
    waitpid(child, &status, 0);

    // 7. Test SIGCHLD on child exit
    sigset_t current_mask;
    sigprocmask(0, NULL, &current_mask);
    char mask_buf[256];
    /* sigset_t is opaque, so report the bit this test actually cares about
     * rather than the raw word. */
    snprintf(mask_buf, sizeof(mask_buf), "sigchld-on-exit: sigchld-blocked=%d\n",
             sigismember(&current_mask, SIGCHLD));
    emit(mask_buf);

    g_sigchld_count = 0;
    int exit_child = fork();
    if (exit_child == 0) {
        _exit(42);
    }
    int exit_status = 0;
    int reaped_pid = waitpid(exit_child, &exit_status, 0);
    if (reaped_pid != exit_child || !WIFEXITED(exit_status) || WEXITSTATUS(exit_status) != 42) {
        fail("sigchld-on-exit-wait");
        return 1;
    }
    usleep(10000);
    if (g_sigchld_count != 1) {
        char err_buf[256];
        sigprocmask(0, NULL, &current_mask);
        snprintf(err_buf, sizeof(err_buf),
                 "sigchld-on-exit: count=%d post-sigchld-blocked=%d\n",
                 g_sigchld_count, sigismember(&current_mask, SIGCHLD));
        emit(err_buf);
        fail("sigchld-on-exit");
        return 1;
    }
    ok("sigchld-on-exit");

    /* A signal sent to a task that is ASLEEP has to reach it now, not when the
     * sleep it is in would have ended anyway.
     *
     * Every kill path woke a target that was blocked or stopped and left a
     * sleeping one alone, so the signal sat in pending_signals until nanosleep's
     * own deadline expired. From outside that is `timeout 3 sleep 60` returning
     * 124 on time and taking sixty seconds -- a bound that reports the right
     * answer while bounding nothing, which corrupts every measurement made
     * through it.
     *
     * The elapsed time is the assertion. Without it this check passes today,
     * thirty seconds late, and proves nothing. */
    {
        struct timespec kt0, kt1;
        clock_gettime(CLOCK_MONOTONIC, &kt0);
        pid_t sleeper = fork();
        if (sleeper == 0) {
            /* Explicitly the default disposition. An ignored signal stays
             * ignored across fork and exec, and whatever launched this binary
             * had already set SIGTERM to SIG_IGN -- so without this the child
             * would be right to sleep through the signal, and the check would
             * be measuring the launcher rather than the kernel. */
            signal(SIGTERM, SIG_DFL);
            struct timespec long_sleep = {30, 0};
            nanosleep(&long_sleep, NULL);
            _exit(0);
        }
        if (sleeper < 0) {
            fail("kill-sleeping-child");
            return 1;
        }
        /* Let the child actually reach nanosleep: signalling it while it is
         * still in fork/exec would test a different state entirely. */
        usleep(200000);
        errno = 0;
        int krc = kill(sleeper, SIGTERM);
        int kerr = errno;
        /* Whether the child is still there two seconds later, and whether a
         * SIGKILL ends it when a SIGTERM did not. A kill that reports success
         * and changes nothing is the failure being measured, so the follow-up
         * says which half is broken: the delivery or this particular signal. */
        int kst = 0;
        int alive_after = 0;
        for (int w = 0; w < 20; w++) {
            if (waitpid(sleeper, &kst, WNOHANG) == sleeper) {
                alive_after = 0;
                break;
            }
            alive_after = 1;
            usleep(100000);
        }
        int killed_by_kill = 0;
        if (alive_after) {
            kill(sleeper, SIGKILL);
            for (int w = 0; w < 20 && alive_after; w++) {
                if (waitpid(sleeper, &kst, WNOHANG) == sleeper) {
                    alive_after = 0;
                    killed_by_kill = 1;
                    break;
                }
                usleep(100000);
            }
        }
        if (alive_after)
            waitpid(sleeper, &kst, 0);
        clock_gettime(CLOCK_MONOTONIC, &kt1);
        long kms = (long)((kt1.tv_sec - kt0.tv_sec) * 1000 +
                          (kt1.tv_nsec - kt0.tv_nsec) / 1000000);
        if (kms < 3000 && WIFSIGNALED(kst) && WTERMSIG(kst) == SIGTERM) {
            ok("kill-sleeping-child");
        } else {
            char kb[160];
            snprintf(kb, sizeof(kb),
                     "kill-sleeping-child: %ldms kill=%d errno=%d signalled=%d sig=%d exit=%d sigkill-ended=%d\n",
                     kms, krc, kerr, WIFSIGNALED(kst),
                     WIFSIGNALED(kst) ? WTERMSIG(kst) : -1,
                     WIFEXITED(kst) ? WEXITSTATUS(kst) : -1, killed_by_kill);
            emit(kb);
            fail("kill-sleeping-child");
            return 1;
        }
    }

    emit("M42-W5PRE: done\n");
    return 0;
}
