/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_SHMEM_FS_H
#define LKPI_LINUX_SHMEM_FS_H
#include <linux/fs.h>
#include <lkpi/page.h>
/* Anonymous, swappable file-backed memory — how a GEM object gets its pages on
 * Linux. b1nix's equivalent is lkpi's shmem_alloc_pages, which hands back the
 * page array directly rather than through a file. The file-shaped entry points
 * are declared for the paths that pass one around; the page-shaped ones are
 * what the core actually allocates through. */
struct file *shmem_file_setup(const char *name, loff_t size,
                              unsigned long flags);
/* A page of a shmem-backed object, faulted in if absent. Linux calls the unit a
 * folio; b1nix has no folio layer, so one folio is one page here and the
 * spelling is kept only because imported code uses it. */
struct folio *shmem_read_folio_gfp(struct address_space *mapping,
                                   unsigned long index, gfp_t gfp);

struct page *shmem_read_mapping_page(struct address_space *mapping,
                                     unsigned long index);
struct address_space;

/*
 * One page of a shmem-backed object, faulted in on demand.
 *
 * Declared and not defined. b1nix's shmem equivalent hands back the whole page
 * array at allocation (see <lkpi/page.h>), so there is no mapping to fault a
 * single page out of, and no address_space to index. A driver taking this path
 * fails to link rather than receiving a page that belongs to nothing.
 */
struct page *shmem_read_mapping_page_gfp(struct address_space *mapping,
                                         unsigned long index, gfp_t gfp);
/* shmem_read_mapping_page is already declared above; only the _gfp form was
 * missing. */


/*
 * Punching a hole in a shmem file's page cache.
 *
 * Declared and deliberately not defined, for the same reason as
 * shmem_read_mapping_page_gfp(): b1nix's GEM objects are backed by anonymous
 * pages, not by a shmem inode, so there is no page cache to punch and a stub
 * would report that pages were discarded when they were not.
 */
struct inode;
void shmem_truncate_range(struct inode *inode, loff_t start, loff_t end);


/*
 * Creating a shmem-backed file, optionally on a private mount.
 *
 * Declared and deliberately not defined, for the same reason as
 * shmem_truncate_range() above: b1nix has no tmpfs a driver can allocate an
 * inode from, and GEM objects here are backed by anonymous pages instead. A
 * caller fails to link rather than being handed a file with no pages behind it.
 */
struct vfsmount;
struct file *shmem_file_setup(const char *name, loff_t size, unsigned long flags);
struct file *shmem_file_setup_with_mnt(struct vfsmount *mnt, const char *name,
                                       loff_t size, unsigned long flags);

#endif
