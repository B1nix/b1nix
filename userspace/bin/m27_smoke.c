#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <syscall.h>
#include <sys/wait.h>

static void marker(const char *s) { write(1, s, strlen(s)); }

/* M27 user/passwd/login basics. Exercises the /etc/passwd database that
 * getpwnam/getpwuid parse, plus the privilege-drop (setgid/setuid) sequence a
 * login program performs after authenticating. */
int main(void) {
  marker("M27-USER: start\n");

  struct passwd *pw;

  pw = getpwnam("root");
  if (pw && pw->pw_uid == 0 && pw->pw_gid == 0 &&
      strcmp(pw->pw_name, "root") == 0 &&
      strcmp(pw->pw_shell, "/bin/bash") == 0)
    marker("M27-USER: ok getpwnam-root\n");
  else
    marker("M27-USER: fail getpwnam-root\n");

  pw = getpwnam("user");
  if (pw && pw->pw_uid == 1000 && pw->pw_gid == 1000 &&
      strcmp(pw->pw_dir, "/home/user") == 0)
    marker("M27-USER: ok getpwnam-user\n");
  else
    marker("M27-USER: fail getpwnam-user\n");

  pw = getpwuid(1000);
  if (pw && strcmp(pw->pw_name, "user") == 0)
    marker("M27-USER: ok getpwuid\n");
  else
    marker("M27-USER: fail getpwuid\n");

  if (!getpwnam("nobody"))
    marker("M27-USER: ok unknown-user\n");
  else
    marker("M27-USER: fail unknown-user\n");

  /* Drop privileges exactly as login does after authenticating. Done in a
   * child so the parent stays root; setuid(0->1000) is irreversible. */
  int pid = fork();
  if (pid == 0) {
    int ok = setgid(1000) == 0 && setuid(1000) == 0 &&
             syscall(SYS_GETUID) == 1000 && syscall(SYS_GETGID) == 1000;
    syscall(SYS_EXIT, ok ? 0 : 1);
  } else if (pid > 0) {
    int st = 0;
    waitpid(pid, &st, 0);
    if (st == 0)
      marker("M27-USER: ok setuid-drop\n");
    else
      marker("M27-USER: fail setuid-drop\n");
  } else {
    marker("M27-USER: fail setuid-drop\n");
  }

  marker("M27-USER: done\n");
  return 0;
}
