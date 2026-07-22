#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <errno.h>

int main(int argc, char **argv) {
  const char *name = 0, *value = "", *xname = 0, *file = 0;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
      name = argv[++i];
    else if (strcmp(argv[i], "-v") == 0 && i + 1 < argc)
      value = argv[++i];
    else if (strcmp(argv[i], "-x") == 0 && i + 1 < argc)
      xname = argv[++i];
    else if (argv[i][0] != '-')
      file = argv[i];
  }
  if (!file) {
    fprintf(stderr, "setfattr: missing file operand\n");
    return 1;
  }
  int rc;
  if (xname) {
    rc = removexattr(file, xname);
  } else if (name) {
    rc = setxattr(file, name, value, strlen(value), 0);
  } else {
    fprintf(stderr, "setfattr: need -n <name> or -x <name>\n");
    return 1;
  }
  if (rc < 0) {
    fprintf(stderr, "setfattr: %s: %s\n", file, strerror(errno));
    return 1;
  }
  return 0;
}
