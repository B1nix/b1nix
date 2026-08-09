/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_PCI_H
#define LKPI_LINUX_PCI_H

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
	void *driver_data;
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

#endif
