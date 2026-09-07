/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_PARSER_H
#define LKPI_LINUX_PARSER_H

#include <linux/types.h>

/*
 * The old mount-option parser: a table of patterns like "commit=%u", matched
 * against one option at a time, with `%u`/`%d`/`%s`/`%o`/`%x` capturing
 * substrings.
 *
 * ext4 has moved to the fs_context parser and btrfs has not, so both interfaces
 * are needed. A substring_t is a pointer pair into the ORIGINAL string, not a
 * copy — which is why `match_strdup` exists and why the caller must not free
 * the option string while substrings are still in use.
 */

#define MAX_OPT_ARGS 3

/*
 * A typedef rather than a plain struct, because that is how upstream declares
 * it and imported code writes `substring_t args[MAX_OPT_ARGS]` without the
 * struct tag.
 */
typedef struct {
	char *from;
	char *to;
} substring_t;

typedef struct {
	int token;
	const char *pattern;
} match_table_t[];

int match_token(char *s, const match_table_t table, substring_t args[]);
int match_int(substring_t *s, int *result);
int match_uint(substring_t *s, unsigned int *result);
int match_u64(substring_t *s, u64 *result);
int match_octal(substring_t *s, int *result);
int match_hex(substring_t *s, int *result);
bool match_wildcard(const char *pattern, const char *str);
size_t match_strlcpy(char *dest, const substring_t *src, size_t size);
char *match_strdup(const substring_t *s);

#endif
