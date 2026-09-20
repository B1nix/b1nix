/*
 * ext_stress — ext4 filesystem stress test (mkdir/open/write/symlink/link/
 * rename/unlink/rmdir cycle).  Ported from deleted kernel/user/programs.c
 * ext_stress_main().  Runs 50 iterations per mount point.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <unistd.h>

static void marker(const char *t) { write(1, t, strlen(t)); }

static int run_stress(const char *mp) {
  /* Check the mount is actually active via /proc/mounts. */
  FILE *f = fopen("/proc/mounts", "r");
  if (!f) return 0;
  char line[256];
  int found = 0;
  while (fgets(line, sizeof(line), f)) {
    if (strstr(line, mp)) { found = 1; break; }
  }
  fclose(f);
  if (!found) return 0;

  /* Quick sanity: touch + delete a test file. */
  char tmp[256];
  snprintf(tmp, sizeof(tmp), "%s/.stress_test", mp);
  int fd = open(tmp, O_CREAT | O_RDWR, 0666);
  if (fd < 0) return 0;
  close(fd);
  unlink(tmp);

  marker("EXT-STRESS: running on ");
  marker(mp);
  marker("\n");

  char dir_p[256], file_p[256], ren_p[256], sym_p[256], lnk_p[256];
  char wbuf[128];

  for (int i = 0; i < 50; i++) {
    snprintf(dir_p, sizeof(dir_p), "%s/dir_%d", mp, i);
    if (mkdir(dir_p, 0755) < 0 && errno != EEXIST) {
      marker("EXT-STRESS: mkdir failed\n"); return 1;
    }
    snprintf(file_p, sizeof(file_p), "%s/dir_%d/file_%d", mp, i, i);
    fd = open(file_p, O_CREAT | O_RDWR | O_TRUNC, 0666);
    if (fd < 0) { marker("EXT-STRESS: open failed\n"); return 1; }
    int n = snprintf(wbuf, sizeof(wbuf),
                     "Stress data for iteration %d.\n", i);
    if (write(fd, wbuf, n) < 0) {
      marker("EXT-STRESS: write failed\n"); close(fd); return 1;
    }
    fsync(fd); close(fd);

    snprintf(sym_p, sizeof(sym_p), "%s/dir_%d/sym_%d", mp, i, i);
    if (symlink(file_p, sym_p) < 0) {
      marker("EXT-STRESS: symlink failed\n"); return 1;
    }
    snprintf(lnk_p, sizeof(lnk_p), "%s/dir_%d/link_%d", mp, i, i);
    if (link(file_p, lnk_p) < 0) {
      marker("EXT-STRESS: link failed\n"); return 1;
    }
    snprintf(ren_p, sizeof(ren_p), "%s/dir_%d/renamed_%d", mp, i, i);
    if (rename(file_p, ren_p) < 0) {
      marker("EXT-STRESS: rename failed\n"); return 1;
    }
    if (unlink(sym_p) < 0) {
      marker("EXT-STRESS: unlink sym failed\n"); return 1;
    }
    if (unlink(lnk_p) < 0) {
      marker("EXT-STRESS: unlink link failed\n"); return 1;
    }
    if (unlink(ren_p) < 0) {
      marker("EXT-STRESS: unlink renamed failed\n"); return 1;
    }
    if (rmdir(dir_p) < 0) {
      marker("EXT-STRESS: rmdir failed\n"); return 1;
    }
  }
  marker("EXT-STRESS: done on ");
  marker(mp);
  marker("\n");
  return 0;
}

int main(void) {
  marker("EXT-STRESS: start\n");
  run_stress("/mnt/ext4");
  run_stress("/mnt/ext3");
  marker("EXT-STRESS: done\n");
  return 0;
}
