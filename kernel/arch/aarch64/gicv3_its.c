/* GICv3 ITS: message-signalled interrupts on aarch64.
 *
 * An MSI is a memory write. On x86 the address names the local APIC and the
 * data is the vector, so a driver programs the pair into its MSI-X table and
 * the interrupt arrives. There is no such target on this board: a device's
 * write goes to the ITS, which looks the writing device up by its DeviceID,
 * translates the EventID it wrote into an LPI number, and sends that LPI to the
 * redistributor of the CPU the LPI's collection is mapped to.
 *
 * So three things have to exist before a device can raise an MSI here:
 *   - the ITS's own tables (a device table and a collection table) and its
 *     command queue, which is how the CPU talks to it at all;
 *   - LPI configuration and pending tables in the redistributor, and LPIs
 *     enabled there;
 *   - per device: MAPD to give it an interrupt-translation table, MAPTI to
 *     bind one of its EventIDs to an LPI, and MAPC to point that LPI's
 *     collection at a CPU.
 *
 * What a driver sees is unchanged: msi_alloc_vector() then pci_msix_enable(),
 * exactly as on x86_64. The difference is hidden in arch_msi_prepare(), which
 * this file implements — it is handed the device that is about to be enabled,
 * and hands back the address and data that make an MSI work on this machine.
 */

#include <b1nix/arch.h>
#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/gicv3.h>
#include <b1nix/irq.h>
#include <b1nix/mm.h>
#include <b1nix/pci.h>
#include <b1nix/types.h>
#include <string.h>

#define GITS_CTLR        0x0000
#define GITS_TYPER       0x0008
#define GITS_CBASER      0x0080
#define GITS_CWRITER     0x0088
#define GITS_CREADR      0x0090
#define GITS_BASER(n)    (0x0100 + (n) * 8)
#define GITS_TRANSLATER  0x10040

#define GITS_BASER_TYPE_DEVICE     1
#define GITS_BASER_TYPE_COLLECTION 4

/* Baser/CBaser bits shared by both: valid, inner-shareable, cacheable, and a
 * page size the ITS is told in the same word. */
#define GITS_BASER_VALID     (1ULL << 63)
#define GITS_BASER_INNER_WB  (5ULL << 59)  /* InnerCache = RaWaWb */
#define GITS_BASER_SHARE_IS  (1ULL << 10)
#define GITS_BASER_PAGE_64K  (2ULL << 8)

#define ITS_CMD_QUEUE_SIZE   (64 * 1024)
#define ITS_TABLE_PAGE       (64 * 1024)

/* Commands, one 32-byte entry each. */
#define ITS_CMD_MOVI   0x01
#define ITS_CMD_INT    0x03
#define ITS_CMD_MAPD   0x08
#define ITS_CMD_MAPC   0x09
#define ITS_CMD_MAPTI  0x0a
#define ITS_CMD_INV    0x0c
#define ITS_CMD_INVALL 0x0d
#define ITS_CMD_SYNC   0x05

/* LPIs start at 8192 by architecture. One per MSI vector this kernel hands
 * out, which keeps the mapping between the two arithmetic. */
#define LPI_BASE 8192u

struct its_cmd {
	u64 raw[4];
};

static u64 g_its;
static int g_ready;
static u64 g_cmd_queue;      /* physical == virtual here (identity mapped) */
static u32 g_cmd_write;      /* index of the next free slot */
static u64 g_prop_table;     /* LPI configuration, shared by all CPUs */
static u64 g_pend_table;     /* LPI pending bits for the boot CPU */
static u32 g_lpi_id_bits;
/* GITS_TYPER.PTA: 1 means MAPC and SYNC name a redistributor by its physical
 * address, 0 means they name it by the processor number it reports. Getting
 * this wrong binds the collection to a target that does not exist, and the
 * only symptom is that no interrupt ever arrives. */
static int g_pta;
/* How many DeviceIDs the device table actually covers. A MAPD above this is
 * discarded by the ITS without a word. */
static u32 g_devid_limit;
/* Every device that has been MAPD'd, so a second vector on the same device
 * does not hand it a second interrupt-translation table. */
#define ITS_MAX_DEVICES 16
static struct {
	u32 devid;
	u64 itt;
	u32 next_event;
} g_devices[ITS_MAX_DEVICES];
static u32 g_device_count;
/* The EventID each vector was mapped to, so a self-test can check that the
 * device's table really holds the pair this kernel handed it. */
static u32 g_vector_event[MSI_VECTOR_COUNT];
static u8 g_vector_mapped[MSI_VECTOR_COUNT];

/* The ITS's tables and its command queue must sit on a 64 KiB boundary — the
 * registers that point at them have no bits for anything finer, and QEMU's ITS
 * stalls its command queue outright when handed an address it cannot use. The
 * frame allocator only promises 4 KiB, so over-allocate and round up; the slack
 * is a few pages, once, at boot. */
static u64 alloc_aligned_64k(u64 size) {
	u64 pages = (size + (64 * 1024) + PAGE_SIZE - 1) / PAGE_SIZE;
	u64 base = pmm_alloc_frames((usize)pages);

	if (!base)
		return 0;
	return (base + 0xffffULL) & ~0xffffULL;
}

static inline volatile u32 *its32(u64 off) {
	return (volatile u32 *)(usize)(g_its + off);
}

static inline volatile u64 *its64(u64 off) {
	return (volatile u64 *)(usize)(g_its + off);
}

/* Push one command and wait for the ITS to consume it.
 *
 * Waiting on every command is not how a busy kernel would do it — the queue
 * exists so commands can be batched — but MSI setup happens a handful of times
 * at probe, and a failure to be consumed is far easier to see here than three
 * commands later. */
static int its_send(const struct its_cmd *cmd) {
	struct its_cmd *queue = (struct its_cmd *)(usize)g_cmd_queue;
	u32 slots = ITS_CMD_QUEUE_SIZE / sizeof(struct its_cmd);

	queue[g_cmd_write] = *cmd;
	__asm__ volatile("dsb ishst" ::: "memory");

	g_cmd_write = (g_cmd_write + 1) % slots;
	*its64(GITS_CWRITER) = (u64)g_cmd_write * sizeof(struct its_cmd);

	for (int i = 0; i < 1000000; i++) {
		u64 read = *its64(GITS_CREADR) / sizeof(struct its_cmd);

		if (read == g_cmd_write)
			return 0;
		cpu_relax();
	}
	console_write("its: command queue stalled at 0x");
	console_write_hex64(*its64(GITS_CWRITER));
	console_write(" read 0x");
	console_write_hex64(*its64(GITS_CREADR));
	console_write(" ctlr 0x");
	console_write_hex64(*its32(GITS_CTLR));
	console_write("\n");
	return -1;
}

/* The value MAPC and SYNC use to name this CPU's redistributor. */
static u64 its_target(void) {
	return g_pta ? (gicv3_rdbase() & ~0xffffULL)
	             : ((u64)gicv3_processor_number() << 16);
}

static int its_sync(u64 target) {
	struct its_cmd c = {{ITS_CMD_SYNC, 0, target, 0}};

	return its_send(&c);
}

static int its_mapd(u32 devid, u64 itt, u32 size_bits) {
	struct its_cmd c = {{
	    ITS_CMD_MAPD | ((u64)devid << 32),
	    size_bits ? (u64)(size_bits - 1) : 0,
	    (itt & ~0xffULL) | (1ULL << 63), /* ITT address + Valid */
	    0,
	}};

	return its_send(&c);
}

static int its_mapc(u32 collection, u64 target) {
	struct its_cmd c = {{
	    ITS_CMD_MAPC,
	    0,
	    target | (u64)collection | (1ULL << 63),
	    0,
	}};

	return its_send(&c);
}

static int its_mapti(u32 devid, u32 event, u32 lpi, u32 collection) {
	struct its_cmd c = {{
	    ITS_CMD_MAPTI | ((u64)devid << 32),
	    (u64)event | ((u64)lpi << 32),
	    (u64)collection,
	    0,
	}};

	return its_send(&c);
}

static int its_inv(u32 devid, u32 event) {
	struct its_cmd c = {{ITS_CMD_INV | ((u64)devid << 32), (u64)event, 0, 0}};

	return its_send(&c);
}

/* One ITS table, sized from what the ITS says it needs.
 *
 * Sizing it by guess is the mistake that costs a day: a device table with room
 * for 8192 DeviceIDs looks perfectly healthy, and every MAPD for a device above
 * that is simply discarded — the command queue keeps draining, nothing is
 * logged, and the only symptom is that the interrupt never arrives. The width
 * of a DeviceID is GITS_TYPER.Devbits, and the width of an entry is the table's
 * own Entry_Size field. */
static int its_setup_baser(int index) {
	u64 baser = *its64(GITS_BASER(index));
	u32 type = (u32)((baser >> 56) & 7);

	if (type != GITS_BASER_TYPE_DEVICE && type != GITS_BASER_TYPE_COLLECTION)
		return 0; /* not a table this kernel has to provide */

	u64 typer = *its64(GITS_TYPER);
	u32 entry_size = (u32)((baser >> 48) & 0x1f) + 1;
	u32 id_bits;

	if (type == GITS_BASER_TYPE_DEVICE) {
		id_bits = (u32)((typer >> 13) & 0x1f) + 1;   /* Devbits */
	} else {
		/* Collection ids: CIL says whether CIDbits is meaningful; when it is
		 * not, the ITS supports 16 bits of them. This kernel uses exactly one
		 * collection, so a single page is always enough — but the field still
		 * has to be told the truth about the table's size. */
		id_bits = ((typer >> 36) & 1) ? (u32)((typer >> 32) & 0xf) + 1 : 16;
	}
	if (id_bits > 20)
		id_bits = 20; /* a 16 MiB table is far past anything this kernel maps */

	u64 bytes = (1ULL << id_bits) * entry_size;
	u64 pages = (bytes + ITS_TABLE_PAGE - 1) / ITS_TABLE_PAGE;

	if (type == GITS_BASER_TYPE_COLLECTION && pages > 1)
		pages = 1; /* one collection, and the ITS indexes it from zero */
	if (pages > 16)
		pages = 16;

	u64 page = alloc_aligned_64k(pages * ITS_TABLE_PAGE);
	if (!page) {
		console_write("its: out of memory for a table\n");
		return -1;
	}
	memset((void *)(usize)page, 0, (usize)(pages * ITS_TABLE_PAGE));

	*its64(GITS_BASER(index)) = GITS_BASER_VALID | GITS_BASER_INNER_WB |
	                            GITS_BASER_SHARE_IS | GITS_BASER_PAGE_64K |
	                            (page & 0x0000fffffffff000ULL) | (pages - 1);

	/* The ITS may refuse the page size and report back what it will take. A
	 * kernel that ignores the read-back gets a table the hardware is not
	 * using, and every command against it is silently wrong. */
	u64 got = *its64(GITS_BASER(index));
	if (!(got & GITS_BASER_VALID)) {
		console_write("its: table type ");
		console_write_dec(type);
		console_write(" refused\n");
		return -1;
	}
	if (type == GITS_BASER_TYPE_DEVICE)
		g_devid_limit = (u32)((pages * ITS_TABLE_PAGE) / entry_size);
	return 0;
}

/* Redistributor plumbing for LPIs: the configuration table says which LPIs are
 * enabled and at what priority, the pending table is the hardware's own
 * bookkeeping. Both are memory this kernel owns. */
static int its_lpi_tables(void) {
	u64 typer = *its64(GITS_TYPER);
	u32 id_bits = (u32)((typer >> 8) & 0x1f) + 1;

	g_pta = (typer >> 19) & 1;

	if (id_bits > 16)
		id_bits = 16; /* 64K LPIs is far past what this kernel hands out */
	g_lpi_id_bits = id_bits;

	u32 lpi_count = 1u << id_bits;
	u64 prop_size = lpi_count;            /* one byte per LPI */
	u64 pend_size = lpi_count / 8;        /* one bit per LPI */

	if (prop_size < PAGE_SIZE)
		prop_size = PAGE_SIZE;
	if (pend_size < PAGE_SIZE)
		pend_size = PAGE_SIZE;

	g_prop_table = alloc_aligned_64k(prop_size);
	g_pend_table = alloc_aligned_64k(pend_size);
	if (!g_prop_table || !g_pend_table) {
		console_write("its: out of memory for the LPI tables\n");
		return -1;
	}
	/* Every LPI enabled, at a priority the CPU interface's mask lets through.
	 * The ITS decides which LPI a device's write becomes; an LPI nothing is
	 * mapped to is never delivered, so a blanket enable costs nothing. */
	memset((void *)(usize)g_prop_table, 0xa1, (usize)prop_size);
	memset((void *)(usize)g_pend_table, 0, (usize)pend_size);

	return gicv3_lpi_enable(g_prop_table, id_bits, g_pend_table);
}

int its_init(void) {
	if (!gicv3_present())
		return -1;
	g_its = fdt_its_base();
	if (!g_its) {
		console_write("its: none in the device tree\n");
		return -1;
	}

	/* Disabled while its tables are handed over. */
	*its32(GITS_CTLR) = 0;

	for (int i = 0; i < 8; i++) {
		if (its_setup_baser(i) < 0)
			return -1;
	}

	g_cmd_queue = alloc_aligned_64k(ITS_CMD_QUEUE_SIZE);
	if (!g_cmd_queue) {
		console_write("its: out of memory for the command queue\n");
		return -1;
	}
	memset((void *)(usize)g_cmd_queue, 0, ITS_CMD_QUEUE_SIZE);
	g_cmd_write = 0;

	*its64(GITS_CBASER) = (g_cmd_queue & 0x0000fffffffff000ULL) |
	                      GITS_BASER_VALID | GITS_BASER_INNER_WB |
	                      GITS_BASER_SHARE_IS |
	                      ((ITS_CMD_QUEUE_SIZE / (4 * 1024)) - 1);
	*its64(GITS_CWRITER) = 0;

	if (its_lpi_tables() < 0)
		return -1;

	*its32(GITS_CTLR) = 1;

	/* Collection 0 lives on the boot CPU: that is where every MSI this kernel
	 * routes will be delivered, the same policy the SPI routing uses. */
	if (its_mapc(0, its_target()) < 0)
		return -1;
	its_sync(its_target());

	g_ready = 1;
	console_write("its: 0x");
	console_write_hex64(g_its);
	console_write(g_pta ? " (pta) ready, " : " ready, ");
	console_write_dec(1u << g_lpi_id_bits);
	console_write(" LPIs\n");
	return 0;
}

int its_ready(void) { return g_ready; }

int arch_msi_supported(void) { return g_ready; }

/* The physical address a device writes to raise an MSI. Behind an SMMU it has
 * to be mapped into the device's domain like any other target. */
u64 its_translater_phys(void) { return g_ready ? g_its + GITS_TRANSLATER : 0; }

/* The LPI an MSI vector maps to, and back. Keeping this arithmetic rather than
 * a table means the IRQ path can turn an INTID into a vector with a subtract. */
u32 its_lpi_for_vector(int vector) {
	return LPI_BASE + (u32)(vector - (int)MSI_VECTOR_BASE);
}

int its_vector_for_lpi(u32 lpi) {
	if (lpi < LPI_BASE)
		return -1;
	u32 index = lpi - LPI_BASE;

	if (index >= MSI_VECTOR_COUNT)
		return -1;
	return (int)MSI_VECTOR_BASE + (int)index;
}

/* Give a device an ITT the first time it asks, and hand out its next EventID. */
static int its_device(u32 devid, u32 *event_out) {
	for (u32 i = 0; i < g_device_count; i++) {
		if (g_devices[i].devid == devid) {
			*event_out = g_devices[i].next_event++;
			return 0;
		}
	}
	if (g_device_count >= ITS_MAX_DEVICES)
		return -1;

	/* One ITT per device. The entry size the ITS reports is per event; a
	 * handful of events per device is all this kernel uses, but the table has
	 * to be at least 256-byte aligned, so a page is the simple answer. */
	u64 itt = pmm_alloc_frame();
	if (!itt)
		return -1;
	memset((void *)(usize)itt, 0, PAGE_SIZE);

	if (its_mapd(devid, itt, 5 /* 32 events */) < 0) {
		pmm_free_frame(itt);
		return -1;
	}

	g_devices[g_device_count].devid = devid;
	g_devices[g_device_count].itt = itt;
	g_devices[g_device_count].next_event = 1;
	*event_out = 0;
	g_device_count++;
	return 0;
}

/* ── Self-test ───────────────────────────────────────────────────────────────
 *
 * The ITS can raise an LPI itself, without a device writing anything: the INT
 * command means "pretend device D wrote event E". That separates the two
 * halves of an MSI — the controller's translation and delivery, and the
 * device's write — so a failure says which one is broken instead of just
 * "no interrupt arrived".
 *
 * A synthetic DeviceID is used deliberately: no real device is disturbed, and
 * the id is one no PCI function can produce on this board (bus 255). */
#define ITS_TEST_DEVID 0xff00u

static volatile u32 g_its_test_hits;
volatile u32 g_its_lpi_hits;

static int its_test_handler(void *ctx) {
	(void)ctx;
	__atomic_fetch_add(&g_its_test_hits, 1, __ATOMIC_ACQ_REL);
	return 1;
}

void its_selftest(void) {
	if (!bootinfo_has_flag("b1nix.test=1"))
		return;
	if (!g_ready) {
		console_write("M98-ITS: skip int-delivery (no ITS on this board)\n");
		return;
	}

	int vector = msi_alloc_vector(its_test_handler, 0);

	if (vector < 0) {
		console_write("M98-ITS: FAIL int-delivery (no free vector)\n");
		return;
	}

	u32 event = 0;

	u32 test_devid = (g_devid_limit > 1) ? (g_devid_limit - 1) : 1;

	if (its_device(test_devid, &event) < 0 ||
	    its_mapti(test_devid, event, its_lpi_for_vector(vector), 0) < 0) {
		console_write("M98-ITS: FAIL int-delivery (mapping refused)\n");
		msi_free_vector(vector);
		return;
	}
	its_inv(test_devid, event);
	its_sync(its_target());

	u32 before = __atomic_load_n(&g_its_test_hits, __ATOMIC_ACQUIRE);
	struct its_cmd c = {{ITS_CMD_INT | ((u64)test_devid << 32), (u64)event, 0,
	                     0}};

	if (its_send(&c) < 0) {
		console_write("M98-ITS: FAIL int-delivery (INT not accepted)\n");
		msi_free_vector(vector);
		return;
	}

	/* The interrupt is delivered to this CPU; it arrives once interrupts are
	 * next taken, which is immediately unless this runs with them masked. */
	u32 after = before;
	for (int i = 0; i < 1000000 && after == before; i++) {
		after = __atomic_load_n(&g_its_test_hits, __ATOMIC_ACQUIRE);
		cpu_relax();
	}

	if (after == before)
		console_write("M98-ITS: FAIL int-delivery (LPI never arrived)\n");
	else
		console_write("M98-ITS: ok int-delivery\n");
	msi_free_vector(vector);
}

/* Called from pci_msix_enable/pci_msi_enable just before the capability is
 * programmed: bind this device's next EventID to the LPI that belongs to
 * `vector`, and report the address/data pair that makes the write land there.
 *
 * The DeviceID is the PCI Requester ID. QEMU virt's `msi-map` is the identity
 * over the whole bus (`<0 &its 0 0x10000>`), which is also what a board with
 * one host bridge and no aliasing looks like; a tree that says otherwise would
 * need that map read here. */
int arch_msi_prepare(u8 bus, u8 slot, u8 func, int vector, u64 *addr_out,
                     u32 *data_out) {
	if (!g_ready)
		return -1;

	u32 devid = ((u32)bus << 8) | ((u32)slot << 3) | (u32)func;

	if (g_devid_limit && devid >= g_devid_limit) {
		console_write("its: device id past the device table\n");
		return -1;
	}
	u32 event = 0;

	if (its_device(devid, &event) < 0)
		return -1;

	u32 lpi = its_lpi_for_vector(vector);

	if (its_mapti(devid, event, lpi, 0) < 0)
		return -1;
	its_inv(devid, event);
	its_sync(its_target());

	*addr_out = g_its + GITS_TRANSLATER;
	*data_out = event;

	u32 vi = (u32)(vector - (int)MSI_VECTOR_BASE);
	if (vi < MSI_VECTOR_COUNT) {
		g_vector_event[vi] = event;
		g_vector_mapped[vi] = 1;
	}
	return 0;
}

/* The address/data pair this arch programmed for `vector`. A self-test that
 * wants to know the device is armed for the right interrupt has to compare
 * against this rather than against the vector number: only x86 puts the vector
 * in the data word. */
int arch_msi_expected(int vector, u64 *addr_out, u32 *data_out) {
	u32 vi = (u32)(vector - (int)MSI_VECTOR_BASE);

	if (!g_ready || vector < (int)MSI_VECTOR_BASE || vi >= MSI_VECTOR_COUNT ||
	    !g_vector_mapped[vi])
		return -1;
	*addr_out = g_its + GITS_TRANSLATER;
	*data_out = g_vector_event[vi];
	return 0;
}
