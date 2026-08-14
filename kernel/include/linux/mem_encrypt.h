/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_MEM_ENCRYPT_H
#define LKPI_LINUX_MEM_ENCRYPT_H
#include <linux/types.h>
/* AMD SME/SEV memory encryption. b1nix does not enable it, so memory is not
 * encrypted and a device sees the same bytes the CPU wrote — which is exactly
 * what this reports. */
static inline bool mem_encrypt_active(void) { return false; }
static inline bool cc_platform_has(int attr) { (void)attr; return false; }
#endif
