#ifndef B1NIX_U_SPAWN_H
#define B1NIX_U_SPAWN_H

/* POSIX posix_spawn() — implemented in libc/posix_compat.c as fork()+exec()
 * (b1nix has no spawn syscall; this is the standard libc fallback). The opaque
 * action/attr objects are kept small so a foreign caller (Rust std's libc FFI)
 * that stack-allocates its own larger definition has room to spare; only the
 * libc functions here ever interpret their bytes. */

#include <sys/types.h>
#include <signal.h>
#include <sched.h>

#ifdef __cplusplus
extern "C" {
#endif

/* posix_spawnattr flags. */
#define POSIX_SPAWN_RESETIDS      0x01
#define POSIX_SPAWN_SETPGROUP     0x02
#define POSIX_SPAWN_SETSIGDEF     0x04
#define POSIX_SPAWN_SETSIGMASK    0x08
#define POSIX_SPAWN_SETSID        0x80

typedef struct {
  short    flags;
  pid_t    pgroup;
  sigset_t sigdefault;
  sigset_t sigmask;
} posix_spawnattr_t;

/* A single file action (open / close / dup2 / chdir), applied in order in the
 * child before exec. */
struct __spawn_action;
typedef struct {
  int                    count;
  int                    cap;
  struct __spawn_action *actions;
} posix_spawn_file_actions_t;

int posix_spawn(pid_t *pid, const char *path,
                const posix_spawn_file_actions_t *file_actions,
                const posix_spawnattr_t *attrp,
                char *const argv[], char *const envp[]);
int posix_spawnp(pid_t *pid, const char *file,
                 const posix_spawn_file_actions_t *file_actions,
                 const posix_spawnattr_t *attrp,
                 char *const argv[], char *const envp[]);

int posix_spawn_file_actions_init(posix_spawn_file_actions_t *fa);
int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *fa);
int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *fa, int fd,
                                     const char *path, int oflag, mode_t mode);
int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *fa, int fd);
int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *fa, int fd,
                                     int newfd);
int posix_spawn_file_actions_addchdir_np(posix_spawn_file_actions_t *fa,
                                         const char *path);

int posix_spawnattr_init(posix_spawnattr_t *attr);
int posix_spawnattr_destroy(posix_spawnattr_t *attr);
int posix_spawnattr_getflags(const posix_spawnattr_t *attr, short *flags);
int posix_spawnattr_setflags(posix_spawnattr_t *attr, short flags);
int posix_spawnattr_getpgroup(const posix_spawnattr_t *attr, pid_t *pgroup);
int posix_spawnattr_setpgroup(posix_spawnattr_t *attr, pid_t pgroup);
int posix_spawnattr_getsigdefault(const posix_spawnattr_t *attr, sigset_t *sd);
int posix_spawnattr_setsigdefault(posix_spawnattr_t *attr, const sigset_t *sd);
int posix_spawnattr_getsigmask(const posix_spawnattr_t *attr, sigset_t *sm);
int posix_spawnattr_setsigmask(posix_spawnattr_t *attr, const sigset_t *sm);

#ifdef __cplusplus
}
#endif

#endif /* B1NIX_U_SPAWN_H */
