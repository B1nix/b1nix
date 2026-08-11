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


/*
 * The ACPI resource walk an i2c-attached DSI panel is described by.
 *
 * b1nix parses ACPI tables but does not expose device resources this way, and
 * neither M102 target has a DSI panel — the ports are DDI. So the lookup finds
 * no i2c serial-bus resource, the walk stops, and the DSI code leaves its bus
 * number unset, which is the same state it reaches on a machine where the panel
 * is not i2c-attached.
 */
struct acpi_resource;
struct acpi_resource_source { char *string_ptr; };
struct acpi_resource_i2c_serialbus {
	u16 slave_address;
	struct acpi_resource_source resource_source;
};
typedef u32 acpi_status;
typedef void *acpi_handle;
#define AE_OK 0u
#define AE_NOT_FOUND 5u
#define ACPI_FAILURE(s)  ((s) != AE_OK)
#define ACPI_SUCCESS(s)  ((s) == AE_OK)

static inline bool i2c_acpi_get_i2c_resource(struct acpi_resource *ares,
                                             struct acpi_resource_i2c_serialbus **sb)
{ (void)ares; (void)sb; return false; }

static inline acpi_status acpi_get_handle(acpi_handle parent, const char *path,
                                          acpi_handle *out)
{ (void)parent; (void)path; (void)out; return AE_NOT_FOUND; }


/* The ACPI device a struct device corresponds to, and its resource list. No
 * device here has an ACPI companion — see above — so the companion is absent
 * and the resource walk visits nothing. */
struct acpi_device;
#define ACPI_COMPANION(dev) ((struct acpi_device *)NULL)
static inline acpi_handle acpi_device_handle(struct acpi_device *adev)
{ (void)adev; return NULL; }
static inline int acpi_dev_get_resources(struct acpi_device *adev,
                                         struct list_head *list,
                                         int (*preproc)(struct acpi_resource *, void *),
                                         void *preproc_data)
{ (void)adev; (void)list; (void)preproc; (void)preproc_data; return 0; }
static inline void acpi_dev_free_resource_list(struct list_head *list)
{ (void)list; }


/*
 * ACPI bus events, which firmware raises for things like a lid switch or a
 * brightness hotkey. b1nix delivers none to drivers, so a registered notifier
 * is never called — the driver simply never hears about a firmware-initiated
 * display change, and keeps the state it set itself.
 */
struct acpi_bus_event {
	char device_class[20];
	char bus_id[15];
	u32 type;
	int data;
};
#define ACPI_VIDEO_CLASS "video"
#define ACPI_VIDEO_NOTIFY_PROBE 0x81

struct notifier_block;
static inline int register_acpi_notifier(struct notifier_block *nb)
{ (void)nb; return 0; }
static inline int unregister_acpi_notifier(struct notifier_block *nb)
{ (void)nb; return 0; }

#endif
