/* SPDX-License-Identifier: MIT */
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
#endif
