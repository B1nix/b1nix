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

static void pci_print_hex16(u16 v)
{
	static const char hx[] = "0123456789abcdef";
	for (int i = 12; i >= 0; i -= 4)
		console_putc(hx[(v >> i) & 0xf]);
}

/* Enumerate and print EVERY PCI function so unrecognised hardware (e.g. a NIC
 * with no driver yet) is identifiable by vendor:device — visible later via
 * `dmesg | grep pci`. Class 0x02 (network) / 0x01 (storage) / 0x0c03 (usb) are
 * flagged so they stand out. */
void pci_init(void)
{
	console_write("pci: enumerating devices (bus:slot.func vendor:device class)\n");
	for (u16 bus = 0; bus < 256; bus++) {
		for (u8 slot = 0; slot < 32; slot++) {
			if (pci_config_read16((u8)bus, slot, 0, 0) == 0xFFFF)
				continue;
			u8 htype = pci_config_read8((u8)bus, slot, 0, 0x0E);
			u8 nfuncs = (htype & 0x80) ? 8 : 1;
			for (u8 func = 0; func < nfuncs; func++) {
				u16 vendor = pci_config_read16((u8)bus, slot, func, 0);
				if (vendor == 0xFFFF)
					continue;
				u16 device = pci_config_read16((u8)bus, slot, func, 2);
				u8 cls = pci_config_read8((u8)bus, slot, func, 0x0B);
				u8 sub = pci_config_read8((u8)bus, slot, func, 0x0A);
				console_write("pci: ");
				console_write_dec((u32)bus);
				console_putc(':');
				console_write_dec((u32)slot);
				console_putc('.');
				console_write_dec((u32)func);
				console_write("  ");
				pci_print_hex16(vendor);
				console_putc(':');
				pci_print_hex16(device);
				console_write("  class=");
				pci_print_hex16(((u16)cls << 8) | sub);
				if (cls == 0x02)
					console_write("  [network]");
				else if (cls == 0x01)
					console_write("  [storage]");
				else if (cls == 0x0C && sub == 0x03)
					console_write("  [usb]");
				console_putc('\n');
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
						info->irq_line = pci_config_read8((u8)bus, slot, func, 0x3C);
					}
					return 1;
				}
			}
		}
	}
	return 0;
}

int pci_find_class(u8 class_code, u8 subclass, u8 index, struct pci_device_info *info)
{
	u8 seen = 0;

	for (u16 bus = 0; bus < 256; bus++) {
		for (u8 slot = 0; slot < 32; slot++) {
			u16 vendor = pci_config_read16((u8)bus, slot, 0, 0);
			if (vendor == 0xFFFF) continue;

			u8 header_type = pci_config_read8((u8)bus, slot, 0, 0x0E);
			u8 max_func = (header_type & 0x80) ? 8 : 1;

			for (u8 func = 0; func < max_func; func++) {
				vendor = pci_config_read16((u8)bus, slot, func, 0);
				if (vendor == 0xFFFF) continue;

				u8 cls = pci_config_read8((u8)bus, slot, func, 0x0B);
				u8 sub = pci_config_read8((u8)bus, slot, func, 0x0A);
				if (cls != class_code || sub != subclass) continue;

				if (seen++ != index) continue;

				if (info) {
					info->bus = (u8)bus;
					info->slot = slot;
					info->func = func;
					info->vendor_id = vendor;
					info->device_id = pci_config_read16((u8)bus, slot, func, 2);
					info->class_code = cls;
					info->subclass = sub;
					info->prog_if = pci_config_read8((u8)bus, slot, func, 0x09);
					info->irq_line = pci_config_read8((u8)bus, slot, func, 0x3C);
				}
				return 1;
			}
		}
	}

	return 0;
}

u32 pci_get_vram_size(u16 vendor_id, u16 device_id)
{
	if (vendor_id == 0x1234 && device_id == 0x1111) {
		return 16 * 1024 * 1024; // QEMU Standard VGA (16 MB)
	}
	if (vendor_id == 0x1af4 && device_id == 0x1050) {
		return 16 * 1024 * 1024; // VirtIO GPU (16 MB)
	}
	if (vendor_id == 0x15ad && device_id == 0x0405) {
		return 16 * 1024 * 1024; // VMware SVGA II (16 MB)
	}
	if (vendor_id == 0x80ee && device_id == 0xbeef) {
		return 16 * 1024 * 1024; // VirtualBox Graphics Adapter (16 MB)
	}
	return 0;
}
