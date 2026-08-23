#ifndef B1NIX_BOOTINFO_H
#define B1NIX_BOOTINFO_H

#include <b1nix/types.h>

#define BOOTINFO_MAX_MEMORY_REGIONS 32

enum boot_memory_region_type {
	BOOT_MEMORY_AVAILABLE = 1,
	BOOT_MEMORY_RESERVED = 2,
	BOOT_MEMORY_ACPI_RECLAIMABLE = 3,
	BOOT_MEMORY_NVS = 4,
	BOOT_MEMORY_BADRAM = 5,
};

struct boot_memory_region {
	u64 base;
	u64 length;
	u32 type;
};

struct boot_framebuffer {
	u64 address;
	u32 pitch;
	u32 width;
	u32 height;
	u8 bpp;
	u8 type;
};

struct boot_info {
	struct boot_memory_region memory_regions[BOOTINFO_MAX_MEMORY_REGIONS];
	usize memory_region_count;
	struct boot_framebuffer framebuffer;
	int has_framebuffer;
	/* 128 bytes silently truncated the boot line: a passthrough run passes
	 * eight b1nix.* options and the last of them fell off the end, so the
	 * kernel behaved as if it had never been asked. Linux allows 2048 here;
	 * this is the same order, and the copy now says when it has to cut. */
	char command_line[1024];
	u64 ramdisk_addr;
	u64 ramdisk_size;
	int has_ramdisk;
};

void bootinfo_init_from_multiboot2(u32 magic, u32 info_address);
const struct boot_info *bootinfo_get(void);
const char *bootinfo_cmdline(void);
int bootinfo_has_flag(const char *flag);

/*
 * Look up a "key=value" token on the kernel command line. On a match the
 * value is copied into out (always NUL-terminated, truncated to out_size-1)
 * and 1 is returned; out is left untouched and 0 is returned otherwise. The
 * key must match a whole token up to '=' (so "foo" never matches "foobar=x").
 * out may be NULL / out_size 0 to test presence only.
 */
int bootinfo_get_kv(const char *key, char *out, usize out_size);

/*
 * Decimal "key=N" from the command line, or fallback when the key is absent,
 * empty, or not a number. The single parser behind every `b1nix.*=N` tunable,
 * so an explicit override has the same precedence everywhere.
 */
u32 bootinfo_get_u32(const char *key, u32 fallback);

#if defined(__aarch64__)
/* The device tree the loader passes in x0, read before the console exists:
 * the UART's base address comes out of it, so nothing can be printed until it
 * has been walked. Idempotent — bootinfo_init_from_fdt() calls it too, and
 * then reports what was found. */
void bootinfo_fdt_scan(u64 dtb_address);
void bootinfo_init_from_fdt(u64 dtb_address);

/* CPUs the device tree lists, and the MPIDR each one reports — the id PSCI's
 * CPU_ON takes as its target. */
u32 fdt_cpu_count(void);
u64 fdt_cpu_mpidr(u32 index);

/* How the firmware parked the secondaries, and where a spin-table one polls
 * for its entry address. */
#define FDT_ENABLE_METHOD_PSCI       0
#define FDT_ENABLE_METHOD_SPIN_TABLE 1
int fdt_cpu_enable_method(void);
u64 fdt_cpu_release_addr(u32 index);

/* Board addresses taken from the tree, falling back to QEMU virt's when it
 * does not say: the GICv2 distributor and CPU interface, and the console UART. */
u64 fdt_gicd_base(void);
u64 fdt_gicc_base(void);
/* GICv3: the redistributor region (one frame per CPU) and, when the tree has
 * one, the ITS — the only path to message-signalled interrupts on this board.
 * fdt_gic_is_v3() is 0 on a GICv2 machine, where the two above are all there
 * is. */
u64 fdt_gicr_base(void);
u64 fdt_gicr_size(void);
int fdt_gic_is_v3(void);
u64 fdt_its_base(void);
u64 fdt_its_size(void);
u64 fdt_uart_base(void);
/* The same console base as a variable, for the console write that happens
 * before any of the above can be called. */
extern u64 g_aarch64_uart_base;

/* The ARM SMMUv3 the tree describes, if the board has one. QEMU virt only
 * grows the node under `-machine virt,iommu=smmuv3`, so a zero base means
 * "this machine has no DMA remapping unit", not "the walk failed". */
u64 fdt_smmuv3_base(void);
u64 fdt_smmuv3_size(void);
u32 fdt_smmuv3_irq_count(void);
u32 fdt_smmuv3_irq(u32 index);

/* Translate a PCI requester id into the StreamID the SMMU indexes its stream
 * table by, using the host bridge's `iommu-map`. Returns 0 when this requester
 * is not routed through the unit at all. */
int fdt_pci_stream_id(u16 rid, u32 *sid_out);

/* Which controller that base names. A Raspberry Pi 4 running the stock
 * firmware config wires the PL011 to Bluetooth and leaves the console on the
 * BCM2835 mini-UART, so both have to be drivable; the two share no register
 * layout, so serial.c switches on this. */
extern int g_aarch64_uart_is_mini;
/* Base of the AUX block the mini-UART sits in — AUX_ENABLES, the register that
 * makes the port answer at all, is at + 0x04 of it. Zero when the console is a
 * PL011. */
extern u64 g_aarch64_aux_base;

/* The PCI host bridge the tree describes, if any. There is no default: a board
 * whose tree names no host bridge gets no bus scan, because the addresses that
 * used to be compiled in are QEMU virt's and are RAM on a Raspberry Pi. */
#define FDT_PCI_HOST_NONE    0
#define FDT_PCI_HOST_ECAM    1  /* "pci-host-ecam-generic": config space is a
                                 * flat window, bus << 20 | dev << 15 | fn << 12 */
#define FDT_PCI_HOST_BCM2711 2  /* "brcm,bcm2711-pcie": a controller that has to
                                 * be brought up before config space answers */
int fdt_pci_host_kind(void);
/* reg[0] of the host bridge node: the ECAM window for a generic one, the
 * controller's own registers for a BCM2711. */
u64 fdt_pci_cfg_base(void);
u64 fdt_pci_cfg_size(void);
/* The memory window behind the bridge, from its `ranges`: the CPU addresses it
 * occupies, what those are on the bus, and how big it is. BARs are assigned out
 * of this. */
/* The Broadcom SoC on a Raspberry Pi. Each is zero on a board whose device
 * tree describes no such block. */
u64 fdt_mbox_base(void);
u64 fdt_gpio_base(void);
u64 fdt_pm_base(void);
u64 fdt_systimer_base(void);
u64 fdt_genet_base(void);

u64 fdt_pci_mmio_base(void);
/* The host bridge's port-I/O window: where a PCI card's I/O BAR lands on a
 * machine whose CPU has no port-I/O instructions. */
u64 fdt_pci_io_base(void);
u64 fdt_pci_io_pci_base(void);
u64 fdt_pci_io_size(void);
u64 fdt_pci_mmio_pci_base(void);
u64 fdt_pci_mmio_size(void);

/* The BCM2711's EMMC2 host, the SD card controller a Raspberry Pi 4 boots
 * from. Zero when the tree describes none. */
u64 fdt_emmc2_base(void);
#endif

#endif
