/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_KSTRTOX_H
#define LKPI_LINUX_KSTRTOX_H

#include <linux/types.h>

/*
 * String to integer, with the errors reported rather than guessed at.
 *
 * The whole point over simple_strtoul is the return value: 0 on success, and
 * -EINVAL or -ERANGE otherwise, with the parsed value written through the
 * pointer. A mount option parser that cannot tell "0" from "not a number"
 * accepts `commit=banana` as `commit=0`.
 *
 * base 0 means "infer from the prefix" — 0x hex, 0 octal, otherwise decimal.
 */
int kstrtoul(const char *s, unsigned int base, unsigned long *res);
int kstrtol(const char *s, unsigned int base, long *res);
int kstrtoull(const char *s, unsigned int base, unsigned long long *res);
int kstrtoll(const char *s, unsigned int base, long long *res);
int kstrtouint(const char *s, unsigned int base, unsigned int *res);
int kstrtoint(const char *s, unsigned int base, int *res);
int kstrtou64(const char *s, unsigned int base, u64 *res);
int kstrtos64(const char *s, unsigned int base, s64 *res);
int kstrtou32(const char *s, unsigned int base, u32 *res);
int kstrtos32(const char *s, unsigned int base, s32 *res);
int kstrtou16(const char *s, unsigned int base, u16 *res);
int kstrtou8(const char *s, unsigned int base, u8 *res);
int kstrtobool(const char *s, bool *res);

unsigned long simple_strtoul(const char *cp, char **endp, unsigned int base);
long simple_strtol(const char *cp, char **endp, unsigned int base);
unsigned long long simple_strtoull(const char *cp, char **endp,
                                   unsigned int base);

#endif
