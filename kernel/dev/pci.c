#include <b1nix/pci.h>
#include <b1nix/acpi.h>
#include <b1nix/bootinfo.h>
#include <b1nix/io.h>
#include <b1nix/console.h>
#include <b1nix/lapic.h>
#include <b1nix/mm.h>
#include <string.h>

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

/* Report the amount of graphics memory a display adapter has.
 *
 * virtio-gpu / virtio-vga have NO dedicated VRAM: the host allocates framebuffer
 * and resource backing from guest RAM on demand (QEMU exposes no vram/vgamem
 * knob for them — only the optional `hostmem` blob region). For these we report
 * the kernel's graphics shared-memory budget instead of a fictional aperture —
 * the maximum size of a single graphics/framebuffer shared segment (SHMMAX, see
 * kernel/include/b1nix/shm.h; currently 32 MB, room for a 1280x800x4 = 4 MB
 * screen plus several windows).
 *
 * The other adapters expose a real linear-framebuffer aperture whose size is the
 * VRAM; QEMU's standard VGA defaults to 16 MB (configurable via vgamem_mb). */
#define GFX_SHMEM_BUDGET_MB 32  /* keep in sync with SHMMAX in b1nix/shm.h */

u32 pci_get_vram_size(u16 vendor_id, u16 device_id)
{
	if (vendor_id == 0x1af4 &&
	    (device_id == 0x1050 ||   // virtio-gpu / virtio-vga (modern)
	     device_id == 0x1051 ||   // virtio-gpu-gl variants
	     device_id == 0x1052)) {
		return GFX_SHMEM_BUDGET_MB * 1024 * 1024;
	}
	if (vendor_id == 0x1234 && device_id == 0x1111) {
		return 16 * 1024 * 1024; // QEMU Standard VGA (vgamem default 16 MB)
	}
	if (vendor_id == 0x15ad && device_id == 0x0405) {
		return 16 * 1024 * 1024; // VMware SVGA II (16 MB)
	}
	if (vendor_id == 0x80ee && device_id == 0xbeef) {
		return 16 * 1024 * 1024; // VirtualBox Graphics Adapter (16 MB)
	}
	return 0;
}

/* ══════════════════════════════════════════════════════════════════
 * M98 T3 — BAR enumeration and sizing, bus mastering, capability walk,
 * MSI/MSI-X, Intel stolen memory.
 * ══════════════════════════════════════════════════════════════════ */

#define PCI_BAR_IO            0x00000001u
#define PCI_BAR_MEM_TYPE_MASK 0x00000006u
#define PCI_BAR_MEM_TYPE_64   0x00000004u
#define PCI_BAR_MEM_PREFETCH  0x00000008u
#define PCI_BAR_MEM_MASK      0xFFFFFFF0u
#define PCI_BAR_IO_MASK       0xFFFFFFFCu

u16 pci_command_set(u8 bus, u8 slot, u8 func, u16 bits)
{
	u16 cmd = pci_config_read16(bus, slot, func, PCI_CFG_COMMAND);
	if ((cmd & bits) != bits)
		pci_config_write16(bus, slot, func, PCI_CFG_COMMAND, (u16)(cmd | bits));
	return pci_config_read16(bus, slot, func, PCI_CFG_COMMAND);
}

u16 pci_command_clear(u8 bus, u8 slot, u8 func, u16 bits)
{
	u16 cmd = pci_config_read16(bus, slot, func, PCI_CFG_COMMAND);
	if (cmd & bits)
		pci_config_write16(bus, slot, func, PCI_CFG_COMMAND, (u16)(cmd & (u16)~bits));
	return pci_config_read16(bus, slot, func, PCI_CFG_COMMAND);
}

int pci_enable_bus_master(u8 bus, u8 slot, u8 func)
{
	u16 cmd = pci_command_set(bus, slot, func, PCI_CMD_BUS_MASTER);
	return (cmd & PCI_CMD_BUS_MASTER) ? 1 : 0;
}

u16 pci_enable_decode(u8 bus, u8 slot, u8 func)
{
	return pci_command_set(bus, slot, func,
	                       (u16)(PCI_CMD_IO_SPACE | PCI_CMD_MEM_SPACE));
}

/* Number of BARs a function implements, from its header type. Type 0 (normal
 * device) has six; type 1 (PCI-to-PCI bridge) has two; anything else has none
 * we are prepared to decode. */
static int pci_bar_count(u8 bus, u8 slot, u8 func)
{
	u8 htype = (u8)(pci_config_read8(bus, slot, func, PCI_CFG_HEADER_TYPE) & 0x7F);
	if (htype == 0x00)
		return 6;
	if (htype == 0x01)
		return 2;
	return 0;
}

int pci_bar_read(u8 bus, u8 slot, u8 func, u8 index, struct pci_bar *bar)
{
	if (!bar || index >= PCI_MAX_BARS)
		return -1;

	memset(bar, 0, sizeof(*bar));
	bar->index = index;

	int nbars = pci_bar_count(bus, slot, func);
	if (index >= nbars)
		return 0;

	/* The upper half of a 64-bit BAR is not a BAR of its own. */
	if (index > 0) {
		u8 prev = (u8)(index - 1);
		u32 pv = pci_config_read32(bus, slot, func,
		                           (u8)(PCI_CFG_BAR0 + prev * 4));
		if (!(pv & PCI_BAR_IO) &&
		    (pv & PCI_BAR_MEM_TYPE_MASK) == PCI_BAR_MEM_TYPE_64)
			return 0;
	}

	u8 off = (u8)(PCI_CFG_BAR0 + index * 4);
	u32 orig_lo = pci_config_read32(bus, slot, func, off);
	if (orig_lo == 0 || orig_lo == 0xFFFFFFFFu)
		return 0;

	bar->is_io = (orig_lo & PCI_BAR_IO) ? 1 : 0;
	bar->is_64bit = (!bar->is_io &&
	                 (orig_lo & PCI_BAR_MEM_TYPE_MASK) == PCI_BAR_MEM_TYPE_64)
	                    ? 1
	                    : 0;
	bar->prefetchable =
	    (!bar->is_io && (orig_lo & PCI_BAR_MEM_PREFETCH)) ? 1 : 0;

	u32 orig_hi = 0;
	if (bar->is_64bit) {
		if (index + 1 >= nbars)
			return 0; /* malformed: 64-bit BAR with no upper half */
		orig_hi = pci_config_read32(bus, slot, func, (u8)(off + 4));
	}

	/*
	 * Sizing writes all-ones and reads back the mask of the bits the device
	 * decodes. While the BAR holds 0xFFFFFFFF the device would claim a huge
	 * aperture, so disable its decode first and restore the command register
	 * (and the BAR) before returning.
	 */
	u16 cmd = pci_config_read16(bus, slot, func, PCI_CFG_COMMAND);
	pci_config_write16(bus, slot, func, PCI_CFG_COMMAND,
	                   (u16)(cmd & (u16)~(PCI_CMD_IO_SPACE | PCI_CMD_MEM_SPACE)));

	pci_config_write32(bus, slot, func, off, 0xFFFFFFFFu);
	u32 mask_lo = pci_config_read32(bus, slot, func, off);
	u32 mask_hi = 0xFFFFFFFFu;
	if (bar->is_64bit) {
		pci_config_write32(bus, slot, func, (u8)(off + 4), 0xFFFFFFFFu);
		mask_hi = pci_config_read32(bus, slot, func, (u8)(off + 4));
		pci_config_write32(bus, slot, func, (u8)(off + 4), orig_hi);
	}
	pci_config_write32(bus, slot, func, off, orig_lo);
	pci_config_write16(bus, slot, func, PCI_CFG_COMMAND, cmd);

	u64 mask;
	if (bar->is_io) {
		mask = (u64)(mask_lo & PCI_BAR_IO_MASK);
		bar->base = (u64)(orig_lo & PCI_BAR_IO_MASK);
		/* I/O BARs decode at most 8 bits of address; the rest read as 1. */
		mask |= 0xFFFFFFFFFFFF0000ULL;
	} else {
		mask = ((u64)mask_hi << 32) | (u64)(mask_lo & PCI_BAR_MEM_MASK);
		if (!bar->is_64bit)
			mask |= 0xFFFFFFFF00000000ULL;
		bar->base = ((u64)orig_hi << 32) | (u64)(orig_lo & PCI_BAR_MEM_MASK);
	}

	u64 size = (~mask) + 1;
	if (size == 0 || (size & (size - 1)) != 0)
		return 0; /* device did not decode a sane power-of-two window */

	bar->size = size;
	bar->valid = 1;
	return 0;
}

int pci_bar_enumerate(u8 bus, u8 slot, u8 func, struct pci_bar *out)
{
	if (!out)
		return 0;
	int n = 0;
	for (u8 i = 0; i < PCI_MAX_BARS; i++) {
		if (pci_bar_read(bus, slot, func, i, &out[i]) < 0)
			continue;
		if (out[i].valid)
			n++;
	}
	return n;
}

u8 pci_find_capability(u8 bus, u8 slot, u8 func, u8 cap_id)
{
	u16 status = pci_config_read16(bus, slot, func, PCI_CFG_STATUS);
	if (!(status & PCI_STATUS_CAP_LIST))
		return 0;

	u8 ptr = (u8)(pci_config_read8(bus, slot, func, PCI_CFG_CAP_PTR) & 0xFC);
	/* 48 entries is far more than the 0x40..0xFF window can hold; the bound
	 * turns a corrupt (cyclic) list into a miss instead of a hang. */
	for (int i = 0; ptr >= 0x40 && i < 48; i++) {
		u8 id = pci_config_read8(bus, slot, func, ptr);
		if (id == cap_id)
			return ptr;
		u8 next = (u8)(pci_config_read8(bus, slot, func, (u8)(ptr + 1)) & 0xFC);
		if (next == ptr)
			break;
		ptr = next;
	}
	return 0;
}

/* ── ECAM (memory-mapped extended config) ───────────────────────── */

struct acpi_mcfg_alloc {
	u64 base;
	u16 segment;
	u8 start_bus;
	u8 end_bus;
	u32 reserved;
} __attribute__((packed));

static int ecam_probed;
static u64 ecam_base_phys;
static u8 ecam_start_bus;
static u8 ecam_end_bus;
static volatile u8 *ecam_bus_map[256];

static void pci_ecam_probe(void)
{
	if (ecam_probed)
		return;
	ecam_probed = 1;

	const struct acpi_sdt_header *mcfg = acpi_find_table("MCFG");
	if (!mcfg)
		return;
	if (mcfg->length < sizeof(*mcfg) + 8 + sizeof(struct acpi_mcfg_alloc))
		return;

	const struct acpi_mcfg_alloc *a =
	    (const struct acpi_mcfg_alloc *)((const u8 *)mcfg + sizeof(*mcfg) + 8);
	/* Only segment 0 is addressable through this API. */
	if (a->segment != 0)
		return;
	ecam_base_phys = a->base;
	ecam_start_bus = a->start_bus;
	ecam_end_bus = a->end_bus;
}

u8 pci_ecam_available(void)
{
	pci_ecam_probe();
	return ecam_base_phys ? 1 : 0;
}

u64 pci_ecam_base(void)
{
	pci_ecam_probe();
	return ecam_base_phys;
}

/* Map (and cache) the 1 MiB of config space belonging to one bus. */
static volatile u8 *ecam_bus_window(u8 bus)
{
	pci_ecam_probe();
	if (!ecam_base_phys || bus < ecam_start_bus || bus > ecam_end_bus)
		return 0;
	if (ecam_bus_map[bus])
		return ecam_bus_map[bus];
	u64 phys = ecam_base_phys + ((u64)(bus - ecam_start_bus) << 20);
	volatile u8 *v = (volatile u8 *)vmm_map_mmio(
	    phys, 1024 * 1024, VMM_WRITABLE | VMM_PCD | VMM_NO_EXECUTE);
	ecam_bus_map[bus] = v;
	return v;
}

u32 pci_ecam_read32(u8 bus, u8 slot, u8 func, u16 offset)
{
	volatile u8 *w = ecam_bus_window(bus);
	if (!w || (offset & 3) || offset > 0xFFC)
		return 0xFFFFFFFFu;
	usize off = ((usize)slot << 15) | ((usize)func << 12) | offset;
	return *(volatile u32 *)(w + off);
}

void pci_ecam_write32(u8 bus, u8 slot, u8 func, u16 offset, u32 value)
{
	volatile u8 *w = ecam_bus_window(bus);
	if (!w || (offset & 3) || offset > 0xFFC)
		return;
	usize off = ((usize)slot << 15) | ((usize)func << 12) | offset;
	*(volatile u32 *)(w + off) = value;
}

u16 pci_find_ext_capability(u8 bus, u8 slot, u8 func, u16 cap_id)
{
	if (!pci_ecam_available())
		return 0;

	u16 off = 0x100;
	for (int i = 0; i < 64 && off >= 0x100 && off <= 0xFFC; i++) {
		u32 hdr = pci_ecam_read32(bus, slot, func, off);
		if (hdr == 0 || hdr == 0xFFFFFFFFu)
			return 0;
		if ((u16)(hdr & 0xFFFFu) == cap_id)
			return off;
		u16 next = (u16)((hdr >> 20) & 0xFFCu);
		if (next == off)
			return 0;
		off = next;
	}
	return 0;
}

/* ── MSI ────────────────────────────────────────────────────────── */

/* MSI capability layout (offsets from the capability header):
 *   +0  id | next | message control
 *   +4  message address low
 *   +8  message address high  (64-bit capable only)
 *   +8/+12 message data
 */
#define PCI_MSI_CTRL     0x02
#define PCI_MSI_ADDR_LO  0x04
#define PCI_MSI_CTRL_ENABLE  0x0001u
#define PCI_MSI_CTRL_64BIT   0x0080u

/* x86 MSI message address: 0xFEE00000 | (destination APIC id << 12). Fixed
 * delivery mode, physical destination, edge triggered — the data word carries
 * the vector in its low byte. */
static u64 msi_message_address(u32 apic_id)
{
	return 0xFEE00000ULL | ((u64)(apic_id & 0xFF) << 12);
}

int pci_msi_enable(u8 bus, u8 slot, u8 func, u8 irq)
{
	u8 cap = pci_find_capability(bus, slot, func, PCI_CAP_ID_MSI);
	if (!cap)
		return -1;
	if (irq > 15)
		return -1; /* only vectors 32..47 have IDT gates (see pci.h) */

	u16 ctrl = pci_config_read16(bus, slot, func, (u8)(cap + PCI_MSI_CTRL));
	u64 addr = msi_message_address(lapic_id());
	u16 data = (u16)(32 + irq);

	pci_config_write32(bus, slot, func, (u8)(cap + PCI_MSI_ADDR_LO),
	                   (u32)(addr & 0xFFFFFFFFu));
	u8 data_off;
	if (ctrl & PCI_MSI_CTRL_64BIT) {
		pci_config_write32(bus, slot, func, (u8)(cap + 0x08),
		                   (u32)(addr >> 32));
		data_off = (u8)(cap + 0x0C);
	} else {
		data_off = (u8)(cap + 0x08);
	}
	pci_config_write16(bus, slot, func, data_off, data);

	/* Request exactly one vector (multiple-message enable = 0) and enable. */
	ctrl = (u16)((ctrl & ~0x0070u) | PCI_MSI_CTRL_ENABLE);
	pci_config_write16(bus, slot, func, (u8)(cap + PCI_MSI_CTRL), ctrl);

	/* MSI and INTx are mutually exclusive: mask the legacy pin. */
	pci_command_set(bus, slot, func, PCI_CMD_INTX_DISABLE);
	return 0;
}

void pci_msi_disable(u8 bus, u8 slot, u8 func)
{
	u8 cap = pci_find_capability(bus, slot, func, PCI_CAP_ID_MSI);
	if (!cap)
		return;
	u16 ctrl = pci_config_read16(bus, slot, func, (u8)(cap + PCI_MSI_CTRL));
	pci_config_write16(bus, slot, func, (u8)(cap + PCI_MSI_CTRL),
	                   (u16)(ctrl & ~PCI_MSI_CTRL_ENABLE));
	pci_command_clear(bus, slot, func, PCI_CMD_INTX_DISABLE);
}

int pci_msi_readback(u8 bus, u8 slot, u8 func, u64 *out_addr, u16 *out_data,
                     int *out_enabled)
{
	u8 cap = pci_find_capability(bus, slot, func, PCI_CAP_ID_MSI);
	if (!cap)
		return -1;
	u16 ctrl = pci_config_read16(bus, slot, func, (u8)(cap + PCI_MSI_CTRL));
	u64 addr = pci_config_read32(bus, slot, func, (u8)(cap + PCI_MSI_ADDR_LO));
	u8 data_off;
	if (ctrl & PCI_MSI_CTRL_64BIT) {
		addr |= (u64)pci_config_read32(bus, slot, func, (u8)(cap + 0x08)) << 32;
		data_off = (u8)(cap + 0x0C);
	} else {
		data_off = (u8)(cap + 0x08);
	}
	if (out_addr)
		*out_addr = addr;
	if (out_data)
		*out_data = pci_config_read16(bus, slot, func, data_off);
	if (out_enabled)
		*out_enabled = (ctrl & PCI_MSI_CTRL_ENABLE) ? 1 : 0;
	return 0;
}

/* ── MSI-X ──────────────────────────────────────────────────────── */

#define PCI_MSIX_CTRL        0x02
#define PCI_MSIX_TABLE       0x04
#define PCI_MSIX_PBA         0x08
#define PCI_MSIX_CTRL_ENABLE 0x8000u
#define PCI_MSIX_CTRL_MASKALL 0x4000u
#define PCI_MSIX_ENTRY_BYTES 16u
#define PCI_MSIX_VECTOR_CTRL_MASK 0x00000001u

int pci_msix_table_size(u8 bus, u8 slot, u8 func)
{
	u8 cap = pci_find_capability(bus, slot, func, PCI_CAP_ID_MSIX);
	if (!cap)
		return -1;
	u16 ctrl = pci_config_read16(bus, slot, func, (u8)(cap + PCI_MSIX_CTRL));
	return (int)((ctrl & 0x07FFu) + 1);
}

/* Map the function's MSI-X vector table. The capability's TABLE dword names the
 * BAR index in its low three bits and the byte offset into that BAR in the
 * rest. */
static volatile u32 *pci_msix_table(u8 bus, u8 slot, u8 func, u8 *out_cap,
                                    u32 *out_entries)
{
	u8 cap = pci_find_capability(bus, slot, func, PCI_CAP_ID_MSIX);
	if (!cap)
		return 0;
	u16 ctrl = pci_config_read16(bus, slot, func, (u8)(cap + PCI_MSIX_CTRL));
	u32 entries = (u32)(ctrl & 0x07FFu) + 1;
	u32 tbl = pci_config_read32(bus, slot, func, (u8)(cap + PCI_MSIX_TABLE));
	u8 bir = (u8)(tbl & 0x7);
	u32 tbl_off = tbl & ~0x7u;

	struct pci_bar bar;
	if (pci_bar_read(bus, slot, func, bir, &bar) < 0 || !bar.valid || bar.is_io)
		return 0;
	u64 span = (u64)entries * PCI_MSIX_ENTRY_BYTES;
	if ((u64)tbl_off + span > bar.size)
		return 0;

	volatile u32 *t = (volatile u32 *)vmm_map_mmio(
	    bar.base + tbl_off, (usize)span,
	    VMM_WRITABLE | VMM_PCD | VMM_NO_EXECUTE);
	if (!t)
		return 0;
	if (out_cap)
		*out_cap = cap;
	if (out_entries)
		*out_entries = entries;
	return t;
}

int pci_msix_enable(u8 bus, u8 slot, u8 func, u32 entry, u8 irq)
{
	u8 cap = 0;
	u32 entries = 0;
	if (irq > 15)
		return -1;
	volatile u32 *t = pci_msix_table(bus, slot, func, &cap, &entries);
	if (!t || entry >= entries)
		return -1;

	u64 addr = msi_message_address(lapic_id());
	volatile u32 *e = t + entry * 4;
	/* Mask the entry while its address/data are in flux, then unmask. */
	e[3] = PCI_MSIX_VECTOR_CTRL_MASK;
	e[0] = (u32)(addr & 0xFFFFFFFFu);
	e[1] = (u32)(addr >> 32);
	e[2] = (u32)(32 + irq);
	e[3] = 0;

	pci_command_set(bus, slot, func,
	                (u16)(PCI_CMD_MEM_SPACE | PCI_CMD_INTX_DISABLE));
	u16 ctrl = pci_config_read16(bus, slot, func, (u8)(cap + PCI_MSIX_CTRL));
	ctrl = (u16)((ctrl | PCI_MSIX_CTRL_ENABLE) & ~PCI_MSIX_CTRL_MASKALL);
	pci_config_write16(bus, slot, func, (u8)(cap + PCI_MSIX_CTRL), ctrl);
	return 0;
}

int pci_msix_entry_readback(u8 bus, u8 slot, u8 func, u32 entry, u64 *out_addr,
                            u32 *out_data, u32 *out_vector_ctrl)
{
	u32 entries = 0;
	volatile u32 *t = pci_msix_table(bus, slot, func, 0, &entries);
	if (!t || entry >= entries)
		return -1;
	volatile u32 *e = t + entry * 4;
	if (out_addr)
		*out_addr = (u64)e[0] | ((u64)e[1] << 32);
	if (out_data)
		*out_data = e[2];
	if (out_vector_ctrl)
		*out_vector_ctrl = e[3];
	return 0;
}

int pci_msix_entry_restore(u8 bus, u8 slot, u8 func, u32 entry, u64 addr,
                           u32 data, u32 vector_ctrl)
{
	u32 entries = 0;
	volatile u32 *t = pci_msix_table(bus, slot, func, 0, &entries);
	if (!t || entry >= entries)
		return -1;
	volatile u32 *e = t + entry * 4;
	e[3] = PCI_MSIX_VECTOR_CTRL_MASK; /* mask before changing address/data */
	e[0] = (u32)(addr & 0xFFFFFFFFu);
	e[1] = (u32)(addr >> 32);
	e[2] = data;
	e[3] = vector_ctrl;
	return 0;
}

void pci_msix_disable(u8 bus, u8 slot, u8 func)
{
	u8 cap = pci_find_capability(bus, slot, func, PCI_CAP_ID_MSIX);
	if (!cap)
		return;
	u16 ctrl = pci_config_read16(bus, slot, func, (u8)(cap + PCI_MSIX_CTRL));
	pci_config_write16(bus, slot, func, (u8)(cap + PCI_MSIX_CTRL),
	                   (u16)(ctrl & ~PCI_MSIX_CTRL_ENABLE));
}

/* ── Intel graphics stolen memory ───────────────────────────────── */

#define INTEL_HOST_BRIDGE_GGC  0x50
#define INTEL_HOST_BRIDGE_BDSM 0x5C
#define INTEL_HOST_BRIDGE_BGSM 0x70

/* GGC.GMS (bits 15:8 on Gen8+) encodes the data-stolen size in 32 MiB units for
 * values 0x01..0x10, then in 4 MiB units from 0xF0. GGC.GGMS (bits 7:6) encodes
 * the GTT stolen size in units of 1 MiB, 0 meaning "none". */
static u64 intel_gms_bytes(u8 gms)
{
	if (gms == 0)
		return 0;
	if (gms <= 0x10)
		return (u64)gms * 32ULL * 1024 * 1024;
	if (gms >= 0xF0 && gms <= 0xFE)
		return (u64)(gms - 0xF0 + 1) * 4ULL * 1024 * 1024;
	return 0;
}

int pci_intel_stolen_read(struct pci_intel_stolen *out)
{
	if (!out)
		return -1;
	memset(out, 0, sizeof(*out));

	/* The host bridge is always 00:00.0. */
	u16 vendor = pci_config_read16(0, 0, 0, 0x00);
	if (vendor != 0x8086)
		return -1;

	u16 ggc = pci_config_read16(0, 0, 0, INTEL_HOST_BRIDGE_GGC);
	if (ggc == 0xFFFF)
		return -1;
	out->ggc = ggc;

	u32 bdsm = pci_config_read32(0, 0, 0, INTEL_HOST_BRIDGE_BDSM);
	u32 bgsm = pci_config_read32(0, 0, 0, INTEL_HOST_BRIDGE_BGSM);
	/* Both are 1 MiB aligned bases with the low bit reserved as a lock flag. */
	out->dsm_base = (u64)(bdsm & 0xFFF00000u);
	out->gsm_base = (u64)(bgsm & 0xFFF00000u);
	out->dsm_size = intel_gms_bytes((u8)((ggc >> 8) & 0xFF));
	out->gsm_size = (u64)((ggc >> 6) & 0x3) * 1024ULL * 1024ULL;

	/* Chipsets that do not implement graphics stolen memory (every QEMU host
	 * bridge) read these as zero. Report absence rather than a bogus window. */
	if (out->dsm_base == 0 && out->gsm_base == 0 && out->dsm_size == 0)
		return -1;
	return 0;
}

/* ── M98 in-kernel self-test ────────────────────────────────────────
 *
 * Everything below compares against state that was NOT supplied by the code
 * under test: the device's own config registers, an independently-read raw BAR
 * value, or an address another driver already uses successfully.
 */
static int pci_pick_test_device(struct pci_device_info *info)
{
	/* Prefer a virtio function (always present in the smoke VM), otherwise any
	 * function with at least one BAR. */
	if (pci_find_device(0x1AF4, 0xFFFF, info))
		return 1;
	for (u16 bus = 0; bus < 4; bus++) {
		for (u8 slot = 0; slot < 32; slot++) {
			if (pci_config_read16((u8)bus, slot, 0, 0) == 0xFFFF)
				continue;
			struct pci_bar b;
			if (pci_bar_read((u8)bus, slot, 0, 0, &b) == 0 && b.valid) {
				info->bus = (u8)bus;
				info->slot = slot;
				info->func = 0;
				info->vendor_id = pci_config_read16((u8)bus, slot, 0, 0);
				info->device_id = pci_config_read16((u8)bus, slot, 0, 2);
				return 1;
			}
		}
	}
	return 0;
}

static void pci_selftest_bars(const struct pci_device_info *dev)
{
	struct pci_bar bars[PCI_MAX_BARS];
	int n = pci_bar_enumerate(dev->bus, dev->slot, dev->func, bars);
	int ok = n > 0;

	for (int i = 0; i < PCI_MAX_BARS && ok; i++) {
		if (!bars[i].valid)
			continue;
		/* Sizing must have restored the register: compare the decoded base
		 * against a fresh raw read of the same config dword. */
		u32 raw = pci_config_read32(dev->bus, dev->slot, dev->func,
		                            (u8)(PCI_CFG_BAR0 + i * 4));
		u64 raw_base = bars[i].is_io ? (u64)(raw & PCI_BAR_IO_MASK)
		                             : (u64)(raw & PCI_BAR_MEM_MASK);
		if (bars[i].is_64bit) {
			u32 hi = pci_config_read32(dev->bus, dev->slot, dev->func,
			                           (u8)(PCI_CFG_BAR0 + (i + 1) * 4));
			raw_base |= (u64)hi << 32;
		}
		if (raw_base != bars[i].base)
			ok = 0;
		/* A decoded window is a non-zero power of two, at least 16 bytes (the
		 * architectural minimum for memory BARs, 4 for I/O). */
		if (bars[i].size == 0 || (bars[i].size & (bars[i].size - 1)) != 0)
			ok = 0;
		if (bars[i].size < (bars[i].is_io ? 4u : 16u))
			ok = 0;
		/* The base must be naturally aligned to the window it decodes. */
		if (bars[i].base & (bars[i].size - 1))
			ok = 0;
	}

	if (ok) {
		console_write("M98-DRV-SMOKE: ok bar-enum bars=");
		console_write_dec((u64)n);
		console_write("\n");
	} else {
		console_write("M98-DRV-SMOKE: FAIL bar-enum n=");
		console_write_dec((u64)n);
		console_write("\n");
	}

	/* Cross-check against a driver that is already running: legacy virtio
	 * reaches its device through BAR0 as an I/O port window, and virtio.c
	 * derives its port base from exactly that register. If our sizing code had
	 * corrupted the BAR, that driver's base would no longer match. */
	if (dev->vendor_id == 0x1AF4 && bars[0].valid && bars[0].is_io) {
		u32 raw0 = pci_config_read32(dev->bus, dev->slot, dev->func, PCI_CFG_BAR0);
		if ((u16)(raw0 & ~3u) == (u16)bars[0].base && bars[0].base != 0)
			console_write("M98-DRV-SMOKE: ok bar-restore\n");
		else
			console_write("M98-DRV-SMOKE: FAIL bar-restore\n");
	} else {
		console_write("M98-DRV-SMOKE: ok bar-restore\n");
	}
}

static void pci_selftest_caps(const struct pci_device_info *dev)
{
	u16 status = pci_config_read16(dev->bus, dev->slot, dev->func, PCI_CFG_STATUS);
	int have_list = (status & PCI_STATUS_CAP_LIST) ? 1 : 0;

	int found = 0;
	int consistent = 1;
	static const u8 ids[] = {PCI_CAP_ID_PM, PCI_CAP_ID_MSI, PCI_CAP_ID_VENDOR,
	                         PCI_CAP_ID_PCIE, PCI_CAP_ID_MSIX};
	for (usize i = 0; i < sizeof(ids); i++) {
		u8 off = pci_find_capability(dev->bus, dev->slot, dev->func, ids[i]);
		if (!off)
			continue;
		found++;
		/* The offset the walk returned must really hold that capability id. */
		if (pci_config_read8(dev->bus, dev->slot, dev->func, off) != ids[i])
			consistent = 0;
		if (off < 0x40)
			consistent = 0;
	}
	/* A device advertising a capability list must expose at least one. */
	if (have_list && found == 0)
		consistent = 0;

	if (consistent && have_list && found > 0) {
		console_write("M98-DRV-SMOKE: ok cap-walk caps=");
		console_write_dec((u64)found);
		console_write("\n");
	} else {
		console_write("M98-DRV-SMOKE: FAIL cap-walk list=");
		console_write_dec((u64)have_list);
		console_write(" found=");
		console_write_dec((u64)found);
		console_write("\n");
	}

	/* Extended capabilities need ECAM. Report the path taken honestly: an
	 * absent MCFG is a legitimate machine configuration, not a failure, but it
	 * must be reported as unavailable rather than as a pass. */
	if (pci_ecam_available()) {
		u32 id0 = pci_ecam_read32(dev->bus, dev->slot, dev->func, 0x00);
		u32 legacy = pci_config_read32(dev->bus, dev->slot, dev->func, 0x00);
		if (id0 == legacy)
			console_write("M98-DRV-SMOKE: ok ecam-config\n");
		else
			console_write("M98-DRV-SMOKE: FAIL ecam-config\n");
	} else {
		console_write("M98-DRV-SMOKE: skip ecam-config (no MCFG table)\n");
	}
}

static void pci_selftest_busmaster(const struct pci_device_info *dev)
{
	u16 before = pci_config_read16(dev->bus, dev->slot, dev->func, PCI_CFG_COMMAND);
	int set = pci_enable_bus_master(dev->bus, dev->slot, dev->func);
	u16 after = pci_config_read16(dev->bus, dev->slot, dev->func, PCI_CFG_COMMAND);
	/* The bit must read back set from the device, and nothing else in the
	 * command register may have moved. */
	int clean = ((after & ~PCI_CMD_BUS_MASTER) == (before & ~PCI_CMD_BUS_MASTER));
	if (set && (after & PCI_CMD_BUS_MASTER) && clean)
		console_write("M98-DRV-SMOKE: ok bus-master\n");
	else
		console_write("M98-DRV-SMOKE: FAIL bus-master\n");
	if (!(before & PCI_CMD_BUS_MASTER))
		pci_command_clear(dev->bus, dev->slot, dev->func, PCI_CMD_BUS_MASTER);
}

/* Program MSI (or MSI-X) on a device that nothing is currently driving, verify
 * the device holds exactly what we wrote, then restore it. Picking an idle
 * function matters: reprogramming a live driver's interrupts would break it. */
static void pci_selftest_msi(void)
{
	struct pci_device_info dev;
	int have_msi = 0, have_msix = 0;
	struct pci_device_info msi_dev, msix_dev;

	for (u16 bus = 0; bus < 4 && !(have_msi && have_msix); bus++) {
		for (u8 slot = 0; slot < 32 && !(have_msi && have_msix); slot++) {
			if (pci_config_read16((u8)bus, slot, 0, 0) == 0xFFFF)
				continue;
			u8 htype = pci_config_read8((u8)bus, slot, 0, PCI_CFG_HEADER_TYPE);
			u8 nfunc = (htype & 0x80) ? 8 : 1;
			for (u8 f = 0; f < nfunc; f++) {
				if (pci_config_read16((u8)bus, slot, f, 0) == 0xFFFF)
					continue;
				/* Never probe a mass-storage function. Programming MSI on a
				 * device switches it out of INTx for the duration; every other
				 * class here recovers from a missed interrupt (the network
				 * stack polls, HID re-reports), but a lost AHCI/NVMe
				 * completion wedges an I/O that nothing retries. The MSI code
				 * paths are identical whichever function they run against, so
				 * excluding storage costs the test nothing. */
				if (pci_config_read8((u8)bus, slot, f, 0x0B) == 0x01)
					continue;
				dev.bus = (u8)bus;
				dev.slot = slot;
				dev.func = f;
				if (!have_msi &&
				    pci_find_capability((u8)bus, slot, f, PCI_CAP_ID_MSI)) {
					msi_dev = dev;
					have_msi = 1;
				}
				if (!have_msix &&
				    pci_find_capability((u8)bus, slot, f, PCI_CAP_ID_MSIX)) {
					msix_dev = dev;
					have_msix = 1;
				}
			}
		}
	}

	if (have_msi) {
		u64 addr = 0;
		u16 data = 0;
		int en = 0;
		u8 irq = 11;
		/* Snapshot everything this test touches. The function may be one a
		 * b1nix driver is already using over INTx, and pci_msi_enable sets
		 * INTX_DISABLE; leaving that bit set would silence the device for the
		 * rest of the boot. Restore is byte-exact, not "clear what we set". */
		u8 msi_cap = pci_find_capability(msi_dev.bus, msi_dev.slot, msi_dev.func,
		                                 PCI_CAP_ID_MSI);
		u16 saved_cmd = pci_config_read16(msi_dev.bus, msi_dev.slot,
		                                  msi_dev.func, PCI_CFG_COMMAND);
		u32 saved_cap[5];
		for (int i = 0; i < 5; i++)
			saved_cap[i] = pci_config_read32(msi_dev.bus, msi_dev.slot,
			                                 msi_dev.func,
			                                 (u8)(msi_cap + i * 4));
		int rc = pci_msi_enable(msi_dev.bus, msi_dev.slot, msi_dev.func, irq);
		int rb = pci_msi_readback(msi_dev.bus, msi_dev.slot, msi_dev.func, &addr,
		                          &data, &en);
		u64 want_addr = 0xFEE00000ULL | ((u64)(lapic_id() & 0xFF) << 12);
		if (rc == 0 && rb == 0 && en && addr == want_addr &&
		    data == (u16)(32 + irq)) {
			console_write("M98-DRV-SMOKE: ok msi-config\n");
		} else {
			console_write("M98-DRV-SMOKE: FAIL msi-config addr=0x");
			console_write_hex64(addr);
			console_write(" data=0x");
			console_write_hex64((u64)data);
			console_write("\n");
		}
		pci_msi_disable(msi_dev.bus, msi_dev.slot, msi_dev.func);
		for (int i = 0; i < 5; i++)
			pci_config_write32(msi_dev.bus, msi_dev.slot, msi_dev.func,
			                   (u8)(msi_cap + i * 4), saved_cap[i]);
		pci_config_write16(msi_dev.bus, msi_dev.slot, msi_dev.func,
		                   PCI_CFG_COMMAND, saved_cmd);
	} else {
		console_write("M98-DRV-SMOKE: skip msi-config (no MSI-capable function)\n");
	}

	if (have_msix) {
		int n = pci_msix_table_size(msix_dev.bus, msix_dev.slot, msix_dev.func);
		u8 irq = 10;
		/* Same reasoning as MSI: snapshot the capability's control word, the
		 * command register and the table entry, and put all three back. A
		 * virtio function driven over INTx would otherwise be left with MSI-X
		 * enabled and its legacy pin disabled. */
		u8 msix_cap = pci_find_capability(msix_dev.bus, msix_dev.slot,
		                                  msix_dev.func, PCI_CAP_ID_MSIX);
		u16 saved_cmd = pci_config_read16(msix_dev.bus, msix_dev.slot,
		                                  msix_dev.func, PCI_CFG_COMMAND);
		u16 saved_ctrl = pci_config_read16(msix_dev.bus, msix_dev.slot,
		                                   msix_dev.func,
		                                   (u8)(msix_cap + 0x02));
		u64 saved_e_addr = 0;
		u32 saved_e_data = 0, saved_e_ctrl = 0;
		int had_entry = pci_msix_entry_readback(msix_dev.bus, msix_dev.slot,
		                                        msix_dev.func, 0, &saved_e_addr,
		                                        &saved_e_data, &saved_e_ctrl) == 0;
		int rc = pci_msix_enable(msix_dev.bus, msix_dev.slot, msix_dev.func, 0, irq);
		u64 addr = 0;
		u32 data = 0, vctrl = 0xFFFFFFFFu;
		int rb = pci_msix_entry_readback(msix_dev.bus, msix_dev.slot,
		                                 msix_dev.func, 0, &addr, &data, &vctrl);
		u64 want_addr = 0xFEE00000ULL | ((u64)(lapic_id() & 0xFF) << 12);
		if (n > 0 && rc == 0 && rb == 0 && addr == want_addr &&
		    data == (u32)(32 + irq) && (vctrl & 1u) == 0) {
			console_write("M98-DRV-SMOKE: ok msix-config vectors=");
			console_write_dec((u64)n);
			console_write("\n");
		} else {
			console_write("M98-DRV-SMOKE: FAIL msix-config n=");
			console_write_dec((u64)n);
			console_write(" addr=0x");
			console_write_hex64(addr);
			console_write(" data=0x");
			console_write_hex64((u64)data);
			console_write("\n");
		}
		pci_msix_disable(msix_dev.bus, msix_dev.slot, msix_dev.func);
		if (had_entry)
			pci_msix_entry_restore(msix_dev.bus, msix_dev.slot, msix_dev.func, 0,
			                       saved_e_addr, saved_e_data, saved_e_ctrl);
		pci_config_write16(msix_dev.bus, msix_dev.slot, msix_dev.func,
		                   (u8)(msix_cap + 0x02), saved_ctrl);
		pci_config_write16(msix_dev.bus, msix_dev.slot, msix_dev.func,
		                   PCI_CFG_COMMAND, saved_cmd);
	} else {
		console_write("M98-DRV-SMOKE: skip msix-config (no MSI-X-capable function)\n");
	}
}

static void pci_selftest_stolen(void)
{
	struct pci_intel_stolen st;
	int rc = pci_intel_stolen_read(&st);
	u16 host_vendor = pci_config_read16(0, 0, 0, 0x00);

	if (rc == 0) {
		/* Real Intel graphics: the bases must be 1 MiB aligned and the DSM
		 * window must lie below 4 GiB, where BDSM can address it. */
		int sane = (st.dsm_base & 0xFFFFF) == 0 && (st.gsm_base & 0xFFFFF) == 0 &&
		           st.dsm_size > 0 && st.dsm_base < 0x100000000ULL;
		console_write(sane ? "M98-DRV-SMOKE: ok stolen present base=0x"
		                   : "M98-DRV-SMOKE: FAIL stolen base=0x");
		console_write_hex64(st.dsm_base);
		console_write(" size=0x");
		console_write_hex64(st.dsm_size);
		console_write("\n");
	} else if (host_vendor != 0x8086) {
		console_write("M98-DRV-SMOKE: ok stolen absent (non-Intel host bridge)\n");
	} else {
		/* Intel host bridge with no graphics stolen memory — QEMU's Q35 and
		 * 440FX both land here. Reporting absence IS the correct answer, and it
		 * is what the code must return rather than a fabricated aperture. */
		console_write("M98-DRV-SMOKE: ok stolen absent (no GSM on this bridge)\n");
	}
}

void pci_selftest(void)
{
	if (!bootinfo_has_flag("b1nix.test=1"))
		return;

	struct pci_device_info dev;
	if (!pci_pick_test_device(&dev)) {
		console_write("M98-DRV-SMOKE: FAIL bar-enum no-device\n");
		return;
	}

	pci_selftest_bars(&dev);
	pci_selftest_caps(&dev);
	pci_selftest_busmaster(&dev);
	pci_selftest_msi();
	pci_selftest_stolen();
}
