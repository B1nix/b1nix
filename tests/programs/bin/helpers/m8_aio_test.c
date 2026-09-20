#include <b1nix/aio.h>
#include <stdint.h>
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <syscall.h>
#include <unistd.h>

#include <sys/syscall.h>

/* musl's syscall() already returns -1 with errno set — don't overwrite errno
 * with -rc (that clobbered every real error to EPERM in the FAIL output). */
static long ksys(long n, long a0, long a1, long a2, long a3, long a4) {
  return syscall(n, a0, a1, a2, a3, a4);
}

int main(void) {
  printf("M8-AIO-SMOKE: start\n");

  uint64_t ctx = 0;
  if (ksys(SYS_io_setup, 16, (long)&ctx, 0, 0, 0) < 0) {
    printf("M8-AIO-SMOKE: FAIL io_setup errno=%d\n", errno);
    return 1;
  }

  int fd = open("/tmp/aio_test.txt", O_CREAT | O_RDWR | O_TRUNC, 0666);
  if (fd < 0) {
    printf("M8-AIO-SMOKE: FAIL open errno=%d\n", errno);
    return 1;
  }

  const char *msg = "b1nix-aio";
  struct b1nix_aio_sqe w = {0};
  w.user_data = 0x1111;
  w.fd = fd;
  w.opcode = B1NIX_AIO_OP_WRITE;
  w.offset = 0;
  w.addr = (uint64_t)(uintptr_t)msg;
  w.len = (uint32_t)strlen(msg);

  long sub = ksys(SYS_io_submit, (long)ctx, 1, (long)&w, 0, 0);
  if (sub != 1) {
    printf("M8-AIO-SMOKE: FAIL io_submit(write) rc=%ld errno=%d\n", sub,
           errno);
    close(fd);
    return 1;
  }

  struct b1nix_aio_cqe cqe = {0};
  long got = ksys(SYS_io_getevents, (long)ctx, 1, 1, (long)&cqe, 200);
  if (got != 1 || cqe.res != (ssize_t)w.len) {
    printf("M8-AIO-SMOKE: FAIL io_getevents(write) got=%ld res=%ld\n", got,
           (long)cqe.res);
    close(fd);
    return 1;
  }
  printf("M8-AIO-SMOKE: ok write\n");

  char in[32] = {0};
  struct b1nix_aio_sqe r = {0};
  r.user_data = 0x2222;
  r.fd = fd;
  r.opcode = B1NIX_AIO_OP_READ;
  r.offset = 0;
  r.addr = (uint64_t)(uintptr_t)in;
  r.len = (uint32_t)strlen(msg);

  sub = ksys(SYS_io_submit, (long)ctx, 1, (long)&r, 0, 0);
  if (sub != 1) {
    printf("M8-AIO-SMOKE: FAIL io_submit(read) rc=%ld errno=%d\n", sub,
           errno);
    close(fd);
    return 1;
  }

  memset(&cqe, 0, sizeof(cqe));
  got = ksys(SYS_io_getevents, (long)ctx, 1, 1, (long)&cqe, 200);
  if (got != 1 || cqe.res != (ssize_t)r.len) {
    printf("M8-AIO-SMOKE: FAIL io_getevents(read) got=%ld res=%ld\n", got,
           (long)cqe.res);
    close(fd);
    return 1;
  }
  if (memcmp(in, msg, w.len) != 0) {
    printf("M8-AIO-SMOKE: FAIL payload mismatch\n");
    close(fd);
    return 1;
  }

  printf("M8-AIO-SMOKE: ok read\n");
  printf("M8-AIO-SMOKE: done\n");
  close(fd);
  return 0;
}
