/* M56 smoke test — event-loop and IPC primitives.
 *
 * Exercises each primitive for real and emits "M56-SMOKE: ok <name>" only on
 * verified success:
 *   eventfd  — write adds, read drains the counter; semaphore mode decrements
 *   epoll    — epoll_wait wakes on a ready eventfd, and times out when idle
 *   timerfd  — fires after the armed interval, read returns the expiration count
 *   signalfd — a raised (blocked) signal is delivered as a readable record
 *   seal     — F_SEAL_WRITE on a sealable memfd rejects a subsequent write
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>

#define SIG_TEST SIGUSR1 /* 19 on b1nix */

static void marker(const char *s) { write(1, s, strlen(s)); }

static int test_eventfd(void) {
    int efd = eventfd(0, 0);
    if (efd < 0)
        return 0;

    /* write adds to the counter; a single read drains it all (non-semaphore). */
    eventfd_t v = 7;
    if (eventfd_write(efd, v) != 0) { close(efd); return 0; }
    if (eventfd_write(efd, 5) != 0) { close(efd); return 0; }
    eventfd_t got = 0;
    if (eventfd_read(efd, &got) != 0 || got != 12) { close(efd); return 0; }
    close(efd);

    /* EFD_SEMAPHORE: each read returns 1 and decrements by 1. */
    int sfd = eventfd(2, EFD_SEMAPHORE | EFD_NONBLOCK);
    if (sfd < 0)
        return 0;
    got = 0;
    if (eventfd_read(sfd, &got) != 0 || got != 1) { close(sfd); return 0; }
    if (eventfd_read(sfd, &got) != 0 || got != 1) { close(sfd); return 0; }
    /* Counter now 0: a non-blocking read must fail with EAGAIN. */
    if (eventfd_read(sfd, &got) == 0) { close(sfd); return 0; }
    close(sfd);
    return 1;
}

static int test_epoll(void) {
    int ep = epoll_create1(0);
    if (ep < 0)
        return 0;
    int efd = eventfd(0, EFD_NONBLOCK);
    if (efd < 0) { close(ep); return 0; }

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = efd;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, efd, &ev) != 0) goto fail;

    /* Nothing ready yet: a 50 ms wait must time out (return 0). */
    struct epoll_event out[4];
    int n = epoll_wait(ep, out, 4, 50);
    if (n != 0) goto fail;

    /* Make the eventfd readable, then epoll_wait must report exactly it. */
    if (eventfd_write(efd, 1) != 0) goto fail;
    n = epoll_wait(ep, out, 4, 1000);
    if (n != 1 || out[0].data.fd != efd || !(out[0].events & EPOLLIN))
        goto fail;

    /* Drain it; with no readiness left, wait times out again. */
    eventfd_t drain;
    eventfd_read(efd, &drain);
    n = epoll_wait(ep, out, 4, 50);
    if (n != 0) goto fail;

    close(efd);
    close(ep);
    return 1;
fail:
    close(efd);
    close(ep);
    return 0;
}

static int test_timerfd(void) {
    int tfd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (tfd < 0)
        return 0;

    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    its.it_value.tv_sec = 0;
    its.it_value.tv_nsec = 120 * 1000 * 1000; /* 120 ms one-shot */
    if (timerfd_settime(tfd, 0, &its, NULL) != 0) { close(tfd); return 0; }

    /* Blocking read returns once the timer fires, with expiration count >= 1. */
    uint64_t expir = 0;
    ssize_t r = read(tfd, &expir, sizeof(expir));
    if (r != (ssize_t)sizeof(expir) || expir < 1) { close(tfd); return 0; }

    /* Also confirm timerfd is pollable via epoll. */
    int ep = epoll_create1(0);
    if (ep < 0) { close(tfd); return 0; }
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = tfd;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, tfd, &ev) != 0) { close(ep); close(tfd); return 0; }
    its.it_value.tv_nsec = 80 * 1000 * 1000; /* 80 ms */
    if (timerfd_settime(tfd, 0, &its, NULL) != 0) { close(ep); close(tfd); return 0; }
    struct epoll_event out[2];
    int n = epoll_wait(ep, out, 2, 1000);
    int ok = (n == 1 && out[0].data.fd == tfd);
    if (ok) {
        read(tfd, &expir, sizeof(expir));
    }
    close(ep);
    close(tfd);
    return ok;
}

static int test_signalfd(void) {
    /* Block the test signal so it stays pending (handler delivery is what
     * signalfd replaces) and create the signalfd watching it. */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIG_TEST);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) != 0)
        return 0;

    int sfd = signalfd(-1, &mask, 0);
    if (sfd < 0)
        return 0;

    /* Raise the signal at ourselves; it must surface as a readable record. */
    if (kill(getpid(), SIG_TEST) != 0) { close(sfd); return 0; }

    struct signalfd_siginfo si;
    memset(&si, 0, sizeof(si));
    ssize_t r = read(sfd, &si, sizeof(si));
    int ok = (r == (ssize_t)sizeof(si) && si.ssi_signo == (uint32_t)SIG_TEST);
    close(sfd);
    sigprocmask(SIG_UNBLOCK, &mask, NULL);
    return ok;
}

static int test_seal(void) {
    int fd = memfd_create("m56seal", MFD_ALLOW_SEALING);
    if (fd < 0)
        return 0;

    /* Grow it and write some bytes first (sealing is added afterward). */
    if (ftruncate(fd, 64) != 0) { close(fd); return 0; }
    char buf[8] = "hello";
    if (write(fd, buf, 5) != 5) { close(fd); return 0; }

    /* Seal against writes; a subsequent write must fail with EPERM. */
    if (fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE) != 0) { close(fd); return 0; }
    if (fcntl(fd, F_GET_SEALS, 0) != F_SEAL_WRITE) { close(fd); return 0; }
    lseek(fd, 0, SEEK_SET);
    ssize_t w = write(fd, buf, 5);
    int ok = (w < 0 && errno == EPERM);

    /* Sealing on a non-sealable memfd is rejected. */
    int fd2 = memfd_create("m56nosel", 0);
    if (fd2 >= 0) {
        if (fcntl(fd2, F_ADD_SEALS, F_SEAL_WRITE) == 0)
            ok = 0; /* should have been refused */
        close(fd2);
    }
    close(fd);
    return ok;
}

/* A repeating timerfd must wake epoll_wait on schedule. This is the shape of
 * every real event loop (libwayland's among them): if timers do not drive
 * epoll, everything socket-driven keeps working while everything time-driven —
 * a compositor's frame clock, a retry backoff, a watchdog — silently never
 * runs, which is the kind of failure that looks like a hang somewhere else. */
static int test_timerfd_epoll_cadence(void) {
    int tfd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (tfd < 0)
        return 0;
    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    its.it_value.tv_nsec = 50 * 1000 * 1000;
    its.it_interval.tv_nsec = 50 * 1000 * 1000;
    if (timerfd_settime(tfd, 0, &its, NULL) != 0) { close(tfd); return 0; }

    int ep = epoll_create1(0);
    if (ep < 0) { close(tfd); return 0; }
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = tfd };
    if (epoll_ctl(ep, EPOLL_CTL_ADD, tfd, &ev) != 0) { close(ep); close(tfd); return 0; }

    int wakeups = 0;
    unsigned long long expirations = 0;
    for (int i = 0; i < 10; i++) {
        struct epoll_event out[2];
        int n = epoll_wait(ep, out, 2, 2000); /* generous: 40x the period */
        if (n <= 0)
            break;
        unsigned long long exp = 0;
        if (read(tfd, &exp, sizeof(exp)) == (ssize_t)sizeof(exp))
            expirations += exp;
        wakeups++;
    }
    close(ep);
    close(tfd);
    return (wakeups == 10 && expirations >= 10);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    marker("M56-SMOKE: start\n");

    if (test_eventfd())  marker("M56-SMOKE: ok eventfd\n");
    else                 marker("M56-SMOKE: FAIL eventfd\n");

    if (test_timerfd_epoll_cadence()) marker("M56-SMOKE: ok timerfd-epoll\n");
    else                              marker("M56-SMOKE: FAIL timerfd-epoll\n");

    if (test_epoll())    marker("M56-SMOKE: ok epoll\n");
    else                 marker("M56-SMOKE: FAIL epoll\n");

    if (test_timerfd())  marker("M56-SMOKE: ok timerfd\n");
    else                 marker("M56-SMOKE: FAIL timerfd\n");

    if (test_signalfd()) marker("M56-SMOKE: ok signalfd\n");
    else                 marker("M56-SMOKE: FAIL signalfd\n");

    if (test_seal())     marker("M56-SMOKE: ok seal\n");
    else                 marker("M56-SMOKE: FAIL seal\n");

    marker("M56-SMOKE: done\n");
    return 0;
}
