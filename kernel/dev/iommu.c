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
#include <b1nix/pci.h>
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

#define VTD_IRTA   0xB8

#define GCMD_TE    (1u << 31) /* translation enable */
#define GCMD_SRTP  (1u << 30) /* set root table pointer */
#define GCMD_IRE   (1u << 25) /* interrupt remapping enable */
#define GCMD_SIRTP (1u << 24) /* set interrupt remap table pointer */
#define GCMD_CFI   (1u << 23) /* allow compatibility-format interrupts */
#define GSTS_TES   (1u << 31)
#define GSTS_RTPS  (1u << 30)
#define GSTS_IRES  (1u << 25)
#define GSTS_IRTPS (1u << 24)
#define GSTS_CFIS  (1u << 23)

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

/* A domain is one device address space: its own page tables and its own id.
 * Domain 1 is the default translated domain (M100b's single domain, kept so a
 * driver that just wants "not the identity domain" still has one); further ids
 * are handed out per device. */
#define DOMAIN_ID 1
/* Ids 1 and 2 are the default translated and identity domains; everything the
 * allocator hands out starts above them. The ceiling comes from the unit
 * (CAP.ND), not from a number picked here. */
#define DOMAIN_ID_FIRST 3

struct iommu_domain {
	struct iommu_domain *next;
	u64 *root;
	u64 root_phys;
	u16 id;
};

static struct iommu_domain *domain_list;
static u16 domain_next_id = DOMAIN_ID_FIRST;
static u16 domain_id_max;
static u64 *domain_root;      /* the default translated domain's root */
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

/* GCMD is not a set of independent bits: every write conveys the whole intended
 * state of the enable bits, so writing one alone clears the rest. (Enabling
 * interrupt remapping this way switched translation back off, which the status
 * register reported and the fault-injection check then failed to see.) The
 * shadow holds the persistent bits; one-shot commands are OR'd in per write. */
static u32 gcmd_shadow;

static u32 vtd_read32(u32 off) { return *(volatile u32 *)(vtd_regs + off); }
static void vtd_write32(u32 off, u32 v) { *(volatile u32 *)(vtd_regs + off) = v; }
static u64 vtd_read64(u32 off) { return *(volatile u64 *)(vtd_regs + off); }
static void vtd_write64(u32 off, u64 v) { *(volatile u64 *)(vtd_regs + off) = v; }

/* ── interrupt remapping ────────────────────────────────────────── */

#define IR_ENTRIES 256u

/* One interrupt remapping entry: 128 bits. Low half carries present, fault
 * processing disable, destination mode, redirection hint, trigger mode, the
 * vector and the destination id; high half carries the source id and how it is
 * validated. */
struct ir_entry {
	u64 low;
	u64 high;
};

static struct ir_entry *ir_table;
static u64 ir_table_phys;
static u8 ir_used[IR_ENTRIES];
static int ir_enabled;

static void gcmd_write(u32 oneshot, u32 persistent_set)
{
	gcmd_shadow |= persistent_set;
	*(volatile u32 *)(vtd_regs + VTD_GCMD) = gcmd_shadow | oneshot;
}

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
static int context_set_domain(u8 bus, u8 devfn, u64 root_phys, u16 domain_id)
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
	if (root_phys) {
		lo = root_phys | ((u64)CTX_TT_TRANSLATED << 2) | 1ULL;
		hi = ((u64)domain_id << 8) | (u64)(vtd_agaw_levels - 2);
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

/* The M100b shape: translated means the default domain, otherwise identity. */
static int context_set(u8 bus, u8 devfn, int translated)
{
	return context_set_domain(bus, devfn,
	                          translated ? domain_root_phys : 0, DOMAIN_ID);
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

/* ── domains ────────────────────────────────────────────────────── */

struct iommu_domain *iommu_domain_create(void)
{
	if (!vtd_enabled)
		return 0;
	struct iommu_domain *dom = kmalloc(sizeof(*dom));
	if (!dom)
		return 0;
	dom->root = alloc_table_page(&dom->root_phys);
	if (!dom->root) {
		kfree(dom);
		return 0;
	}

	u64 flags;
	spin_lock_irqsave(&iommu_lock, &flags);
	if (domain_next_id > domain_id_max) {
		spin_unlock_irqrestore(&iommu_lock, flags);
		pmm_free_frame(dom->root_phys);
		kfree(dom);
		return 0; /* the unit has no id left to give this domain */
	}
	dom->id = domain_next_id++;
	dom->next = domain_list;
	domain_list = dom;
	spin_unlock_irqrestore(&iommu_lock, flags);
	return dom;
}

/* Free the page tables under `table`, `level` levels deep, leaving the frames
 * a caller mapped alone: those belong to whoever asked for the mapping. */
static void sl_free_tree(u64 *table, u32 level)
{
	if (!table || level < 2)
		return;
	for (u32 i = 0; i < 512; i++) {
		u64 e = table[i];
		if (!(e & (SL_READ | SL_WRITE)) || (e & SL_LARGE))
			continue;
		u64 child_phys = e & SL_ADDR_MASK;
		if (level > 2)
			sl_free_tree((u64 *)(usize)(child_phys + vmm_direct_map_base()),
			             level - 1);
		pmm_free_frame(child_phys);
		table[i] = 0;
	}
}

void iommu_domain_destroy(struct iommu_domain *dom)
{
	if (!dom)
		return;
	u64 flags;
	spin_lock_irqsave(&iommu_lock, &flags);
	struct iommu_domain **link = &domain_list;
	for (struct iommu_domain *it = domain_list; it; link = &it->next, it = it->next) {
		if (it == dom) {
			*link = it->next;
			break;
		}
	}
	/* The tables go back to the allocator with the domain. Leaving them
	 * behind leaked a page per level for every domain ever created, which a
	 * per-device domain model turns from a curiosity into a real leak. */
	sl_free_tree(dom->root, vtd_agaw_levels);
	spin_unlock_irqrestore(&iommu_lock, flags);
	pmm_free_frame(dom->root_phys);
	kfree(dom);
	vtd_flush_iotlb_global();
}

u16 iommu_domain_id(const struct iommu_domain *dom)
{
	return dom ? dom->id : 0;
}

/* How many domains the unit can tell apart, and how many are in use. */
u16 iommu_domain_capacity(void) { return vtd_enabled ? domain_id_max : 0; }

int iommu_domain_map(struct iommu_domain *dom, u64 iova, u64 phys, usize size,
                     int writable)
{
	if (!dom || !vtd_enabled)
		return -1;
	if ((iova & (PAGE_SIZE - 1)) || (phys & (PAGE_SIZE - 1)))
		return -1;
	usize pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
	u64 flags;
	spin_lock_irqsave(&iommu_lock, &flags);
	for (usize i = 0; i < pages; i++) {
		u64 *pte = sl_leaf_in(dom->root, iova + (u64)i * PAGE_SIZE, 1);
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

int iommu_domain_unmap(struct iommu_domain *dom, u64 iova, usize size)
{
	if (!dom || !vtd_enabled)
		return -1;
	usize pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
	u64 flags;
	spin_lock_irqsave(&iommu_lock, &flags);
	for (usize i = 0; i < pages; i++) {
		u64 *pte = sl_leaf_in(dom->root, iova + (u64)i * PAGE_SIZE, 0);
		if (pte)
			*pte = 0;
	}
	spin_unlock_irqrestore(&iommu_lock, flags);
	vtd_flush_iotlb_global();
	return 0;
}

u64 iommu_domain_translate(const struct iommu_domain *dom, u64 iova)
{
	if (!dom || !vtd_enabled)
		return 0;
	u64 flags;
	spin_lock_irqsave(&iommu_lock, &flags);
	u64 *pte = sl_leaf_in(dom->root, iova & ~(u64)(PAGE_SIZE - 1), 0);
	u64 out = 0;
	if (pte && (*pte & (SL_READ | SL_WRITE)))
		out = (*pte & SL_ADDR_MASK) | (iova & (PAGE_SIZE - 1));
	spin_unlock_irqrestore(&iommu_lock, flags);
	return out;
}

/* ── groups ─────────────────────────────────────────────────────────
 *
 * Two functions belong to the same group when the unit cannot tell their
 * requests apart well enough to isolate them. On this hardware that is the
 * functions of one multifunction device: they share a slot, and a driver that
 * moves one of them moves what the others can reach too. Encoding the group as
 * (bus, slot) says exactly that, and it is what attach uses — there is no way
 * to ask for less than a group.
 */
/* Walk up until something can isolate this device.
 *
 * A bridge that does not enforce ACS lets its children reach each other without
 * the transfer ever going past the unit, so everything under it is one group —
 * and if that bridge's own parent cannot isolate either, the group grows again.
 * The group is therefore the topmost bridge in the chain that cannot enforce
 * isolation; if the device's immediate parent can, the device stands alone.
 *
 * Returns 1 and fills the bridge when the group is a subtree, 0 when it is just
 * this device's functions.
 */
static int group_root(u8 bus, u8 slot, u8 *gb, u8 *gs, u8 *gf)
{
	u8 b = bus;
	int have = 0;
	for (int depth = 0; depth < 8; depth++) {
		u8 pb, ps, pf;
		if (pci_bridge_for_bus(b, &pb, &ps, &pf) != 0)
			break; /* reached the root bus */
		if (pci_acs_isolating(pb, ps, pf))
			break; /* this port keeps its children apart */
		if (gb) *gb = pb;
		if (gs) *gs = ps;
		if (gf) *gf = pf;
		have = 1;
		b = pb;
	}
	(void)slot;
	return have;
}

u32 iommu_group_of(u8 bus, u8 slot, u8 func)
{
	u8 gb = 0, gs = 0, gf = 0;
	if (group_root(bus, slot, &gb, &gs, &gf))
		return 0x80000000u | ((u32)gb << 8) | gs;

	/* ARI numbers functions across the bus instead of eight per device, so a
	 * group of "this slot" would name a set that does not exist. The bus is
	 * the group there — which is also what SR-IOV needs, since a virtual
	 * function is its physical function's traffic under another number. */
	if (pci_has_ari(bus, slot, func) || pci_has_ari(bus, 0, 0) ||
	    pci_has_sriov(bus, slot, func))
		return 0x40000000u | ((u32)bus << 8);

	/* Otherwise the functions of one multifunction device. */
	return ((u32)bus << 8) | slot;
}

/* Every function the group covers, called back one at a time. */
static void group_for_each(u8 bus, u8 slot,
                           void (*fn)(u8 bus, u8 devfn, void *ctx), void *ctx)
{
	u8 gb = 0, gs = 0, gf = 0;
	if (group_root(bus, slot, &gb, &gs, &gf)) {
		u32 buses = pci_config_read32(gb, gs, gf, 0x18);
		u8 secondary = (u8)((buses >> 8) & 0xFF);
		u8 subordinate = (u8)((buses >> 16) & 0xFF);
		for (u16 b = secondary; b <= subordinate; b++)
			for (u8 sl = 0; sl < 32; sl++)
				for (u8 f = 0; f < 8; f++)
					fn((u8)b, (u8)((sl << 3) | f), ctx);
		/* ...and the bridge itself, which issues its own requests. */
		fn(gb, (u8)((gs << 3) | gf), ctx);
		return;
	}

	u32 group = iommu_group_of(bus, slot, 0);
	if ((group & 0xC0000000u) == 0x40000000u) {
		/* An ARI or SR-IOV device owns the whole bus's function space. */
		for (u32 devfn = 0; devfn < 256; devfn++)
			fn(bus, (u8)devfn, ctx);
		return;
	}
	for (u8 f = 0; f < 8; f++)
		fn(bus, (u8)((slot << 3) | f), ctx);
}

struct group_move {
	u64 root;
	u16 id;
	int rc;
};

static void group_move_one(u8 bus, u8 devfn, void *ctx)
{
	struct group_move *mv = ctx;
	if (mv->rc == 0)
		mv->rc = context_set_domain(bus, devfn, mv->root, mv->id);
}

int iommu_attach_group(struct iommu_domain *dom, u8 bus, u8 slot, u8 func)
{
	if (!vtd_enabled)
		return -1;
	(void)func;
	u64 flags;
	struct group_move mv;
	mv.root = dom ? dom->root_phys : 0;
	mv.id = dom ? dom->id : DOMAIN_ID;
	mv.rc = 0;
	spin_lock_irqsave(&iommu_lock, &flags);
	/* Every function the group covers moves, present or not: one that appears
	 * later must not be left pointing at the domain its group has left. */
	group_for_each(bus, slot, group_move_one, &mv);
	int rc = mv.rc;
	spin_unlock_irqrestore(&iommu_lock, flags);
	if (rc == 0) {
		vtd_flush_context_global();
		vtd_flush_iotlb_global();
	}
	return rc;
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

int iommu_ir_active(void) { return ir_enabled; }

static void ir_flush(void)
{
	/* The interrupt-entry cache is invalidated through the same command
	 * register as the context cache on this generation. */
	vtd_flush_context_global();
}

int iommu_ir_alloc(u8 vector, u32 apic_id, u16 source)
{
	if (!ir_enabled)
		return -1;
	u64 flags;
	int idx = -1;
	spin_lock_irqsave(&iommu_lock, &flags);
	for (u32 i = 0; i < IR_ENTRIES; i++) {
		if (!ir_used[i]) {
			ir_used[i] = 1;
			idx = (int)i;
			break;
		}
	}
	if (idx >= 0) {
		u64 low = 1ULL |                        /* present */
		          (0ULL << 2) |                 /* destination mode: physical */
		          (0ULL << 3) |                 /* redirection hint: off */
		          (0ULL << 4) |                 /* trigger mode: edge */
		          (0ULL << 5) |                 /* delivery mode: fixed */
		          ((u64)vector << 16) |
		          ((u64)(apic_id & 0xFFFFFFFFu) << 32);
		/* Bind the entry to one requester: an interrupt claiming this handle
		 * from any other device is a violation, not a race. */
		u64 high = ((u64)source << 0) | (1ULL << 18); /* SVT = verify SID */
		ir_table[idx].high = high;
		ir_table[idx].low = low;
	}
	spin_unlock_irqrestore(&iommu_lock, flags);
	if (idx >= 0)
		ir_flush();
	return idx;
}

void iommu_ir_free(int handle)
{
	if (!ir_enabled || handle < 0 || (u32)handle >= IR_ENTRIES)
		return;
	u64 flags;
	spin_lock_irqsave(&iommu_lock, &flags);
	ir_table[handle].low = 0;
	ir_table[handle].high = 0;
	ir_used[handle] = 0;
	spin_unlock_irqrestore(&iommu_lock, flags);
	ir_flush();
}

/* Remappable MSI address format: the handle rides in the address, and the data
 * word carries no vector at all — the unit supplies it from the entry. */
u64 iommu_ir_message_address(int handle)
{
	if (!ir_enabled || handle < 0)
		return 0;
	u64 h = (u64)handle;
	return 0xFEE00000ULL |
	       ((h & 0x7FFFULL) << 5) |   /* handle[14:0] */
	       (1ULL << 4) |              /* SHV: subhandle valid */
	       (1ULL << 3) |              /* remappable format */
	       (((h >> 15) & 1ULL) << 2); /* handle[15] */
}

u32 iommu_ir_message_data(int handle)
{
	(void)handle;
	return 0; /* subhandle 0 */
}

int iommu_ir_entry_read(int handle, u8 *vector, u32 *apic_id, u16 *source)
{
	if (!ir_enabled || handle < 0 || (u32)handle >= IR_ENTRIES)
		return -1;
	u64 low = ir_table[handle].low;
	u64 high = ir_table[handle].high;
	if (!(low & 1ULL))
		return -1;
	if (vector)
		*vector = (u8)((low >> 16) & 0xFF);
	if (apic_id)
		*apic_id = (u32)((low >> 32) & 0xFFFFFFFFu);
	if (source)
		*source = (u16)(high & 0xFFFF);
	return 0;
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

static u64 last_fault_addr;
static u16 last_fault_source;
static u8 last_fault_reason;

void iommu_fault_last(u64 *addr, u16 *source, u8 *reason)
{
	if (addr) *addr = last_fault_addr;
	if (source) *source = last_fault_source;
	if (reason) *reason = last_fault_reason;
}

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
			last_fault_addr = vtd_read64(base + i * 16);
			last_fault_source = (u16)(hi & 0xFFFF);
			last_fault_reason = (u8)((hi >> 32) & 0xFF);
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

/* ── ACS policy ─────────────────────────────────────────────────────
 *
 * Whether a switch port forwards traffic between its children decides how
 * small a group can be, and it is ours to set. Setting it is a trade, not a
 * free win: the redirects that make devices separable also push
 * device-to-device transfers up through the root complex, which is slower and
 * on some hardware simply unavailable. So it happens once, here, under a
 * policy the boot can state:
 *
 *   b1nix.acs=on    turn the controls on wherever the hardware has them (default)
 *   b1nix.acs=keep  leave whatever the firmware set, and group accordingly
 *   b1nix.acs=off   turn them off, and accept the larger groups that follow
 */
/* Is bus:slot.func named in a comma-separated list of bdfs? */
static int acs_keep_lists(const char *list, u8 bus, u8 slot, u8 func)
{
	const char *p = list;
	while (*p) {
		u32 v[3] = {0, 0, 0};
		int field = 0;
		int digits = 0;
		while (*p && *p != ',') {
			char c = *p;
			if (c == ':' || c == '.') {
				field++;
				digits = 0;
				if (field > 2)
					break;
			} else {
				u32 d;
				if (c >= '0' && c <= '9') d = (u32)(c - '0');
				else if (c >= 'a' && c <= 'f') d = (u32)(c - 'a' + 10);
				else if (c >= 'A' && c <= 'F') d = (u32)(c - 'A' + 10);
				else { field = 3; break; }
				v[field] = v[field] * 16 + d;
				digits++;
			}
			p++;
		}
		while (*p && *p != ',')
			p++;
		if (*p == ',')
			p++;
		if (field == 2 && digits > 0 && v[0] == bus && v[1] == slot &&
		    v[2] == func)
			return 1;
	}
	return 0;
}

static void acs_configure(void)
{
	char mode[16];
	int want_on = 1, want_off = 0;
	if (bootinfo_get_kv("b1nix.acs", mode, sizeof(mode)) && mode[0]) {
		if (mode[0] == 'k') { want_on = 0; }
		else if (mode[0] == 'o' && mode[1] == 'f') { want_on = 0; want_off = 1; }
	}

	/* A port may be listed for exception: b1nix.acs-keep=<bdf>[,<bdf>...],
	 * each as bus:slot.func in hex. Those ports are left exactly as they were
	 * found — which is what a machine whose firmware configured ACS on purpose
	 * needs, and what a device that must keep peer-to-peer traffic needs. */
	char keep_list[128];
	int have_keep = bootinfo_get_kv("b1nix.acs-keep", keep_list,
	                                sizeof(keep_list)) && keep_list[0];

	u32 capable = 0, isolating = 0, kept = 0;
	for (u32 bus = 0; bus < 256; bus++) {
		for (u32 slot = 0; slot < 32; slot++) {
			if (pci_config_read16((u8)bus, (u8)slot, 0, 0) == 0xFFFF)
				continue;
			u8 htype = pci_config_read8((u8)bus, (u8)slot, 0,
			                            PCI_CFG_HEADER_TYPE);
			u8 nfunc = (htype & 0x80) ? 8 : 1;
			for (u8 f = 0; f < nfunc; f++) {
				if (pci_config_read16((u8)bus, (u8)slot, f, 0) == 0xFFFF)
					continue;
				u8 hf = (u8)(pci_config_read8((u8)bus, (u8)slot, f,
				                              PCI_CFG_HEADER_TYPE) & 0x7F);
				if (hf != 1)
					continue;
				if (!pci_find_ext_capability((u8)bus, (u8)slot, f, 0x000D))
					continue;
				capable++;
				int excepted = have_keep &&
				               acs_keep_lists(keep_list, (u8)bus, (u8)slot, f);
				if (excepted) {
					kept++;
				} else if (want_on) {
					pci_acs_enable((u8)bus, (u8)slot, f);
				} else if (want_off) {
					pci_acs_disable((u8)bus, (u8)slot, f);
				}
				if (pci_acs_isolating((u8)bus, (u8)slot, f))
					isolating++;
			}
		}
	}

	char line[160];
	snprintf(line, sizeof(line),
	         "iommu: ACS %s — %u ports have it, %u isolating, %u left alone\n",
	         want_on ? "on" : (want_off ? "off" : "kept"),
	         (unsigned)capable, (unsigned)isolating, (unsigned)kept);
	console_write(line);

	/* With a policy this specific, the ports it applies to are worth naming:
	 * a wrong bdf in the list is otherwise invisible. */
	for (u32 bus = 0; bus < 256; bus++) {
		for (u32 slot = 0; slot < 32; slot++) {
			if (pci_config_read16((u8)bus, (u8)slot, 0, 0) == 0xFFFF)
				continue;
			u8 htype = pci_config_read8((u8)bus, (u8)slot, 0,
			                            PCI_CFG_HEADER_TYPE);
			u8 nfunc = (htype & 0x80) ? 8 : 1;
			for (u8 f = 0; f < nfunc; f++) {
				if ((u8)(pci_config_read8((u8)bus, (u8)slot, f,
				                          PCI_CFG_HEADER_TYPE) & 0x7F) != 1)
					continue;
				if (!pci_find_ext_capability((u8)bus, (u8)slot, f, 0x000D))
					continue;
				snprintf(line, sizeof(line), "iommu: acs port %02x:%02x.%u %s\n",
				         (unsigned)bus, (unsigned)slot, (unsigned)f,
				         pci_acs_isolating((u8)bus, (u8)slot, f) ? "isolating"
				                                                 : "open");
				console_write(line);
			}
		}
	}
}

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

	/* CAP.ND encodes how many domain ids the unit supports: 0 -> 16, 1 -> 64,
	 * and so on up to 2^16. Asking for more than that would make two domains
	 * indistinguishable to the hardware. */
	{
		u32 nd = (u32)(vtd_cap & 0x7);
		u32 ids = 16u << (nd * 2);
		if (ids > 65536u)
			ids = 65536u;
		domain_id_max = (u16)(ids - 1);
	}

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
	/* Every bus that has anything on it, not a fixed few: a device on a bus
	 * with no context table behind it faults on its first transfer, and a
	 * bridge can put its children on any bus number the firmware chose. (A
	 * device behind a root port landed on a bus past the old limit of four and
	 * did exactly that, which showed up as a fault during an unrelated
	 * check.) */
	for (u32 bus = 0; bus < 256; bus++) {
		int populated = 0;
		for (u32 slot = 0; slot < 32 && !populated; slot++)
			if (pci_config_read16((u8)bus, (u8)slot, 0, 0) != 0xFFFF)
				populated = 1;
		if (!populated)
			continue;
		for (u32 devfn = 0; devfn < 256; devfn++)
			if (context_set((u8)bus, (u8)devfn, 0) != 0)
				return;
	}

	vtd_write64(VTD_RTADDR, root_table_phys);
	gcmd_write(GCMD_SRTP, 0);
	while (!(vtd_read32(VTD_GSTS) & GSTS_RTPS))
		;
	vtd_flush_context_global();
	vtd_flush_iotlb_global();

	gcmd_write(0, GCMD_TE);
	while (!(vtd_read32(VTD_GSTS) & GSTS_TES))
		;
	vtd_enabled = 1;

	/* Decide, once and out loud, which ports keep their children apart. Every
	 * grouping decision after this only reads that state. */
	acs_configure();

	/* Interrupt remapping, when the unit has it (ECAP.IR). Compatibility-format
	 * interrupts stay allowed: the IOAPIC and every driver still on the legacy
	 * message format must keep working, exactly as pass-through kept DMA
	 * working when translation went on. */
	if (vtd_ecap & (1ULL << 3)) {
		ir_table = (struct ir_entry *)alloc_table_page(&ir_table_phys);
		if (ir_table) {
			/* IRTA: table address plus log2(entries) - 1 in the low bits. */
			u64 size_field = 7; /* 256 entries */
			vtd_write64(VTD_IRTA, ir_table_phys | size_field);
			gcmd_write(GCMD_SIRTP, 0);
			while (!(vtd_read32(VTD_GSTS) & GSTS_IRTPS))
				;
			/* CFI keeps compatibility-format interrupts (the IOAPIC's, and
			 * every driver still on the legacy message) working, the same way
			 * pass-through kept DMA working when translation went on. */
			gcmd_write(0, GCMD_CFI | GCMD_IRE);
			while (!(vtd_read32(VTD_GSTS) & GSTS_IRES))
				;
			ir_enabled = 1;
		}
	}

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

static void iommu_report_as(const char *suite, const char *name, int ok,
                            u64 detail)
{
	console_write(suite);
	console_write(ok ? ": ok " : ": FAIL ");
	console_write(name);
	if (!ok) {
		console_write(" detail=");
		console_write_dec(detail);
	}
	console_write("\n");
}

static void iommu_report(const char *name, int ok, u64 detail)
{
	iommu_report_as("M100B-SMOKE", name, ok, detail);
}

static void iommu_report_c(const char *name, int ok, u64 detail)
{
	iommu_report_as("M100C-SMOKE", name, ok, detail);
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

	/* And the other half of the property: something that was not granted must
	 * not get through. */
	{
		int probe = ac97_iommu_violation_probe();
		if (probe < 0)
			console_write("M100B-SMOKE: skip vtd-blocks-violation (no codec)\n");
		else
			iommu_report("vtd-blocks-violation", probe == 1, 0);
	}

	/* ── M100c: domains, groups, interrupt remapping ─────────────── */

	/* Two domains, and a page that exists in one of them only. Isolation
	 * between devices is the claim; this is it stated in page tables. */
	{
		struct iommu_domain *a = iommu_domain_create();
		struct iommu_domain *b = iommu_domain_create();
		u64 frame = pmm_alloc_frame();
		int ok = a && b && frame && iommu_domain_id(a) != iommu_domain_id(b);
		if (ok && iommu_domain_map(a, 0x40000000ULL, frame, PAGE_SIZE, 1) != 0)
			ok = 0;
		if (ok && iommu_domain_translate(a, 0x40000000ULL) != frame)
			ok = 0;
		/* The other domain never saw it. */
		if (ok && iommu_domain_translate(b, 0x40000000ULL) != 0)
			ok = 0;
		if (ok)
			iommu_domain_unmap(a, 0x40000000ULL, PAGE_SIZE);
		if (a) iommu_domain_destroy(a);
		if (b) iommu_domain_destroy(b);
		if (frame) pmm_free_frame(frame);
		iommu_report_c("domains-isolated", ok, 0);
	}

	/* Domain ids come from the unit, and a destroyed domain gives its tables
	 * back: with one domain per device, leaking a page per level per domain
	 * would be a real leak rather than a curiosity. */
	{
		/* One throwaway cycle first. The domain object comes from the kernel
		 * heap, and the first allocation of its size can grow the heap by a
		 * whole chunk — frames that are gone for good and have nothing to do
		 * with the page tables this checks. */
		{
			struct iommu_domain *warm = iommu_domain_create();
			u64 wf = pmm_alloc_frame();
			if (warm && wf)
				iommu_domain_map(warm, 0x50000000ULL, wf, PAGE_SIZE, 1);
			if (warm)
				iommu_domain_destroy(warm);
			if (wf)
				pmm_free_frame(wf);
		}

		/*
		 * Measured over two identical cycles, reporting the second.
		 *
		 * The counter is global, so it also moves when the kernel heap grows —
		 * and the heap grows on the first allocation of a size, which is not
		 * something this check is about. One warm-up cycle used to be enough;
		 * it stopped being enough when an unrelated structure changed size and
		 * shifted which allocation was the first of its bucket. Repeating the
		 * measurement separates the two for good: a genuine leak repeats every
		 * cycle, heap growth happens once.
		 */
		usize before = 0, after = 0;
		int ok = 1;

		for (int pass = 0; pass < 2 && ok; pass++) {
			before = pmm_owned_free_frames();
			struct iommu_domain *d = iommu_domain_create();
			ok = d != 0 && iommu_domain_capacity() >= 16;
			u64 frame = pmm_alloc_frame();
			if (ok && frame)
				ok = iommu_domain_map(d, 0x50000000ULL, frame, PAGE_SIZE, 1) == 0;
			if (d)
				iommu_domain_destroy(d);
			if (frame)
				pmm_free_frame(frame);
			after = pmm_owned_free_frames();
		}
		/* Every page the domain took — root and each level of the walk — is
		 * back. */
		if (after != before)
			ok = 0;
		iommu_report_c("domain-tables-freed", ok,
		               (u64)(before > after ? before - after : after - before));
	}

	/* A group moves as a unit: every function of the slot ends up in the
	 * domain, not just the one that was named. */
	{
		struct iommu_domain *dom = iommu_domain_create();
		int ok = dom != 0;
		if (ok) {
			u32 g0 = iommu_group_of(0, 3, 0);
			u32 g1 = iommu_group_of(0, 3, 1);
			u32 other = iommu_group_of(0, 4, 0);
			if (g0 != g1 || g0 == other)
				ok = 0;
			if (ok && iommu_attach_group(dom, 0, 3, 0) != 0)
				ok = 0;
			if (ok) {
				/* Read the context entries back: both functions must name the
				 * domain's page tables. */
				for (u8 f = 0; f < 2 && ok; f++) {
					u64 *ctx = &context_tables[0][((3 << 3) | f) * 2];
					if ((ctx[0] & SL_ADDR_MASK) != dom->root_phys ||
					    ((ctx[1] >> 8) & 0xFFFF) != iommu_domain_id(dom))
						ok = 0;
				}
			}
			iommu_attach_group(0, 0, 3, 0); /* back to where it was */
			iommu_domain_destroy(dom);
		}
		iommu_report_c("group-moves-together", ok, 0);
	}

	/* A bridge that cannot stop peer traffic is the unit of isolation: two
	 * devices under it reach each other without the unit ever seeing the
	 * transfer, so they are one group whatever the page tables say. Looked up
	 * on a bridge this machine really has rather than assumed. */
	{
		u8 legacy_bus = 0, lb = 0, ls = 0, lf = 0;
		for (u16 b = 1; b < 256 && !legacy_bus; b++) {
			if (pci_config_read16((u8)b, 1, 0, 0) == 0xFFFF &&
			    pci_config_read16((u8)b, 2, 0, 0) == 0xFFFF)
				continue;
			if (pci_bridge_for_bus((u8)b, &lb, &ls, &lf) != 0)
				continue;
			if (!pci_acs_isolating(lb, ls, lf))
				legacy_bus = (u8)b;
		}
		if (!legacy_bus) {
			console_write("M100C-SMOKE: skip group-behind-bridge (no bridge without ACS)\n");
		} else {
			u32 g0 = iommu_group_of(legacy_bus, 1, 0);
			u32 g1 = iommu_group_of(legacy_bus, 2, 0);
			u32 root_group = iommu_group_of(0, 3, 0);
			int ok = g0 == g1 && g0 != root_group;

			/* And moving one of them rewrites the bridge's own context entry
			 * too, since the bridge is what the unit sees. */
			struct iommu_domain *dom = iommu_domain_create();
			if (ok && dom) {
				if (iommu_attach_group(dom, legacy_bus, 1, 0) != 0)
					ok = 0;
				if (ok) {
					u64 *ctx = &context_tables[lb][((ls << 3) | lf) * 2];
					if ((ctx[0] & SL_ADDR_MASK) != dom->root_phys)
						ok = 0;
				}
				iommu_attach_group(0, legacy_bus, 1, 0);
			} else {
				ok = 0;
			}
			if (dom)
				iommu_domain_destroy(dom);
			iommu_report_c("group-behind-bridge", ok, (u64)legacy_bus);
		}
	}

	/* ACS: two endpoints behind different downstream ports of a switch. When
	 * the ports enforce ACS neither can reach the other without going upstream
	 * past the unit, so they are separate groups; when they cannot enforce it,
	 * the honest answer is one group. Endpoints are picked by header type — a
	 * bridge's own bus says nothing about who can reach whom. */
	{
		u8 ep_bus[2] = {0, 0};
		u8 ep_slot[2] = {0, 0};
		u8 par_bus[2] = {0, 0}, par_slot[2] = {0, 0}, par_func[2] = {0, 0};
		int n = 0;

		for (u16 b = 1; b < 256 && n < 2; b++) {
			for (u8 sl = 0; sl < 32 && n < 2; sl++) {
				if (pci_config_read16((u8)b, sl, 0, 0) == 0xFFFF)
					continue;
				u8 htype = (u8)(pci_config_read8((u8)b, sl, 0,
				                                 PCI_CFG_HEADER_TYPE) & 0x7F);
				if (htype == 1)
					continue; /* a bridge, not an endpoint */
				u8 pb, ps, pf;
				if (pci_bridge_for_bus((u8)b, &pb, &ps, &pf) != 0)
					continue;
				if (n == 1 && pb == par_bus[0] && ps == par_slot[0] &&
				    pf == par_func[0])
					continue; /* same port: that pair proves nothing here */
				ep_bus[n] = (u8)b;
				ep_slot[n] = sl;
				par_bus[n] = pb;
				par_slot[n] = ps;
				par_func[n] = pf;
				n++;
			}
		}

		if (n < 2) {
			console_write("M100C-SMOKE: skip acs-splits-group (no two ports)\n");
		} else {
			int enforced = pci_acs_isolating(par_bus[0], par_slot[0], par_func[0]) &&
			               pci_acs_isolating(par_bus[1], par_slot[1], par_func[1]);
			u32 g1 = iommu_group_of(ep_bus[0], ep_slot[0], 0);
			u32 g2 = iommu_group_of(ep_bus[1], ep_slot[1], 0);
			int ok = enforced ? (g1 != g2) : (g1 == g2);
			iommu_report_c("acs-splits-group", ok,
			               ((u64)enforced << 32) | (u64)g1);
		}
	}

	/* Asking which group a device is in must not reprogram the machine. The
	 * ACS control words are snapshotted, every grouping question the other
	 * checks ask is asked again, and the words must be unchanged — the earlier
	 * version enabled ACS from inside the query, which made a device's group
	 * depend on whether anyone had asked before. */
	{
		u32 before[8], after[8];
		u8 pb[8], ps[8], pf[8];
		u32 n = 0;
		for (u32 bus = 0; bus < 256 && n < 8; bus++) {
			for (u32 slot = 0; slot < 32 && n < 8; slot++) {
				if (pci_config_read16((u8)bus, (u8)slot, 0, 0) == 0xFFFF)
					continue;
				u8 hf = (u8)(pci_config_read8((u8)bus, (u8)slot, 0,
				                              PCI_CFG_HEADER_TYPE) & 0x7F);
				if (hf != 1)
					continue;
				u16 cap = pci_find_ext_capability((u8)bus, (u8)slot, 0, 0x000D);
				if (!cap)
					continue;
				pb[n] = (u8)bus; ps[n] = (u8)slot; pf[n] = 0;
				before[n] = pci_ecam_read32((u8)bus, (u8)slot, 0,
				                            (u16)(cap + 0x04));
				n++;
			}
		}
		if (n == 0) {
			console_write("M100C-SMOKE: skip group-query-is-read-only (no ACS ports)\n");
		} else {
			for (u32 bus = 0; bus < 8; bus++)
				for (u8 slot = 0; slot < 8; slot++)
					(void)iommu_group_of((u8)bus, slot, 0);
			int ok = 1;
			for (u32 i = 0; i < n; i++) {
				u16 cap = pci_find_ext_capability(pb[i], ps[i], pf[i], 0x000D);
				after[i] = pci_ecam_read32(pb[i], ps[i], pf[i],
				                           (u16)(cap + 0x04));
				if (after[i] != before[i])
					ok = 0;
			}
			iommu_report_c("group-query-is-read-only", ok, (u64)n);
		}
	}

	/* The policy can spare a single port. A machine whose firmware configured
	 * ACS deliberately, or a device that must keep peer-to-peer traffic, needs
	 * that — and the exception is only worth anything if it is exact: the
	 * named port must be untouched while every other one obeys the policy. */
	{
		char keep[128];
		if (!bootinfo_get_kv("b1nix.acs-keep", keep, sizeof(keep)) || !keep[0]) {
			console_write("M100C-SMOKE: skip acs-keeps-port (no exception asked for)\n");
		} else {
			int named = 0, named_open = 0, others = 0, others_isolating = 0;
			for (u32 bus = 0; bus < 256; bus++) {
				for (u32 slot = 0; slot < 32; slot++) {
					if (pci_config_read16((u8)bus, (u8)slot, 0, 0) == 0xFFFF)
						continue;
					for (u8 f = 0; f < 8; f++) {
						if ((u8)(pci_config_read8((u8)bus, (u8)slot, f,
						                          PCI_CFG_HEADER_TYPE) & 0x7F) != 1)
							continue;
						if (!pci_find_ext_capability((u8)bus, (u8)slot, f, 0x000D))
							continue;
						int iso = pci_acs_isolating((u8)bus, (u8)slot, f);
						if (acs_keep_lists(keep, (u8)bus, (u8)slot, f)) {
							named++;
							if (!iso)
								named_open++;
						} else {
							others++;
							if (iso)
								others_isolating++;
						}
					}
				}
			}
			/* The named port was left as the machine powers up (open), every
			 * other one took the policy. */
			int ok = named > 0 && named == named_open && others > 0 &&
			         others == others_isolating;
			iommu_report_c("acs-keeps-port", ok,
			               ((u64)named << 24) | ((u64)named_open << 16) |
			                   ((u64)others << 8) | (u64)others_isolating);
		}
	}

	/* ARI / SR-IOV: functions numbered across the bus. A device that has
	 * either owns the bus's function space, so the group is the bus. */
	{
		int found = 0;
		int ok = 1;
		for (u16 b = 0; b < 256 && !found; b++) {
			for (u8 sl = 0; sl < 32 && !found; sl++) {
				if (pci_config_read16((u8)b, sl, 0, 0) == 0xFFFF)
					continue;
				if (!pci_has_ari((u8)b, sl, 0) && !pci_has_sriov((u8)b, sl, 0))
					continue;
				found = 1;
				u32 g = iommu_group_of((u8)b, sl, 0);
				/* Every function of that bus lands in the same group... */
				if (g != iommu_group_of((u8)b, sl, 7) ||
				    g != iommu_group_of((u8)b, (u8)((sl + 1) & 0x1F), 0))
					ok = 0;
				/* ...and it is not the plain slot group. */
				if (g == (((u32)b << 8) | sl))
					ok = 0;
			}
		}
		if (!found)
			console_write("M100C-SMOKE: skip ari-owns-bus (no ARI or SR-IOV device)\n");
		else
			iommu_report_c("ari-owns-bus", ok, 0);
	}

	if (!ir_enabled) {
		console_write("M100C-SMOKE: skip ir (unit has no interrupt remapping)\n");
	} else {
		/* The entry is what the kernel asked for, is bound to one requester,
		 * and the message the device gets carries a handle rather than the
		 * vector itself. */
		int h = iommu_ir_alloc(0x33, 0, 0x0102);
		u8 v = 0; u32 dest = 0xFFFF; u16 src = 0;
		int ok = h >= 0 && iommu_ir_entry_read(h, &v, &dest, &src) == 0 &&
		         v == 0x33 && dest == 0 && src == 0x0102;
		u64 addr = iommu_ir_message_address(h);
		/* Remappable format: the format bit is set and the handle is in the
		 * address, so the data word no longer names a vector. */
		if (ok && (!(addr & (1ULL << 3)) ||
		           ((addr >> 5) & 0x7FFF) != (u64)h ||
		           iommu_ir_message_data(h) != 0))
			ok = 0;
		iommu_ir_free(h);
		if (ok && iommu_ir_entry_read(h, 0, 0, 0) == 0)
			ok = 0; /* a freed entry must stop being present */
		iommu_report_c("ir-entry", ok, (u64)h);
		console_write("M100C-SMOKE: ok ir-enabled\n");

		int rej = nvme_ir_rejection_probe();
		if (rej < 0)
			console_write("M100C-SMOKE: skip ir-rejects-unknown (no remapped device)\n");
		else
			iommu_report_c("ir-rejects-unknown", rej == 1, 0);
	}

	console_write("M100B-SMOKE: done\n");
}
