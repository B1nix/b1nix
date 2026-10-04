/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_ASM_BYTEORDER_H
#define LKPI_ASM_BYTEORDER_H
#include <linux/byteorder.h>
/* Both ports are little-endian. The uapi headers that lay out on-disk or
 * on-wire bitfields (cdrom.h's TOC entries) choose their field order by this. */
#ifndef __LITTLE_ENDIAN_BITFIELD
#define __LITTLE_ENDIAN_BITFIELD
#endif
#endif
