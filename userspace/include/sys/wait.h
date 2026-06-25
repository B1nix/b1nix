#ifndef B1NIX_SYS_WAIT_H
#define B1NIX_SYS_WAIT_H

#include <sys/types.h>
#include <signal.h>

struct rusage;

typedef enum {
  P_ALL = 0,
  P_PID = 1,
  P_PGID = 2
} idtype_t;

#ifdef __cplusplus
extern "C" {
#endif

pid_t wait(int *wstatus);
pid_t waitpid(pid_t pid, int *wstatus, int options);
int waitid(idtype_t idtype, id_t id, siginfo_t *infop, int options);
pid_t wait4(pid_t pid, int *wstatus, int options, struct rusage *rusage);
pid_t wait3(int *wstatus, int options, struct rusage *rusage);

/* wait4 / wait3 (added for the Chromium port, M60-62). b1nix has no per-child
 * resource accounting, so the rusage out-param (if non-NULL) is zero-filled. */
struct rusage;
pid_t wait4(pid_t pid, int *wstatus, int options, struct rusage *rusage);
pid_t wait3(int *wstatus, int options, struct rusage *rusage);

#ifdef __cplusplus
}
#endif

#define WNOHANG 1
#define WUNTRACED 2
#define WSTOPPED 2
#define WEXITED 4
#define WCONTINUED 8
#define WNOWAIT 0x01000000
/* Linux-specific waitpid flags (b1nix has no separate clone-thread wait domain,
 * so __WALL/__WCLONE are accepted but don't change behavior). */
#define __WNOTHREAD 0x20000000
#define __WALL      0x40000000
#define __WCLONE    0x80000000

#define WIFEXITED(status)   (((status) & 0x7f) == 0)
#define WEXITSTATUS(status) (((status) >> 8) & 0xff)
#define WIFSIGNALED(status) (((status) & 0x7f) != 0 && ((status) & 0x7f) != 0x7f)
#define WTERMSIG(status)    ((status) & 0x7f)
#define WCOREDUMP(status)   ((status) & 0x80)
#define WIFSTOPPED(status)  (((status) & 0xff) == 0x7f)
#define WSTOPSIG(status)    (((status) >> 8) & 0xff)
#define WIFCONTINUED(status) ((status) == 0xffff)

#endif
