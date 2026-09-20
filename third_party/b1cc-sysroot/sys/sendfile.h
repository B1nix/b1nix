#ifndef B1NIX_U_SYS_SENDFILE_H
#define B1NIX_U_SYS_SENDFILE_H

#include <sys/types.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Zero-copy file transfer. b1nix has no sendfile syscall, so this is a real
 * read()/write() copy loop (correct, just not zero-copy). */
/* sendfile(2): copy data between two descriptors. b1nix has no zero-copy
 * sendfile syscall, so this is a libc read()/write() emulation (correct, just
 * not zero-copy). If `offset` is non-NULL, reading starts at *offset in `in_fd`
 * (via pread) and *offset is advanced; the in_fd file offset is left unchanged.
 * If `offset` is NULL, the current in_fd/out_fd offsets are used and advanced.
 * Added for the Chromium port (M60-62). */
ssize_t sendfile(int out_fd, int in_fd, off_t *offset, size_t count);

#ifdef __cplusplus
}
#endif

#endif /* B1NIX_U_SYS_SENDFILE_H */
