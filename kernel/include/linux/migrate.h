/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_MIGRATE_H
#define LKPI_LINUX_MIGRATE_H

#include <linux/types.h>
#include <linux/errno.h>

/*
 * Page migration: moving a page's contents to a different physical frame while
 * keeping every reference to it valid. Used for compaction and for NUMA
 * balancing, neither of which b1nix does.
 *
 * `filemap_migrate_folio` and friends are what a filesystem installs in its
 * address_space_operations. They are declared and return -EAGAIN, which is the
 * "could not migrate this one" answer the migration core is written to accept —
 * and since nothing here ever calls them, that answer is never actually given.
 */

struct address_space;
struct folio;

enum migrate_mode {
	MIGRATE_ASYNC,
	MIGRATE_SYNC_LIGHT,
	MIGRATE_SYNC,
	MIGRATE_SYNC_NO_COPY,
};

static inline int filemap_migrate_folio(struct address_space *mapping,
                                        struct folio *dst, struct folio *src,
                                        enum migrate_mode mode)
{ (void)mapping; (void)dst; (void)src; (void)mode; return -EAGAIN; }
static inline int migrate_folio(struct address_space *mapping,
                                struct folio *dst, struct folio *src,
                                enum migrate_mode mode)
{ (void)mapping; (void)dst; (void)src; (void)mode; return -EAGAIN; }
static inline int buffer_migrate_folio(struct address_space *mapping,
                                       struct folio *dst, struct folio *src,
                                       enum migrate_mode mode)
{ (void)mapping; (void)dst; (void)src; (void)mode; return -EAGAIN; }
static inline int buffer_migrate_folio_norefs(struct address_space *mapping,
                                              struct folio *dst,
                                              struct folio *src,
                                              enum migrate_mode mode)
{ (void)mapping; (void)dst; (void)src; (void)mode; return -EAGAIN; }
static inline void folio_migrate_copy(struct folio *dst, struct folio *src)
{ (void)dst; (void)src; }

#endif
