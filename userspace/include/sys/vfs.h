#ifndef B1NIX_U_SYS_VFS_H
#define B1NIX_U_SYS_VFS_H

/* <sys/vfs.h>: the glibc spelling of <sys/statfs.h>. b1nix's `struct statfs`
 * and statfs()/fstatfs() prototypes live in <unistd.h>; this header exists so
 * code that includes the conventional <sys/vfs.h> (e.g. Chromium base) resolves
 * them. Added for the Chromium port (M60-62). */
#include <sys/statfs.h>

#endif /* B1NIX_U_SYS_VFS_H */
