#include <b1nix/pci.h>
#include <b1nix/io.h>
#include <b1nix/console.h>

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

static u32 pci_get_config_address(u8 bus, u8 slot, u8 func, u8 offset)
{
	return (u32)((bus << 16) | (slot << 11) | (func << 8) | (offset & 0xFC) | ((u32)0x80000000));
}

u32 pci_config_read32(u8 bus, u8 slot, u8 func, u8 offset)
{
	outl(PCI_CONFIG_ADDRESS, pci_get_config_address(bus, slot, func, offset));
	return inl(PCI_CONFIG_DATA);
}

u16 pci_config_read16(u8 bus, u8 slot, u8 func, u8 offset)
{
	u32 val = pci_config_read32(bus, slot, func, offset);
	return (u16)((val >> ((offset & 2) * 8)) & 0xFFFF);
}

u8 pci_config_read8(u8 bus, u8 slot, u8 func, u8 offset)
{
	u32 val = pci_config_read32(bus, slot, func, offset);
	return (u8)((val >> ((offset & 3) * 8)) & 0xFF);
}

void pci_config_write32(u8 bus, u8 slot, u8 func, u8 offset, u32 value)
{
	outl(PCI_CONFIG_ADDRESS, pci_get_config_address(bus, slot, func, offset));
	outl(PCI_CONFIG_DATA, value);
}

void pci_config_write16(u8 bus, u8 slot, u8 func, u8 offset, u16 value)
{
	u32 current = pci_config_read32(bus, slot, func, offset);
	u32 mask = 0xFFFF << ((offset & 2) * 8);
	current = (current & ~mask) | ((u32)value << ((offset & 2) * 8));
	pci_config_write32(bus, slot, func, offset, current);
}

void pci_config_write8(u8 bus, u8 slot, u8 func, u8 offset, u8 value)
{
	u32 current = pci_config_read32(bus, slot, func, offset);
	u32 mask = 0xFF << ((offset & 3) * 8);
	current = (current & ~mask) | ((u32)value << ((offset & 3) * 8));
	pci_config_write32(bus, slot, func, offset, current);
}

// A simple scan just to keep records or log. For finding devices, we use pci_find_device.
void pci_init(void)
{
	console_write("pci: enumerating bus...\n");
	// A simple sanity check on bus 0
	for (u16 bus = 0; bus < 256; bus++) {
		for (u8 slot = 0; slot < 32; slot++) {
			u16 vendor = pci_config_read16((u8)bus, slot, 0, 0);
			if (vendor != 0xFFFF) {
				// We found a device
				// Just basic logging could be added here if needed, but not necessary.
			}
		}
	}
}

int pci_find_device(u16 vendor_id, u16 device_id, struct pci_device_info *info)
{
	for (u16 bus = 0; bus < 256; bus++) {
		for (u8 slot = 0; slot < 32; slot++) {
			u16 vendor = pci_config_read16((u8)bus, slot, 0, 0);
			if (vendor == 0xFFFF) continue;
			
			u8 header_type = pci_config_read8((u8)bus, slot, 0, 0x0E);
			u8 max_func = (header_type & 0x80) ? 8 : 1;
			
			for (u8 func = 0; func < max_func; func++) {
				vendor = pci_config_read16((u8)bus, slot, func, 0);
				if (vendor == 0xFFFF) continue;
				
				u16 dev = pci_config_read16((u8)bus, slot, func, 2);
				if (vendor == vendor_id && (device_id == 0xFFFF || dev == device_id)) {
					if (info) {
						info->bus = (u8)bus;
						info->slot = slot;
						info->func = func;
						info->vendor_id = vendor;
						info->device_id = dev;
						info->class_code = pci_config_read8((u8)bus, slot, func, 0x0B);
						info->subclass = pci_config_read8((u8)bus, slot, func, 0x0A);
						info->prog_if = pci_config_read8((u8)bus, slot, func, 0x09);
					}
					return 1;
				}
			}
		}
	}
	return 0;
}
