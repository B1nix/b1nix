#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/pci.h>
#include <b1nix/types.h>
#include <b1nix/video.h>

#ifdef __aarch64__

void video_init(void)
{
	console_write("video: pci display scan unavailable on aarch64\n");
}

void video_dump_info(void)
{
	console_write("Video\n");
	console_write(" pci: unavailable on this arch\n");
}

usize video_adapter_count(void)
{
	return 0;
}

#else

/* Framebuffer parameters (resolution, bpp, pitch, base address) are
 * supplied by GRUB via Multiboot2 and consumed at runtime through
 * bootinfo_get()->framebuffer. There is no hardcoded mode. Real
 * dynamic mode-setting (VBE INT 10h or UEFI GOP) is out of scope:
 * we boot via BIOS+GRUB in long mode, so INT 10h would require a
 * v86 emulator we don't have; GOP would require UEFI boot. Already
 * runtime-driven via the boot framebuffer; deeper mode-set deferred. */
#define VIDEO_MAX_ADAPTERS 8

struct video_adapter {
	struct pci_device_info pci;
	u32 bars[6];
};

static struct video_adapter adapters[VIDEO_MAX_ADAPTERS];
static usize adapter_count;
static struct boot_framebuffer bootfb;
static int has_bootfb;

static const char *vendor_name(u16 vendor)
{
	switch (vendor) {
	case 0x1002: return "AMD";
	case 0x1013: return "Cirrus";
	case 0x102b: return "Matrox";
	case 0x10de: return "NVIDIA";
	case 0x1234: return "QEMU/Bochs";
	case 0x1414: return "Microsoft";
	case 0x15ad: return "VMware";
	case 0x1af4: return "VirtIO";
	case 0x8086: return "Intel";
	default: return "unknown";
	}
}

static const char *display_kind(u8 subclass)
{
	switch (subclass) {
	case 0x00: return "VGA";
	case 0x01: return "XGA";
	case 0x02: return "3D";
	default: return "display";
	}
}

static void print_pci_bdf(const struct pci_device_info *pci)
{
	console_write_hex32(pci->bus);
	console_putc(':');
	console_write_hex32(pci->slot);
	console_putc('.');
	console_write_hex32(pci->func);
}

static void print_bar(u32 bar)
{
	if (bar == 0) {
		console_write("none");
		return;
	}
	if (bar & 1) {
		console_write("io 0x");
		console_write_hex32(bar & ~3U);
		return;
	}
	console_write("mem 0x");
	console_write_hex32(bar & ~0xfU);
	if ((bar & 0x6) == 0x4) {
		console_write(" 64-bit");
	}
}

void video_init(void)
{
	const struct boot_info *info = bootinfo_get();
	has_bootfb = info->has_framebuffer;
	if (has_bootfb) {
		bootfb = info->framebuffer;
	}

	adapter_count = 0;
	for (u8 idx = 0; idx < VIDEO_MAX_ADAPTERS; idx++) {
		struct pci_device_info pci;
		if (!pci_find_class(0x03, 0x00, idx, &pci)) {
			break;
		}

		struct video_adapter *adapter = &adapters[adapter_count++];
		adapter->pci = pci;
		for (u8 bar = 0; bar < 6; bar++) {
			adapter->bars[bar] = pci_config_read32(pci.bus, pci.slot, pci.func, (u8)(0x10 + bar * 4));
		}
	}

	for (u8 idx = 0; adapter_count < VIDEO_MAX_ADAPTERS; idx++) {
		struct pci_device_info pci;
		if (!pci_find_class(0x03, 0x02, idx, &pci)) {
			break;
		}

		struct video_adapter *adapter = &adapters[adapter_count++];
		adapter->pci = pci;
		for (u8 bar = 0; bar < 6; bar++) {
			adapter->bars[bar] = pci_config_read32(pci.bus, pci.slot, pci.func, (u8)(0x10 + bar * 4));
		}
	}

	console_write("video: bootfb ");
	console_write(has_bootfb ? "yes" : "no");
	console_write(", pci adapters 0x");
	console_write_hex64(adapter_count);
	console_write("\n");
}

void video_dump_info(void)
{
	console_write("Video\n");
	if (has_bootfb) {
		console_write(" bootfb: addr 0x");
		console_write_hex64(bootfb.address);
		console_write(" ");
		console_write_dec(bootfb.width);
		console_putc('x');
		console_write_dec(bootfb.height);
		console_write("x");
		console_write_dec(bootfb.bpp);
		console_write(" pitch ");
		console_write_dec(bootfb.pitch);
		console_write("\n");
	} else {
		console_write(" bootfb: none\n");
	}

	if (adapter_count == 0) {
		console_write(" pci: no display adapters found\n");
		return;
	}

	for (usize i = 0; i < adapter_count; i++) {
		const struct video_adapter *adapter = &adapters[i];
		const struct pci_device_info *pci = &adapter->pci;
		console_write(" pci ");
		print_pci_bdf(pci);
		console_putc(' ');
		console_write(vendor_name(pci->vendor_id));
		console_putc(' ');
		console_write(display_kind(pci->subclass));
		console_write(" vendor 0x");
		console_write_hex32(pci->vendor_id);
		console_write(" device 0x");
		console_write_hex32(pci->device_id);
		console_write(" prog_if 0x");
		console_write_hex32(pci->prog_if);
		console_write("\n");

		for (u8 bar = 0; bar < 6; bar++) {
			if (adapter->bars[bar] == 0) continue;
			console_write("  bar");
			console_write_dec(bar);
			console_write(": ");
			print_bar(adapter->bars[bar]);
			console_write("\n");
		}
	}
}

usize video_adapter_count(void)
{
	return adapter_count;
}

#endif
