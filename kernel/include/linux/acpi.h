/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_ACPI_H
#define LKPI_LINUX_ACPI_H
#include <b1nix/acpi.h>
#include <linux/types.h>
/* b1nix parses the tables the IOMMU and MADT need (kernel/dev/acpi.c) but has
 * no AML interpreter, so the method-evaluation entry points imported code uses
 * for panel and switcheroo handling report absence. */
typedef void *acpi_handle;
typedef u32 acpi_status;
#define AE_OK 0
#define AE_NOT_FOUND 5
static inline bool acpi_disabled_stub(void) { return true; }
#define ACPI_HANDLE(dev) ((acpi_handle)0)

#endif
