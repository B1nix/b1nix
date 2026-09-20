/*
 * m24_stress — scheduler stress: sequential spawn-wait of /bin/true across
 * 24 iterations to verify task slot recycling under load.
 * Ported from deleted kernel/user/programs.c m24_stress_main().
 */
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void marker(const char *t) { write(1, t, strlen(t)); }

int main(void) {
  marker("M24-STRESS: start\n");
  int failures = 0;

  for (int i = 0; i < 24; i++) {
    pid_t pid = fork();
    if (pid < 0) { failures++; continue; }
    if (pid == 0) {
      char *args[] = {"/bin/true", NULL};
      execve("/bin/true", args, NULL);
      _exit(127);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
      failures++;
  }

  if (failures) {
    marker("M24-STRESS: fail\n");
    return 1;
  }
  marker("M24-STRESS: done\n");
  return 0;
}
