/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * QEMU's fw_cfg channel, and the one thing b1nix needs from it: the graphics
 * OpRegion of a passed-through Intel display.
 *
 * A real machine's firmware builds an OpRegion in memory and writes its address
 * into the display device's ASLS configuration register; the driver reads it
 * back and finds the VBT inside — the board's own description of which port is
 * wired where, and the electrical parameters its traces need. QEMU prepares the
 * same OpRegion and offers it here as etc/igd-opregion, expecting the guest
 * firmware to place it. SeaBIOS only does that for a device of VGA class, and a
 * passed-through iGPU that is not driving the console presents as a secondary
 * display controller instead — so on this path nothing ever places it, the
 * driver finds no VBT, and it falls back to defaults.
 *
 * Defaults are enough to light a pipe and drive a DDI. They are not enough for
 * a receiver to lock onto the result: the signal levels and pre-emphasis are
 * board data, and getting them wrong produces exactly what is seen here — a
 * transcoder enabled, a frame counter advancing, and a monitor that wakes, finds
 * nothing it can decode, and goes back to sleep.
 *
 * So this does the firmware's job. It reads the OpRegion out of fw_cfg, copies
 * it somewhere the device can reach, and writes that address into ASLS before
 * the driver probes.
 *
 * What it does not do: the DMA interface. The port interface is a byte stream
 * and an OpRegion is 8 KiB, which is a few thousand reads at boot — not worth a
 * second code path.
 */

#include <b1nix/fw_cfg.h>

#include <b1nix/io.h>
#include <stdio.h>
#include <b1nix/console.h>
#include <b1nix/klog.h>
#include <b1nix/mm.h>
#include <b1nix/pci.h>
#include <string.h>

#define FW_CFG_PORT_SEL  0x510
#define FW_CFG_PORT_DATA 0x511

#define FW_CFG_SIGNATURE 0x0000
#define FW_CFG_FILE_DIR  0x0019

/* The directory and its entries are big-endian, being a firmware interface. */
static u32 be32(u32 v)
{
	return ((v & 0xff) << 24) | ((v & 0xff00) << 8) | ((v >> 8) & 0xff00) |
	       ((v >> 24) & 0xff);
}

static void fw_cfg_select(u16 key)
{
	outw(FW_CFG_PORT_SEL, key);
}

static void fw_cfg_read(void *buf, usize len)
{
	u8 *out = buf;

	for (usize i = 0; i < len; i++)
		out[i] = inb(FW_CFG_PORT_DATA);
}

int fw_cfg_present(void)
{
	char sig[4];

	fw_cfg_select(FW_CFG_SIGNATURE);
	fw_cfg_read(sig, sizeof(sig));
	return sig[0] == 'Q' && sig[1] == 'E' && sig[2] == 'M' && sig[3] == 'U';
}

/*
 * Find a named file and report the key to select it by, and its size.
 *
 * The directory is a count followed by fixed-size entries, and it can only be
 * read forwards — so the whole thing is walked even after a match, or rather
 * the walk simply stops, because nothing else reads it afterwards.
 */
static int fw_cfg_find(const char *name, u16 *out_key, u32 *out_size)
{
	struct fw_cfg_file {
		u32 size;
		u16 select;
		u16 reserved;
		char name[56];
	} entry;
	u32 count;

	fw_cfg_select(FW_CFG_FILE_DIR);
	fw_cfg_read(&count, sizeof(count));
	count = be32(count);

	/* A directory larger than this is not a directory. The bound matters
	 * because a wrong read here would otherwise walk the port forever. */
	if (count > 512)
		return 0;

	for (u32 i = 0; i < count; i++) {
		fw_cfg_read(&entry, sizeof(entry));
		entry.name[sizeof(entry.name) - 1] = 0;
		if (strcmp(entry.name, name) == 0) {
			*out_key = (u16)((entry.select >> 8) | (entry.select << 8));
			*out_size = be32(entry.size);
			return 1;
		}
	}
	return 0;
}

/*
 * Place the OpRegion for the display at `bus:slot.func`, if QEMU is offering
 * one and the device has no OpRegion already.
 *
 * Returns 1 when ASLS was written.
 */
int fw_cfg_place_igd_opregion(u8 bus, u8 slot, u8 func)
{
	/* ASLS — the "ACPI software scratch" register the graphics driver reads its
	 * OpRegion address out of. Its location is architectural for Intel display
	 * devices. */
	const u8 ASLS_OFFSET = 0xfc;
	u16 key;
	u32 asls;
	u32 size;
	u64 phys;
	void *dst;

	/* Each refusal says which one it was: "no OpRegion" has three quite
	 * different causes and they need different answers. */
	asls = pci_config_read32(bus, slot, func, ASLS_OFFSET);
	console_write("fw_cfg: looking for a graphics OpRegion\n");

	if (asls != 0) {
		console_write("fw_cfg: the display already has an OpRegion; leaving it\n");
		return 0;
	}

	if (!fw_cfg_present()) {
		console_write("fw_cfg: no fw_cfg channel on this machine\n");
		return 0;
	}

	if (!fw_cfg_find("etc/igd-opregion", &key, &size) || size == 0) {
		console_write("fw_cfg: QEMU is not offering etc/igd-opregion\n");
		return 0;
	}

	/*
	 * Below 4 GiB, because ASLS is a 32-bit register: an OpRegion above that
	 * line cannot be described to the device at all. Page-aligned because the
	 * driver maps it.
	 */
	phys = pmm_alloc_frames_below((size + PAGE_SIZE - 1) / PAGE_SIZE,
	                              0xffffffffull);
	if (!phys) {
		console_write("fw_cfg: no memory below 4 GiB for the graphics OpRegion\n");
		return 0;
	}

	dst = (void *)(usize)(phys + DIRECT_MAP_BASE);
	fw_cfg_select(key);
	fw_cfg_read(dst, size);

	pci_config_write32(bus, slot, func, ASLS_OFFSET, (u32)phys);

	{
		char line[96];

		snprintf(line, sizeof(line),
		         "fw_cfg: placed a %u-byte graphics OpRegion at %08x\n", size,
		         (unsigned)phys);
		console_write(line);
	}
	return 1;
}
