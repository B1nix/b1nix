#ifndef B1NIX_PCI_H
#define B1NIX_PCI_H

#include <b1nix/types.h>

struct pci_device_info {
	u8 bus;
	u8 slot;
	u8 func;
	u16 vendor_id;
	u16 device_id;
	u8 class_code;
	u8 subclass;
	u8 prog_if;
	u8 irq_line;
};

void pci_init(void);
u32 pci_config_read32(u8 bus, u8 slot, u8 func, u8 offset);
u16 pci_config_read16(u8 bus, u8 slot, u8 func, u8 offset);
u8 pci_config_read8(u8 bus, u8 slot, u8 func, u8 offset);
void pci_config_write32(u8 bus, u8 slot, u8 func, u8 offset, u32 value);
void pci_config_write16(u8 bus, u8 slot, u8 func, u8 offset, u16 value);
void pci_config_write8(u8 bus, u8 slot, u8 func, u8 offset, u8 value);
int pci_find_device(u16 vendor_id, u16 device_id, struct pci_device_info *info);
int pci_find_class(u8 class_code, u8 subclass, u8 index, struct pci_device_info *info);
u32 pci_get_vram_size(u16 vendor_id, u16 device_id);

#endif
