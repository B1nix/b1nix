/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_DAX_H
#define LKPI_LINUX_DAX_H

#include <linux/types.h>
#include <linux/errno.h>
/* pfn_t, which the fault interfaces below pass by value. */
#include <linux/pfn_t.h>

/*
 * Direct access: mapping persistent memory into a process without a page cache
 * copy. b1nix has no pmem device and no DAX.
 *
 * `IS_DAX` is therefore always false, which is what keeps the rest of this
 * header unreachable — every DAX path in ext4 is behind that test. The
 * functions still have to exist, and each returns the error rather than a
 * plausible success: a `dax_iomap_rw` that returned a byte count would report
 * data transferred that never was.
 */

struct dax_device;
struct block_device;
struct inode;
struct iov_iter;
struct iomap_ops;
struct vm_fault;

#define DAX_INODE  0
#define DAX_NEVER  1
#define DAX_ALWAYS 2

/* Without DAX only a mapping that does not ask for MAP_SYNC is supported. */
static inline bool daxdev_mapping_supported(unsigned long vm_flags,
                                            const struct inode *inode,
                                            struct dax_device *dax_dev)
{ (void)inode; (void)dax_dev; return !(vm_flags & VM_SYNC); }
static inline bool dax_compatible(struct dax_device *dax_dev, ...)
{ (void)dax_dev; return false; }
static inline struct dax_device *fs_dax_get_by_bdev(struct block_device *bdev,
                                                    u64 *start_off, void *holder,
                                                    const void *ops)
{ (void)bdev; (void)start_off; (void)holder; (void)ops; return NULL; }
static inline void fs_put_dax(struct dax_device *dax_dev, void *holder)
{ (void)dax_dev; (void)holder; }
static inline ssize_t dax_iomap_rw(struct kiocb *iocb, struct iov_iter *iter,
                                   const struct iomap_ops *ops)
{ (void)iocb; (void)iter; (void)ops; return -EOPNOTSUPP; }
static inline int dax_iomap_fault(struct vm_fault *vmf, unsigned int order,
                                  pfn_t *pfnp, int *iomap_errp,
                                  const struct iomap_ops *ops)
{ (void)vmf; (void)order; (void)pfnp; (void)iomap_errp; (void)ops;
  return VM_FAULT_SIGBUS; }
static inline int dax_finish_sync_fault(struct vm_fault *vmf,
                                        unsigned int order, pfn_t pfn)
{ (void)vmf; (void)order; (void)pfn; return VM_FAULT_SIGBUS; }
static inline int dax_writeback_mapping_range(struct address_space *mapping,
                                              struct dax_device *dax_dev,
                                              struct writeback_control *wbc)
{ (void)mapping; (void)dax_dev; (void)wbc; return 0; }
static inline struct page *dax_layout_busy_page(struct address_space *mapping)
{ (void)mapping; return NULL; }
static inline struct page *dax_layout_busy_page_range(struct address_space *m,
                                                      loff_t start, loff_t end)
{ (void)m; (void)start; (void)end; return NULL; }
static inline int dax_zero_range(struct inode *inode, loff_t pos, loff_t len,
                                 bool *did_zero, const struct iomap_ops *ops)
{ (void)inode; (void)pos; (void)len; (void)did_zero; (void)ops;
  return -EOPNOTSUPP; }
static inline int dax_truncate_page(struct inode *inode, loff_t pos,
                                    bool *did_zero, const struct iomap_ops *ops)
{ (void)inode; (void)pos; (void)did_zero; (void)ops; return -EOPNOTSUPP; }

/* No DAX: there is no direct-mapped page to wait out, so a layout break has
 * nothing to do. */
static inline int dax_break_layout(struct inode *inode, loff_t start, loff_t end,
                                   void (cb)(struct inode *))
{ (void)inode; (void)start; (void)end; (void)cb; return 0; }
static inline int dax_break_layout_inode(struct inode *inode,
                                         void (cb)(struct inode *))
{ return dax_break_layout(inode, 0, LLONG_MAX, cb); }
static inline void dax_break_layout_final(struct inode *inode)
{ (void)inode; }

#endif
