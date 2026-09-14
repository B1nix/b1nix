/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_UNICODE_H
#define LKPI_LINUX_UNICODE_H

#include <linux/types.h>
#include <linux/errno.h>

/*
 * Unicode case folding and normalisation, for ext4's case-insensitive
 * directories. b1nix does not carry the 500 KiB of Unicode tables that needs.
 *
 * `utf8_load` therefore fails, and that failure is the mechanism: ext4 calls it
 * at mount time when the superblock advertises casefold, and refuses the mount
 * when it fails. A filesystem with case-insensitive directories is not mounted
 * case-sensitively — it is not mounted, which is correct: mounting it the wrong
 * way would let two names collide that the filesystem believes are one.
 */

struct unicode_map;

struct qstr;

static inline struct unicode_map *utf8_load(unsigned int version)
{ (void)version; return ERR_PTR(-EINVAL); }
static inline void utf8_unload(struct unicode_map *um) { (void)um; }
static inline int utf8_validate(const struct unicode_map *um,
                                const struct qstr *str)
{ (void)um; (void)str; return -EINVAL; }
static inline int utf8_strncmp(const struct unicode_map *um,
                               const struct qstr *s1, const struct qstr *s2)
{ (void)um; (void)s1; (void)s2; return -EINVAL; }
static inline int utf8_strncasecmp(const struct unicode_map *um,
                                   const struct qstr *s1, const struct qstr *s2)
{ (void)um; (void)s1; (void)s2; return -EINVAL; }
static inline int utf8_strncasecmp_folded(const struct unicode_map *um,
                                          const struct qstr *cf,
                                          const struct qstr *s1)
{ (void)um; (void)cf; (void)s1; return -EINVAL; }
static inline int utf8_casefold(const struct unicode_map *um,
                                const struct qstr *str, unsigned char *dest,
                                size_t dlen)
{ (void)um; (void)str; (void)dest; (void)dlen; return -EINVAL; }
static inline int utf8_casefold_hash(const struct unicode_map *um,
                                     const void *salt, struct qstr *str)
{ (void)um; (void)salt; (void)str; return -EINVAL; }
static inline int utf8_normalize(const struct unicode_map *um,
                                 const struct qstr *str, unsigned char *dest,
                                 size_t dlen)
{ (void)um; (void)str; (void)dest; (void)dlen; return -EINVAL; }

#define UNICODE_AGE(MAJ, MIN, REV) \
	(((unsigned int)(MAJ) << 16) | ((unsigned int)(MIN) << 8) | (unsigned int)(REV))
#define unicode_major(v) (((v) >> 16) & 0xff)
#define unicode_minor(v) (((v) >> 8) & 0xff)
#define unicode_rev(v)   ((v) & 0xff)

#endif
