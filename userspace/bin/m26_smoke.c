#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
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

  /* The cross+native toolchain (clang/binutils/make) is genuinely ported. */
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

  /* Directory iteration: opendir/readdir over /bin must list this very binary
   * (/bin/m26-smoke), proving the libc opendir/readdir implementation works
   * end-to-end over SYS_GETDENTS. This is the foundation the GNU Make port
   * relies on (Make stats/globs directory contents), and replaces the old
   * NULL-returning dirent stubs. */
  {
    DIR *d = opendir("/bin");
    if (!d) {
      marker("M26-SMOKE: fail readdir-opendir\n");
      return 1;
    }
    int found_self = 0, n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != 0) {
      n++;
      if (strcmp(e->d_name, "m26-smoke") == 0)
        found_self = 1;
    }
    closedir(d);
    if (n > 0 && found_self) {
      marker("M26-SMOKE: ok readdir\n");
    } else {
      marker("M26-SMOKE: fail readdir\n");
      return 1;
    }
  }

  marker("M26-SMOKE: done\n");
  return 0;
}
