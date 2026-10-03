/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_IOMMU_H
#define LKPI_LINUX_IOMMU_H
#include <linux/types.h>
/* KVM only asks whether devices are assigned to a VM, which needs VFIO: never
 * here. */
struct iommu_group;
static inline void iommu_group_put(struct iommu_group *group) { (void)group; }
#endif
