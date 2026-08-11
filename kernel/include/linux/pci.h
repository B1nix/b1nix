/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_PCI_H
#define LKPI_LINUX_PCI_H
#include <linux/ioport.h>

#include <b1nix/pci.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/types.h>

/*
 * PCI, onto b1nix's enumerator.
 *
 * b1nix addresses a function by (bus, slot, func) rather than by a struct the
 * bus layer owns, so `struct pci_dev` here carries that triple plus the device
 * model's `struct device`. Everything imported source reads out of a pci_dev —
 * vendor, device, revision, BARs — is filled in when the struct is created, so
 * a driver's field access does not turn into a config-space read behind its
 * back.
 */

/* PCI segment (domain) number. b1nix enumerates one segment, so it is always
 * zero — which is what a machine with a single host bridge reports too. */
struct pci_bus { unsigned char number; };
static inline int pci_domain_nr(struct pci_bus *bus) { (void)bus; return 0; }

struct pci_dev {
	struct device dev;
	/* The bus object, as Linux has it — imported code reads bus->number. */
	struct pci_bus *bus;
	u8 bus_nr;
	u8 slot;
	u8 func;
	/* The packed slot/function byte, as imported code reads it. */
	unsigned int devfn;
	u16 vendor;
	u16 device;
	u16 subsystem_vendor;
	u16 subsystem_device;
	u8 revision;
	u32 class;
	int irq;
	/* Set when MSI was enabled for this device, so a driver can tell an
	 * MSI vector from a shared legacy line. */
	unsigned int msi_enabled : 1;
	void *driver_data;
	/* The decoded windows, indexed by BAR number — see the note below. */
	struct resource resource[6];
};

#define to_pci_dev(d) container_of(d, struct pci_dev, dev)

static inline void *pci_get_drvdata(struct pci_dev *pdev)
{
	return pdev ? pdev->driver_data : 0;
}

static inline void pci_set_drvdata(struct pci_dev *pdev, void *data)
{
	if (pdev)
		pdev->driver_data = data;
}

/* Config space. The (bus, slot, func) triple is what b1nix's accessors take. */
static inline int pci_read_config_byte(struct pci_dev *d, int where, u8 *val)
{
	*val = (u8)(pci_config_read32(d->bus_nr, d->slot, d->func, (u8)(where & ~3)) >>
	            ((where & 3) * 8));
	return 0;
}

static inline int pci_read_config_word(struct pci_dev *d, int where, u16 *val)
{
	*val = (u16)(pci_config_read32(d->bus_nr, d->slot, d->func, (u8)(where & ~3)) >>
	             ((where & 2) * 8));
	return 0;
}

static inline int pci_read_config_dword(struct pci_dev *d, int where, u32 *val)
{
	*val = pci_config_read32(d->bus_nr, d->slot, d->func, (u8)where);
	return 0;
}

static inline int pci_write_config_dword(struct pci_dev *d, int where, u32 val)
{
	pci_config_write32(d->bus_nr, d->slot, d->func, (u8)where, val);
	return 0;
}

static inline int pci_enable_device(struct pci_dev *d)
{
	pci_enable_decode(d->bus_nr, d->slot, d->func);
	return 0;
}

static inline void pci_set_master(struct pci_dev *d)
{
	pci_enable_bus_master(d->bus_nr, d->slot, d->func);
}

/* Named apart from b1nix's own pci_find_capability, which takes the triple:
 * one wrapping the other under the same name is a silent infinite recursion. */
static inline int pci_dev_find_capability(struct pci_dev *d, int cap)
{
	return pci_find_capability(d->bus_nr, d->slot, d->func, (u8)cap);
}

/* Is this device on the PCI bus? Everything M102 targets is, so this is the
 * counterpart to dev_is_platform's constant no. */
static inline bool dev_is_pci(const struct device *dev)
{
	(void)dev;
	return true;
}

/* The device and function halves of a PCI devfn byte. The encoding is what
 * lspci and every tool prints, so it is reproduced rather than chosen. */
#define PCI_SLOT(devfn) (((devfn) >> 3) & 0x1f)
#define PCI_FUNC(devfn) ((devfn) & 0x07)
#define PCI_DEVFN(slot, func) ((((slot) & 0x1f) << 3) | ((func) & 0x07))

#define PCI_VENDOR_ID_INTEL 0x8086
#define PCI_ANY_ID (~0u)


/*
 * Device power states.
 *
 * i915 names these constantly — saving them across suspend, refusing to touch
 * registers below D0 — so the enum has to exist even though b1nix keeps every
 * device in D0: there is no system suspend here (see <linux/pm.h>), and runtime
 * PM gates the engine rather than the PCI state. Reporting D0 is therefore the
 * truth, not a placeholder.
 */
typedef int pci_power_t;
#define PCI_D0        ((pci_power_t)0)
#define PCI_D1        ((pci_power_t)1)
#define PCI_D2        ((pci_power_t)2)
#define PCI_D3hot     ((pci_power_t)3)
#define PCI_D3cold    ((pci_power_t)4)
#define PCI_UNKNOWN   ((pci_power_t)5)
#define PCI_POWER_ERROR ((pci_power_t)-1)


/* BAR geometry, straight off the device's resource table. */
#define pci_resource_start(dev, bar) ((dev)->resource[bar].start)
#define pci_resource_end(dev, bar)   ((dev)->resource[bar].end)
#define pci_resource_len(dev, bar)   \
	((pci_resource_start(dev, bar) == 0 && pci_resource_end(dev, bar) == 0) ? 0 : \
	 (pci_resource_end(dev, bar) - pci_resource_start(dev, bar) + 1))
#define pci_resource_flags(dev, bar) ((dev)->resource[bar].flags)


/*
 * The device's decoded windows.
 *
 * Six entries because that is how many BARs a PCI function has; the array is
 * indexed by BAR number, so a 64-bit BAR occupies its own index and leaves the
 * next one empty — matching what the accessors above expect and what upstream
 * drivers assume when they name a specific BAR.
 *
 * Filled by lkpi_pci_dev_fill_resources() from b1nix's own sizing, not left for
 * a driver to work out.
 */
struct pci_resources_holder;
void lkpi_pci_dev_fill_resources(struct pci_dev *pdev);


/* Byte and word config accesses. The 32-bit forms are already here; these are
 * the narrower ones, and the read-modify-write is done by b1nix's config
 * accessors rather than here, so a partial-width write does not disturb its
 * neighbours. */
/* The read side already exists above as inlines; only the writes were
 * missing. */
int pci_write_config_byte(struct pci_dev *dev, int where, u8 val);
int pci_write_config_word(struct pci_dev *dev, int where, u16 val);


/* The table a driver matches devices against. PCI_ANY_ID is what an entry uses
 * for a field it does not care about. */
struct pci_device_id {
	u32 vendor, device;
	u32 subvendor, subdevice;
	u32 class, class_mask;
	unsigned long driver_data;
	u32 override_only;
};

#define PCI_ANY_ID (~0u)
#define PCI_DEVICE(vend, dev) \
	.vendor = (vend), .device = (dev), .subvendor = PCI_ANY_ID, .subdevice = PCI_ANY_ID

/* Map part of a BAR. The offset-and-length form matters for an aperture far
 * larger than anything worth mapping whole. */
void __iomem *pci_iomap_range(struct pci_dev *dev, int bar, unsigned long offset,
                              unsigned long maxlen);
void __iomem *pci_iomap(struct pci_dev *dev, int bar, unsigned long maxlen);
void pci_iounmap(struct pci_dev *dev, void __iomem *addr);


/* Enabling and powering a device. b1nix leaves every device in D0 (see the note
 * on pci_power_t), so a state change to D0 succeeds and anything deeper is
 * refused rather than silently ignored — a driver told it reached D3 would skip
 * the register saves it does on the way back. */
/* pci_enable_device and pci_set_master already exist above as inlines; these
 * are the ones that were missing. */
void pci_disable_device(struct pci_dev *dev);
int pci_set_power_state(struct pci_dev *dev, pci_power_t state);
void pci_clear_master(struct pci_dev *dev);
int pci_save_state(struct pci_dev *dev);
void pci_restore_state(struct pci_dev *dev);


/* The id constants travel with the PCI interface: a driver matching on a
 * subsystem vendor includes only this header. */
#include <linux/pci_ids.h>


/*
 * A PCI driver, as the core binds it.
 *
 * b1nix binds its own drivers through its own enumeration, so nothing here
 * walks this table — a driver that registers is recorded and its probe is
 * called by the code that owns the device, not by a bus match. The shape is
 * upstream's because the driver defines it either way.
 */
struct pci_driver {
	const char *name;
	const struct pci_device_id *id_table;
	int (*probe)(struct pci_dev *dev, const struct pci_device_id *id);
	void (*remove)(struct pci_dev *dev);
	void (*shutdown)(struct pci_dev *dev);
	/* The generic driver half. i915 sets its PM ops through .driver.pm,
	 * which is why this is the embedded structure and not a bare pointer. */
	struct device_driver driver;
};

int pci_register_driver(struct pci_driver *drv);
void pci_unregister_driver(struct pci_driver *drv);


/* Config access through a bus rather than a device, for a function that is not
 * the driver's own — i915 reads the host bridge this way. */
int pci_bus_read_config_byte(struct pci_bus *bus, unsigned int devfn, int where, u8 *val);
int pci_bus_read_config_word(struct pci_bus *bus, unsigned int devfn, int where, u16 *val);
int pci_bus_read_config_dword(struct pci_bus *bus, unsigned int devfn, int where, u32 *val);
int pci_bus_write_config_byte(struct pci_bus *bus, unsigned int devfn, int where, u8 val);
int pci_bus_write_config_word(struct pci_bus *bus, unsigned int devfn, int where, u16 val);
int pci_bus_write_config_dword(struct pci_bus *bus, unsigned int devfn, int where, u32 val);

/* Match a device against a driver's id table. Returns the matching entry, or
 * NULL — the driver reads driver_data off it, so a wrong match is a wrong
 * platform description rather than merely a wrong bind. */
const struct pci_device_id *pci_match_id(const struct pci_device_id *ids,
                                         struct pci_dev *dev);

/* The option ROM. b1nix does not map it: the VBT that i915 needs on these
 * parts comes from the OpRegion, and a driver that falls back to the ROM will
 * see the failure rather than parse whatever happens to be at that address. */
void __iomem *pci_map_rom(struct pci_dev *pdev, size_t *size);
void pci_unmap_rom(struct pci_dev *pdev, void __iomem *rom);


#define PCI_DEVID(bus, devfn) ((((u16)(bus)) << 8) | (devfn))
#define PCI_CLASS_BRIDGE_ISA 0x0601
#define PCIBIOS_MIN_MEM      0x100000ul

/* Drop a reference taken by one of the lookup helpers. b1nix's PCI devices are
 * enumerated once at boot and live for the life of the system, so there is no
 * refcount to drop and no device that can go away underneath a driver. */
static inline void pci_dev_put(struct pci_dev *dev) { (void)dev; }
static inline struct pci_dev *pci_dev_get(struct pci_dev *dev) { return dev; }

/* Find a device by class code, continuing from a previous result. Walks the
 * same enumerated list the rest of the PCI layer uses. */
struct pci_dev *pci_get_class(unsigned int class_code, struct pci_dev *from);
struct pci_dev *pci_get_domain_bus_and_slot(int domain, unsigned int bus,
                                            unsigned int devfn);

/* Is any device matching this table present? Used to detect a companion chip.
 * Same walk, reporting presence only. */
struct pci_device_id;
int pci_dev_present(const struct pci_device_id *ids);

/* The root port above a device. b1nix's PCI topology is flat — devices are
 * enumerated by bus/device/function with no parent links recorded — so there is
 * no port to return and callers take their no-root-port path. */
static inline struct pci_dev *pcie_find_root_port(struct pci_dev *dev)
{ (void)dev; return NULL; }

/*
 * D3cold policy, and the legacy single-vector MSI entry points.
 *
 * D3cold is the power state where the device's main rail is off. b1nix never
 * enters it — runtime PM here stops at D3hot — so allowing or disallowing it
 * changes nothing that can happen. The MSI calls route to the MSI-X machinery
 * M98 built, with one vector.
 */
static inline void pci_d3cold_disable(struct pci_dev *dev) { (void)dev; }
static inline void pci_d3cold_enable(struct pci_dev *dev) { (void)dev; }
int pci_enable_msi(struct pci_dev *dev);
void pci_disable_msi(struct pci_dev *dev);

/*
 * Allocating a window out of a bus's address space.
 *
 * Declared and deliberately not defined. b1nix assigns BARs during enumeration
 * and keeps no free-space map per bus afterwards, so there is nothing to
 * allocate from. A driver that needs a new window — i915's stolen-memory
 * fallback on old chipsets does — fails to link rather than being handed a
 * range that overlaps something already programmed.
 */
struct resource;
int pci_bus_alloc_resource(struct pci_bus *bus, struct resource *res,
                           resource_size_t size, resource_size_t align,
                           resource_size_t min, unsigned long type_mask,
                           resource_size_t (*alignf)(void *, const struct resource *,
                                                     resource_size_t, resource_size_t),
                           void *alignf_data);
resource_size_t pcibios_align_resource(void *data, const struct resource *res,
                                       resource_size_t size, resource_size_t align);

#endif
