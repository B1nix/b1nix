#include <stdio.h>
#include <string.h>
#include <sys/reboot.h>
#include <unistd.h>

int main(int argc, char **argv) {
  const char *name = "halt";
  if (argc > 0 && argv[0]) {
    const char *slash = strrchr(argv[0], '/');
    name = slash ? slash + 1 : argv[0];
  }

  int cmd;
  if (strcmp(name, "reboot") == 0)
    cmd = RB_AUTOBOOT;
  else if (strcmp(name, "poweroff") == 0 || strcmp(name, "shutdown") == 0)
    cmd = RB_POWER_OFF;
  else
    cmd = RB_HALT_SYSTEM;

  reboot(cmd);
  printf("%s: failed\n", name);
  return 1;
}
