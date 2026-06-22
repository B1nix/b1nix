#ifndef _SYS_INOTIFY_H
#define _SYS_INOTIFY_H

/* Minimal <sys/inotify.h> for the b1nix libc (added for the Chromium port,
 * M60-62). b1nix has NO inotify file-change notification mechanism, so the
 * functions are honest stubs that fail with ENOSYS. The struct/constants are
 * provided so callers (perfetto file watching, etc.) compile; callers that
 * check the inotify_init() return value fall back to their non-inotify path.
 * This is NOT a fake success — it reports the feature as unimplemented. */

#include <stdint.h>
#include <sys/types.h>
#include <errno.h>

#ifdef __cplusplus
extern "C" {
#endif

struct inotify_event {
    int      wd;
    uint32_t mask;
    uint32_t cookie;
    uint32_t len;
    char     name[];
};

/* Event masks (values match Linux for source compatibility). */
#define IN_ACCESS        0x00000001
#define IN_MODIFY        0x00000002
#define IN_ATTRIB        0x00000004
#define IN_CLOSE_WRITE   0x00000008
#define IN_CLOSE_NOWRITE 0x00000010
#define IN_CLOSE         (IN_CLOSE_WRITE | IN_CLOSE_NOWRITE)
#define IN_OPEN          0x00000020
#define IN_MOVED_FROM    0x00000040
#define IN_MOVED_TO      0x00000080
#define IN_MOVE          (IN_MOVED_FROM | IN_MOVED_TO)
#define IN_CREATE        0x00000100
#define IN_DELETE        0x00000200
#define IN_DELETE_SELF   0x00000400
#define IN_MOVE_SELF     0x00000800
#define IN_UNMOUNT       0x00002000
#define IN_Q_OVERFLOW    0x00004000
#define IN_IGNORED       0x00008000
#define IN_ONLYDIR       0x01000000
#define IN_DONT_FOLLOW   0x02000000
#define IN_EXCL_UNLINK   0x04000000
#define IN_MASK_ADD      0x20000000
#define IN_ISDIR         0x40000000
#define IN_ONESHOT       0x80000000
#define IN_ALL_EVENTS    0x00000fff

/* inotify_init1() flags. */
#define IN_CLOEXEC  02000000
#define IN_NONBLOCK 00004000

static inline int inotify_init(void) { errno = ENOSYS; return -1; }
static inline int inotify_init1(int flags) { (void)flags; errno = ENOSYS; return -1; }
static inline int inotify_add_watch(int fd, const char *pathname, uint32_t mask) {
    (void)fd; (void)pathname; (void)mask; errno = ENOSYS; return -1;
}
static inline int inotify_rm_watch(int fd, int wd) {
    (void)fd; (void)wd; errno = ENOSYS; return -1;
}

#ifdef __cplusplus
}
#endif

#endif /* _SYS_INOTIFY_H */
