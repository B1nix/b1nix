/* Minimal utmp/utmpx login-accounting implementation for b1nix.
 *
 * musl deliberately ships no-op stubs for these (src/legacy/utmpx.c) and
 * exposes pututline/getutline/setutent/endutent/utmpname as WEAK aliases of
 * the utmpx no-ops. b1nix supports a real utmp file, so these strong
 * definitions override the weak stubs when this object is on the link line.
 *
 * The file is a flat array of `struct utmp` records; pututline rewrites a
 * record with a matching ut_line in place (or appends), getutline scans for a
 * live entry on the requested line. Enough for login/who and the M29 test. */
#include <utmp.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

static int utmp_fd = -1;
static char utmp_file[256] = "/var/run/utmp";

int utmpname(const char *file) {
  if (!file)
    return -1;
  strncpy(utmp_file, file, sizeof(utmp_file) - 1);
  utmp_file[sizeof(utmp_file) - 1] = 0;
  if (utmp_fd >= 0) {
    close(utmp_fd);
    utmp_fd = -1;
  }
  return 0;
}

void setutent(void) {
  if (utmp_fd < 0)
    utmp_fd = open(utmp_file, O_RDWR | O_CREAT, 0644);
  else
    lseek(utmp_fd, 0, SEEK_SET);
}

void endutent(void) {
  if (utmp_fd >= 0) {
    close(utmp_fd);
    utmp_fd = -1;
  }
}

struct utmp *getutent(void) {
  static struct utmp ent;
  if (utmp_fd < 0)
    setutent();
  if (utmp_fd < 0)
    return 0;
  if (read(utmp_fd, &ent, sizeof(ent)) != (ssize_t)sizeof(ent))
    return 0;
  return &ent;
}

struct utmp *getutline(const struct utmp *line) {
  static struct utmp ent;
  if (utmp_fd < 0)
    setutent();
  if (utmp_fd < 0)
    return 0;
  while (read(utmp_fd, &ent, sizeof(ent)) == (ssize_t)sizeof(ent)) {
    if ((ent.ut_type == USER_PROCESS || ent.ut_type == LOGIN_PROCESS) &&
        strncmp(ent.ut_line, line->ut_line, sizeof(ent.ut_line)) == 0)
      return &ent;
  }
  return 0;
}

struct utmp *pututline(const struct utmp *ut) {
  static struct utmp ent;
  if (utmp_fd < 0)
    setutent();
  if (utmp_fd < 0)
    return 0;
  /* Overwrite the record on the same line if present. */
  lseek(utmp_fd, 0, SEEK_SET);
  long pos = 0;
  while (read(utmp_fd, &ent, sizeof(ent)) == (ssize_t)sizeof(ent)) {
    if (strncmp(ent.ut_line, ut->ut_line, sizeof(ent.ut_line)) == 0) {
      if (lseek(utmp_fd, pos, SEEK_SET) < 0 ||
          write(utmp_fd, ut, sizeof(*ut)) != (ssize_t)sizeof(*ut))
        return 0;
      ent = *ut;
      return &ent;
    }
    pos += (long)sizeof(ent);
  }
  /* Otherwise append. */
  if (lseek(utmp_fd, 0, SEEK_END) < 0 ||
      write(utmp_fd, ut, sizeof(*ut)) != (ssize_t)sizeof(*ut))
    return 0;
  ent = *ut;
  return &ent;
}
