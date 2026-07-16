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

/* ── Reentrant variants (getpwnam_r / getpwuid_r) ─────────────────────────── *
 * b1nix has no shadow database; these wrap the non-reentrant lookups, copying
 * the string fields into the caller-provided buffer so *result points at the
 * caller's struct. POSIX contract: return 0 and set *result=pwd on success;
 * return 0 and set *result=NULL when the entry is not found; return an errno on
 * error (ERANGE if buf is too small). */
#include <errno.h>

static int pw_copy_r(struct passwd *src, struct passwd *pwd, char *buf,
                     size_t buflen, struct passwd **result) {
    if (!src) { *result = 0; return 0; }
    /* pack the seven strings into buf */
    const char *fields[5];
    fields[0] = src->pw_name   ? src->pw_name   : "";
    fields[1] = src->pw_passwd ? src->pw_passwd : "";
    fields[2] = src->pw_gecos  ? src->pw_gecos  : "";
    fields[3] = src->pw_dir    ? src->pw_dir    : "";
    fields[4] = src->pw_shell  ? src->pw_shell  : "";
    size_t need = 0;
    for (int i = 0; i < 5; i++) need += strlen(fields[i]) + 1;
    if (need > buflen) { *result = 0; return ERANGE; }
    char *p = buf;
    char *dst[5];
    for (int i = 0; i < 5; i++) {
        size_t n = strlen(fields[i]) + 1;
        memcpy(p, fields[i], n);
        dst[i] = p;
        p += n;
    }
    pwd->pw_name   = dst[0];
    pwd->pw_passwd = dst[1];
    pwd->pw_uid    = src->pw_uid;
    pwd->pw_gid    = src->pw_gid;
    pwd->pw_gecos  = dst[2];
    pwd->pw_dir    = dst[3];
    pwd->pw_shell  = dst[4];
    *result = pwd;
    return 0;
}

int getpwnam_r(const char *name, struct passwd *pwd, char *buf, size_t buflen,
               struct passwd **result) {
    return pw_copy_r(getpwnam(name), pwd, buf, buflen, result);
}

int getpwuid_r(uid_t uid, struct passwd *pwd, char *buf, size_t buflen,
               struct passwd **result) {
    return pw_copy_r(getpwuid(uid), pwd, buf, buflen, result);
}
