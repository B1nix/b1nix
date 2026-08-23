#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/panic.h>
#include <b1nix/platform.h>
#include <string.h>
#include "platform.h"

static struct boot_info global_bootinfo;

#ifndef AARCH64_DEFAULT_CMDLINE
#define AARCH64_DEFAULT_CMDLINE "init=/bin/m12_smoke b1nix.test=1 b1nix.kvtest=abc123 b1nix.ssh-loopback=1"
#endif
static char aarch64_cmdline_buf[512] = AARCH64_DEFAULT_CMDLINE;

/* ── Board addresses ─────────────────────────────────────────────────────────
 *
 * Board addresses are detected via platform_detect(dtb_addr) and verified/refined
 * against the device tree passed in x0. */
static u64 g_gicd_base;
static u64 g_gicc_base;
static u64 g_gicr_base;   /* GICv3 redistributor region */
static u64 g_gicr_size;
static u64 g_emmc2_base;   /* BCM2711 EMMC2 (SD card) */
/* The rest of a Raspberry Pi's SoC. Every one of these is zero on a board
 * whose tree does not describe it, which is how each driver decides whether
 * it has anything to drive. */
static u64 g_mbox_base;     /* VideoCore mailbox (the firmware interface) */
static u64 g_gpio_base;
static u64 g_pm_base;       /* power management: the reset/watchdog block */
static u64 g_systimer_base;
static u64 g_genet_base;    /* the Pi 4's Ethernet MAC */
static u64 g_its_base;    /* GICv3 ITS, when the tree has one */
static u64 g_its_size;
static int g_gic_is_v3;
/* Read by serial.c on the very first console write, so it has to be a plain
 * variable with a working default rather than a call that can fail. */
u64 g_aarch64_uart_base;
static int g_uart_from_fdt;
static int g_gic_from_fdt;
/* The board's console is a BCM2835 mini-UART ("aux" UART) rather than a PL011.
 * The two share no register at all, so serial.c reads this to pick which set of
 * offsets g_aarch64_uart_base names. */
int g_aarch64_uart_is_mini;
/* The AUX block the mini-UART lives in. Its three peripherals (the mini-UART
 * and two SPI masters) share one enable register at AUX_ENABLES, block + 0x04,
 * and nothing in the mini-UART's own window answers until the bit for it is
 * set — so the driver needs the block's base as well as the port's. */
u64 g_aarch64_aux_base;

/* The PCI host bridge, from the tree. Nothing is assumed when the tree
 * describes none: a board with no host bridge gets no bus scan, rather than a
 * scan of whatever happens to live at QEMU virt's addresses. */
static int g_pci_host_kind = FDT_PCI_HOST_NONE;
static u64 g_pci_cfg_base;
static u64 g_pci_cfg_size;
static u64 g_pci_mmio_base;      /* CPU address of the MMIO window */
static u64 g_pci_io_base;        /* CPU address of the port-I/O window */
static u64 g_pci_io_pci_base;    /* the same window's address on the bus */
static u64 g_pci_io_size;
static u64 g_pci_mmio_pci_base;  /* what that window is on the bus */
static u64 g_pci_mmio_size;
/* Set once a real RAM bank has been read, so the fallback bank planted before
 * the walk is replaced rather than added to. */
static int g_ram_from_fdt;

static u32 fdt32_to_cpu(u32 val) {
    return ((val & 0xFF000000u) >> 24) |
           ((val & 0x00FF0000u) >> 8) |
           ((val & 0x0000FF00u) << 8) |
           ((val & 0x000000FFu) << 24);
}

static u64 fdt_read_cells(const u32 *p, u32 cells)
{
	u64 v = 0;
	for (u32 i = 0; i < cells; i++)
		v = (v << 32) | fdt32_to_cpu(p[i]);
	return v;
}

/* ── PCI INTx routing ────────────────────────────────────────────────────────
 *
 * A PCI device's config-space Interrupt Line (0x3C) is an x86 idea: it holds
 * the 8259/IOAPIC input the firmware wired the device to. No firmware runs
 * before this kernel on an arm64 board, so that byte is meaningless here and
 * every driver that trusted it registered its handler on a line no interrupt
 * ever arrives at — AHCI, NVMe, virtio-blk, virtio-gpu and e1000 all fell back
 * to polling without saying so.
 *
 * What replaces it is the host bridge's `interrupt-map`, a table the device
 * tree carries: (child address, child INTx pin) -> (interrupt parent, parent
 * specifier). The entries are captured verbatim here, during the walk that is
 * already reading this tree, and decoded on demand by pci_intx_gsi().
 *
 * Cell counts are assumed rather than followed from each node: 3 address cells
 * and 1 interrupt cell on the PCI side, a phandle, and 3 cells on the GIC side
 * (type, number, flags). That is the layout of every arm64 board with a GIC,
 * and an entry table that does not divide into it is rejected outright so the
 * caller falls back rather than reading a mis-parsed line. */
#define FDT_INTMAP_MAX_CELLS 256
static u32 g_intmap[FDT_INTMAP_MAX_CELLS];
static u32 g_intmap_cells;    /* cells actually captured */
static u32 g_intmap_stride;   /* cells per entry, 0 until validated */
static u32 g_intmap_mask[4] = {0xffffffffu, 0, 0, 0xffffffffu};

/* Cells per entry, worked out from the table itself.
 *
 * An entry is: child address (3 cells) + child interrupt (1) + the parent's
 * phandle (1) + the parent's own specifier, whose width depends on the
 * interrupt controller's #address-cells and #interrupt-cells. Following that
 * properly means resolving the phandle to a node that, in this tree, is
 * written AFTER the bridge — a second pass, for a number that only ever takes
 * one of a few values. QEMU virt's GIC declares two address cells, so an entry
 * is 3+1+1+2+3 = 10 cells; a controller with none makes it 8. Assuming 8 read
 * the SPI number out of the padding and every device came out on GIC ID 32.
 *
 * So try each width and keep the one whose every entry is internally
 * consistent: an INTx pin of 1..4, a non-zero parent phandle, an interrupt
 * type of SPI or PPI, and a plausible interrupt number. A table that fits none
 * of them is rejected, and the caller falls back. */
static u32 fdt_intmap_stride(const u32 *cells, u32 count)
{
	for (u32 stride = 8; stride <= 11; stride++) {
		if (count % stride)
			continue;

		int ok = 1;
		for (u32 i = 0; i < count && ok; i += stride) {
			u32 pin = cells[i + 3];
			u32 phandle = cells[i + 4];
			u32 type = cells[i + stride - 3];
			u32 number = cells[i + stride - 2];

			if (pin < 1 || pin > 4 || phandle == 0 || type > 1 || number > 1020)
				ok = 0;
		}
		if (ok)
			return stride;
	}
	return 0;
}

/* ── SMMUv3 and the stream ids in front of it ────────────────────────────────
 *
 * QEMU virt only grows a `smmuv3@...` node when it is asked for one
 * (`-machine virt,iommu=smmuv3`), so absence here is the ordinary case and not
 * an error. What the tree says is the unit's register window, and — on the
 * PCIe host bridge — an `iommu-map`, which is what turns a PCI requester id
 * into the StreamID the unit indexes its stream table by.
 *
 * `iommu-map` is a list of quadruples: <rid-base, iommu-phandle, sid-base,
 * length>. The phandle has to be resolved against the SMMU node's own
 * `phandle`, and that node may be written either side of the bridge, so both
 * are captured verbatim during the single walk and matched afterwards in
 * fdt_pci_stream_id(). */
#define FDT_IOMMU_MAP_MAX_CELLS 32
static u64 g_smmu_base;
static u64 g_smmu_size;
static u32 g_smmu_phandle;
static u32 g_smmu_irq[4];      /* eventq, priq, cmdq-sync, gerror — SPI numbers */
static u32 g_smmu_irq_count;
static u32 g_iommu_map[FDT_IOMMU_MAP_MAX_CELLS];
static u32 g_iommu_map_cells;

u64 fdt_smmuv3_base(void) { return g_smmu_base; }
u64 fdt_smmuv3_size(void) { return g_smmu_size; }
u32 fdt_smmuv3_irq_count(void) { return g_smmu_irq_count; }
u32 fdt_smmuv3_irq(u32 index)
{
	return index < g_smmu_irq_count ? g_smmu_irq[index] : 0;
}

int fdt_pci_stream_id(u16 rid, u32 *sid_out)
{
	if (!g_smmu_base || !g_iommu_map_cells)
		return 0;
	for (u32 i = 0; i + 4 <= g_iommu_map_cells; i += 4) {
		u32 rid_base = g_iommu_map[i];
		u32 phandle = g_iommu_map[i + 1];
		u32 sid_base = g_iommu_map[i + 2];
		u32 length = g_iommu_map[i + 3];

		if (g_smmu_phandle && phandle != g_smmu_phandle)
			continue;
		if ((u32)rid < rid_base || (u32)rid - rid_base >= length)
			continue;
		if (sid_out)
			*sid_out = sid_base + ((u32)rid - rid_base);
		return 1;
	}
	return 0;
}

/* ── Secondary CPUs ──────────────────────────────────────────────────────────
 *
 * Each /cpus/cpu@N node carries the value this CPU's MPIDR_EL1 reports, which
 * is the id PSCI's CPU_ON takes as its target, and the method by which it is
 * started. QEMU virt parks its CPUs in PSCI; a Raspberry Pi's firmware parks
 * them in a spin-table, each one polling the address in its own
 * `cpu-release-addr`. The list is captured here for smp_boot_aps(); how many
 * of them are actually started is its decision. */
#define FDT_MAX_CPUS 64
static u64 g_cpu_mpidr[FDT_MAX_CPUS];
static u64 g_cpu_release_addr[FDT_MAX_CPUS];
static u32 g_cpu_count;
static int g_cpu_enable_method = FDT_ENABLE_METHOD_PSCI;

u32 fdt_cpu_count(void) { return g_cpu_count; }
u64 fdt_cpu_mpidr(u32 index)
{
	return index < g_cpu_count ? g_cpu_mpidr[index] : 0;
}
u64 fdt_cpu_release_addr(u32 index)
{
	return index < g_cpu_count ? g_cpu_release_addr[index] : 0;
}
int fdt_cpu_enable_method(void) { return g_cpu_enable_method; }

u64 fdt_gicd_base(void) { return g_gicd_base; }
u64 fdt_gicc_base(void) { return g_gicc_base; }
u64 fdt_gicr_base(void) { return g_gicr_base; }
u64 fdt_gicr_size(void) { return g_gicr_size; }
int fdt_gic_is_v3(void) { return g_gic_is_v3; }
u64 fdt_its_base(void) { return g_its_base; }
u64 fdt_emmc2_base(void) { return g_emmc2_base; }
u64 fdt_mbox_base(void) { return g_mbox_base; }
u64 fdt_gpio_base(void) { return g_gpio_base; }
u64 fdt_pm_base(void) { return g_pm_base; }
u64 fdt_systimer_base(void) { return g_systimer_base; }
u64 fdt_genet_base(void) { return g_genet_base; }
u64 fdt_its_size(void) { return g_its_size; }
u64 fdt_uart_base(void) { return g_aarch64_uart_base; }

int fdt_pci_host_kind(void) { return g_pci_host_kind; }
u64 fdt_pci_cfg_base(void) { return g_pci_cfg_base; }
u64 fdt_pci_cfg_size(void) { return g_pci_cfg_size; }
u64 fdt_pci_mmio_base(void) { return g_pci_mmio_base; }
u64 fdt_pci_io_base(void) { return g_pci_io_base; }
u64 fdt_pci_io_pci_base(void) { return g_pci_io_pci_base; }
u64 fdt_pci_io_size(void) { return g_pci_io_size; }
u64 fdt_pci_mmio_pci_base(void) { return g_pci_mmio_pci_base; }
u64 fdt_pci_mmio_size(void) { return g_pci_mmio_size; }

/* GIC interrupt ID for a device's INTx pin (1=INTA .. 4=INTD), or 0 when the
 * tree does not describe one. SPIs start at 32 and PPIs at 16 — the device
 * tree numbers them from each range's own base. */
u32 pci_intx_gsi(u8 bus, u8 slot, u8 func, u8 pin)
{
	if (!g_intmap_stride || pin < 1 || pin > 4)
		return 0;

	u32 child_hi = ((u32)bus << 16) | ((u32)slot << 11) | ((u32)func << 8);

	for (u32 i = 0; i < g_intmap_cells; i += g_intmap_stride) {
		const u32 *e = &g_intmap[i];

		if ((child_hi & g_intmap_mask[0]) != (e[0] & g_intmap_mask[0]))
			continue;
		if (((u32)pin & g_intmap_mask[3]) != (e[3] & g_intmap_mask[3]))
			continue;

		u32 type = e[g_intmap_stride - 3], number = e[g_intmap_stride - 2];
		if (type == 0)
			return number + 32; /* SPI */
		return number + 16;     /* PPI */
	}
	return 0;
}

/* ── The walk ────────────────────────────────────────────────────────────────
 *
 * A node's properties may appear in any order, and several of them only make
 * sense together (a `reg` means one thing in a node whose `compatible` says
 * PL011 and another in one that says GIC — and `compatible` is written after
 * `reg` in QEMU's own tree). So each node's interesting properties are
 * captured as pointers into the blob while it is open, and the node is
 * interpreted at its FDT_END_NODE, when all of them have been seen.
 *
 * Addresses are translated on the way out through every ancestor's `ranges`.
 * That is not optional here: a Raspberry Pi's tree puts its devices under
 * /soc, whose `ranges` maps the legacy bus address 0x7e000000 onto the real
 * 0xfe000000 — programming the untranslated address writes into RAM. */
#define FDT_MAX_DEPTH 16
#define FDT_MAX_RANGES 8
#define FDT_MAX_ALIASES 12
#define FDT_PATH_MAX 192

struct fdt_node {
	const char *name;
	u32 path_len;              /* length of g_path once this node is open */
	const u32 *reg;
	u32 reg_len;
	const char *compatible;
	u32 compatible_len;
	const char *device_type;
	const char *enable_method;
	const u32 *release_addr;
	u32 release_addr_len;
	const u32 *ranges;
	u32 ranges_len;
	const u32 *intmap;
	u32 intmap_len;
	const u32 *intmap_mask;
	u32 intmap_mask_len;
	const u32 *iommu_map;
	u32 iommu_map_len;
	const u32 *interrupts;
	u32 interrupts_len;
	u32 phandle;
	/* Declared by this node for its children; the FDT defaults are 2 and 1. */
	u32 addr_cells;
	u32 size_cells;
	/* Decoded `ranges`, filled the first time a child needs a translation. */
	int ranges_decoded;
	u32 range_count;
	u64 range_child[FDT_MAX_RANGES];
	u64 range_parent[FDT_MAX_RANGES];
	u64 range_size[FDT_MAX_RANGES];
};

static struct fdt_node g_nodes[FDT_MAX_DEPTH];
static char g_path[FDT_PATH_MAX];

struct fdt_alias {
	const char *name;
	const char *path;
};
static struct fdt_alias g_aliases[FDT_MAX_ALIASES];
static u32 g_alias_count;
static const char *g_stdout_path;

/* UART nodes the tree describes, so /chosen's choice can be resolved against
 * them after the walk (the node it names may appear before or after it). */
#define FDT_MAX_UARTS 8
struct fdt_uart {
	char path[FDT_PATH_MAX];
	u64 base;
	int is_pl011;
};
static struct fdt_uart g_uarts[FDT_MAX_UARTS];
static u32 g_uart_count;

/* Does a `compatible` property (a list of NUL-terminated strings) contain
 * `want`? */
static int fdt_compatible_is(const char *compat, u32 len, const char *want)
{
	u32 i = 0;

	if (!compat)
		return 0;
	while (i < len) {
		const char *entry = compat + i;
		u32 entry_len = (u32)strlen(entry);

		if (strcmp(entry, want) == 0)
			return 1;
		i += entry_len + 1;
	}
	return 0;
}

static void fdt_decode_ranges(struct fdt_node *node, u32 parent_addr_cells)
{
	node->ranges_decoded = 1;
	node->range_count = 0;
	if (!node->ranges || node->ranges_len == 0)
		return; /* an empty `ranges` is an identity mapping */

	u32 stride = node->addr_cells + parent_addr_cells + node->size_cells;
	if (stride == 0 || node->ranges_len % (stride * 4))
		return;

	u32 entries = node->ranges_len / (stride * 4);
	const u32 *p = node->ranges;

	for (u32 i = 0; i < entries && node->range_count < FDT_MAX_RANGES; i++) {
		node->range_child[node->range_count] = fdt_read_cells(p, node->addr_cells);
		node->range_parent[node->range_count] =
		    fdt_read_cells(p + node->addr_cells, parent_addr_cells);
		node->range_size[node->range_count] =
		    fdt_read_cells(p + node->addr_cells + parent_addr_cells,
		                   node->size_cells);
		node->range_count++;
		p += stride;
	}
}

/* Translate an address read from a node at `depth` (so expressed in the
 * address space of its parent) up to the CPU's own. */
static u64 fdt_translate(u64 addr, int depth)
{
	for (int level = depth - 1; level >= 1; level--) {
		struct fdt_node *node = &g_nodes[level];

		if (!node->ranges_decoded || node->range_count == 0)
			continue; /* no `ranges`, or an empty one: identity */
		for (u32 i = 0; i < node->range_count; i++) {
			if (addr >= node->range_child[i] &&
			    addr - node->range_child[i] < node->range_size[i]) {
				addr = addr - node->range_child[i] + node->range_parent[i];
				break;
			}
		}
	}
	return addr;
}

/* One (address, size) pair out of a `reg`, indexed by entry. */
static int fdt_reg_entry(const struct fdt_node *node, int depth, u32 index,
                         u64 *addr_out, u64 *size_out)
{
	u32 ac = g_nodes[depth - 1].addr_cells;
	u32 sc = g_nodes[depth - 1].size_cells;
	u32 stride = ac + sc;

	if (!node->reg || stride == 0 || ac > 2 || sc > 2)
		return 0;
	if (node->reg_len < (index + 1) * stride * 4)
		return 0;

	const u32 *p = node->reg + index * stride;
	u64 addr = fdt_read_cells(p, ac);

	if (addr_out)
		*addr_out = fdt_translate(addr, depth);
	if (size_out)
		*size_out = sc ? fdt_read_cells(p + ac, sc) : 0;
	return 1;
}

/* A PCI host bridge's `ranges` describes which CPU addresses reach devices
 * behind it, and which PCI address each such window appears at on the bus. The
 * child address is three cells: a flags word, then the 64-bit PCI address.
 * Bits 25:24 of the flags are the space — 0 config, 1 I/O, 2 32-bit memory,
 * 3 64-bit memory — and this kernel wants a memory one, preferring the 32-bit
 * window because that is what a BAR without the 64-bit bit can hold. */
#define FDT_PCI_SPACE_MASK  0x03000000u
#define FDT_PCI_SPACE_MEM32 0x02000000u
#define FDT_PCI_SPACE_MEM64 0x03000000u
#define FDT_PCI_SPACE_IO    0x01000000u

static void fdt_pci_read_windows(const struct fdt_node *node, int depth)
{
	u32 child_cells = node->addr_cells ? node->addr_cells : 3;
	u32 parent_cells = g_nodes[depth - 1].addr_cells;
	u32 size_cells = node->size_cells ? node->size_cells : 2;
	u32 stride = child_cells + parent_cells + size_cells;

	if (!node->ranges || stride == 0 || child_cells < 3 ||
	    node->ranges_len % (stride * 4))
		return;

	u32 entries = node->ranges_len / (stride * 4);
	const u32 *p = node->ranges;

	for (u32 i = 0; i < entries; i++, p += stride) {
		u32 space = fdt32_to_cpu(p[0]) & FDT_PCI_SPACE_MASK;

		if (space == FDT_PCI_SPACE_IO) {
			/* The port-I/O window. A PCI card with an I/O BAR - the AC'97
			 * codec is one - is reachable on a machine with no port I/O
			 * instructions only through this. */
			g_pci_io_pci_base = fdt_read_cells(p + 1, 2);
			g_pci_io_base = fdt_translate(
			    fdt_read_cells(p + child_cells, parent_cells), depth);
			g_pci_io_size = fdt_read_cells(p + child_cells + parent_cells,
			                               size_cells);
			continue;
		}
		if (space != FDT_PCI_SPACE_MEM32 && space != FDT_PCI_SPACE_MEM64)
			continue;
		/* A 64-bit window is taken only when no 32-bit one was found: a
		 * device whose BAR is 32 bits wide cannot be placed in it. */
		if (space == FDT_PCI_SPACE_MEM64 && g_pci_mmio_size)
			continue;

		g_pci_mmio_pci_base = fdt_read_cells(p + 1, 2);
		g_pci_mmio_base =
		    fdt_translate(fdt_read_cells(p + child_cells, parent_cells), depth);
		g_pci_mmio_size = fdt_read_cells(p + child_cells + parent_cells,
		                                 size_cells);
		if (space == FDT_PCI_SPACE_MEM32)
			return; /* the preferred one: stop looking */
	}
}

static void fdt_note_uart(const struct fdt_node *node, u64 base, int is_pl011)
{
	if (g_uart_count >= FDT_MAX_UARTS)
		return;

	struct fdt_uart *u = &g_uarts[g_uart_count++];
	usize len = strlen(g_path);

	if (len >= sizeof(u->path))
		len = sizeof(u->path) - 1;
	memcpy(u->path, g_path, len);
	u->path[len] = '\0';
	u->base = base;
	u->is_pl011 = is_pl011;
	(void)node;
}

/* Interpret a node now that every one of its properties has been seen. */
static void fdt_finish_node(struct fdt_node *node, int depth)
{
	/* RAM banks. A `memory` node may carry several of them, and a board may
	 * carry several such nodes; each becomes a boot_info region so the direct
	 * map and the pmm see the machine the firmware describes. */
	int is_memory = (node->device_type && strcmp(node->device_type, "memory") == 0) ||
	                (depth == 2 && strncmp(node->name, "memory", 6) == 0 &&
	                 (node->name[6] == '\0' || node->name[6] == '@'));
	if (is_memory && node->reg) {
		for (u32 i = 0; i < BOOTINFO_MAX_MEMORY_REGIONS; i++) {
			u64 base, size;

			if (!fdt_reg_entry(node, depth, i, &base, &size) || size == 0)
				break;
			if (global_bootinfo.memory_region_count >= BOOTINFO_MAX_MEMORY_REGIONS)
				break;
			/* The fallback region planted before the walk is overwritten by
			 * the first real bank rather than added to. */
			usize slot = g_ram_from_fdt ? global_bootinfo.memory_region_count : 0;
			global_bootinfo.memory_regions[slot].base = base;
			global_bootinfo.memory_regions[slot].length = size;
			global_bootinfo.memory_regions[slot].type = BOOT_MEMORY_AVAILABLE;
			global_bootinfo.memory_region_count = slot + 1;
			g_ram_from_fdt = 1;
		}
	}

	/* A GICv3: reg[0] is the distributor and reg[1] the redistributor region
	 * (one 128 KiB frame per CPU). There is no memory-mapped CPU interface —
	 * that moved into system registers, which is the whole reason this kernel
	 * cares about the distinction: only a v3 can have an ITS, and only an ITS
	 * gives this board message-signalled interrupts. */
	if (!g_gic_from_fdt &&
	    fdt_compatible_is(node->compatible, node->compatible_len,
	                      "arm,gic-v3")) {
		u64 dist, dist_len, redist, redist_len;

		if (fdt_reg_entry(node, depth, 0, &dist, &dist_len) &&
		    fdt_reg_entry(node, depth, 1, &redist, &redist_len) &&
		    dist && redist) {
			g_gicd_base = dist;
			g_gicr_base = redist;
			g_gicr_size = redist_len;
			g_gic_is_v3 = 1;
			g_gic_from_fdt = 1;
		}
	}

	/* The SD/MMC controller a Pi 4 boots from. */
	if (!g_emmc2_base &&
	    fdt_compatible_is(node->compatible, node->compatible_len,
	                      "brcm,bcm2711-emmc2")) {
		u64 base;

		if (fdt_reg_entry(node, depth, 0, &base, 0) && base)
			g_emmc2_base = base;
	}

	/* The rest of the Broadcom SoC. One table rather than five copies of the
	 * same four lines. */
	{
		static const struct { const char *compat; u64 *slot; } bcm[] = {
			{ "brcm,bcm2835-mbox",        &g_mbox_base },
			{ "brcm,bcm2711-gpio",        &g_gpio_base },
			{ "brcm,bcm2835-gpio",        &g_gpio_base },
			{ "brcm,bcm2835-pm",          &g_pm_base },
			{ "brcm,bcm2835-pm-wdt",      &g_pm_base },
			{ "brcm,bcm2835-system-timer", &g_systimer_base },
			{ "brcm,bcm2711-genet-v5",    &g_genet_base },
		};

		for (u32 i = 0; i < sizeof(bcm) / sizeof(bcm[0]); i++) {
			u64 base;

			if (*bcm[i].slot)
				continue;
			if (!fdt_compatible_is(node->compatible, node->compatible_len,
			                       bcm[i].compat))
				continue;
			if (fdt_reg_entry(node, depth, 0, &base, 0) && base)
				*bcm[i].slot = base;
		}
	}

	/* The ITS lives under the GICv3 node and is what a PCI device's MSI write
	 * lands in. */
	if (fdt_compatible_is(node->compatible, node->compatible_len,
	                      "arm,gic-v3-its")) {
		u64 base, len;

		if (fdt_reg_entry(node, depth, 0, &base, &len) && base) {
			g_its_base = base;
			g_its_size = len;
		}
	}

	/* The interrupt controller: reg[0] is the distributor, reg[1] the CPU
	 * interface. GIC-400 (Raspberry Pi 4) and the "cortex-a15-gic" QEMU virt
	 * declares are the same GICv2 programming model. */
	if (!g_gic_from_fdt &&
	    (fdt_compatible_is(node->compatible, node->compatible_len, "arm,gic-400") ||
	     fdt_compatible_is(node->compatible, node->compatible_len,
	                       "arm,cortex-a15-gic"))) {
		u64 dist, cpu;

		if (fdt_reg_entry(node, depth, 0, &dist, 0) &&
		    fdt_reg_entry(node, depth, 1, &cpu, 0) && dist && cpu) {
			g_gicd_base = dist;
			g_gicc_base = cpu;
			g_gic_from_fdt = 1;
		}
	}

	if (fdt_compatible_is(node->compatible, node->compatible_len, "arm,pl011")) {
		u64 base;

		if (fdt_reg_entry(node, depth, 0, &base, 0) && base)
			fdt_note_uart(node, base, 1);
	} else if (fdt_compatible_is(node->compatible, node->compatible_len,
	                             "brcm,bcm2835-aux-uart")) {
		u64 base;

		if (fdt_reg_entry(node, depth, 0, &base, 0))
			fdt_note_uart(node, base, 0);
	}

	/* A CPU: its `reg` is the MPIDR affinity value PSCI's CPU_ON targets, and
	 * is not a bus address — no `ranges` translation applies to it. */
	if (depth == 3 && strncmp(node->name, "cpu@", 4) == 0 &&
	    strcmp(g_nodes[2].name, "cpus") == 0 && node->reg &&
	    g_cpu_count < FDT_MAX_CPUS) {
		u32 ac = g_nodes[depth - 1].addr_cells;

		if (ac >= 1 && ac <= 2 && node->reg_len >= ac * 4) {
			g_cpu_mpidr[g_cpu_count] = fdt_read_cells(node->reg, ac);
			if (node->release_addr && node->release_addr_len >= 4) {
				u32 cells = node->release_addr_len >= 8 ? 2 : 1;
				g_cpu_release_addr[g_cpu_count] =
				    fdt_read_cells(node->release_addr, cells);
			}
			g_cpu_count++;
		}
	}
	/* `enable-method` is written on each cpu node, and on some trees on /cpus
	 * itself. Either says how every secondary is started. */
	if (node->enable_method) {
		if (strcmp(node->enable_method, "spin-table") == 0)
			g_cpu_enable_method = FDT_ENABLE_METHOD_SPIN_TABLE;
		else if (strncmp(node->enable_method, "psci", 4) == 0)
			g_cpu_enable_method = FDT_ENABLE_METHOD_PSCI;
	}

	/* The PCI host bridge: where its config space is, and which window of CPU
	 * addresses reaches devices behind it. Both used to be QEMU virt's
	 * constants compiled in, which on a Raspberry Pi name RAM — the enumerator
	 * found a "device" at 0x3f000000 and wrote BAR-sizing probes into it. */
	if (node->device_type && strcmp(node->device_type, "pci") == 0 &&
	    g_pci_host_kind == FDT_PCI_HOST_NONE) {
		int kind = FDT_PCI_HOST_NONE;

		if (fdt_compatible_is(node->compatible, node->compatible_len,
		                      "pci-host-ecam-generic"))
			kind = FDT_PCI_HOST_ECAM;
		else if (fdt_compatible_is(node->compatible, node->compatible_len,
		                           "brcm,bcm2711-pcie"))
			kind = FDT_PCI_HOST_BCM2711;

		u64 base, size;

		if (kind != FDT_PCI_HOST_NONE &&
		    fdt_reg_entry(node, depth, 0, &base, &size) && base) {
			g_pci_host_kind = kind;
			g_pci_cfg_base = base;
			g_pci_cfg_size = size;
			fdt_pci_read_windows(node, depth);
		}
	}

	/* The PCI host bridge's INTx routing table. */
	if (node->intmap && node->intmap_len >= 32 && (node->intmap_len % 4) == 0) {
		u32 n = node->intmap_len / 4;

		if (n > FDT_INTMAP_MAX_CELLS)
			n = FDT_INTMAP_MAX_CELLS;
		for (u32 i = 0; i < n; i++)
			g_intmap[i] = fdt32_to_cpu(node->intmap[i]);
		g_intmap_stride = fdt_intmap_stride(g_intmap, n);
		g_intmap_cells = g_intmap_stride ? n : 0;
	}
	if (node->intmap_mask && node->intmap_mask_len == 16) {
		for (u32 i = 0; i < 4; i++)
			g_intmap_mask[i] = fdt32_to_cpu(node->intmap_mask[i]);
	}

	/* The PCI host bridge's requester-id -> StreamID map. Captured verbatim;
	 * the phandle in each entry is matched against the SMMU node later. */
	if (node->iommu_map && node->iommu_map_len >= 16 &&
	    (node->iommu_map_len % 4) == 0) {
		u32 n = node->iommu_map_len / 4;

		if (n > FDT_IOMMU_MAP_MAX_CELLS)
			n = FDT_IOMMU_MAP_MAX_CELLS;
		for (u32 i = 0; i < n; i++)
			g_iommu_map[i] = fdt32_to_cpu(node->iommu_map[i]);
		g_iommu_map_cells = n & ~3u;
	}

	/* The SMMUv3 itself: one register window, and up to four wired
	 * interrupts (eventq, priq, cmdq-sync, gerror), each a three-cell GIC
	 * specifier of (type, number, flags) with SPI numbers biased by 32. */
	if (!g_smmu_base &&
	    fdt_compatible_is(node->compatible, node->compatible_len,
	                      "arm,smmu-v3")) {
		u64 base, size;

		if (fdt_reg_entry(node, depth, 0, &base, &size) && base) {
			g_smmu_base = base;
			g_smmu_size = size ? size : 0x20000;
			g_smmu_phandle = node->phandle;
			if (node->interrupts) {
				u32 n = node->interrupts_len / 12;

				if (n > 4)
					n = 4;
				for (u32 i = 0; i < n; i++) {
					u32 type = fdt32_to_cpu(node->interrupts[i * 3 + 0]);
					u32 num = fdt32_to_cpu(node->interrupts[i * 3 + 1]);

					g_smmu_irq[i] = num + (type == 0 ? 32u : 16u);
				}
				g_smmu_irq_count = n;
			}
		}
	}
}

/* Resolve /chosen's stdout-path (possibly through /aliases) to one of the UART
 * nodes the walk found, and pick the register base the console will use. */
static void fdt_pick_console(void)
{
	const struct fdt_uart *chosen = 0;
	char want[FDT_PATH_MAX];

	if (g_stdout_path) {
		usize len = 0;

		/* "serial0:115200n8" — the options after the colon are the loader's
		 * business, not ours. */
		while (g_stdout_path[len] && g_stdout_path[len] != ':' &&
		       len < sizeof(want) - 1)
			len++;
		memcpy(want, g_stdout_path, len);
		want[len] = '\0';

		const char *path = want;
		if (path[0] != '/') {
			for (u32 i = 0; i < g_alias_count; i++) {
				if (strcmp(g_aliases[i].name, path) == 0) {
					path = g_aliases[i].path;
					break;
				}
			}
		}
		for (u32 i = 0; i < g_uart_count; i++) {
			if (strcmp(g_uarts[i].path, path) == 0) {
				chosen = &g_uarts[i];
				break;
			}
		}
	}

	if (!chosen) {
		for (u32 i = 0; i < g_uart_count; i++) {
			if (g_uarts[i].is_pl011) {
				chosen = &g_uarts[i];
				break;
			}
		}
	}
	if (chosen) {
		g_aarch64_uart_base = chosen->base;
		g_uart_from_fdt = 1;
		g_aarch64_uart_is_mini = !chosen->is_pl011;
		if (g_aarch64_uart_is_mini) {
			/* Two conventions are in the wild for what a
			 * "brcm,bcm2835-aux-uart" node's `reg` names. The Raspberry Pi
			 * and upstream Linux trees point it at the mini-UART's own
			 * registers, 0x40 into the AUX block (serial@7e215040); a few
			 * hand-written trees point it at the block itself. Tell them
			 * apart by the offset, because getting it wrong writes the
			 * enable bit into a data register. */
			if ((chosen->base & 0xff) == 0x40) {
				g_aarch64_aux_base = chosen->base - 0x40;
			} else {
				g_aarch64_aux_base = chosen->base;
				g_aarch64_uart_base = chosen->base + 0x40;
				platform_set_uart_base(g_aarch64_uart_base);
			}
			platform_set_aux_base(g_aarch64_aux_base);
		}
	}
}

static int g_fdt_scanned;

void bootinfo_fdt_scan(u64 dtb_address)
{
	if (g_fdt_scanned)
		return;
	g_fdt_scanned = 1;

	platform_detect(dtb_address);
	if (!g_gicd_base)
		g_gicd_base = platform_gicd_base();
	if (!g_gicc_base)
		g_gicc_base = platform_gicc_base();
	if (!g_aarch64_uart_base)
		g_aarch64_uart_base = platform_uart_base();

	/* Fallback for a boot with no device tree at all (bare ELF handed to
	 * QEMU, or a loader that does not pass one): the QEMU virt board's RAM
	 * always starts at 0x40000000, and 256 MiB is the smallest useful size. */
	global_bootinfo.memory_region_count = 1;
	global_bootinfo.memory_regions[0].base = 0x40000000;
	global_bootinfo.memory_regions[0].length = 256 * 1024 * 1024;
	global_bootinfo.memory_regions[0].type = BOOT_MEMORY_AVAILABLE;

	global_bootinfo.has_framebuffer = 0; // Disable framebuffer for now

	if (!dtb_address)
		return;

	const u32 *fdt = (const u32 *)dtb_address;
	if (fdt32_to_cpu(fdt[0]) != 0xd00dfeed)
		return;

	u32 off_struct = fdt32_to_cpu(fdt[2]);
	u32 off_strings = fdt32_to_cpu(fdt[3]);
	const char *strings = (const char *)dtb_address + off_strings;
	const u32 *p = (const u32 *)((const char *)dtb_address + off_struct);
	int depth = 0;

	/* The root's parent: the FDT defaults, which the root itself overrides
	 * with its own #address-cells/#size-cells before any child is read. */
	g_nodes[0].addr_cells = 2;
	g_nodes[0].size_cells = 1;
	g_path[0] = '\0';

	while (*p) {
		u32 tag = fdt32_to_cpu(*p++);

		if (tag == 1) { /* FDT_BEGIN_NODE */
			const char *name = (const char *)p;

			/* This node's parent is complete now that a child has been
			 * reached, so its `ranges` can be decoded — children's addresses
			 * translate through it. */
			if (depth >= 1 && !g_nodes[depth].ranges_decoded)
				fdt_decode_ranges(&g_nodes[depth], g_nodes[depth - 1].addr_cells);

			depth++;
			if (depth < FDT_MAX_DEPTH) {
				struct fdt_node *node = &g_nodes[depth];
				usize path_len = depth > 1 ? g_nodes[depth - 1].path_len : 0;

				memset(node, 0, sizeof(*node));
				node->name = name;
				node->addr_cells = 2;
				node->size_cells = 1;

				/* The root's name is empty, so the path of /soc/serial@... is
				 * built by appending "/<name>" per level. */
				if (depth > 1) {
					usize n = strlen(name);

					if (path_len + 1 + n < sizeof(g_path)) {
						g_path[path_len] = '/';
						memcpy(g_path + path_len + 1, name, n);
						path_len += 1 + n;
					}
				}
				g_path[path_len] = '\0';
				node->path_len = (u32)path_len;
			}
			p += (strlen(name) + 1 + 3) / 4;
		} else if (tag == 3) { /* FDT_PROP */
			u32 len = fdt32_to_cpu(*p++);
			u32 nameoff = fdt32_to_cpu(*p++);
			const char *prop_name = strings + nameoff;
			const char *val = (const char *)p;
			struct fdt_node *node =
			    depth < FDT_MAX_DEPTH ? &g_nodes[depth] : (struct fdt_node *)0;

			if (strcmp(prop_name, "bootargs") == 0 && len > 0 &&
			    len < sizeof(aarch64_cmdline_buf)) {
				if (val[0]) {
					strncpy(aarch64_cmdline_buf, val, sizeof(aarch64_cmdline_buf) - 1);
					aarch64_cmdline_buf[sizeof(aarch64_cmdline_buf) - 1] = '\0';
				}
			} else if (strcmp(prop_name, "stdout-path") == 0 && len > 0) {
				g_stdout_path = val;
			} else if (depth == 2 && strcmp(g_nodes[2].name, "aliases") == 0 &&
			           len > 0 && g_alias_count < FDT_MAX_ALIASES) {
				g_aliases[g_alias_count].name = prop_name;
				g_aliases[g_alias_count].path = val;
				g_alias_count++;
			} else if (node) {
				if (strcmp(prop_name, "#address-cells") == 0 && len == 4)
					node->addr_cells = fdt32_to_cpu(p[0]);
				else if (strcmp(prop_name, "#size-cells") == 0 && len == 4)
					node->size_cells = fdt32_to_cpu(p[0]);
				else if (strcmp(prop_name, "reg") == 0) {
					node->reg = p;
					node->reg_len = len;
				} else if (strcmp(prop_name, "compatible") == 0) {
					node->compatible = val;
					node->compatible_len = len;
				} else if (strcmp(prop_name, "device_type") == 0) {
					node->device_type = val;
				} else if (strcmp(prop_name, "enable-method") == 0) {
					node->enable_method = val;
				} else if (strcmp(prop_name, "cpu-release-addr") == 0) {
					node->release_addr = p;
					node->release_addr_len = len;
				} else if (strcmp(prop_name, "ranges") == 0) {
					node->ranges = p;
					node->ranges_len = len;
				} else if (strcmp(prop_name, "interrupt-map") == 0) {
					node->intmap = p;
					node->intmap_len = len;
				} else if (strcmp(prop_name, "interrupt-map-mask") == 0) {
					node->intmap_mask = p;
					node->intmap_mask_len = len;
				} else if (strcmp(prop_name, "iommu-map") == 0) {
					node->iommu_map = p;
					node->iommu_map_len = len;
				} else if (strcmp(prop_name, "interrupts") == 0) {
					node->interrupts = p;
					node->interrupts_len = len;
				} else if (strcmp(prop_name, "phandle") == 0 && len == 4) {
					node->phandle = fdt32_to_cpu(p[0]);
				}
			}
			p += (len + 3) / 4;
		} else if (tag == 2) { /* FDT_END_NODE */
			if (depth >= 1 && depth < FDT_MAX_DEPTH) {
				if (!g_nodes[depth].ranges_decoded)
					fdt_decode_ranges(&g_nodes[depth],
					                  g_nodes[depth - 1].addr_cells);
				/* g_path is trimmed back to this node's own path first: it is
				 * what fdt_note_uart records. */
				g_path[g_nodes[depth].path_len] = '\0';
				fdt_finish_node(&g_nodes[depth], depth);
				g_path[depth > 1 ? g_nodes[depth - 1].path_len : 0] = '\0';
			}
			if (depth > 0)
				depth--;
		} else if (tag == 4) { /* FDT_NOP */
			continue;
		} else if (tag == 9) { /* FDT_END */
			break;
		}
	}

	fdt_pick_console();
}

void bootinfo_init_from_fdt(u64 dtb_address)
{
	bootinfo_fdt_scan(dtb_address);

	/* console_write rather than printf: this runs before the log buffer is
	 * drained to the serial port, and printf output is not seen at all here. */
	console_write("aarch64: platform: ");
	console_write(platform_name());
	console_write("\n");
	for (usize i = 0; i < global_bootinfo.memory_region_count; i++) {
		console_write("aarch64: RAM bank 0x");
		console_write_hex64(global_bootinfo.memory_regions[i].base);
		console_write(" + ");
		console_write_dec(global_bootinfo.memory_regions[i].length >> 20);
		console_write(" MiB\n");
	}
	console_write("aarch64: GIC dist 0x");
	console_write_hex64(g_gicd_base);
	console_write(" cpu 0x");
	console_write_hex64(g_gicc_base);
	console_write(g_gic_from_fdt ? " (device tree)\n" : " (default)\n");
	console_write(g_aarch64_uart_is_mini ? "aarch64: mini-UART 0x"
	                                     : "aarch64: PL011 0x");
	console_write_hex64(g_aarch64_uart_base);
	console_write(g_uart_from_fdt ? " (device tree)\n" : " (default)\n");
	if (g_aarch64_uart_is_mini) {
		console_write("aarch64: AUX block 0x");
		console_write_hex64(g_aarch64_aux_base);
		console_write("\n");
	}
	if (g_pci_host_kind == FDT_PCI_HOST_NONE) {
		console_write("aarch64: no PCI host bridge in the device tree\n");
	} else {
		console_write(g_pci_host_kind == FDT_PCI_HOST_BCM2711
		                  ? "aarch64: BCM2711 PCIe at 0x"
		                  : "aarch64: PCI ECAM at 0x");
		console_write_hex64(g_pci_cfg_base);
		console_write(", MMIO window 0x");
		console_write_hex64(g_pci_mmio_base);
		console_write(" + ");
		console_write_dec(g_pci_mmio_size >> 20);
		console_write(" MiB (bus 0x");
		console_write_hex64(g_pci_mmio_pci_base);
		console_write(")\n");
	}
	if (g_intmap_stride) {
		console_write("aarch64: PCI INTx map, ");
		console_write_dec(g_intmap_cells / g_intmap_stride);
		console_write(" entries of ");
		console_write_dec(g_intmap_stride);
		console_write(" cells\n");
	} else {
		console_write("aarch64: no usable PCI interrupt-map in the device tree\n");
	}
	if (g_cpu_count > 1) {
		console_write("aarch64: ");
		console_write_dec(g_cpu_count);
		console_write(" CPUs in the device tree, started by ");
		console_write(g_cpu_enable_method == FDT_ENABLE_METHOD_SPIN_TABLE
		                  ? "spin-table\n"
		                  : "PSCI\n");
	}
	console_write("aarch64: bootinfo initialized\n");
}

const struct boot_info *bootinfo_get(void)
{
	return &global_bootinfo;
}

const char *bootinfo_cmdline(void)
{
	return aarch64_cmdline_buf;
}

int bootinfo_has_flag(const char *flag)
{
	if (!flag || !flag[0]) return 0;
	const char *p = aarch64_cmdline_buf;
	usize flen = strlen(flag);
	while (*p) {
		while (*p == ' ') p++;
		if (!*p) break;
		if (strncmp(p, flag, flen) == 0 && (p[flen] == ' ' || p[flen] == '\0' || p[flen] == '=')) {
			return 1;
		}
		while (*p && *p != ' ') p++;
	}
	return 0;
}

int bootinfo_get_kv(const char *key, char *out, usize out_size)
{
	if (!key || !key[0]) return 0;
	const char *cmd = aarch64_cmdline_buf;
	usize klen = strlen(key);
	for (usize i = 0; cmd[i];) {
		while (cmd[i] == ' ') i++;
		if (!cmd[i]) break;
		usize start = i;
		while (cmd[i] && cmd[i] != ' ') i++;
		usize tok_len = i - start;
		if (tok_len > klen && cmd[start + klen] == '=' &&
		    strncmp(cmd + start, key, klen) == 0) {
			const char *val = cmd + start + klen + 1;
			usize val_len = tok_len - klen - 1;
			if (out && out_size > 0) {
				usize copy_len = val_len < out_size - 1 ? val_len : out_size - 1;
				memcpy(out, val, copy_len);
				out[copy_len] = '\0';
			}
			return 1;
		}
	}
	return 0;
}
