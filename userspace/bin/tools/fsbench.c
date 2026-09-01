#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CHUNK_SIZE (64 * 1024)
#define NUM_CHUNKS 64  /* 4 MB */

static char g_buf[CHUNK_SIZE];

static void bench_target(const char *name, const char *filepath) {
  struct timespec t0, t1;
  long total_bytes = (long)CHUNK_SIZE * NUM_CHUNKS;

  printf("FSBENCH: Testing %s...\n", name);
  fflush(stdout);

  /* 1. Sequential Write */
  int fd = open(filepath, O_CREAT | O_RDWR | O_TRUNC, 0644);
  if (fd < 0) {
    printf("FSBENCH [%s]: open write failed\n", name);
    fflush(stdout);
    return;
  }

  clock_gettime(CLOCK_MONOTONIC, &t0);
  for (int i = 0; i < NUM_CHUNKS; i++) {
    if (write(fd, g_buf, CHUNK_SIZE) != CHUNK_SIZE) break;
  }
  fsync(fd);
  clock_gettime(CLOCK_MONOTONIC, &t1);
  close(fd);

  double write_sec = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
  double write_mbs = (total_bytes / (1024.0 * 1024.0)) / (write_sec > 0 ? write_sec : 0.0001);

  /* 2. Sequential Read */
  fd = open(filepath, O_RDONLY);
  if (fd < 0) {
    printf("FSBENCH [%s]: open read failed\n", name);
    fflush(stdout);
    return;
  }

  clock_gettime(CLOCK_MONOTONIC, &t0);
  long read_bytes = 0;
  for (int i = 0; i < NUM_CHUNKS; i++) {
    ssize_t n = read(fd, g_buf, CHUNK_SIZE);
    if (n <= 0) break;
    read_bytes += n;
  }
  clock_gettime(CLOCK_MONOTONIC, &t1);
  close(fd);

  double read_sec = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
  double read_mbs = (read_bytes / (1024.0 * 1024.0)) / (read_sec > 0 ? read_sec : 0.0001);

  printf("FSBENCH [%s]: Write = %.2f MB/s (%.3f s) | Read = %.2f MB/s (%.3f s) [Total: %ld MB]\n",
         name, write_mbs, write_sec, read_mbs, read_sec, total_bytes / (1024 * 1024));
  fflush(stdout);

  unlink(filepath);
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  memset(g_buf, 0xAB, sizeof(g_buf));

  printf("=== B1NIX Filesystem & Storage Benchmark (4 MB workload) ===\n");
  fflush(stdout);

  /* 1. Tmpfs / in-memory VFS */
  mkdir("/tmp/bench", 0755);
  bench_target("Tmpfs (In-Memory Fastpath)", "/tmp/bench/test_tmpfs.bin");

  /* 2. VirtIO-9P Host Share */
  mkdir("/mnt/9p", 0755);
  if (mount("hostshare", "/mnt/9p", "9p", 0, NULL) == 0) {
    bench_target("VirtIO-9P (Host Direct)", "/mnt/9p/test_9p.bin");
  } else {
    printf("FSBENCH [VirtIO-9P]: mount failed\n");
    fflush(stdout);
  }

  /* 3. Ext4 on VirtIO-Blk / Rootfs */
  bench_target("Ext4 (VirtIO-Blk Disk)", "/home/test_ext4.bin");

  printf("=== Benchmark Done ===\n");
  fflush(stdout);
  return 0;
}
