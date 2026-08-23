#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/ptrace.h>
#include <errno.h>
#include <string.h>

static pid_t wait_stop(pid_t pid, int *status, int timeout_ms) {
  for (int i = 0; i < timeout_ms / 10; i++) {
    pid_t w = waitpid(pid, status, WUNTRACED | WNOHANG);
    if (w > 0)
      return w;
    if (w < 0)
      return w;
    usleep(10000);
  }
  return 0;
}

int main() {
  int sv[2];
  if (pipe(sv) != 0) {
    printf("pipe failed\n");
    return 1;
  }
  pid_t pid = fork();
  if (pid == 0) {
    close(sv[0]);
    char c = 'r';
    write(sv[1], &c, 1);
    for (;;) usleep(1000);
  }
  close(sv[1]);
  char c = 0;
  read(sv[0], &c, 1);

  errno = 0;
  long sz = ptrace(PTRACE_SEIZE, pid, 0, 0);
  printf("sz = %ld, errno = %d\n", sz, errno);

  int status = 0;
  errno = 0;
  pid_t early = waitpid(pid, &status, WUNTRACED | WNOHANG);
  printf("early = %d, status = %d, errno = %d\n", early, status, errno);

  errno = 0;
  long in = ptrace(PTRACE_INTERRUPT, pid, 0, 0);
  printf("in = %ld, errno = %d\n", in, errno);

  errno = 0;
  pid_t w = wait_stop(pid, &status, 10000);
  printf("w = %d, status = 0x%x, errno = %d\n", w, status, errno);

  long gr = -999;
#ifdef __aarch64__
  struct {
    unsigned long regs[31];
    unsigned long sp;
    unsigned long pc;
    unsigned long pstate;
  } regs;
  memset(&regs, 0, sizeof(regs));
  errno = 0;
  gr = ptrace(PTRACE_GETREGS, pid, 0, &regs);
  printf("gr = %ld, errno = %d, pc = %lx\n", gr, errno, regs.pc);
#endif

  ptrace(PTRACE_KILL, pid, 0, 0);
  waitpid(pid, NULL, 0);
  close(sv[0]);
  return 0;
}
