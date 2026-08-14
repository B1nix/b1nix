/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_KDEV_T_H
#define LKPI_LINUX_KDEV_T_H
#include <linux/types.h>
/* Device numbers, encoded the way userspace already reads them out of stat(2),
 * so the split is reproduced rather than chosen. */
typedef u32 dev_t;
#define MINORBITS 20
#define MINORMASK ((1U << MINORBITS) - 1)
#define MAJOR(dev) ((unsigned int)((dev) >> MINORBITS))
#define MINOR(dev) ((unsigned int)((dev) & MINORMASK))
/* The 16-bit device number an old interface reports. b1nix's numbers fit, so
 * this never truncates; the name is kept because callers expect the encoding
 * rather than the raw value. */
static inline u16 old_encode_dev(dev_t dev)
{ return (u16)(((MAJOR(dev) & 0xff) << 8) | (MINOR(dev) & 0xff)); }

#define MKDEV(ma, mi) (((ma) << MINORBITS) | (mi))
#endif
