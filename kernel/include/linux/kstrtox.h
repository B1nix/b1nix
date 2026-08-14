/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_KSTRTOX_H
#define LKPI_LINUX_KSTRTOX_H
#include <linux/types.h>
/* String to number. simple_strtol is the old form that stops at the first
 * non-digit and never reports failure; imported mode-parsing code uses it and
 * checks the end pointer itself. */
long simple_strtol(const char *cp, char **endp, unsigned int base);
unsigned long simple_strtoul(const char *cp, char **endp, unsigned int base);
int kstrtoint(const char *s, unsigned int base, int *res);
int kstrtouint(const char *s, unsigned int base, unsigned int *res);

/* The wider parsers. Same contract as the ones already here: 0 on success, a
 * negative errno on a malformed string or overflow, and the output untouched on
 * failure — callers rely on that when they parse into a live setting. */
int kstrtoull(const char *s, unsigned int base, unsigned long long *res);
int kstrtoll(const char *s, unsigned int base, long long *res);
int kstrtou32(const char *s, unsigned int base, u32 *res);
int kstrtos32(const char *s, unsigned int base, s32 *res);
int kstrtou16(const char *s, unsigned int base, u16 *res);
int kstrtou8(const char *s, unsigned int base, u8 *res);
int kstrtobool(const char *s, bool *res);


/* Parse a bool straight out of a userspace write. The copy is bounded by the
 * local buffer and the string is terminated here, because the caller's buffer
 * is not NUL-terminated — reading past it is the bug this exists to avoid. */
int kstrtobool_from_user(const char __user *s, size_t count, bool *res);

#endif
