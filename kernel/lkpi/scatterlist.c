/*
 * SPDX-License-Identifier: MIT
 *
 * M99 linuxkpi: scatterlists. See kernel/include/lkpi/scatterlist.h.
 */

#include <b1nix/errno.h>
#include <b1nix/mm.h>
#include <lkpi/scatterlist.h>
#include <string.h>

int sg_alloc_table(struct sg_table *sgt, u32 max_ents)
{
	if (!sgt || max_ents == 0)
		return -EINVAL;
	memset(sgt, 0, sizeof(*sgt));
	sgt->sgl = kzalloc((usize)max_ents * sizeof(struct scatterlist));
	if (!sgt->sgl)
		return -ENOMEM;
	sgt->orig_nents = max_ents;
	return 0;
}

void sg_free_table(struct sg_table *sgt)
{
	if (!sgt)
		return;
	if (sgt->sgl)
		kfree(sgt->sgl);
	memset(sgt, 0, sizeof(*sgt));
}

int sg_append(struct sg_table *sgt, u64 phys, u32 offset, u32 length)
{
	if (!sgt || !sgt->sgl)
		return -EINVAL;
	if (length == 0)
		return -EINVAL;

	/* Coalesce with the previous run when this one starts exactly where that
	 * one ended. A contiguous allocation then costs a single entry, which is
	 * what makes sg_is_contiguous() meaningful. */
	if (sgt->nents) {
		struct scatterlist *prev = &sgt->sgl[sgt->nents - 1];
		if (prev->phys + prev->offset + prev->length == phys + offset &&
		    (u64)prev->length + length <= 0xFFFFFFFFULL) {
			prev->length += length;
			sgt->total_bytes += length;
			return 0;
		}
	}

	if (sgt->nents >= sgt->orig_nents)
		return -ENOSPC;

	struct scatterlist *sg = &sgt->sgl[sgt->nents++];
	sg->phys = phys;
	sg->offset = offset;
	sg->length = length;
	sgt->total_bytes += length;
	return 0;
}

int sg_alloc_table_from_pages(struct sg_table *sgt, const u64 *frames,
                              u32 nframes)
{
	if (!sgt || !frames || nframes == 0)
		return -EINVAL;
	int rc = sg_alloc_table(sgt, nframes);
	if (rc < 0)
		return rc;
	for (u32 i = 0; i < nframes; i++) {
		rc = sg_append(sgt, frames[i], 0, (u32)PAGE_SIZE);
		if (rc < 0) {
			sg_free_table(sgt);
			return rc;
		}
	}
	return 0;
}

u64 sg_phys_at(const struct sg_table *sgt, u64 offset)
{
	if (!sgt || !sgt->sgl || offset >= sgt->total_bytes)
		return 0;
	u64 walked = 0;
	for (u32 i = 0; i < sgt->nents; i++) {
		const struct scatterlist *sg = &sgt->sgl[i];
		if (offset < walked + sg->length)
			return sg->phys + sg->offset + (offset - walked);
		walked += sg->length;
	}
	return 0;
}

int sg_is_contiguous(const struct sg_table *sgt)
{
	if (!sgt || sgt->nents == 0)
		return 0;
	return sgt->nents == 1;
}

static usize sg_copy(const struct sg_table *sgt, u64 offset, void *buf,
                     const void *src, usize len, int to_buffer)
{
	if (!sgt || !sgt->sgl || len == 0)
		return 0;
	if (offset >= sgt->total_bytes)
		return 0;
	if (offset + len > sgt->total_bytes)
		len = (usize)(sgt->total_bytes - offset);

	u64 dm = vmm_direct_map_base();
	usize done = 0;
	u64 walked = 0;
	for (u32 i = 0; i < sgt->nents && done < len; i++) {
		const struct scatterlist *sg = &sgt->sgl[i];
		u64 run_end = walked + sg->length;
		if (offset + done >= run_end) {
			walked = run_end;
			continue;
		}
		u64 within = (offset + done) - walked;
		usize chunk = (usize)(sg->length - within);
		if (chunk > len - done)
			chunk = len - done;
		u8 *kern = (u8 *)(usize)(sg->phys + sg->offset + within + dm);
		if (to_buffer)
			memcpy((u8 *)buf + done, kern, chunk);
		else
			memcpy(kern, (const u8 *)src + done, chunk);
		done += chunk;
		walked = run_end;
	}
	return done;
}

usize sg_copy_to_buffer(const struct sg_table *sgt, u64 offset, void *buf,
                        usize len)
{
	return sg_copy(sgt, offset, buf, 0, len, 1);
}

usize sg_copy_from_buffer(const struct sg_table *sgt, u64 offset,
                          const void *buf, usize len)
{
	return sg_copy(sgt, offset, 0, buf, len, 0);
}
