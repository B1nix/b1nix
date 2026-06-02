/* M32b: minimal group-database shim for userspace ports (dropbear).
 *
 * b1nix has no /etc/group database and no supplementary-group model, so this
 * returns a single synthetic entry per gid and treats initgroups/setgroups as
 * no-ops. Enough for sshd to look up a login group name and set the primary
 * gid (done via setgid elsewhere). */

#include <grp.h>
#include <string.h>

static struct group g_grp;
static char g_name[32];
static char *g_empty_mem[] = {0};

static struct group *fill(gid_t gid) {
  if (gid == 0)
    strcpy(g_name, "root");
  else
    strcpy(g_name, "users");
  g_grp.gr_name = g_name;
  g_grp.gr_passwd = (char *)"x";
  g_grp.gr_gid = gid;
  g_grp.gr_mem = g_empty_mem;
  return &g_grp;
}

struct group *getgrgid(gid_t gid) { return fill(gid); }

struct group *getgrnam(const char *name) {
  if (name && strcmp(name, "root") == 0)
    return fill(0);
  return fill(100); /* "users" */
}

int getgrgid_r(gid_t gid, struct group *grp, char *buf, size_t buflen,
               struct group **result) {
  const char *nm = (gid == 0) ? "root" : "users";
  size_t nlen = strlen(nm) + 1;
  if (buflen < nlen + sizeof(char *)) {
    *result = 0;
    return -1;
  }
  memcpy(buf, nm, nlen);
  char **mem = (char **)(buf + nlen);
  mem[0] = 0;
  grp->gr_name = buf;
  grp->gr_passwd = (char *)"x";
  grp->gr_gid = gid;
  grp->gr_mem = mem;
  *result = grp;
  return 0;
}

int getgrnam_r(const char *name, struct group *grp, char *buf, size_t buflen,
               struct group **result) {
  gid_t gid = (name && strcmp(name, "root") == 0) ? 0 : 100;
  return getgrgid_r(gid, grp, buf, buflen, result);
}

int initgroups(const char *user, gid_t group) {
  (void)user;
  (void)group;
  return 0;
}

int setgroups(size_t size, const gid_t *list) {
  (void)size;
  (void)list;
  return 0;
}
