#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <syscall.h>

struct b1nix_selfhost_status {
  unsigned int abi_version;
  unsigned int target_ready;
  unsigned int binutils_ready;
  unsigned int make_ready;
  unsigned int can_build_kernel_inside_b1nix;
  char target_triple[32];
  char compiler[32];
  char assembler[32];
  char linker[32];
  char make[32];
};

static void marker(const char *s) {
  write(1, s, strlen(s));
}

int main(void) {
  struct b1nix_selfhost_status status;
  memset(&status, 0, sizeof(status));

  marker("M26-SMOKE: start\n");

  long rc = syscall(SYS_SELFHOST_STATUS, &status);
  if (rc < 0) {
    marker("M26-SMOKE: fail selfhost-status\n");
    return 1;
  }
  marker("M26-SMOKE: ok selfhost-status\n");

  /* The cross+native toolchain (gcc/binutils/make) is genuinely ported. */
  if (status.target_ready && status.binutils_ready && status.make_ready) {
    marker("M26-SMOKE: ok toolchain-ready\n");
  } else {
    marker("M26-SMOKE: fail toolchain-ready\n");
    return 1;
  }

  /* Full in-guest kernel self-build is not yet verified — report the real
   * state instead of faking an "ok". Flip to "ok" only once an in-guest
   * kernel.elf actually builds. */
  if (status.can_build_kernel_inside_b1nix == 1) {
    marker("M26-SMOKE: ok can-build-kernel\n");
  } else {
    marker("M26-SMOKE: pending can-build-kernel\n");
  }

  marker("M26-SMOKE: done\n");
  return 0;
}
