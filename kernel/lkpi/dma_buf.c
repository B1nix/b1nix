/*
 * SPDX-License-Identifier: MIT
 *
 * M101 linuxkpi: dma-buf, buffer sharing between drivers.
 *
 * The exporter owns the pages and supplies an ops table; an importer attaches
 * and asks for an sg table mapped for *its* device. The mapping is per
 * attachment rather than a property of the buffer, and that is not a detail:
 * two devices behind different IOMMU domains see the same memory at different
 * device addresses, so one shared table would hand one of them the other's
 * addresses and the transfer would land in the wrong place.
 *
 * A buffer is refcounted through the file that names it, which is what makes
 * "userspace closed the descriptor but the importer still holds it" work.
 */

#include <linux/dma-buf.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/anon_inodes.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/slab.h>

static int dma_buf_file_release(struct inode *inode, struct file *file)
{
	(void)inode;
	struct dma_buf *dmabuf = file ? (struct dma_buf *)file->private_data : 0;
	if (!dmabuf)
		return 0;

	/* Every attachment should have been detached by its importer; a leftover
	 * one means an importer outlived the buffer it borrowed, which is the
	 * caller's bug and is left visible rather than papered over. */
	if (dmabuf->ops && dmabuf->ops->release)
		dmabuf->ops->release(dmabuf);
	lkpi_kfree(dmabuf);
	return 0;
}

static const struct file_operations dma_buf_fops = {
	.release = dma_buf_file_release,
};

struct dma_buf *dma_buf_export(const struct dma_buf_export_info *exp_info)
{
	if (!exp_info || !exp_info->ops || exp_info->size == 0)
		return ERR_PTR(-EINVAL);

	struct dma_buf *dmabuf = (struct dma_buf *)lkpi_kmalloc(
		sizeof(*dmabuf), GFP_KERNEL | __GFP_ZERO);
	if (!dmabuf)
		return ERR_PTR(-ENOMEM);

	dmabuf->size = exp_info->size;
	dmabuf->ops = exp_info->ops;
	dmabuf->priv = exp_info->priv;
	dmabuf->resv = exp_info->resv;
	INIT_LIST_HEAD(&dmabuf->attachments);

	/* The file is what holds the reference userspace can pass around. */
	dmabuf->file = anon_inode_getfile("dmabuf", &dma_buf_fops, dmabuf,
	                                  exp_info->flags);
	if (!dmabuf->file) {
		lkpi_kfree(dmabuf);
		return ERR_PTR(-ENOMEM);
	}
	return dmabuf;
}

int dma_buf_fd(struct dma_buf *dmabuf, int flags)
{
	if (!dmabuf || !dmabuf->file)
		return -EINVAL;

	int fd = get_unused_fd_flags((unsigned int)flags);
	if (fd < 0)
		return fd;
	fd_install((unsigned int)fd, dmabuf->file);
	return fd;
}

struct dma_buf *dma_buf_get(int fd)
{
	struct file *f = fget((unsigned int)fd);
	if (!f)
		return ERR_PTR(-EBADF);
	/* A descriptor that is not a dma-buf must be refused rather than
	 * reinterpreted: private_data would be some other subsystem's object. */
	if (f->f_op != &dma_buf_fops) {
		fput(f);
		return ERR_PTR(-EINVAL);
	}
	return (struct dma_buf *)f->private_data;
}

void dma_buf_put(struct dma_buf *dmabuf)
{
	if (dmabuf && dmabuf->file)
		fput(dmabuf->file);
}

void get_dma_buf(struct dma_buf *dmabuf)
{
	if (dmabuf && dmabuf->file)
		file_clone_open(dmabuf->file);
}

struct dma_buf_attachment *dma_buf_attach(struct dma_buf *dmabuf,
                                          struct device *dev)
{
	if (!dmabuf || !dev)
		return ERR_PTR(-EINVAL);

	struct dma_buf_attachment *attach = (struct dma_buf_attachment *)
		lkpi_kmalloc(sizeof(*attach), GFP_KERNEL | __GFP_ZERO);
	if (!attach)
		return ERR_PTR(-ENOMEM);

	attach->dmabuf = dmabuf;
	attach->dev = dev;

	if (dmabuf->ops && dmabuf->ops->attach) {
		int err = dmabuf->ops->attach(dmabuf, attach);
		if (err) {
			lkpi_kfree(attach);
			return ERR_PTR(err);
		}
	}
	list_add(&attach->node, &dmabuf->attachments);
	return attach;
}

void dma_buf_detach(struct dma_buf *dmabuf, struct dma_buf_attachment *attach)
{
	if (!dmabuf || !attach)
		return;
	/* An attachment still holding a mapping would leave the device addresses
	 * live after the importer is gone, so it is unmapped first. */
	if (attach->sgt)
		dma_buf_unmap_attachment(attach, attach->sgt, DMA_BIDIRECTIONAL);
	list_del(&attach->node);
	if (dmabuf->ops && dmabuf->ops->detach)
		dmabuf->ops->detach(dmabuf, attach);
	lkpi_kfree(attach);
}

struct sg_table *dma_buf_map_attachment(struct dma_buf_attachment *attach,
                                        enum dma_data_direction direction)
{
	if (!attach || !attach->dmabuf || !attach->dmabuf->ops ||
	    !attach->dmabuf->ops->map_dma_buf)
		return ERR_PTR(-EINVAL);

	struct sg_table *sgt = attach->dmabuf->ops->map_dma_buf(attach, direction);
	if (IS_ERR(sgt))
		return sgt;
	attach->sgt = sgt;
	return sgt;
}

void dma_buf_unmap_attachment(struct dma_buf_attachment *attach,
                              struct sg_table *sgt,
                              enum dma_data_direction direction)
{
	if (!attach || !attach->dmabuf || !attach->dmabuf->ops ||
	    !attach->dmabuf->ops->unmap_dma_buf)
		return;
	attach->dmabuf->ops->unmap_dma_buf(attach, sgt, direction);
	if (attach->sgt == sgt)
		attach->sgt = 0;
}

int dma_buf_vmap(struct dma_buf *dmabuf, struct iosys_map *map)
{
	if (!dmabuf || !map)
		return -EINVAL;
	if (!dmabuf->ops || !dmabuf->ops->vmap)
		return -ENOTSUPP;
	return dmabuf->ops->vmap(dmabuf, map);
}

void dma_buf_vunmap(struct dma_buf *dmabuf, struct iosys_map *map)
{
	if (dmabuf && dmabuf->ops && dmabuf->ops->vunmap)
		dmabuf->ops->vunmap(dmabuf, map);
}

int dma_buf_begin_cpu_access(struct dma_buf *dmabuf, enum dma_data_direction dir)
{
	(void)dir;
	if (!dmabuf)
		return -EINVAL;
	/* No ops entry means the exporter needs no maintenance — b1nix is
	 * cache-coherent for DMA — so there is nothing to do and nothing failed. */
	return 0;
}

int dma_buf_end_cpu_access(struct dma_buf *dmabuf, enum dma_data_direction dir)
{
	(void)dir;
	return dmabuf ? 0 : -EINVAL;
}
