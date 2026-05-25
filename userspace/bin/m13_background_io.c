#include <b1nix/syscall.h>
#include <b1nix/sched.h>
#include <b1nix/errno.h>

/* Helper macros for waitpid status */
#define WIFSTOPPED(status) (((status) & 0xff) == 0x7f)
#define WSTOPSIG(status)   (((status) & 0xff00) >> 8)
#define WIFSIGNALED(status) (!WIFSTOPPED(status) && !WIFEXITED(status))
#define WIFEXITED(status)  (((status) & 0x7f) == 0)

int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;

  int pid = fork();
  if (pid < 0) {
    b1nix_write(1, "fork failed\n", 12);
    return 1;
  }

  if (pid == 0) {
    /* Child Process */
    /* Create new process group but don't set as foreground */
    setpgid(0, 0);

    /* Attempt to read from stdin (which is foreground-controlled) */
    char c;
    int res = read(0, &c, 1);

    /* We should never reach here if SIGTTIN worked as intended,
       unless it's an orphaned group where it returns -EIO. 
       But our parent is alive, so we expect to be STOPPED by SIGTTIN. */
    if (res < 0) {
      b1nix_write(1, "child read failed\n", 18);
    } else {
      b1nix_write(1, "child read succeeded somehow\n", 29);
    }
    
    _exit(0);
  }

  /* Parent Process */
  /* We wait for the child to stop */
  int status = 0;
  int wpid = waitpid(pid, &status, B1NIX_WUNTRACED);

  if (wpid < 0) {
    b1nix_write(1, "waitpid failed\n", 15);
    kill(pid, SIGKILL);
    return 1;
  }

  /* Validate child was stopped by SIGTTIN */
  if (WIFSTOPPED(status) && WSTOPSIG(status) == SIGTTIN) {
    b1nix_write(1, "M13-JC-SMOKE: ok sigttin\n", 25);
  } else {
    b1nix_write(1, "M13-JC-SMOKE: fail\n", 19);
  }

  /* Cleanup: kill the child */
  kill(pid, SIGKILL);

  /* Wait for it to die to avoid zombies */
  waitpid(pid, &status, 0);

  return 0;
}
