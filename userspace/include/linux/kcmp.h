#ifndef _LINUX_KCMP_H
#define _LINUX_KCMP_H

/* Minimal <linux/kcmp.h> for b1nix. b1nix has no kcmp(2). Chromium's GPU seccomp
 * policy references KCMP_FILE in a syscall-allow rule — dead on b1nix
 * (--no-sandbox). Standard Linux enum so the (never-run) policy compiles. */

enum kcmp_type {
    KCMP_FILE = 0,
    KCMP_VM,
    KCMP_FILES,
    KCMP_FS,
    KCMP_SIGHAND,
    KCMP_IO,
    KCMP_SYSVSEM,
    KCMP_EPOLL_TFD,
    KCMP_TYPES,
};

#endif /* _LINUX_KCMP_H */
