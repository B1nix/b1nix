#ifndef B1NIX_U_GRP_H
#define B1NIX_U_GRP_H

#include <sys/types.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct group {
  char *gr_name;
  char *gr_passwd;
  gid_t gr_gid;
  char **gr_mem;
};

struct group *getgrgid(gid_t gid);
struct group *getgrnam(const char *name);
int getgrnam_r(const char *name, struct group *grp, char *buf, size_t buflen,
               struct group **result);
int getgrgid_r(gid_t gid, struct group *grp, char *buf, size_t buflen,
               struct group **result);
int initgroups(const char *user, gid_t group);
int setgroups(size_t size, const gid_t *list);

#ifdef __cplusplus
}
#endif

#endif
