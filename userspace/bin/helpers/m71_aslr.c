/* M71 smoke: ASLR proof.
 *
 * A PIE/ET_DYN binary is relocated to whatever base the kernel picks. With
 * `b1nix.aslr` enabled the base is randomized per-exec, so the runtime address
 * of a fixed symbol (&main = load_base + const offset) differs between two
 * independent execs of this same binary. The parent execs itself twice (as a
 * "child", capturing the printed address over a pipe) and asserts the two bases
 * differ — a same-base result would mean randomization did not happen.
 *
 * This test is launched by the kernel only when `b1nix.aslr` is set, so it
 * always runs with randomization active; if the two bases matched it would be a
 * real ASLR failure, not an expected no-op.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void marker(const char *s) {
  write(1, s, strlen(s));
  write(1, "\n", 1);
}

/* Exec a child copy of ourselves; it prints &main as hex on its stdout, which
 * we capture and parse. Returns the address, or 0 on failure. */
static unsigned long run_child(char **envp) {
  int p[2];
  if (pipe(p) < 0)
    return 0;
  pid_t c = fork();
  if (c == 0) {
    close(p[0]);
    dup2(p[1], 1);
    close(p[1]);
    char *av[] = {"/bin/m71_aslr", "child", 0};
    execve("/bin/m71_aslr", av, envp);
    _exit(127);
  }
  close(p[1]);
  char buf[32];
  int n = (int)read(p[0], buf, sizeof(buf) - 1);
  close(p[0]);
  int st = 0;
  waitpid(c, &st, 0);
  if (n <= 0)
    return 0;
  buf[n] = 0;
  return strtoul(buf, 0, 16);
}

int main(int argc, char **argv, char **envp) {
  if (argc > 1 && strcmp(argv[1], "child") == 0) {
    char buf[24];
    int n = snprintf(buf, sizeof(buf), "%lx\n",
                     (unsigned long)(uintptr_t)&main);
    write(1, buf, n);
    return 0;
  }

  marker("M71-ASLR: start");
  unsigned long a = run_child(envp);
  unsigned long b = run_child(envp);
  if (a == 0 || b == 0) {
    marker("M71-ASLR: FAIL exec");
    return 1;
  }
  if (a != b)
    marker("M71-ASLR: ok randomized");
  else
    marker("M71-ASLR: FAIL same-base");
  return 0;
}
