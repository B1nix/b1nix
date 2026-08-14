/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_UUID_H
#define LKPI_LINUX_UUID_H
#include <linux/string.h>
#include <linux/types.h>

#define UUID_SIZE 16
typedef struct { __u8 b[UUID_SIZE]; } uuid_t;
typedef struct { __u8 b[UUID_SIZE]; } guid_t;

static inline bool uuid_equal(const uuid_t *a, const uuid_t *b)
{ return memcmp(a, b, UUID_SIZE) == 0; }
static inline void uuid_copy(uuid_t *dst, const uuid_t *src)
{ memcpy(dst, src, UUID_SIZE); }
static inline bool guid_equal(const guid_t *a, const guid_t *b)
{ return memcmp(a, b, UUID_SIZE) == 0; }

/* Room for the canonical 8-4-4-4-12 text form, without the terminator — the
 * length callers size a buffer with. */
#define UUID_STRING_LEN 36


/* Is this string a well-formed UUID? Format check only — 36 characters,
 * hyphens in the four canonical places, hex elsewhere. */
bool uuid_is_valid(const char *uuid);

#endif
