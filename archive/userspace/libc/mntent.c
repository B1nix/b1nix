/* mntent.c — /etc/fstab and /proc/mounts iteration (getmntent family).
 *
 * Minimal, POSIX/glibc-shaped implementation sufficient for the upstream
 * BusyBox `df` applet (migration wave 2b), which reads /proc/mounts via
 * setmntent()/getmntent()/endmntent(). Fields are whitespace-separated; blank
 * and `#`-comment lines are skipped. Octal-escape (\040) decoding is not
 * performed — the b1nix procfs does not emit escaped mount paths.
 */
#include <mntent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct mntent g_mntent;
static char g_mntent_buf[4096];
static char g_empty[1] = "";

/* Extract the next whitespace-delimited field in place, NUL-terminating it and
 * advancing *pp past it. Returns NULL at end of line. */
static char *parse_field(char **pp) {
  char *p = *pp;
  while (*p == ' ' || *p == '\t')
    p++;
  if (*p == '\0' || *p == '\n') {
    *pp = p;
    return NULL;
  }
  char *start = p;
  while (*p && *p != ' ' && *p != '\t' && *p != '\n')
    p++;
  if (*p) {
    *p = '\0';
    p++;
  }
  *pp = p;
  return start;
}

FILE *setmntent(const char *filename, const char *type) {
  return fopen(filename, type);
}

struct mntent *getmntent_r(FILE *stream, struct mntent *result, char *buffer,
                           int bufsize) {
  if (!stream || !result || !buffer || bufsize <= 0)
    return NULL;

  char *line;
  do {
    if (!fgets(buffer, bufsize, stream))
      return NULL;
    line = buffer;
    while (*line == ' ' || *line == '\t')
      line++;
  } while (*line == '\0' || *line == '\n' || *line == '#');

  char *p = buffer;
  result->mnt_fsname = parse_field(&p);
  result->mnt_dir = parse_field(&p);
  result->mnt_type = parse_field(&p);
  result->mnt_opts = parse_field(&p);
  char *freq = parse_field(&p);
  char *passno = parse_field(&p);

  if (!result->mnt_fsname || !result->mnt_dir)
    return NULL;
  if (!result->mnt_type)
    result->mnt_type = g_empty;
  if (!result->mnt_opts)
    result->mnt_opts = g_empty;
  result->mnt_freq = freq ? atoi(freq) : 0;
  result->mnt_passno = passno ? atoi(passno) : 0;
  return result;
}

struct mntent *getmntent(FILE *stream) {
  return getmntent_r(stream, &g_mntent, g_mntent_buf, sizeof(g_mntent_buf));
}

int addmntent(FILE *stream, const struct mntent *mnt) {
  if (!stream || !mnt)
    return 1;
  if (fprintf(stream, "%s %s %s %s %d %d\n", mnt->mnt_fsname, mnt->mnt_dir,
              mnt->mnt_type, mnt->mnt_opts, mnt->mnt_freq, mnt->mnt_passno) < 0)
    return 1;
  return 0;
}

int endmntent(FILE *stream) {
  if (stream)
    fclose(stream);
  return 1;
}

char *hasmntopt(const struct mntent *mnt, const char *opt) {
  if (!mnt || !mnt->mnt_opts || !opt)
    return NULL;
  size_t optlen = strlen(opt);
  char *p = mnt->mnt_opts;
  while (p && *p) {
    if (strncmp(p, opt, optlen) == 0 &&
        (p[optlen] == '\0' || p[optlen] == ',' || p[optlen] == '='))
      return p;
    p = strchr(p, ',');
    if (p)
      p++;
  }
  return NULL;
}
