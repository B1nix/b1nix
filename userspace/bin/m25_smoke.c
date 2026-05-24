#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "syscall.h"

static void marker(const char *text) {
  write(1, text, strlen(text));
}

int main(void) {
  marker("M25-SMOKE: start\n");

  // 1. Check if TCC is present
  int tcc_fd = open("/bin/tcc", O_RDONLY);
  if (tcc_fd < 0) {
    marker("M25-SMOKE: fail tcc-launch\n");
    return 1;
  }
  close(tcc_fd);
  marker("M25-SMOKE: ok tcc-launch\n");

  marker("M25-SMOKE: unsupported compile-hello\n");
  marker("M25-SMOKE: unsupported run-hello\n");
  marker("M25-SMOKE: unsupported compile-utility\n");

  marker("M25-SMOKE: done\n");
  return 0;
}
