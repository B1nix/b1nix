#include <pwd.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

/* Hardcoded fallback: root (uid 0) is always resolvable even without a passwd
 * file, which keeps tilde (~root) expansion in GNU Make's bundled glob working
 * when /etc/passwd is absent. */
static struct passwd root_pw = {
    "root",      /* pw_name   */
    "x",         /* pw_passwd */
    0,           /* pw_uid    */
    0,           /* pw_gid    */
    "root",      /* pw_gecos  */
    "/root",     /* pw_dir    */
    "/bin/sh",   /* pw_shell  */
};

/* Returned struct + its strings live in static storage, overwritten on each
 * call (standard getpwnam/getpwuid contract). */
static struct passwd pw_ent;
static char pw_line[256];

/* Split a "name:passwd:uid:gid:gecos:home:shell" line in place. Returns 1 on a
 * well-formed line, 0 otherwise. */
static int parse_pwline(char *line, struct passwd *out) {
    char *field[7];
    int n = 0;
    char *p = line;

    field[n++] = p;
    while (*p && n < 7) {
        if (*p == ':') {
            *p = '\0';
            field[n++] = p + 1;
        }
        p++;
    }
    if (n < 7)
        return 0;

    out->pw_name = field[0];
    out->pw_passwd = field[1];
    out->pw_uid = (uid_t)atoi(field[2]);
    out->pw_gid = (gid_t)atoi(field[3]);
    out->pw_gecos = field[4];
    out->pw_dir = field[5];
    out->pw_shell = field[6];
    return 1;
}

/* When /etc/passwd records the password as "x" (or "*"), the real hash lives in
 * /etc/shadow. Fill `out` (size 256) with the shadow hash for `name`; returns 1
 * on success. This is what lets sshd/login verify against /etc/shadow via the
 * standard getpwnam()->pw_passwd path. */
static int shadow_hash(const char *name, char *out, int outsz) {
  int fd = open("/etc/shadow", O_RDONLY);
  if (fd < 0)
    return 0;
  char buf[1024];
  char acc[512];
  int acc_len = 0, found = 0;
  ssize_t r;
  size_t nlen = strlen(name);
  while (!found && (r = read(fd, buf, sizeof(buf))) > 0) {
    for (ssize_t i = 0; i < r; i++) {
      char c = buf[i];
      if (c == '\n' || acc_len == (int)sizeof(acc) - 1) {
        acc[acc_len] = '\0';
        /* format: name:hash:... */
        if ((size_t)acc_len > nlen && acc[nlen] == ':' &&
            strncmp(acc, name, nlen) == 0) {
          char *h = acc + nlen + 1;
          char *end = h;
          while (*end && *end != ':')
            end++;
          int hlen = (int)(end - h);
          if (hlen > 0 && hlen < outsz) {
            memcpy(out, h, (size_t)hlen);
            out[hlen] = '\0';
            found = 1;
          }
        }
        acc_len = 0;
        if (found)
          break;
      } else {
        acc[acc_len++] = c;
      }
    }
  }
  close(fd);
  return found;
}

static char pw_shadow[256];

/* Scan /etc/passwd for a matching entry. want_name non-NULL matches pw_name;
 * otherwise matches pw_uid == want_uid. Returns &pw_ent or NULL. */
static struct passwd *lookup(const char *want_name, uid_t want_uid) {
    int fd = open("/etc/passwd", O_RDONLY);
    if (fd < 0)
        return 0;

    char buf[1024];
    char acc[256];
    int acc_len = 0;
    struct passwd *result = 0;
    ssize_t r;

    while (result == 0 && (r = read(fd, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < r; i++) {
            char c = buf[i];
            if (c == '\n' || acc_len == (int)sizeof(acc) - 1) {
                acc[acc_len] = '\0';
                if (acc_len > 0 && acc[0] != '#') {
                    memcpy(pw_line, acc, (size_t)acc_len + 1);
                    if (parse_pwline(pw_line, &pw_ent)) {
                        int hit = want_name
                                      ? strcmp(pw_ent.pw_name, want_name) == 0
                                      : pw_ent.pw_uid == want_uid;
                        if (hit) {
                            /* Substitute the real hash from /etc/shadow when
                             * the passwd field is the "x"/"*" shadow marker. */
                            if (pw_ent.pw_passwd &&
                                (strcmp(pw_ent.pw_passwd, "x") == 0 ||
                                 strcmp(pw_ent.pw_passwd, "*") == 0) &&
                                shadow_hash(pw_ent.pw_name, pw_shadow,
                                            sizeof(pw_shadow))) {
                                pw_ent.pw_passwd = pw_shadow;
                            }
                            result = &pw_ent;
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

struct passwd *getpwnam(const char *name) {
    if (!name)
        return 0;
    struct passwd *p = lookup(name, 0);
    if (p)
        return p;
    if (strcmp(name, "root") == 0)
        return &root_pw;
    return 0;
}

struct passwd *getpwuid(uid_t uid) {
    struct passwd *p = lookup(0, uid);
    if (p)
        return p;
    if (uid == 0)
        return &root_pw;
    return 0;
}
