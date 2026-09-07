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

/*
 * UUIDs, in both byte orders.
 *
 * A `guid_t` is the little-endian (Microsoft) layout and a `uuid_t` the
 * big-endian (RFC 4122) one. They are different types on purpose: btrfs stores
 * both — the filesystem UUID is a uuid_t, the root item's is a guid_t — and
 * mixing them writes the right sixteen bytes in the wrong order, which every
 * other tool then reads as a different filesystem.
 */
/* The types are defined at the top of this header; only the operations the
 * filesystems need are added here. */
void generate_random_uuid(unsigned char uuid[16]);
void generate_random_guid(unsigned char guid[16]);
void guid_gen(guid_t *u);
void uuid_gen(uuid_t *u);
/* Copy out in the type's own byte order. */
void export_guid(__u8 *dst, const guid_t *src);
void export_uuid(__u8 *dst, const uuid_t *src);
void import_guid(guid_t *dst, const __u8 *src);
void import_uuid(uuid_t *dst, const __u8 *src);
bool uuid_is_null(const uuid_t *uuid);
bool guid_equal(const guid_t *u1, const guid_t *u2);
bool uuid_equal(const uuid_t *u1, const uuid_t *u2);
int uuid_parse(const char *uuid, uuid_t *u);

/* The all-zero UUID, which btrfs writes where a root has no parent. */
extern const guid_t guid_null;
extern const uuid_t uuid_null;

#endif
