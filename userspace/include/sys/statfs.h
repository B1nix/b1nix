#ifndef B1NIX_U_SYS_STATFS_H
#define B1NIX_U_SYS_STATFS_H

/* The b1nix `struct statfs` and the statfs()/fstatfs() prototypes live in
 * <unistd.h>; this header exists so code that includes the conventional
 * <sys/statfs.h> (e.g. upstream BusyBox) resolves them. */
#include <unistd.h>

#endif /* B1NIX_U_SYS_STATFS_H */
