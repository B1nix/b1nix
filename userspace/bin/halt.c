#include <stdio.h>
#include <string.h>
#include <syscall.h>

int main(int argc, char **argv) {
  const char *name = "halt";
  if (argc > 0 && argv[0]) {
    const char *slash = strrchr(argv[0], '/');
    name = slash ? slash + 1 : argv[0];
  }

  long cmd;
  if (strcmp(name, "reboot") == 0)
    cmd = B1NIX_REBOOT_RESTART;
  else if (strcmp(name, "poweroff") == 0 || strcmp(name, "shutdown") == 0)
    cmd = B1NIX_REBOOT_POWEROFF;
  else
    cmd = B1NIX_REBOOT_HALT;

  syscall(SYS_REBOOT, cmd);
  printf("%s: failed\n", name);
  return 1;
}
