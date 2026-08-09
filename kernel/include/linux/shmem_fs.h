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
#endif
