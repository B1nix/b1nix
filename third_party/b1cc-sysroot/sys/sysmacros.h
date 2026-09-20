#ifndef _SYS_SYSMACROS_H
#define _SYS_SYSMACROS_H 1

#include <sys/types.h>

#define major(dev) ((unsigned int)((((dev) >> 32) & 0xfffff000) | (((dev) >> 8) & 0x00000fff)))
#define minor(dev) ((unsigned int)((((dev) >> 12) & 0xffffff00) | (((dev) & 0x000000ff))))
#define makedev(major, minor) (\
  (((dev_t)((major) & 0x00000fff) << 8) | \
   ((dev_t)((major) & 0xfffff000) << 32) | \
   ((dev_t)((minor) & 0x000000ff)) | \
   ((dev_t)((minor) & 0xffffff00) << 12)))

#endif /* _SYS_SYSMACROS_H */
