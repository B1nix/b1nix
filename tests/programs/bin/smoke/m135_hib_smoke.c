/* SPDX-License-Identifier: GPL-2.0-only */
/* m135_hib_smoke — hibernation (M135).
 *
 * The one thing this instance does: write the machine to disk and come back.
 * Before the write it leaves state that a fresh boot would not have -- a value
 * in this process's memory, a file in /tmp (which is RAM) -- and asks for a
 * reboot rather than a power-off, so the same QEMU instance boots again and the
 * kernel finds the image. When the write to /sys/power/state returns, this
 * process is running in the restored machine, and the state must still be
 * there. The lane also counts the boots in the log: two, or the resume did not
 * really go through a reboot.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int fails;

static void ok(const char *what) {
  printf("M135-HIB: ok %s\n", what);
  fflush(stdout);
}

static void bad(const char *what, const char *why, long v) {
  printf("M135-HIB: FAIL %s — %s (%ld, errno=%d)\n", what, why, v, errno);
  fflush(stdout);
  fails++;
}

static void read_str(const char *path, char *buf, int cap) {
  int fd = open(path, O_RDONLY), n = -1;

  buf[0] = 0;
  if (fd < 0)
    return;
  n = (int)read(fd, buf, (size_t)cap - 1);
  close(fd);
  buf[n > 0 ? n : 0] = 0;
  buf[strcspn(buf, "\n")] = 0;
}

static int write_str(const char *path, const char *text) {
  int fd = open(path, O_WRONLY), rc = 0;

  if (fd < 0)
    return errno;
  if (write(fd, text, strlen(text)) != (ssize_t)strlen(text))
    rc = errno ? errno : EIO;
  close(fd);
  return rc;
}

/* Something no fresh boot has: a random word, in memory and in /tmp. */
static volatile unsigned long long g_token;

int main(void) {
  char states[64], disk[64], resume[64], back[64];
  struct timespec t0, t1;
  int rc;

  printf("M135-HIB: start\n");
  fflush(stdout);
  read_str("/sys/power/state", states, sizeof(states));
  read_str("/sys/power/resume", resume, sizeof(resume));
  printf("M135-HIB: state \"%s\" resume \"%s\"\n", states, resume);
  if (!strstr(states, "disk")) {
    bad("disk-offered", "/sys/power/state does not offer disk", 0);
    printf("M135-HIB: done\n");
    return 1;
  }
  ok("disk-offered");

  clock_gettime(CLOCK_REALTIME, &t0);
  g_token = ((unsigned long long)t0.tv_nsec << 20) ^ (unsigned long long)getpid() ^
            0x5b1b1e5ull;
  {
    char text[32];
    int fd = open("/tmp/m135-hib-token", O_WRONLY | O_CREAT | O_TRUNC, 0644);

    snprintf(text, sizeof(text), "%llx", g_token);
    if (fd < 0 || write(fd, text, strlen(text)) < 0) {
      bad("token", "could not leave a token in /tmp", fd);
      return 1;
    }
    close(fd);
  }
  rc = write_str("/sys/power/disk", "reboot");
  read_str("/sys/power/disk", disk, sizeof(disk));
  printf("M135-HIB: disk \"%s\" (write %d); hibernating\n", disk, rc);
  fflush(stdout);

  rc = write_str("/sys/power/state", "disk");
  clock_gettime(CLOCK_REALTIME, &t1);
  printf("M135-HIB: back: write %d after %ld s\n", rc,
         (long)(t1.tv_sec - t0.tv_sec));
  fflush(stdout);

  {
    char want[32];

    snprintf(want, sizeof(want), "%llx", g_token);
    read_str("/tmp/m135-hib-token", back, sizeof(back));
    printf("M135-HIB: token %s, /tmp says %s\n", want, back);
    if (rc == 0 && back[0] && strcmp(back, want) == 0)
      ok("resumed");
    else
      bad("resumed", "the machine that came back is not the one that "
                     "hibernated", rc);
  }
  /* The swap area is a swap area again: a second boot would not resume. */
  printf("M135-HIB: done\n");
  fflush(stdout);
  return fails ? 1 : 0;
}
