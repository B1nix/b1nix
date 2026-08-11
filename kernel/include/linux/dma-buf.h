/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_DMA_BUF_H
#define LKPI_LINUX_DMA_BUF_H

#include <linux/dma-mapping.h>
#include <linux/dma-resv.h>
#include <linux/file.h>
#include <linux/iosys-map.h>
#include <linux/scatterlist.h>
#include <linux/types.h>

/*
 * Buffer sharing between drivers.
 *
 * An exporter owns the pages; an importer attaches and asks for an sg table
 * mapped for *its* device, which is why the mapping is per-attachment rather
 * than a property of the buffer. Two devices behind different IOMMU domains see
 * the same memory at different device addresses, and a shim that returned one
 * shared table would hand one of them the other's addresses.
 *
 * The descriptor-passing half (dma_buf_fd / dma_buf_get) needs anon_inodes and
 * is declared here, wired when that lands. Nothing in the core exports a buffer
 * to userspace before then.
 */

struct dma_buf;
struct dma_buf_attachment;
struct device;

struct dma_buf_ops {
	bool cache_sgt_mapping;
	int (*attach)(struct dma_buf *, struct dma_buf_attachment *);
	void (*detach)(struct dma_buf *, struct dma_buf_attachment *);
	struct sg_table *(*map_dma_buf)(struct dma_buf_attachment *,
	                                enum dma_data_direction dir);
	void (*unmap_dma_buf)(struct dma_buf_attachment *, struct sg_table *,
	                      enum dma_data_direction dir);
	void (*release)(struct dma_buf *);
	int (*mmap)(struct dma_buf *, struct vm_area_struct *);
	int (*vmap)(struct dma_buf *, struct iosys_map *);
	void (*vunmap)(struct dma_buf *, struct iosys_map *);
	/* Bracketing CPU access to a buffer the GPU also touches. The exporter
	 * uses them to flush or invalidate caches around the window — which is why
	 * an importer that skips them reads stale pixels on a non-coherent path,
	 * and why they belong in the ops table rather than being optional. */
	int (*begin_cpu_access)(struct dma_buf *, enum dma_data_direction);
	int (*end_cpu_access)(struct dma_buf *, enum dma_data_direction);
};

struct dma_buf {
	usize size;
	const struct dma_buf_ops *ops;
	void *priv;
	struct dma_resv *resv;
	struct file *file;
	struct list_head attachments;
};

struct dma_buf_attachment {
	struct dma_buf *dmabuf;
	struct device *dev;
	struct list_head node;
	void *priv;
	struct sg_table *sgt;
};

struct dma_buf_export_info {
	const char *exp_name;
	struct module *owner;
	const struct dma_buf_ops *ops;
	usize size;
	int flags;
	struct dma_resv *resv;
	void *priv;
};

#define DEFINE_DMA_BUF_EXPORT_INFO(name) \
	struct dma_buf_export_info name = { .exp_name = __func__ }

struct dma_buf *dma_buf_export(const struct dma_buf_export_info *exp_info);
int dma_buf_fd(struct dma_buf *dmabuf, int flags);
struct dma_buf *dma_buf_get(int fd);
void dma_buf_put(struct dma_buf *dmabuf);
/* Take a reference on a buffer already held. */
void get_dma_buf(struct dma_buf *dmabuf);

struct dma_buf_attachment *dma_buf_attach(struct dma_buf *dmabuf,
                                          struct device *dev);
void dma_buf_detach(struct dma_buf *dmabuf, struct dma_buf_attachment *attach);
/* The "unlocked" spellings, which take the reservation themselves rather than
 * requiring the caller to hold it. Both exist upstream and imported code picks
 * one deliberately; they forward to the same implementation here because
 * nothing yet calls the locked form with a lock already held. */
#define dma_buf_map_attachment_unlocked(a, d)      dma_buf_map_attachment(a, d)
#define dma_buf_unmap_attachment_unlocked(a, s, d) dma_buf_unmap_attachment(a, s, d)
#define dma_buf_vmap_unlocked(b, m)                dma_buf_vmap(b, m)
#define dma_buf_vunmap_unlocked(b, m)              dma_buf_vunmap(b, m)

struct sg_table *dma_buf_map_attachment(struct dma_buf_attachment *attach,
                                        enum dma_data_direction direction);
void dma_buf_unmap_attachment(struct dma_buf_attachment *attach,
                              struct sg_table *sgt,
                              enum dma_data_direction direction);
/*
 * Bracket CPU access to a shared buffer, so the exporter can do whatever cache
 * maintenance its device needs. Forwarded to the exporter's ops when it has
 * them; b1nix is cache-coherent for DMA, so an exporter with none needs no
 * maintenance and success is the correct answer rather than a silent skip.
 */
int dma_buf_begin_cpu_access(struct dma_buf *dmabuf,
                             enum dma_data_direction dir);
int dma_buf_end_cpu_access(struct dma_buf *dmabuf,
                           enum dma_data_direction dir);

int dma_buf_vmap(struct dma_buf *dmabuf, struct iosys_map *map);
void dma_buf_vunmap(struct dma_buf *dmabuf, struct iosys_map *map);

#endif
