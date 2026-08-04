/*
 * AMD-Vi: ACPI IVRS, device table, command ring, page tables.
 *
 * Structured to mirror kernel/dev/iommu.c so the two units can be read side by
 * side, but nothing is shared: the register file, the table formats and the
 * invalidation mechanism are all different, and pretending otherwise would
 * hide the places where they disagree.
 *
 * Passthrough is the starting state for every device, for the same reason it
 * is on the Intel side: every driver in this tree hands the device a physical
 * address, and they would all fault the instant translation applied to them.
 * AMD-Vi spells passthrough in the device table entry (V=1, TV=0), so it needs
 * no page tables at all — unlike VT-d, where the absence of a pass-through
 * capability forced an identity domain.
 */

#include <b1nix/acpi.h>
#include <b1nix/amdvi.h>
#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/mm.h>
#include <b1nix/pci.h>
#include <b1nix/spinlock.h>
#include <stdio.h>
#include <string.h>

/* ── IVRS ───────────────────────────────────────────────────────── */

struct ivrs_header {
	struct acpi_sdt_header header;
	u32 iv_info;
	u64 reserved;
} __attribute__((packed));

/* Every IVRS entry starts with this; type 0x10/0x11/0x40 are IVHD blocks, one
 * per remapping unit, and carry the unit's MMIO base. */
struct ivhd_header {
	u8 type;
	u8 flags;
	u16 length;
	u16 device_id;   /* the unit's own PCI id */
	u16 capability_offset;
	u64 base_address; /* MMIO */
	u16 pci_segment;
	u16 info;
} __attribute__((packed));

/* ── MMIO registers ─────────────────────────────────────────────── */

#define AMDVI_DEV_TABLE_BASE 0x0000
#define AMDVI_CMD_BUF_BASE   0x0008
#define AMDVI_EVENT_BASE     0x0010
#define AMDVI_CONTROL        0x0018
#define AMDVI_STATUS         0x2020
#define AMDVI_CMD_HEAD       0x2000
#define AMDVI_CMD_TAIL       0x2008
#define AMDVI_EVENT_HEAD     0x2010
#define AMDVI_EVENT_TAIL     0x2018

#define CTRL_IOMMU_EN   (1ULL << 0)
#define CTRL_EVENT_LOG_EN (1ULL << 2)
#define CTRL_EVENT_INT_EN (1ULL << 3)
#define CTRL_CMD_BUF_EN (1ULL << 12)

/* Device table entry: 32 bytes. Only the first two quadwords matter here —
 * valid, translation valid, the page table root and its level, and the
 * permission bits. */
#define DTE_VALID       (1ULL << 0)
#define DTE_TRANS_VALID (1ULL << 1)
#define DTE_IR          (1ULL << 61) /* allow reads */
#define DTE_IW          (1ULL << 62) /* allow writes */
#define DTE_MODE_SHIFT  9
#define DTE_ADDR_MASK   0x000FFFFFFFFFF000ULL

/* Page table entry, AMD-Vi's own format: present, read, write, the next-level
 * count in bits 11:9 (0 means this entry maps a page). */
#define PTE_PRESENT (1ULL << 0)
#define PTE_READ    (1ULL << 61)
#define PTE_WRITE   (1ULL << 62)
#define PTE_NEXT_SHIFT 9
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

#define AMDVI_LEVELS 4

/* Commands are 16 bytes; only completion-wait and the two invalidations are
 * needed to bring a unit up and keep it coherent. */
#define CMD_COMPLETION_WAIT   0x01
#define CMD_INVALIDATE_DEVTAB 0x02
#define CMD_INVALIDATE_PAGES  0x03
#define CMD_OPCODE_SHIFT 28

#define CMD_RING_ENTRIES 128u
#define EVENT_RING_ENTRIES 128u
#define DEV_TABLE_ENTRIES 65536u /* one per requester id */

static volatile u8 *amdvi_regs;
static u64 *dev_table;
static u64 dev_table_phys;
static u32 *cmd_ring;
static u64 cmd_ring_phys;
static u32 *event_ring;
static u64 event_ring_phys;
static u32 event_head;
static u64 *domain_root;
static u64 domain_root_phys;
static int amdvi_enabled;
static spinlock_t amdvi_lock = SPINLOCK_INIT;

static u64 reg_read64(u32 off) { return *(volatile u64 *)(amdvi_regs + off); }
static void reg_write64(u32 off, u64 v) { *(volatile u64 *)(amdvi_regs + off) = v; }

int amdvi_active(void) { return amdvi_enabled; }

static void *alloc_zero_pages(usize pages, u64 *phys_out)
{
	u64 frame = pmm_alloc_frames(pages);
	if (!frame)
		return 0;
	void *va = (void *)(usize)(frame + vmm_direct_map_base());
	memset(va, 0, pages * PAGE_SIZE);
	if (phys_out)
		*phys_out = frame;
	return va;
}

/* ── command ring ───────────────────────────────────────────────── */

/* Post one command and wait for the unit to consume the ring. The wait is a
 * completion-wait command with the store bit, which is the only way to know
 * the earlier ones took effect — a tail write on its own says nothing. */
static void amdvi_submit(u32 w0, u32 w1, u32 w2, u32 w3)
{
	u64 tail = reg_read64(AMDVI_CMD_TAIL) & 0x7FFF0ULL;
	u32 index = (u32)(tail / 16);
	cmd_ring[index * 4 + 0] = w0;
	cmd_ring[index * 4 + 1] = w1;
	cmd_ring[index * 4 + 2] = w2;
	cmd_ring[index * 4 + 3] = w3;
	tail = (tail + 16) % (CMD_RING_ENTRIES * 16);
	reg_write64(AMDVI_CMD_TAIL, tail);
}

static void amdvi_completion_wait(void)
{
	/* Store 1 into a scratch location and wait for it: the unit writes it only
	 * after every earlier command has completed. */
	static volatile u64 *sem;
	static u64 sem_phys;
	if (!sem)
		sem = alloc_zero_pages(1, &sem_phys);
	if (!sem)
		return;
	*sem = 0;
	amdvi_submit((u32)((sem_phys & 0xFFFFFFF8u) | 1u), /* store address + s bit */
	             (u32)((CMD_COMPLETION_WAIT << CMD_OPCODE_SHIFT) |
	                   ((u32)(sem_phys >> 32) & 0xFFFFFu)),
	             1u, 0u);
	for (int i = 0; i < 1000000 && *sem == 0; i++)
		__asm__ volatile("pause");
}

static void amdvi_invalidate_device(u16 bdf)
{
	amdvi_submit((u32)bdf, (u32)(CMD_INVALIDATE_DEVTAB << CMD_OPCODE_SHIFT), 0, 0);
	amdvi_completion_wait();
}

static void amdvi_invalidate_all_pages(void)
{
	/* Domain 0, S=1 with an all-ones address: everything. */
	amdvi_submit(0, (u32)(CMD_INVALIDATE_PAGES << CMD_OPCODE_SHIFT),
	             0x00000003u, 0xFFFFFFFFu);
	amdvi_completion_wait();
}

/* ── page tables ────────────────────────────────────────────────── */

static u64 *pt_next(u64 *table, u32 index, u32 level, int create)
{
	u64 e = table[index];
	if (!(e & PTE_PRESENT)) {
		if (!create)
			return 0;
		u64 phys = 0;
		u64 *next = alloc_zero_pages(1, &phys);
		if (!next)
			return 0;
		/* The level of the table this entry points at travels in the entry. */
		table[index] = phys | PTE_PRESENT | PTE_READ | PTE_WRITE |
		               ((u64)(level - 1) << PTE_NEXT_SHIFT);
		return next;
	}
	return (u64 *)(usize)((e & PTE_ADDR_MASK) + vmm_direct_map_base());
}

static u64 *pt_leaf(u64 iova, int create)
{
	u64 *tbl = domain_root;
	if (!tbl)
		return 0;
	for (u32 level = AMDVI_LEVELS; level > 1; level--) {
		u32 shift = 12 + 9 * (level - 1);
		tbl = pt_next(tbl, (u32)((iova >> shift) & 0x1FF), level, create);
		if (!tbl)
			return 0;
	}
	return &tbl[(iova >> 12) & 0x1FF];
}

int amdvi_map(u64 iova, u64 phys, usize size, int writable)
{
	if (!amdvi_enabled || (iova & (PAGE_SIZE - 1)) || (phys & (PAGE_SIZE - 1)))
		return -1;
	usize pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
	u64 flags;
	spin_lock_irqsave(&amdvi_lock, &flags);
	for (usize i = 0; i < pages; i++) {
		u64 *pte = pt_leaf(iova + (u64)i * PAGE_SIZE, 1);
		if (!pte) {
			spin_unlock_irqrestore(&amdvi_lock, flags);
			return -1;
		}
		/* Next-level 0 means "this entry maps a page". */
		*pte = (phys + (u64)i * PAGE_SIZE) | PTE_PRESENT | PTE_READ |
		       (writable ? PTE_WRITE : 0);
	}
	spin_unlock_irqrestore(&amdvi_lock, flags);
	amdvi_invalidate_all_pages();
	return 0;
}

int amdvi_unmap(u64 iova, usize size)
{
	if (!amdvi_enabled)
		return -1;
	usize pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
	u64 flags;
	spin_lock_irqsave(&amdvi_lock, &flags);
	for (usize i = 0; i < pages; i++) {
		u64 *pte = pt_leaf(iova + (u64)i * PAGE_SIZE, 0);
		if (pte)
			*pte = 0;
	}
	spin_unlock_irqrestore(&amdvi_lock, flags);
	amdvi_invalidate_all_pages();
	return 0;
}

u64 amdvi_translate(u64 iova)
{
	if (!amdvi_enabled)
		return 0;
	u64 flags;
	spin_lock_irqsave(&amdvi_lock, &flags);
	u64 *pte = pt_leaf(iova & ~(u64)(PAGE_SIZE - 1), 0);
	u64 out = 0;
	if (pte && (*pte & PTE_PRESENT))
		out = (*pte & PTE_ADDR_MASK) | (iova & (PAGE_SIZE - 1));
	spin_unlock_irqrestore(&amdvi_lock, flags);
	return out;
}

/* ── device table ───────────────────────────────────────────────── */

/* Four quadwords per entry, but only the first two are programmed here. */
static void dte_set(u16 bdf, int translated)
{
	u64 *dte = &dev_table[(u32)bdf * 4];
	u64 lo;
	if (translated)
		lo = DTE_VALID | DTE_TRANS_VALID |
		     (domain_root_phys & DTE_ADDR_MASK) |
		     ((u64)AMDVI_LEVELS << DTE_MODE_SHIFT);
	else
		lo = DTE_VALID; /* valid, no translation: passthrough */
	dte[1] = DTE_IR | DTE_IW; /* domain 0, reads and writes allowed */
	dte[0] = lo;
}

int amdvi_attach_device(u16 bdf)
{
	if (!amdvi_enabled)
		return -1;
	u64 flags;
	spin_lock_irqsave(&amdvi_lock, &flags);
	dte_set(bdf, 1);
	spin_unlock_irqrestore(&amdvi_lock, flags);
	amdvi_invalidate_device(bdf);
	amdvi_invalidate_all_pages();
	return 0;
}

void amdvi_detach_device(u16 bdf)
{
	if (!amdvi_enabled)
		return;
	u64 flags;
	spin_lock_irqsave(&amdvi_lock, &flags);
	dte_set(bdf, 0);
	spin_unlock_irqrestore(&amdvi_lock, flags);
	amdvi_invalidate_device(bdf);
}

/* ── event log ──────────────────────────────────────────────────── */

/* A device that touches what it was not given lands here rather than in a
 * status register, so counting faults means walking the ring. */
u32 amdvi_fault_count(void)
{
	if (!amdvi_enabled)
		return 0;
	u64 tail = reg_read64(AMDVI_EVENT_TAIL) & 0x7FFF0ULL;
	u32 tail_index = (u32)(tail / 16);
	u32 seen = 0;
	while (event_head != tail_index) {
		if (event_ring[event_head * 4 + 3] != 0)
			seen++;
		event_head = (event_head + 1) % EVENT_RING_ENTRIES;
	}
	return seen;
}

void amdvi_fault_clear(void)
{
	if (!amdvi_enabled)
		return;
	(void)amdvi_fault_count();
}

/* ── bring-up ───────────────────────────────────────────────────── */

void amdvi_init(void)
{
	const struct acpi_sdt_header *hdr = acpi_find_table("IVRS");
	if (!hdr)
		return;

	const u8 *p = (const u8 *)hdr + sizeof(struct ivrs_header);
	const u8 *end = (const u8 *)hdr + hdr->length;
	u64 base = 0;
	while (p + sizeof(struct ivhd_header) <= end) {
		const struct ivhd_header *ivhd = (const struct ivhd_header *)p;
		if (ivhd->length == 0)
			break;
		if (ivhd->type == 0x10 || ivhd->type == 0x11 || ivhd->type == 0x40) {
			base = ivhd->base_address;
			break;
		}
		p += ivhd->length;
	}
	if (!base) {
		console_write("amdvi: IVRS with no remapping unit\n");
		return;
	}

	/* The unit's register file is 16 KiB, not one page: the control and base
	 * registers sit at the bottom, but the command and event ring pointers
	 * live at 0x2000. Mapping a single page put those outside the mapping and
	 * the first write to a ring pointer faulted. */
	amdvi_regs = (volatile u8 *)vmm_map_mmio(base, 0x4000,
	                                         VMM_WRITABLE | VMM_NO_EXECUTE);
	if (!amdvi_regs) {
		console_write("amdvi: cannot map unit registers\n");
		return;
	}

	/* One entry per requester id: 65536 * 32 bytes = 2 MiB. Flat, so there is
	 * no per-bus table to create lazily and no bus that can be missing —
	 * the mistake that cost a fault on the Intel side cannot happen here. */
	usize dt_pages = (DEV_TABLE_ENTRIES * 32) / PAGE_SIZE;
	dev_table = alloc_zero_pages(dt_pages, &dev_table_phys);
	cmd_ring = alloc_zero_pages(1, &cmd_ring_phys);
	event_ring = alloc_zero_pages(1, &event_ring_phys);
	domain_root = alloc_zero_pages(1, &domain_root_phys);
	if (!dev_table || !cmd_ring || !event_ring || !domain_root) {
		console_write("amdvi: table allocation failed\n");
		return;
	}

	/* Every device passthrough to begin with. */
	for (u32 i = 0; i < DEV_TABLE_ENTRIES; i++)
		dte_set((u16)i, 0);

	/* Base registers carry the size as a power-of-two code in the low bits. */
	reg_write64(AMDVI_DEV_TABLE_BASE,
	            (dev_table_phys & 0x000FFFFFFFFFF000ULL) | (dt_pages - 1));
	reg_write64(AMDVI_CMD_BUF_BASE,
	            (cmd_ring_phys & 0x000FFFFFFFFFF000ULL) | (8ULL << 56));
	reg_write64(AMDVI_EVENT_BASE,
	            (event_ring_phys & 0x000FFFFFFFFFF000ULL) | (8ULL << 56));
	reg_write64(AMDVI_CMD_HEAD, 0);
	reg_write64(AMDVI_CMD_TAIL, 0);
	reg_write64(AMDVI_EVENT_HEAD, 0);
	reg_write64(AMDVI_EVENT_TAIL, 0);
	event_head = 0;

	reg_write64(AMDVI_CONTROL, CTRL_CMD_BUF_EN | CTRL_EVENT_LOG_EN);
	reg_write64(AMDVI_CONTROL,
	            CTRL_CMD_BUF_EN | CTRL_EVENT_LOG_EN | CTRL_IOMMU_EN);
	amdvi_enabled = 1;

	amdvi_invalidate_all_pages();

	char line[128];
	snprintf(line, sizeof(line),
	         "amdvi: unit at 0x%lx, %u-entry device table, translation on\n",
	         (unsigned long)base, (unsigned)DEV_TABLE_ENTRIES);
	console_write(line);
}

/* ── self-test ──────────────────────────────────────────────────── */

static void amdvi_report(const char *name, int ok, u64 detail)
{
	console_write(ok ? "M100D-SMOKE: ok " : "M100D-SMOKE: FAIL ");
	console_write(name);
	if (!ok) {
		console_write(" detail=");
		console_write_dec(detail);
	}
	console_write("\n");
}

void amdvi_selftest(void)
{
	if (!bootinfo_has_flag("b1nix.test=1"))
		return;
	if (!acpi_find_table("IVRS")) {
		console_write("M100D-SMOKE: skip amdvi (no IVRS on this machine)\n");
		return;
	}
	if (!amdvi_enabled) {
		amdvi_report("amdvi-enable", 0, 1);
		return;
	}

	/* The unit says it is on, and it is pointing at our tables. */
	u64 ctrl = reg_read64(AMDVI_CONTROL);
	u64 dtb = reg_read64(AMDVI_DEV_TABLE_BASE);
	int on = (ctrl & CTRL_IOMMU_EN) && (ctrl & CTRL_CMD_BUF_EN) &&
	         (dtb & 0x000FFFFFFFFFF000ULL) == dev_table_phys;
	amdvi_report("amdvi-enable", on, ctrl);

	/* A mapping is what the tables the unit walks say, and unmapping removes
	 * it. Same claim as on the Intel side, checked against this format. */
	u64 frame = pmm_alloc_frame();
	if (!frame) {
		amdvi_report("amdvi-map", 0, 2);
		return;
	}
	u64 iova = 0x30000000ULL;
	int map_ok = amdvi_map(iova, frame, PAGE_SIZE, 1) == 0 &&
	             amdvi_translate(iova) == frame &&
	             amdvi_translate(iova + 0x80) == frame + 0x80;
	amdvi_unmap(iova, PAGE_SIZE);
	int unmap_ok = amdvi_translate(iova) == 0;
	pmm_free_frame(frame);
	amdvi_report("amdvi-map", map_ok, iova);
	amdvi_report("amdvi-unmap", unmap_ok, 0);

	/* The command ring is the only way this unit is told anything, so a
	 * completion-wait that never completes means every invalidation above was
	 * a guess. */
	u64 head_before = reg_read64(AMDVI_CMD_HEAD) & 0x7FFF0ULL;
	amdvi_invalidate_all_pages();
	u64 head_after = reg_read64(AMDVI_CMD_HEAD) & 0x7FFF0ULL;
	amdvi_report("amdvi-command-ring", head_after != head_before,
	             head_after);

	console_write("M100D-SMOKE: done\n");
}
