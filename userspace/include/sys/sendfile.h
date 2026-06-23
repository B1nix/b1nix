#ifndef B1NIX_U_SYS_SENDFILE_H
#define B1NIX_U_SYS_SENDFILE_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Zero-copy file transfer. b1nix has no sendfile syscall, so this is a real
 * read()/write() copy loop (correct, just not zero-copy). */
ssize_t sendfile(int out_fd, int in_fd, off_t *offset, size_t count);

#ifdef __cplusplus
}
#endif

#endif /* B1NIX_U_SYS_SENDFILE_H */
