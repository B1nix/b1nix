#include <aio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <syscall.h>
#include <unistd.h>

static long ksys(long n, long a0, long a1, long a2, long a3, long a4) {
  long rc = syscall(n, a0, a1, a2, a3, a4);
  if (rc < 0)
    errno = (int)-rc;
  return rc;
}

int main(void) {
  printf("M8-AIO-SMOKE: start\n");

  u64 ctx = 0;
  if (ksys(SYS_IO_SETUP, 16, (long)&ctx, 0, 0, 0) < 0) {
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
  w.addr = (u64)(usize)msg;
  w.len = (u32)strlen(msg);

  long sub = ksys(SYS_IO_SUBMIT, (long)ctx, 1, (long)&w, 0, 0);
  if (sub != 1) {
    printf("M8-AIO-SMOKE: FAIL io_submit(write) rc=%ld errno=%d\n", sub,
           errno);
    close(fd);
    return 1;
  }

  struct b1nix_aio_cqe cqe = {0};
  long got = ksys(SYS_IO_GETEVENTS, (long)ctx, 1, 1, (long)&cqe, 200);
  if (got != 1 || cqe.res != (isize)w.len) {
    printf("M8-AIO-SMOKE: FAIL io_getevents(write) got=%ld res=%ld\n", got,
           (long)cqe.res);
    close(fd);
    return 1;
  }
  printf("M8-AIO-SMOKE: ok write\n");

  char rbuf[32];
  memset(rbuf, 0, sizeof(rbuf));
  struct b1nix_aio_sqe r = {0};
  r.user_data = 0x2222;
  r.fd = fd;
  r.opcode = B1NIX_AIO_OP_READ;
  r.offset = 0;
  r.addr = (u64)(usize)rbuf;
  r.len = (u32)w.len;

  sub = ksys(SYS_IO_SUBMIT, (long)ctx, 1, (long)&r, 0, 0);
  if (sub != 1) {
    printf("M8-AIO-SMOKE: FAIL io_submit(read) rc=%ld errno=%d\n", sub,
           errno);
    close(fd);
    return 1;
  }

  memset(&cqe, 0, sizeof(cqe));
  got = ksys(SYS_IO_GETEVENTS, (long)ctx, 1, 1, (long)&cqe, 200);
  if (got != 1 || cqe.res != (isize)r.len) {
    printf("M8-AIO-SMOKE: FAIL io_getevents(read) got=%ld res=%ld\n", got,
           (long)cqe.res);
    close(fd);
    return 1;
  }
  if (memcmp(rbuf, msg, w.len) != 0) {
    printf("M8-AIO-SMOKE: FAIL payload mismatch\n");
    close(fd);
    return 1;
  }

  printf("M8-AIO-SMOKE: ok read\n");
  printf("M8-AIO-SMOKE: done\n");
  close(fd);
  return 0;
}
