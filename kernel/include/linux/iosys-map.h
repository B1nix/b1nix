/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_IOSYS_MAP_H
#define LKPI_LINUX_IOSYS_MAP_H

#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/types.h>

/*
 * A pointer that remembers whether it is normal memory or device MMIO.
 *
 * The distinction is not cosmetic: MMIO must be touched with the accessors that
 * keep each access single and ordered, while system memory can be memcpy'd. A
 * buffer object may be backed by either, so the DRM core carries this rather
 * than a bare pointer and every access asks which it is.
 */
struct iosys_map {
	union {
		void __iomem *vaddr_iomem;
		void *vaddr;
	};
	bool is_iomem;
};

#define IOSYS_MAP_INIT_VADDR(vaddr_) { .vaddr = (vaddr_), .is_iomem = false }

static inline void iosys_map_set_vaddr(struct iosys_map *map, void *vaddr)
{
	map->vaddr = vaddr;
	map->is_iomem = false;
}

static inline void iosys_map_set_vaddr_iomem(struct iosys_map *map,
                                             void __iomem *vaddr_iomem)
{
	map->vaddr_iomem = vaddr_iomem;
	map->is_iomem = true;
}

static inline bool iosys_map_is_null(const struct iosys_map *map)
{
	return map->is_iomem ? !map->vaddr_iomem : !map->vaddr;
}

static inline bool iosys_map_is_set(const struct iosys_map *map)
{
	return !iosys_map_is_null(map);
}

static inline void iosys_map_clear(struct iosys_map *map)
{
	map->vaddr = 0;
	map->is_iomem = false;
}

static inline void iosys_map_memcpy_to(struct iosys_map *dst, usize dst_offset,
                                       const void *src, usize len)
{
	memcpy((char *)dst->vaddr + dst_offset, src, len);
}

static inline void iosys_map_memcpy_from(void *dst,
                                         const struct iosys_map *src,
                                         usize src_offset, usize len)
{
	memcpy(dst, (const char *)src->vaddr + src_offset, len);
}

static inline void iosys_map_incr(struct iosys_map *map, usize incr)
{
	if (map->is_iomem)
		map->vaddr_iomem = (char *)map->vaddr_iomem + incr;
	else
		map->vaddr = (char *)map->vaddr + incr;
}

static inline void iosys_map_memset(struct iosys_map *dst, usize offset,
                                    int value, usize len)
{
	memset((char *)dst->vaddr + offset, value, len);
}

#endif
