#ifndef B1NIX_U_PWD_H
#define B1NIX_U_PWD_H

#include <sys/types.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct passwd {
    char  *pw_name;   /* user name */
    char  *pw_passwd; /* (unused — b1nix has no shadow db) */
    uid_t  pw_uid;    /* user id */
    gid_t  pw_gid;    /* group id */
    char  *pw_gecos;  /* real name / comment */
    char  *pw_dir;    /* home directory */
    char  *pw_shell;  /* login shell */
};

struct passwd *getpwnam(const char *name);
struct passwd *getpwuid(uid_t uid);

/* Reentrant variants. b1nix has no shadow db; these fill *pwd from the same
 * source as getpwnam/getpwuid, copying strings into buf. */
int getpwnam_r(const char *name, struct passwd *pwd, char *buf, size_t buflen,
               struct passwd **result);
int getpwuid_r(uid_t uid, struct passwd *pwd, char *buf, size_t buflen,
               struct passwd **result);

#ifdef __cplusplus
}
#endif

#endif
