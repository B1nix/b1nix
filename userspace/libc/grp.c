#include <grp.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include "syscall.h"

extern int normalize_errno(long rc);

static struct group gr_ent;
static char gr_line[256];
static char *gr_mem[32];

static struct group root_grp = {
    "root", "x", 0, (char *[]){"root", 0}
};
static struct group users_grp = {
    "users", "x", 1000, (char *[]){"user", 0}
};

/* Split a "group_name:password:GID:user_list" line in place.
 * Returns 1 on success, 0 otherwise. */
static int parse_grline(char *line, struct group *out) {
    char *field[4];
    int n = 0;
    char *p = line;

    field[n++] = p;
    while (*p && n < 4) {
        if (*p == ':') {
            *p = '\0';
            field[n++] = p + 1;
        }
        p++;
    }
    if (n < 3)
        return 0;

    out->gr_name = field[0];
    out->gr_passwd = field[1];
    out->gr_gid = (gid_t)atoi(field[2]);

    int mem_count = 0;
    if (n == 4 && field[3] && field[3][0] != '\0') {
        char *m = field[3];
        /* strip trailing newlines */
        size_t len = strlen(m);
        while (len > 0 && (m[len-1] == '\n' || m[len-1] == '\r')) {
            m[len-1] = '\0';
            len--;
        }
        if (m[0] != '\0') {
            gr_mem[mem_count++] = m;
            while (*m) {
                if (*m == ',') {
                    *m = '\0';
                    gr_mem[mem_count++] = m + 1;
                }
                m++;
            }
        }
    }
    gr_mem[mem_count] = 0;
    out->gr_mem = gr_mem;
    return 1;
}

static struct group *lookup_group(const char *want_name, gid_t want_gid) {
    int fd = open("/etc/group", O_RDONLY);
    if (fd < 0)
        return 0;

    char buf[1024];
    char acc[256];
    int acc_len = 0;
    struct group *result = 0;
    ssize_t r;

    while (result == 0 && (r = read(fd, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < r; i++) {
            char c = buf[i];
            if (c == '\n' || acc_len == (int)sizeof(acc) - 1) {
                acc[acc_len] = '\0';
                if (acc_len > 0 && acc[0] != '#') {
                    memcpy(gr_line, acc, (size_t)acc_len + 1);
                    if (parse_grline(gr_line, &gr_ent)) {
                        int hit = want_name
                                      ? strcmp(gr_ent.gr_name, want_name) == 0
                                      : gr_ent.gr_gid == want_gid;
                        if (hit) {
                            result = &gr_ent;
                            break;
                        }
                    }
                }
                acc_len = 0;
            } else {
                acc[acc_len++] = c;
            }
        }
    }

    close(fd);
    return result;
}

struct group *getgrgid(gid_t gid) {
    struct group *g = lookup_group(0, gid);
    if (g) return g;
    if (gid == 0) return &root_grp;
    if (gid == 1000) return &users_grp;
    return 0;
}

struct group *getgrnam(const char *name) {
    if (!name) return 0;
    struct group *g = lookup_group(name, 0);
    if (g) return g;
    if (strcmp(name, "root") == 0) return &root_grp;
    if (strcmp(name, "users") == 0) return &users_grp;
    return 0;
}

int getgrgid_r(gid_t gid, struct group *grp, char *buf, size_t buflen,
               struct group **result) {
  struct group *g = getgrgid(gid);
  if (!g) {
    *result = 0;
    return ENOENT;
  }
  size_t nlen = strlen(g->gr_name) + 1;
  size_t plen = strlen(g->gr_passwd) + 1;
  size_t mem_strings_len = 0;
  int num_mem = 0;
  while (g->gr_mem[num_mem]) {
    mem_strings_len += strlen(g->gr_mem[num_mem]) + 1;
    num_mem++;
  }
  
  size_t needed = nlen + plen + mem_strings_len + sizeof(char *) * (num_mem + 1) + sizeof(char *);
  if (buflen < needed) {
    *result = 0;
    return ERANGE;
  }
  char *p = buf;
  strcpy(p, g->gr_name);
  grp->gr_name = p;
  p += nlen;
  strcpy(p, g->gr_passwd);
  grp->gr_passwd = p;
  p += plen;
  
  char **mem = (char **)(((size_t)p + sizeof(char *) - 1) & ~(sizeof(char *) - 1));
  p = (char *)(mem + (num_mem + 1));
  for (int i = 0; i < num_mem; i++) {
    strcpy(p, g->gr_mem[i]);
    mem[i] = p;
    p += strlen(g->gr_mem[i]) + 1;
  }
  mem[num_mem] = 0;
  
  grp->gr_gid = g->gr_gid;
  grp->gr_mem = mem;
  *result = grp;
  return 0;
}

int getgrnam_r(const char *name, struct group *grp, char *buf, size_t buflen,
               struct group **result) {
  struct group *g = getgrnam(name);
  if (!g) {
    *result = 0;
    return ENOENT;
  }
  return getgrgid_r(g->gr_gid, grp, buf, buflen, result);
}

int setgroups(size_t size, const gid_t *list) {
  long rc = syscall(SYS_SETGROUPS, size, list);
  if (rc < 0) {
    errno = normalize_errno(rc);
    return -1;
  }
  return 0;
}

int initgroups(const char *user, gid_t group) {
  if (!user) return -1;
  gid_t list[32];
  int count = 0;
  list[count++] = group;

  int fd = open("/etc/group", O_RDONLY);
  if (fd >= 0) {
    char buf[1024];
    char acc[256];
    int acc_len = 0;
    ssize_t r;
    while ((r = read(fd, buf, sizeof(buf))) > 0) {
      for (ssize_t i = 0; i < r; i++) {
        char c = buf[i];
        if (c == '\n' || acc_len == (int)sizeof(acc) - 1) {
          acc[acc_len] = '\0';
          if (acc_len > 0 && acc[0] != '#') {
            struct group temp_grp;
            char temp_line[256];
            memcpy(temp_line, acc, (size_t)acc_len + 1);
            if (parse_grline(temp_line, &temp_grp)) {
              int found = 0;
              for (int j = 0; temp_grp.gr_mem[j]; j++) {
                if (strcmp(temp_grp.gr_mem[j], user) == 0) {
                  found = 1;
                  break;
                }
              }
              if (found) {
                int dup = 0;
                for (int j = 0; j < count; j++) {
                  if (list[j] == temp_grp.gr_gid) {
                    dup = 1;
                    break;
                  }
                }
                if (!dup && count < 32) {
                  list[count++] = temp_grp.gr_gid;
                }
              }
            }
          }
          acc_len = 0;
        } else {
          acc[acc_len++] = c;
        }
      }
    }
    close(fd);
  }
  return setgroups((size_t)count, list);
}

int getgrouplist(const char *user, gid_t group, gid_t *groups, int *ngroups) {
  if (!user || !ngroups) {
    errno = EINVAL;
    return -1;
  }

  gid_t found[32];
  int count = 0;
  found[count++] = group;

  int fd = open("/etc/group", O_RDONLY);
  if (fd >= 0) {
    char buf[1024];
    char acc[256];
    int acc_len = 0;
    ssize_t r;
    while ((r = read(fd, buf, sizeof(buf))) > 0) {
      for (ssize_t i = 0; i < r; i++) {
        char c = buf[i];
        if (c == '\n' || acc_len == (int)sizeof(acc) - 1) {
          acc[acc_len] = '\0';
          if (acc_len > 0 && acc[0] != '#') {
            struct group temp;
            char line[256];
            memcpy(line, acc, (size_t)acc_len + 1);
            if (parse_grline(line, &temp)) {
              int member = 0;
              for (int j = 0; temp.gr_mem[j]; j++) {
                if (strcmp(temp.gr_mem[j], user) == 0) {
                  member = 1;
                  break;
                }
              }
              if (member) {
                int duplicate = 0;
                for (int j = 0; j < count; j++)
                  if (found[j] == temp.gr_gid)
                    duplicate = 1;
                if (!duplicate && count < (int)(sizeof(found) / sizeof(found[0])))
                  found[count++] = temp.gr_gid;
              }
            }
          }
          acc_len = 0;
        } else {
          acc[acc_len++] = c;
        }
      }
    }
    close(fd);
  }

  int capacity = *ngroups;
  *ngroups = count;
  if (!groups || capacity < count) {
    return -1;
  }
  for (int i = 0; i < count; i++)
    groups[i] = found[i];
  return count;
}

void endgrent(void) {
}
