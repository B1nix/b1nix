/*
 * M100b: Intel VT-d DMA remapping.
 *
 * The unit sits between a device and memory and walks its own page tables, so a
 * device address is no longer a physical address. Two things follow: a device
 * can be given exactly the pages it needs and reaches nothing else, and a
 * device whose address window is too narrow can be handed a low address that
 * translates to memory anywhere — the case M99 has to solve by copying.
 *
 * Everything starts in pass-through. Enabling the unit must not change what an
 * unconverted driver sees, and every b1nix driver still builds its own physical
 * addresses; the moment translation applied to them they would fault. So each
 * context entry is programmed pass-through at init, and iommu_attach_device()
 * moves one function into the translated domain when its driver is ready.
 */

#include <b1nix/acpi.h>
#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/iommu.h>
#include <b1nix/nvme.h>
#include <lkpi/dma-mapping.h>
#include <b1nix/mm.h>
#include <b1nix/spinlock.h>
#include <stdio.h>
#include <string.h>

/* ── DMAR ───────────────────────────────────────────────────────── */

struct dmar_header {
	struct acpi_sdt_header header;
	u8 host_address_width; /* MGAW - 1 */
	u8 flags;
	u8 reserved[10];
} __attribute__((packed));

struct dmar_entry {
	u16 type; /* 0 = DRHD */
	u16 length;
} __attribute__((packed));

struct dmar_drhd {
	struct dmar_entry entry;
	u8 flags; /* bit 0: INCLUDE_PCI_ALL */
	u8 reserved;
	u16 segment;
	u64 register_base;
} __attribute__((packed));

/* ── register file ──────────────────────────────────────────────── */

#define VTD_VER    0x00
#define VTD_CAP    0x08
#define VTD_ECAP   0x10
#define VTD_GCMD   0x18
#define VTD_GSTS   0x1C
#define VTD_RTADDR 0x20
#define VTD_CCMD   0x28
#define VTD_FSTS   0x34

#define GCMD_TE   (1u << 31) /* translation enable */
#define GCMD_SRTP (1u << 30) /* set root table pointer */
#define GSTS_TES  (1u << 31)
#define GSTS_RTPS (1u << 30)

#define CCMD_ICC    (1ULL << 63)
#define CCMD_GLOBAL (1ULL << 61)
#define IOTLB_IVT    (1ULL << 63)
#define IOTLB_GLOBAL (1ULL << 60)

/* Context-entry translation types. */
#define CTX_TT_TRANSLATED 0u
#define CTX_TT_PASSTHROUGH 2u

static volatile u8 *vtd_regs;
static u64 vtd_cap, vtd_ecap;
static u32 vtd_agaw_levels;   /* page-table levels the unit is programmed for */
static u32 vtd_address_width;  /* usable device address bits */
static int vtd_enabled;
static int vtd_passthrough_ok;

static u64 *root_table;      /* 256 entries x 16 bytes */
static u64 root_table_phys;
static u64 *context_tables[256];
static u64 context_tables_phys[256];

/* The one translated domain. Devices that opt in share it: b1nix has no notion
 * of a device group yet, and one domain is already the difference between "can
 * reach everything" and "can reach what it was given". */
#define DOMAIN_ID 1
static u64 *domain_root;
static u64 domain_root_phys;

/* The identity domain: physical address in, same address out. It exists
 * because a unit without pass-through cannot be switched on otherwise —
 * every driver in the tree builds its own physical addresses, and they would
 * all fault the moment translation applied to them. Devices sit here until a
 * driver asks for a real one. */
#define IDENT_DOMAIN_ID 2
static u64 *ident_root;
static u64 ident_root_phys;
static spinlock_t iommu_lock = SPINLOCK_INIT;

/* Device address space handed out by iommu_iova_alloc. Deliberately low so a
 * 32-bit-only device can use it. */
#define IOVA_BASE  0x10000000ULL
#define IOVA_PAGES 4096u /* 16 MiB */
static u8 iova_used[IOVA_PAGES];

static u32 vtd_read32(u32 off) { return *(volatile u32 *)(vtd_regs + off); }
static void vtd_write32(u32 off, u32 v) { *(volatile u32 *)(vtd_regs + off) = v; }
static u64 vtd_read64(u32 off) { return *(volatile u64 *)(vtd_regs + off); }
static void vtd_write64(u32 off, u64 v) { *(volatile u64 *)(vtd_regs + off) = v; }

static u32 iotlb_offset(void) { return (u32)(((vtd_ecap >> 8) & 0x3FF) * 16); }

static void *alloc_table_page(u64 *phys_out)
{
	u64 frame = pmm_alloc_frame();
	if (!frame)
		return 0;
	void *va = (void *)(usize)(frame + vmm_direct_map_base());
	memset(va, 0, PAGE_SIZE);
	if (phys_out)
		*phys_out = frame;
	return va;
}

static void vtd_flush_context_global(void)
{
	vtd_write64(VTD_CCMD, CCMD_ICC | CCMD_GLOBAL);
	while (vtd_read64(VTD_CCMD) & CCMD_ICC)
		;
}

static void vtd_flush_iotlb_global(void)
{
	u32 off = iotlb_offset();
	if (!off)
		return;
	/* The IOTLB register pair is at IRO; the command register is the second. */
	vtd_write64(off + 8, IOTLB_IVT | IOTLB_GLOBAL);
	while (vtd_read64(off + 8) & IOTLB_IVT)
		;
}

/* ── context tables ─────────────────────────────────────────────── */

/* Program one function's context entry. `translated` selects the domain's page
 * table; otherwise the entry is pass-through. */
static int context_set(u8 bus, u8 devfn, int translated)
{
	if (!context_tables[bus]) {
		u64 phys = 0;
		u64 *tbl = alloc_table_page(&phys);
		if (!tbl)
			return -1;
		context_tables[bus] = tbl;
		context_tables_phys[bus] = phys;
		/* Root entry: present + context table pointer. */
		root_table[bus * 2] = phys | 1ULL;
		root_table[bus * 2 + 1] = 0;
	}

	u64 *ctx = &context_tables[bus][devfn * 2];
	u64 lo, hi;
	if (translated) {
		lo = domain_root_phys | ((u64)CTX_TT_TRANSLATED << 2) | 1ULL;
		hi = ((u64)DOMAIN_ID << 8) | (u64)(vtd_agaw_levels - 2);
	} else if (vtd_passthrough_ok) {
		lo = ((u64)CTX_TT_PASSTHROUGH << 2) | 1ULL;
		hi = ((u64)IDENT_DOMAIN_ID << 8) | (u64)(vtd_agaw_levels - 2);
	} else {
		/* No pass-through in this unit: the identity domain is the same thing
		 * spelled out in page tables. */
		lo = ident_root_phys | ((u64)CTX_TT_TRANSLATED << 2) | 1ULL;
		hi = ((u64)IDENT_DOMAIN_ID << 8) | (u64)(vtd_agaw_levels - 2);
	}
	ctx[1] = hi;
	ctx[0] = lo;
	return 0;
}

/* ── second-level page tables ───────────────────────────────────── */

#define SL_READ  (1ULL << 0)
#define SL_WRITE (1ULL << 1)
#define SL_LARGE (1ULL << 7)
#define SL_ADDR_MASK 0x000FFFFFFFFFF000ULL

static u64 *sl_next_level(u64 *table, u32 index, int create)
{
	u64 e = table[index];
	if (!(e & (SL_READ | SL_WRITE))) {
		if (!create)
			return 0;
		u64 phys = 0;
		u64 *next = alloc_table_page(&phys);
		if (!next)
			return 0;
		table[index] = phys | SL_READ | SL_WRITE;
		return next;
	}
	return (u64 *)(usize)((e & SL_ADDR_MASK) + vmm_direct_map_base());
}

static u64 *sl_leaf_in(u64 *root, u64 iova, int create)
{
	u64 *tbl = root;
	if (!tbl)
		return 0;
	/* Levels are walked from the top; vtd_agaw_levels is 3 or 4. */
	for (u32 level = vtd_agaw_levels; level > 1; level--) {
		u32 shift = 12 + 9 * (level - 1);
		u32 idx = (u32)((iova >> shift) & 0x1FF);
		tbl = sl_next_level(tbl, idx, create);
		if (!tbl)
			return 0;
	}
	return &tbl[(iova >> 12) & 0x1FF];
}

static u64 *sl_leaf(u64 iova, int create)
{
	return sl_leaf_in(domain_root, iova, create);
}

/* Identity-map [0, size) into `root` with 2 MiB pages: 512 entries per PD, one
 * PD per GiB. Large pages keep this to a few tables instead of the megabytes a
 * 4 KiB identity map of all RAM would cost. */
static int ident_map_all(u64 size)
{
	for (u64 base = 0; base < size; base += (2ULL << 20)) {
		u64 *tbl = ident_root;
		for (u32 level = vtd_agaw_levels; level > 2; level--) {
			u32 shift = 12 + 9 * (level - 1);
			tbl = sl_next_level(tbl, (u32)((base >> shift) & 0x1FF), 1);
			if (!tbl)
				return -1;
		}
		tbl[(base >> 21) & 0x1FF] = base | SL_READ | SL_WRITE | SL_LARGE;
	}
	return 0;
}

int iommu_map(u64 iova, u64 phys, usize size, int writable)
{
	if (!vtd_enabled || (iova & (PAGE_SIZE - 1)) || (phys & (PAGE_SIZE - 1)))
		return -1;
	usize pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
	u64 flags;
	spin_lock_irqsave(&iommu_lock, &flags);
	for (usize i = 0; i < pages; i++) {
		u64 *pte = sl_leaf(iova + (u64)i * PAGE_SIZE, 1);
		if (!pte) {
			spin_unlock_irqrestore(&iommu_lock, flags);
			return -1;
		}
		*pte = (phys + (u64)i * PAGE_SIZE) | SL_READ | (writable ? SL_WRITE : 0);
	}
	spin_unlock_irqrestore(&iommu_lock, flags);
	vtd_flush_iotlb_global();
	return 0;
}

int iommu_unmap(u64 iova, usize size)
{
	if (!vtd_enabled)
		return -1;
	usize pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
	u64 flags;
	spin_lock_irqsave(&iommu_lock, &flags);
	for (usize i = 0; i < pages; i++) {
		u64 *pte = sl_leaf(iova + (u64)i * PAGE_SIZE, 0);
		if (pte)
			*pte = 0;
	}
	spin_unlock_irqrestore(&iommu_lock, flags);
	vtd_flush_iotlb_global();
	return 0;
}

u64 iommu_translate(u64 iova)
{
	if (!vtd_enabled)
		return 0;
	u64 flags;
	spin_lock_irqsave(&iommu_lock, &flags);
	u64 *pte = sl_leaf(iova & ~(u64)(PAGE_SIZE - 1), 0);
	u64 out = 0;
	if (pte && (*pte & (SL_READ | SL_WRITE)))
		out = (*pte & SL_ADDR_MASK) | (iova & (PAGE_SIZE - 1));
	spin_unlock_irqrestore(&iommu_lock, flags);
	return out;
}

int iommu_map_identity(u64 phys, usize size, int writable)
{
	u64 base = phys & ~(u64)(PAGE_SIZE - 1);
	usize span = (usize)((phys - base) + size);
	return iommu_map(base, base, span, writable);
}

/* ── fault reporting ────────────────────────────────────────────── */

#define FSTS_PPF (1u << 1) /* primary pending fault */
#define FSTS_PFO (1u << 0) /* fault overflow */

static u32 frcd_offset(void) { return (u32)(((vtd_cap >> 24) & 0x3FF) * 16); }
static u32 frcd_count(void) { return (u32)(((vtd_cap >> 40) & 0xFF) + 1); }

u32 iommu_fault_count(void)
{
	if (!vtd_enabled)
		return 0;
	u32 fsts = vtd_read32(VTD_FSTS);
	if (!(fsts & (FSTS_PPF | FSTS_PFO)))
		return 0;
	u32 base = frcd_offset();
	u32 n = frcd_count();
	u32 seen = 0;
	for (u32 i = 0; i < n; i++) {
		/* The high qword's top bit is F: this record holds a fault. */
		u64 hi = vtd_read64(base + i * 16 + 8);
		if (hi & (1ULL << 63)) {
			seen++;
			vtd_write64(base + i * 16 + 8, hi); /* write 1 to clear F */
		}
	}
	vtd_write32(VTD_FSTS, fsts);
	return seen;
}

void iommu_fault_clear(void)
{
	if (!vtd_enabled)
		return;
	(void)iommu_fault_count();
}

u64 iommu_iova_alloc(usize size)
{
	if (!vtd_enabled || size == 0)
		return 0;
	usize pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
	u64 flags;
	u64 got = 0;
	spin_lock_irqsave(&iommu_lock, &flags);
	usize run = 0;
	for (usize i = 0; i < IOVA_PAGES; i++) {
		if (iova_used[i]) {
			run = 0;
			continue;
		}
		if (++run == pages) {
			usize start = i + 1 - pages;
			for (usize j = 0; j < pages; j++)
				iova_used[start + j] = 1;
			got = IOVA_BASE + (u64)start * PAGE_SIZE;
			break;
		}
	}
	spin_unlock_irqrestore(&iommu_lock, flags);
	return got;
}

void iommu_iova_free(u64 iova, usize size)
{
	if (!vtd_enabled || iova < IOVA_BASE)
		return;
	usize pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
	usize start = (usize)((iova - IOVA_BASE) / PAGE_SIZE);
	u64 flags;
	spin_lock_irqsave(&iommu_lock, &flags);
	for (usize j = 0; j < pages && start + j < IOVA_PAGES; j++)
		iova_used[start + j] = 0;
	spin_unlock_irqrestore(&iommu_lock, flags);
}

/* ── attach / detach ────────────────────────────────────────────── */

int iommu_attach_device(u8 bus, u8 slot, u8 func)
{
	if (!vtd_enabled)
		return -1;
	u8 devfn = (u8)((slot << 3) | (func & 7));
	u64 flags;
	spin_lock_irqsave(&iommu_lock, &flags);
	int rc = context_set(bus, devfn, 1);
	spin_unlock_irqrestore(&iommu_lock, flags);
	if (rc == 0) {
		vtd_flush_context_global();
		vtd_flush_iotlb_global();
	}
	return rc;
}

void iommu_detach_device(u8 bus, u8 slot, u8 func)
{
	if (!vtd_enabled)
		return;
	u8 devfn = (u8)((slot << 3) | (func & 7));
	u64 flags;
	spin_lock_irqsave(&iommu_lock, &flags);
	(void)context_set(bus, devfn, 0);
	spin_unlock_irqrestore(&iommu_lock, flags);
	vtd_flush_context_global();
	vtd_flush_iotlb_global();
}

int iommu_active(void) { return vtd_enabled; }
u32 iommu_address_width(void) { return vtd_enabled ? vtd_address_width : 0; }

/* ── bring-up ───────────────────────────────────────────────────── */

/* Pick the widest guest address width the unit supports: SAGAW bit 2 is
 * 48-bit/4-level, bit 1 is 39-bit/3-level. */
static int choose_agaw(void)
{
	u32 sagaw = (u32)((vtd_cap >> 8) & 0x1F);
	if (sagaw & (1u << 2)) {
		vtd_agaw_levels = 4;
		vtd_address_width = 48;
		return 0;
	}
	if (sagaw & (1u << 1)) {
		vtd_agaw_levels = 3;
		vtd_address_width = 39;
		return 0;
	}
	return -1;
}

void iommu_init(void)
{
	const struct acpi_sdt_header *hdr = acpi_find_table("DMAR");
	if (!hdr)
		return; /* no remapping hardware described: nothing to do */

	const struct dmar_header *dmar = (const struct dmar_header *)hdr;
	const u8 *p = (const u8 *)hdr + sizeof(struct dmar_header);
	const u8 *end = (const u8 *)hdr + hdr->length;
	u64 reg_base = 0;

	while (p + sizeof(struct dmar_entry) <= end) {
		const struct dmar_entry *e = (const struct dmar_entry *)p;
		if (e->length == 0)
			break;
		if (e->type == 0) { /* DRHD */
			const struct dmar_drhd *d = (const struct dmar_drhd *)p;
			reg_base = d->register_base;
			if (d->flags & 1)
				break; /* INCLUDE_PCI_ALL: this one covers everything */
		}
		p += e->length;
	}
	if (!reg_base) {
		console_write("iommu: DMAR with no remapping unit\n");
		return;
	}

	vtd_regs = (volatile u8 *)vmm_map_mmio(reg_base, PAGE_SIZE,
	                                       VMM_WRITABLE | VMM_NO_EXECUTE);
	if (!vtd_regs) {
		console_write("iommu: cannot map remapping unit registers\n");
		return;
	}
	vtd_cap = vtd_read64(VTD_CAP);
	vtd_ecap = vtd_read64(VTD_ECAP);
	vtd_passthrough_ok = (vtd_ecap & (1ULL << 6)) != 0;

	if (choose_agaw() != 0) {
		console_write("iommu: no supported address width\n");
		return;
	}
	root_table = alloc_table_page(&root_table_phys);
	domain_root = alloc_table_page(&domain_root_phys);
	if (!root_table || !domain_root) {
		console_write("iommu: table allocation failed\n");
		return;
	}

	if (!vtd_passthrough_ok) {
		/* Every driver in the tree hands the device a physical address, so
		 * switching the unit on without pass-through would fault all of them.
		 * An identity domain says the same thing in page tables — and needs
		 * 2 MiB pages to be affordable. */
		if (!(vtd_cap & (1ULL << 34))) {
			console_write("iommu: no pass-through and no 2 MiB pages; "
			              "leaving translation off\n");
			return;
		}
		ident_root = alloc_table_page(&ident_root_phys);
		if (!ident_root || ident_map_all(DIRECT_MAP_SIZE) != 0) {
			console_write("iommu: identity domain build failed\n");
			return;
		}
	}

	/* Every function starts in pass-through, so switching the unit on changes
	 * nothing for a driver that has not asked for translation. */
	for (u32 bus = 0; bus < 256; bus++) {
		root_table[bus * 2] = 0;
		root_table[bus * 2 + 1] = 0;
	}
	for (u32 bus = 0; bus < 4; bus++)
		for (u32 devfn = 0; devfn < 256; devfn++)
			if (context_set((u8)bus, (u8)devfn, 0) != 0)
				return;

	vtd_write64(VTD_RTADDR, root_table_phys);
	vtd_write32(VTD_GCMD, GCMD_SRTP);
	while (!(vtd_read32(VTD_GSTS) & GSTS_RTPS))
		;
	vtd_flush_context_global();
	vtd_flush_iotlb_global();

	vtd_write32(VTD_GCMD, GCMD_TE);
	while (!(vtd_read32(VTD_GSTS) & GSTS_TES))
		;
	vtd_enabled = 1;

	char line[128];
	snprintf(line, sizeof(line),
	         "iommu: VT-d at 0x%lx, %u-bit addresses, %u levels, %s, translation on\n",
	         (unsigned long)reg_base, (unsigned)vtd_address_width,
	         (unsigned)vtd_agaw_levels,
	         vtd_passthrough_ok ? "pass-through" : "identity domain");
	console_write(line);
	(void)dmar;
}

/* ── self-test ──────────────────────────────────────────────────── */

static void iommu_report(const char *name, int ok, u64 detail)
{
	console_write(ok ? "M100B-SMOKE: ok " : "M100B-SMOKE: FAIL ");
	console_write(name);
	if (!ok) {
		console_write(" detail=");
		console_write_dec(detail);
	}
	console_write("\n");
}

void iommu_selftest(void)
{
	if (!bootinfo_has_flag("b1nix.test=1"))
		return;

	if (!acpi_find_table("DMAR")) {
		console_write("M100B-SMOKE: skip vtd (no DMAR on this machine)\n");
		return;
	}
	if (!vtd_enabled) {
		iommu_report("vtd-enable", 0, 1);
		return;
	}

	/* The unit reports translation on, and it is the hardware saying so. */
	int on = (vtd_read32(VTD_GSTS) & (GSTS_TES | GSTS_RTPS)) ==
	         (GSTS_TES | GSTS_RTPS);
	int rt = (vtd_read64(VTD_RTADDR) & SL_ADDR_MASK) == root_table_phys;
	iommu_report("vtd-enable", on && rt, (u64)vtd_read32(VTD_GSTS));

	/* A mapping installed through the API must be what the page tables the
	 * hardware walks actually say, and must disappear on unmap. */
	u64 frame = pmm_alloc_frame();
	if (!frame) {
		iommu_report("vtd-map", 0, 2);
		return;
	}
	u64 iova = iommu_iova_alloc(PAGE_SIZE);
	int map_ok = iova != 0 && iommu_map(iova, frame, PAGE_SIZE, 1) == 0 &&
	             iommu_translate(iova) == frame &&
	             iommu_translate(iova + 0x40) == frame + 0x40;
	iommu_unmap(iova, PAGE_SIZE);
	int unmap_ok = iommu_translate(iova) == 0;
	iommu_iova_free(iova, PAGE_SIZE);
	pmm_free_frame(frame);
	iommu_report("vtd-map", map_ok, iova);
	iommu_report("vtd-unmap", unmap_ok, 0);

	/* The IOVA allocator hands out distinct ranges and takes them back. */
	u64 a = iommu_iova_alloc(2 * PAGE_SIZE);
	u64 b = iommu_iova_alloc(PAGE_SIZE);
	int iova_ok = a && b && (b >= a + 2 * PAGE_SIZE || a >= b + PAGE_SIZE);
	iommu_iova_free(a, 2 * PAGE_SIZE);
	iommu_iova_free(b, PAGE_SIZE);
	u64 c = iommu_iova_alloc(PAGE_SIZE);
	iova_ok = iova_ok && c == a; /* freed space is reused */
	iommu_iova_free(c, PAGE_SIZE);
	iommu_report("vtd-iova", iova_ok, a);

	/* dma_map through translation. A device that has been attached gets an
	 * address out of the IOMMU's own space — no copy — and the page tables the
	 * hardware walks must point that address at the caller's buffer. The
	 * function used here is the host bridge (00:00.0): it issues no DMA of its
	 * own, so moving it between domains cannot disturb a live transfer, and
	 * what is under test is the mapping path, not the bridge. */
	{
		struct dma_device dev;
		u32 *buf = kmalloc(PAGE_SIZE);
		int ok = 1;
		u64 detail = 0;

		if (!buf) {
			iommu_report("vtd-dma-map", 0, 3);
			return;
		}
		buf[0] = 0x1D0D1D0Du;
		if (dma_device_attach(&dev, 0, 0, 0, 0xFFFFFFFFULL) != 0) {
			kfree(buf);
			iommu_report("vtd-dma-map", 0, 4);
			return;
		}

		dma_addr_t h = dma_map_single_dev(&dev, buf, 256, DMA_BIDIRECTIONAL);
		detail = h;
		u64 want = vmm_virt_to_phys(buf);
		/* The address must come out of the IOMMU window, not be the physical
		 * address handed back unchanged, and must translate to the buffer. */
		if (!h || h < IOVA_BASE || h == want)
			ok = 0;
		if (ok && iommu_translate(h) != want)
			ok = 0;
		/* ...and it must satisfy a 32-bit device without anything being
		 * copied: the data lives where it always did. */
		if (ok && (h + 255) > 0xFFFFFFFFULL)
			ok = 0;
		if (ok && *(volatile u32 *)(usize)(iommu_translate(h) +
		                                   vmm_direct_map_base()) != buf[0])
			ok = 0;

		dma_unmap_single_dev(&dev, h, 256, DMA_BIDIRECTIONAL);
		if (ok && iommu_translate(h) != 0)
			ok = 0; /* the mapping outlived the unmap */
		dma_device_detach(&dev);
		kfree(buf);
		iommu_report("vtd-dma-map", ok, detail);
	}

	nvme_iommu_selftest();

	/* And the other half of the property: something that was not granted must
	 * not get through. */
	{
		int probe = ac97_iommu_violation_probe();
		if (probe < 0)
			console_write("M100B-SMOKE: skip vtd-blocks-violation (no codec)\n");
		else
			iommu_report("vtd-blocks-violation", probe == 1, 0);
	}

	console_write("M100B-SMOKE: done\n");
}
