#ifndef B1NIX_U_PWD_H
#define B1NIX_U_PWD_H

#include <sys/types.h>

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

#endif
