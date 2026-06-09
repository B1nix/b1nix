#ifndef _SYS_XATTR_H
#define _SYS_XATTR_H 1

#include <stddef.h>
#include <sys/types.h>

/* setxattr flags (Linux ABI). */
#define XATTR_CREATE  0x1 /* fail if the named attribute already exists */
#define XATTR_REPLACE 0x2 /* fail if the named attribute does not exist */

#ifdef __cplusplus
extern "C" {
#endif

int setxattr(const char *path, const char *name, const void *value,
             size_t size, int flags);
int lsetxattr(const char *path, const char *name, const void *value,
              size_t size, int flags);
ssize_t getxattr(const char *path, const char *name, void *value, size_t size);
ssize_t lgetxattr(const char *path, const char *name, void *value, size_t size);
ssize_t listxattr(const char *path, char *list, size_t size);
ssize_t llistxattr(const char *path, char *list, size_t size);
int removexattr(const char *path, const char *name);
int lremovexattr(const char *path, const char *name);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_XATTR_H */
