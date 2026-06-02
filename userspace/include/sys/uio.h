#ifndef B1NIX_U_SYS_UIO_H
#define B1NIX_U_SYS_UIO_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct iovec {
  void *iov_base;
  size_t iov_len;
};

#ifndef UIO_MAXIOV
#define UIO_MAXIOV 1024
#endif
#ifndef IOV_MAX
#define IOV_MAX UIO_MAXIOV
#endif

ssize_t readv(int fd, const struct iovec *iov, int iovcnt);
ssize_t writev(int fd, const struct iovec *iov, int iovcnt);

#ifdef __cplusplus
}
#endif

#endif
