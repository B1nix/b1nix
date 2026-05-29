#include <pwd.h>
#include <string.h>

/* b1nix has no passwd database; root (uid 0) is the only known account. These
 * back tilde (~user) expansion in GNU Make's bundled glob — only "~"/"~root"
 * resolve, which is all the in-guest build needs. Unknown users return NULL,
 * honestly reflecting the absence of a passwd db. */
static struct passwd root_pw = {
    "root",      /* pw_name   */
    "x",         /* pw_passwd */
    0,           /* pw_uid    */
    0,           /* pw_gid    */
    "root",      /* pw_gecos  */
    "/root",     /* pw_dir    */
    "/bin/sh",   /* pw_shell  */
};

struct passwd *getpwnam(const char *name)
{
    if (name && strcmp(name, "root") == 0)
        return &root_pw;
    return 0;
}

struct passwd *getpwuid(uid_t uid)
{
    if (uid == 0)
        return &root_pw;
    return 0;
}
