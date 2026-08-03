/* b1nix diskbench — userspace block device throughput benchmark (Ring 3). */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv) {
  const char *devpath = "/dev/sata0";
  if (argc > 1) devpath = argv[1];

  int fd = open(devpath, O_RDONLY);
  if (fd < 0) {
    printf("DISKBENCH: failed to open %s\n", devpath);
    return 1;
  }

  printf("DISKBENCH: dev=%s reading in userspace (Ring 3)\n", devpath);
  static char buf[32 * 1024];
  struct timespec start, end;
  clock_gettime(CLOCK_MONOTONIC, &start);

  long total_bytes = 0;
  for (int i = 0; i < 512; i++) {
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n <= 0) break;
    total_bytes += n;
  }
  clock_gettime(CLOCK_MONOTONIC, &end);
  close(fd);

  double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
  double kb_s = (total_bytes / 1024.0) / (elapsed > 0 ? elapsed : 0.001);

  printf("DISKBENCH: read %ld KB in %.3f s (%.1f KB/s)\n", total_bytes / 1024, elapsed, kb_s);
  printf("DISKBENCH: done\n");
  return 0;
}
