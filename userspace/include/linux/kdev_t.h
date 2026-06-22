#ifndef _LINUX_KDEV_T_H
#define _LINUX_KDEV_T_H
/* <linux/kdev_t.h>: uppercase MAJOR/MINOR/MKDEV. b1nix provides the lowercase
 * forms in <sys/sysmacros.h>; map the uppercase macros onto them. Added for the
 * Chromium port (M60-62). */
#include <sys/sysmacros.h>
#define MAJOR(dev)        major(dev)
#define MINOR(dev)        minor(dev)
#define MKDEV(ma, mi)     makedev((ma), (mi))
#endif /* _LINUX_KDEV_T_H */
