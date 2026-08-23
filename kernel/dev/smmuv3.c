/*
 * ARM SMMUv3 — the DMA remapping unit in front of this board's PCIe devices.
 *
 * Structured to read alongside kernel/dev/iommu.c (Intel VT-d) and
 * kernel/dev/amdvi.c (AMD-Vi): probe, bring-up, a domain, and a self-test that
 * proves a device really cannot reach memory it was not granted. Nothing is
 * shared with them. The registers are ARM's, the tables are the AArch64 VMSA
 * long descriptors this kernel already uses for its own address spaces, and
 * every configuration change has to travel through a command queue rather than
 * being a register write.
 *
 * Stage 1, not stage 2. The choice is made from IDR0 at runtime (see
 * smmuv3_init), but on QEMU virt IDR0.S1P is the only stage advertised, and
 * stage 1's translation tables are bit-for-bit the format kernel/arch/aarch64
 * already programs into TTBR0 — so a mapping here can be read back with the
 * same descriptor rules the rest of the port uses, and a mistake in them shows
 * up as a fault rather than as a plausible wrong answer.
 *
 * Bypass is the starting state for every stream, for the reason it is on both
 * x86 units: every driver in this tree hands its device a physical address,
 * and they would all fault the instant translation applied to them. SMMUv3
 * spells that in the stream table entry (V=1, Config=BYPASS), so it needs no
 * page tables at all until a device is deliberately moved into a domain.
 *
 * Interrupts are not wired. The unit's four SPIs are in the device tree and
 * are read out below for the probe line, but the event queue is polled rather
 * than delivered — which is all the fault-counting interface needs, and is
 * what kernel/dev/amdvi.c does with its event log for the same reason.
 */

#include <b1nix/arch.h>
#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/memtype.h>
#include <b1nix/mm.h>
#include <b1nix/pci.h>
#include <b1nix/smmuv3.h>
#include <b1nix/spinlock.h>
#include <stdio.h>
#include <string.h>

/* ── register file (ARM IHI 0070, section 6) ────────────────────────
 *
 * The unit presents two 64 KiB pages; the producer/consumer indices of the
 * event and PRI queues live in the second one, at their page-1 offsets. QEMU
 * makes the two pages exact aliases, but real silicon does not, so the page-1
 * offsets are used as the architecture writes them. */
#define SMMU_IDR0            0x0000
#define SMMU_IDR1            0x0004
#define SMMU_IDR5            0x0014
#define SMMU_CR0             0x0020
#define SMMU_CR0ACK          0x0024
#define SMMU_CR1             0x0028
#define SMMU_CR2             0x002C
#define SMMU_GBPA            0x0044
#define SMMU_IRQ_CTRL        0x0050
#define SMMU_GERROR          0x0060
#define SMMU_GERRORN         0x0064
#define SMMU_STRTAB_BASE     0x0080
#define SMMU_STRTAB_BASE_CFG 0x0088
#define SMMU_CMDQ_BASE       0x0090
#define SMMU_CMDQ_PROD       0x0098
#define SMMU_CMDQ_CONS       0x009C
#define SMMU_EVTQ_BASE       0x00A0
#define SMMU_EVTQ_PROD       0x100A8
#define SMMU_EVTQ_CONS       0x100AC

#define IDR0_S2P     (1u << 0)
#define IDR0_S1P     (1u << 1)
#define IDR0_TTF_SHIFT 2
#define IDR0_TTF_MASK  0x3u
#define IDR0_COHACC  (1u << 4)
#define IDR0_HTTU_SHIFT 6
#define IDR0_HTTU_MASK  0x3u
#define IDR0_STALL_MODEL_SHIFT 24
#define IDR0_STALL_MODEL_MASK  0x3u

#define IDR1_SIDSIZE_MASK  0x3fu
#define IDR1_CMDQS_SHIFT   21
#define IDR1_EVTQS_SHIFT   16
#define IDR1_QS_MASK       0x1fu
#define IDR1_QUEUES_PRESET (1u << 29)
#define IDR1_TABLES_PRESET (1u << 30)

#define IDR5_OAS_MASK  0x7u
#define IDR5_GRAN4K    (1u << 4)
#define IDR5_GRAN16K   (1u << 5)
#define IDR5_GRAN64K   (1u << 6)

#define CR0_SMMUEN  (1u << 0)
#define CR0_EVTQEN  (1u << 2)
#define CR0_CMDQEN  (1u << 3)

/* CR1's cacheability fields do not use the TCR encoding: 1 is write-back. */
#define CR1_CACHE_WB 1u
#define CR1_SH_ISH   3u
#define CR1_TABLE_SH_SHIFT 10
#define CR1_TABLE_OC_SHIFT 8
#define CR1_TABLE_IC_SHIFT 6
#define CR1_QUEUE_SH_SHIFT 4
#define CR1_QUEUE_OC_SHIFT 2
#define CR1_QUEUE_IC_SHIFT 0

#define CR2_RECINVSID (1u << 1)
#define CR2_PTM       (1u << 2)

#define GBPA_UPDATE (1u << 31)
#define GBPA_ABORT  (1u << 20)

#define Q_BASE_RWA       (1ULL << 62)
#define Q_BASE_ADDR_MASK 0x000FFFFFFFFFFFE0ULL /* bits 51:5 */
#define STRTAB_BASE_RA   (1ULL << 62)
#define STRTAB_BASE_MASK 0x000FFFFFFFFFFFC0ULL /* bits 51:6 */
#define STRTAB_BASE_CFG_FMT_LINEAR 0u

/* ── stream table entry (8 quadwords) ───────────────────────────────── */
#define STE_0_V             (1ULL << 0)
#define STE_0_CFG_SHIFT     1
#define STE_0_CFG_BYPASS    4ULL
#define STE_0_CFG_S1_TRANS  5ULL
#define STE_0_S1FMT_SHIFT   4  /* 0 = a linear context-descriptor table */
#define STE_0_S1CTXPTR_MASK 0x000FFFFFFFFFFFC0ULL /* bits 51:6 */
#define STE_0_S1CDMAX_SHIFT 59

#define STE_1_S1CIR_SHIFT    2
#define STE_1_S1COR_SHIFT    4
#define STE_1_S1CSH_SHIFT    6
#define STE_1_S1STALLD       (1ULL << 27)
#define STE_1_STRW_SHIFT     30 /* 0 = NSEL1, the non-secure EL1 regime */
#define STE_1_SHCFG_SHIFT    44
#define STE_1_SHCFG_INCOMING 1ULL

/* ── context descriptor (8 quadwords) ───────────────────────────────── */
#define CD_0_TCR_T0SZ_SHIFT  0
#define CD_0_TCR_TG0_SHIFT   6   /* 0 = 4 KiB granule */
#define CD_0_TCR_IRGN0_SHIFT 8
#define CD_0_TCR_ORGN0_SHIFT 10
#define CD_0_TCR_SH0_SHIFT   12
#define CD_0_TCR_EPD1        (1ULL << 30)
#define CD_0_V               (1ULL << 31)
#define CD_0_TCR_IPS_SHIFT   32
#define CD_0_AA64            (1ULL << 41)
#define CD_0_R               (1ULL << 45)
#define CD_0_A               (1ULL << 46)
#define CD_0_ASET            (1ULL << 47)
#define CD_0_ASID_SHIFT      48
#define CD_1_TTB0_MASK       0x000FFFFFFFFFFFF0ULL /* bits 51:4 */

/* ── commands ───────────────────────────────────────────────────────── */
#define CMD_PREFETCH_CFG  0x01ULL
#define CMD_CFGI_STE      0x03ULL
#define CMD_CFGI_ALL      0x04ULL
#define CMD_TLBI_NH_ASID  0x11ULL
#define CMD_TLBI_NSNH_ALL 0x30ULL
#define CMD_SYNC          0x46ULL

#define CMDQ_LOG2 7u /* 128 entries of 16 bytes = 2 KiB */
#define EVTQ_LOG2 7u /* 128 entries of 32 bytes = 4 KiB */

/* A linear stream table is enough here, and a two-level one would be dead
 * code on this board. The device tree's `iommu-map` is an identity map of PCI
 * requester ids onto StreamIDs, and the PCIe node declares
 * `bus-range = <0 0xf>` — so the widest StreamID any device on this machine
 * can present is 0xFFF. 4096 entries of 64 bytes is 256 KiB, allocated once.
 * IDR1.SIDSIZE is honoured as an upper bound in case a unit advertises fewer
 * StreamID bits than that. */
#define STRTAB_LOG2_WANTED 12u
#define STE_QWORDS 8u

/* Stage-1 translation tables: 4 KiB granule, T0SZ 25 => a 39-bit device
 * address space walked in three levels starting at level 1. That is the same
 * shape kernel/arch/aarch64/paging.c programs into TTBR0, so a descriptor that
 * is wrong here is wrong in a way this port already knows how to read. */
#define SMMU_T0SZ   25u
#define SMMU_IAS    (64u - SMMU_T0SZ)
#define SMMU_ASID   1u

#define PTE_VALID     (1ULL << 0)
#define PTE_TABLE     (1ULL << 1)
#define PTE_PAGE      (1ULL << 1)
#define PTE_ATTRIDX0  (0ULL << 2)
#define PTE_AP_RW     (0ULL << 6)
#define PTE_AP_RO     (2ULL << 6)
#define PTE_SH_ISH    (3ULL << 8)
#define PTE_AF        (1ULL << 10)
#define PTE_ADDR_MASK 0x0000FFFFFFFFF000ULL

/* Event queue records are 32 bytes: the event type is the low byte of word 0,
 * the StreamID is word 0 bits 63:32, and the address the device asked for is
 * word 2. */
#define EVT_TYPE_MASK 0xFFu

static volatile u8 *smmu_regs;
static u64 smmu_base_phys;
static u32 smmu_idr[6];
static u32 strtab_log2;

static u64 *strtab;
static u64 strtab_phys;
static u64 *cmdq;
static u64 cmdq_phys;
static u64 *evtq;
static u64 evtq_phys;
static u32 evtq_cons;   /* our own read index into the event queue */
static u32 evtq_wrap;   /* the wrap bit that goes with it */
static u32 evtq_faults; /* faults seen since the last clear */
static u64 evtq_last_addr;
static u32 evtq_last_sid;
static u8 evtq_last_type;

static u64 *cd_table; /* one context descriptor, shared by the domain */
static u64 cd_phys;
static u64 *pt_root; /* level-1 table of the domain's stage-1 walk */
static u64 pt_root_phys;

static int smmu_enabled;
static spinlock_t smmu_lock = SPINLOCK_INIT;

static u32 reg_read32(u32 off) { return *(volatile u32 *)(smmu_regs + off); }
static u64 reg_read64(u32 off) { return *(volatile u64 *)(smmu_regs + off); }
static void reg_write32(u32 off, u32 v) { *(volatile u32 *)(smmu_regs + off) = v; }
static void reg_write64(u32 off, u64 v) { *(volatile u64 *)(smmu_regs + off) = v; }

int smmuv3_active(void) { return smmu_enabled; }

/* Zeroed physical memory, aligned to its own size.
 *
 * The alignment is not a nicety: SMMU_STRTAB_BASE has no bits below the
 * table's size, so a stream table that merely starts on a page boundary is
 * read by the unit from a *different* address than the one this driver filled
 * in. Every StreamID then looks unconfigured, GBPA aborts the transaction, and
 * the only symptom is that a device's DMA quietly stops working — which is how
 * this was found: NVMe's first admin command never completed. The frame
 * allocator promises 4 KiB, so over-allocate and round up. */
static void *alloc_zero_aligned(usize pages, u64 *phys_out)
{
	u64 bytes = (u64)pages * PAGE_SIZE;
	u64 align = bytes;
	u64 frame = pmm_alloc_frames(pages + (usize)(align / PAGE_SIZE));
	void *va;

	if (!frame)
		return 0;

	u64 aligned = (frame + align - 1) & ~(align - 1);

	va = (void *)(usize)(aligned + vmm_direct_map_base());
	memset(va, 0, (usize)bytes);
	if (phys_out)
		*phys_out = aligned;
	return va;
}

static void *alloc_zero_pages(usize pages, u64 *phys_out)
{
	return alloc_zero_aligned(pages, phys_out);
}

/* ── command queue ──────────────────────────────────────────────────
 *
 * Every configuration change is a command, not a register write: an STE or a
 * page-table entry that has been rewritten is still cached inside the unit
 * until it is told to drop it. So each of the map/attach paths below ends in a
 * command plus a CMD_SYNC, and the sync is waited on — advancing the producer
 * index says only that the entry was written, not that it took effect. */
static void cmdq_push(u64 w0, u64 w1)
{
	u32 entries = 1u << CMDQ_LOG2;
	u32 prod = reg_read32(SMMU_CMDQ_PROD);
	u32 index = prod & (entries - 1);
	u32 wrap = prod & entries;

	cmdq[(u64)index * 2 + 0] = w0;
	cmdq[(u64)index * 2 + 1] = w1;
	mem_mfence();

	index++;
	if (index == entries) {
		index = 0;
		wrap ^= entries;
	}
	reg_write32(SMMU_CMDQ_PROD, index | wrap);
}

/* Wait for the unit to have consumed everything queued so far. */
static int cmdq_sync(void)
{
	cmdq_push(CMD_SYNC, 0); /* CS = 0: signal by advancing CONS, no interrupt */
	for (int i = 0; i < 1000000; i++) {
		if ((reg_read32(SMMU_CMDQ_CONS) & 0xFFFFFu) ==
		    (reg_read32(SMMU_CMDQ_PROD) & 0xFFFFFu))
			return 0;
		cpu_relax();
	}
	return -1;
}

static void cmdq_invalidate_ste(u32 sid)
{
	cmdq_push(CMD_CFGI_STE | ((u64)sid << 32), 1ULL /* Leaf */);
	(void)cmdq_sync();
}

static void cmdq_invalidate_tlb(void)
{
	cmdq_push(CMD_TLBI_NH_ASID | ((u64)SMMU_ASID << 48), 0);
	(void)cmdq_sync();
}

/* ── stage-1 page tables ────────────────────────────────────────────
 *
 * Levels 1..3 of an AArch64 long-descriptor walk. Level 1's index is bits
 * 38:30 of the device address, level 2's 29:21, level 3's 20:12; a level-3
 * entry maps one 4 KiB page. */
static u64 *pt_next(u64 *table, u32 index, int create)
{
	u64 e = table[index];

	if (!(e & PTE_VALID)) {
		u64 phys = 0;
		u64 *next;

		if (!create)
			return 0;
		next = alloc_zero_pages(1, &phys);
		if (!next)
			return 0;
		table[index] = phys | PTE_VALID | PTE_TABLE;
		return next;
	}
	return (u64 *)(usize)((e & PTE_ADDR_MASK) + vmm_direct_map_base());
}

static u64 *pt_leaf(u64 iova, int create)
{
	u64 *tbl = pt_root;

	if (!tbl)
		return 0;
	/* Above the input address size there is nothing to walk: such an address
	 * is a translation fault by construction, which is what we want. */
	if (iova >> SMMU_IAS)
		return 0;
	tbl = pt_next(tbl, (u32)((iova >> 30) & 0x1FF), create);
	if (!tbl)
		return 0;
	tbl = pt_next(tbl, (u32)((iova >> 21) & 0x1FF), create);
	if (!tbl)
		return 0;
	return &tbl[(iova >> 12) & 0x1FF];
}

int smmuv3_map(u64 iova, u64 phys, usize size, int writable)
{
	usize pages;
	u64 flags;

	if (!smmu_enabled || (iova & (PAGE_SIZE - 1)) || (phys & (PAGE_SIZE - 1)))
		return -1;

	pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
	spin_lock_irqsave(&smmu_lock, &flags);
	for (usize i = 0; i < pages; i++) {
		u64 *pte = pt_leaf(iova + (u64)i * PAGE_SIZE, 1);

		if (!pte) {
			spin_unlock_irqrestore(&smmu_lock, flags);
			return -1;
		}
		*pte = (phys + (u64)i * PAGE_SIZE) | PTE_VALID | PTE_PAGE |
		       PTE_ATTRIDX0 | (writable ? PTE_AP_RW : PTE_AP_RO) |
		       PTE_SH_ISH | PTE_AF;
	}
	mem_mfence();
	spin_unlock_irqrestore(&smmu_lock, flags);
	cmdq_invalidate_tlb();
	return 0;
}

int smmuv3_unmap(u64 iova, usize size)
{
	usize pages;
	u64 flags;

	if (!smmu_enabled)
		return -1;

	pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
	spin_lock_irqsave(&smmu_lock, &flags);
	for (usize i = 0; i < pages; i++) {
		u64 *pte = pt_leaf(iova + (u64)i * PAGE_SIZE, 0);

		if (pte)
			*pte = 0;
	}
	mem_mfence();
	spin_unlock_irqrestore(&smmu_lock, flags);
	cmdq_invalidate_tlb();
	return 0;
}

u64 smmuv3_translate(u64 iova)
{
	u64 flags, out = 0;
	u64 *pte;

	if (!smmu_enabled)
		return 0;
	spin_lock_irqsave(&smmu_lock, &flags);
	pte = pt_leaf(iova & ~(u64)(PAGE_SIZE - 1), 0);
	if (pte && (*pte & PTE_VALID))
		out = (*pte & PTE_ADDR_MASK) | (iova & (PAGE_SIZE - 1));
	spin_unlock_irqrestore(&smmu_lock, flags);
	return out;
}

/* ── stream table ───────────────────────────────────────────────────── */

static void ste_set(u32 sid, int translated)
{
	u64 *ste;

	if (sid >= (1u << strtab_log2))
		return;
	ste = &strtab[(u64)sid * STE_QWORDS];

	/* The trailing words first and word 0 (which carries V) last: the entry
	 * must never be visible to the unit half-written. */
	if (translated) {
		u64 w1 = (1ULL << STE_1_S1CIR_SHIFT) | (1ULL << STE_1_S1COR_SHIFT) |
		         ((u64)CR1_SH_ISH << STE_1_S1CSH_SHIFT) |
		         (0ULL << STE_1_STRW_SHIFT) |
		         (STE_1_SHCFG_INCOMING << STE_1_SHCFG_SHIFT);

		/* Stalling a faulting transaction needs somebody to resume it, and
		 * nothing here does — ask for terminate-on-fault. Where the unit
		 * supports no stalls at all (QEMU reports IDR0.STALL_MODEL = 1) the
		 * bit is reserved and must stay clear. */
		if (((smmu_idr[0] >> IDR0_STALL_MODEL_SHIFT) &
		     IDR0_STALL_MODEL_MASK) != 1)
			w1 |= STE_1_S1STALLD;

		ste[1] = w1;
		ste[2] = 0;
		ste[3] = 0;
		mem_mfence();
		ste[0] = STE_0_V | (STE_0_CFG_S1_TRANS << STE_0_CFG_SHIFT) |
		         (0ULL << STE_0_S1FMT_SHIFT) |
		         (cd_phys & STE_0_S1CTXPTR_MASK) |
		         (0ULL << STE_0_S1CDMAX_SHIFT);
	} else {
		ste[1] = STE_1_SHCFG_INCOMING << STE_1_SHCFG_SHIFT;
		ste[2] = 0;
		ste[3] = 0;
		mem_mfence();
		ste[0] = STE_0_V | (STE_0_CFG_BYPASS << STE_0_CFG_SHIFT);
	}
	mem_mfence();
}

int smmuv3_stream_id(u8 bus, u8 slot, u8 func, u32 *sid_out)
{
	u16 rid = (u16)(((u16)bus << 8) | ((slot & 0x1F) << 3) | (func & 7));

	return fdt_pci_stream_id(rid, sid_out);
}

int smmuv3_attach_device(u8 bus, u8 slot, u8 func)
{
	u32 sid = 0;
	u64 flags;

	if (!smmu_enabled || !smmuv3_stream_id(bus, slot, func, &sid))
		return -1;
	if (sid >= (1u << strtab_log2))
		return -1;

	spin_lock_irqsave(&smmu_lock, &flags);
	ste_set(sid, 1);
	spin_unlock_irqrestore(&smmu_lock, flags);
	cmdq_invalidate_ste(sid);
	cmdq_invalidate_tlb();
	return 0;
}

void smmuv3_detach_device(u8 bus, u8 slot, u8 func)
{
	u32 sid = 0;
	u64 flags;

	if (!smmu_enabled || !smmuv3_stream_id(bus, slot, func, &sid))
		return;
	if (sid >= (1u << strtab_log2))
		return;

	spin_lock_irqsave(&smmu_lock, &flags);
	ste_set(sid, 0);
	spin_unlock_irqrestore(&smmu_lock, flags);
	cmdq_invalidate_ste(sid);
}

/* ── event queue ────────────────────────────────────────────────────
 *
 * A device that touches what it was not granted lands here rather than in a
 * status register, so counting faults means draining the queue. Polled, not
 * interrupt-driven — see the file header. */
static void evtq_drain(void)
{
	u32 entries = 1u << EVTQ_LOG2;
	u32 prod = reg_read32(SMMU_EVTQ_PROD);
	u32 prod_index = prod & (entries - 1);
	u32 prod_wrap = (prod & entries) ? 1u : 0u;

	while (evtq_cons != prod_index || evtq_wrap != prod_wrap) {
		u64 *rec = &evtq[(u64)evtq_cons * 4];
		u8 type = (u8)(rec[0] & EVT_TYPE_MASK);

		if (type) {
			evtq_faults++;
			evtq_last_type = type;
			evtq_last_sid = (u32)(rec[0] >> 32);
			evtq_last_addr = rec[2];
		}
		evtq_cons++;
		if (evtq_cons == entries) {
			evtq_cons = 0;
			evtq_wrap ^= 1u;
		}
	}
	reg_write32(SMMU_EVTQ_CONS, evtq_cons | (evtq_wrap ? entries : 0));
}

u32 smmuv3_fault_count(void)
{
	if (!smmu_enabled)
		return 0;
	evtq_drain();
	return evtq_faults;
}

void smmuv3_fault_clear(void)
{
	if (!smmu_enabled)
		return;
	evtq_drain();
	evtq_faults = 0;
}

void smmuv3_fault_last(u64 *addr, u32 *sid, u8 *type)
{
	if (addr)
		*addr = evtq_last_addr;
	if (sid)
		*sid = evtq_last_sid;
	if (type)
		*type = evtq_last_type;
}

/* ── bring-up ───────────────────────────────────────────────────────── */

static int cr0_write_wait(u32 value)
{
	reg_write32(SMMU_CR0, value);
	for (int i = 0; i < 1000000; i++) {
		if (reg_read32(SMMU_CR0ACK) == value)
			return 0;
		cpu_relax();
	}
	return -1;
}

/* IDR5.OAS and the context descriptor's TCR.IPS use the same numbering, so
 * this is a range check rather than a table: three levels of 4 KiB descriptors
 * cannot describe an output above 48 bits. */
static u32 oas_to_ips(u32 oas) { return oas > 5 ? 5u : oas; }

static void smmuv3_report_id(void)
{
	static const u16 oas_bits[8] = { 32, 36, 40, 42, 44, 48, 52, 0 };
	char line[224];
	u32 idr0 = smmu_idr[0];
	u32 idr1 = smmu_idr[1];
	u32 idr5 = smmu_idr[5];
	u32 ttf = (idr0 >> IDR0_TTF_SHIFT) & IDR0_TTF_MASK;

	snprintf(line, sizeof(line),
	         "smmuv3: unit at 0x%lx, stages%s%s, tables=%s, oas=%u bits, "
	         "granules=%s%s%s\n",
	         (unsigned long)smmu_base_phys,
	         (idr0 & IDR0_S1P) ? " s1" : "",
	         (idr0 & IDR0_S2P) ? " s2" : "",
	         ttf == 3 ? "aarch32+64" :
	             (ttf == 2 ? "aarch64" : (ttf == 1 ? "aarch32" : "none")),
	         (unsigned)oas_bits[idr5 & IDR5_OAS_MASK],
	         (idr5 & IDR5_GRAN4K) ? "4K " : "",
	         (idr5 & IDR5_GRAN16K) ? "16K " : "",
	         (idr5 & IDR5_GRAN64K) ? "64K" : "");
	console_write(line);

	snprintf(line, sizeof(line),
	         "smmuv3: sidsize=%u bits, cmdq<=%u entries, evtq<=%u entries, "
	         "linear stream table of %u%s\n",
	         (unsigned)(idr1 & IDR1_SIDSIZE_MASK),
	         (unsigned)(1u << ((idr1 >> IDR1_CMDQS_SHIFT) & IDR1_QS_MASK)),
	         (unsigned)(1u << ((idr1 >> IDR1_EVTQS_SHIFT) & IDR1_QS_MASK)),
	         (unsigned)(1u << strtab_log2),
	         (idr0 & IDR0_COHACC) ? ", coherent table walk" : "");
	console_write(line);
}

void smmuv3_init(void)
{
	u64 base = fdt_smmuv3_base();
	u64 size = fdt_smmuv3_size();
	usize st_pages;
	u32 sidsize, cmdq_max, evtq_max;
	u64 cd0;

	if (!base) {
		console_write("smmuv3: no SMMUv3 in the device tree\n");
		return;
	}

	smmu_base_phys = base;
	smmu_regs = (volatile u8 *)vmm_map_mmio(base, size ? size : 0x20000,
	                                        VMM_WRITABLE | VMM_NO_EXECUTE);
	if (!smmu_regs) {
		console_write("smmuv3: cannot map unit registers\n");
		return;
	}

	for (u32 i = 0; i < 6; i++)
		smmu_idr[i] = reg_read32(SMMU_IDR0 + i * 4);

	sidsize = smmu_idr[1] & IDR1_SIDSIZE_MASK;
	strtab_log2 = sidsize < STRTAB_LOG2_WANTED ? sidsize : STRTAB_LOG2_WANTED;
	smmuv3_report_id();

	/* Refuse rather than guess. Stage 1, AArch64 tables and a 4 KiB granule
	 * are what this driver programs; a unit that cannot do all three would
	 * take the configuration and then fault on every walk. */
	if (!(smmu_idr[0] & IDR0_S1P)) {
		console_write("smmuv3: unit has no stage 1 — not enabling\n");
		return;
	}
	if (((smmu_idr[0] >> IDR0_TTF_SHIFT) & IDR0_TTF_MASK) < 2) {
		console_write("smmuv3: unit has no AArch64 table format — "
		              "not enabling\n");
		return;
	}
	if (!(smmu_idr[5] & IDR5_GRAN4K)) {
		console_write("smmuv3: unit has no 4 KiB granule — not enabling\n");
		return;
	}
	if (smmu_idr[1] & (IDR1_TABLES_PRESET | IDR1_QUEUES_PRESET)) {
		console_write("smmuv3: unit's tables and queues are at fixed "
		              "addresses — not enabling\n");
		return;
	}
	if (strtab_log2 == 0) {
		console_write("smmuv3: unit describes no StreamIDs — not enabling\n");
		return;
	}

	/* Queues no larger than the unit will accept. */
	cmdq_max = (smmu_idr[1] >> IDR1_CMDQS_SHIFT) & IDR1_QS_MASK;
	evtq_max = (smmu_idr[1] >> IDR1_EVTQS_SHIFT) & IDR1_QS_MASK;
	if (cmdq_max < CMDQ_LOG2 || evtq_max < EVTQ_LOG2) {
		console_write("smmuv3: unit's queues are smaller than this driver "
		              "asks for — not enabling\n");
		return;
	}

	st_pages = ((usize)1 << strtab_log2) * STE_QWORDS * sizeof(u64) / PAGE_SIZE;
	if (st_pages == 0)
		st_pages = 1;
	strtab = alloc_zero_pages(st_pages, &strtab_phys);
	cmdq = alloc_zero_pages(1, &cmdq_phys);
	evtq = alloc_zero_pages(1, &evtq_phys);
	cd_table = alloc_zero_pages(1, &cd_phys);
	pt_root = alloc_zero_pages(1, &pt_root_phys);
	if (!strtab || !cmdq || !evtq || !cd_table || !pt_root) {
		console_write("smmuv3: table allocation failed\n");
		return;
	}

	/* Every stream bypassing to begin with. */
	for (u32 sid = 0; sid < (1u << strtab_log2); sid++)
		ste_set(sid, 0);

	/* The single context descriptor the domain uses. It is programmed now and
	 * only referenced once a device is attached. */
	cd0 = ((u64)SMMU_T0SZ << CD_0_TCR_T0SZ_SHIFT) |
	      (0ULL << CD_0_TCR_TG0_SHIFT) |
	      ((u64)CR1_CACHE_WB << CD_0_TCR_IRGN0_SHIFT) |
	      ((u64)CR1_CACHE_WB << CD_0_TCR_ORGN0_SHIFT) |
	      ((u64)CR1_SH_ISH << CD_0_TCR_SH0_SHIFT) |
	      ((u64)oas_to_ips(smmu_idr[5] & IDR5_OAS_MASK) << CD_0_TCR_IPS_SHIFT) |
	      CD_0_TCR_EPD1 | CD_0_AA64 | CD_0_ASET | CD_0_V |
	      ((u64)SMMU_ASID << CD_0_ASID_SHIFT);
	/* CD.A and CD.R are set whatever the unit says about HTTU.
	 *
	 * The tempting reading is that they are hardware-update controls and mean
	 * nothing without HTTU, so a unit that lacks it should be handed zeroes.
	 * That reading costs a day: the unit rejects the descriptor outright with
	 * C_BAD_CD (event 0x0a, which arrives with a faulting address of zero and
	 * names only the StreamID), the device's DMA stops, and its first command
	 * never completes. Linux sets both unconditionally for the same reason. */
	cd0 |= CD_0_A | CD_0_R;
	cd_table[0] = cd0;
	cd_table[1] = pt_root_phys & CD_1_TTB0_MASK;
	cd_table[2] = 0;
	cd_table[3] = 0x00000000000000FFULL; /* MAIR attr0: normal write-back */
	mem_mfence();

	/* Bring-up proper: everything off first, then the queues, then the unit.
	 * CR0 is acknowledged in CR0ACK, so each step is waited on rather than
	 * assumed. */
	if (cr0_write_wait(0) != 0) {
		console_write("smmuv3: unit did not acknowledge being disabled\n");
		return;
	}
	reg_write32(SMMU_CR1, ((u32)CR1_SH_ISH << CR1_TABLE_SH_SHIFT) |
	                      ((u32)CR1_CACHE_WB << CR1_TABLE_OC_SHIFT) |
	                      ((u32)CR1_CACHE_WB << CR1_TABLE_IC_SHIFT) |
	                      ((u32)CR1_SH_ISH << CR1_QUEUE_SH_SHIFT) |
	                      ((u32)CR1_CACHE_WB << CR1_QUEUE_OC_SHIFT) |
	                      ((u32)CR1_CACHE_WB << CR1_QUEUE_IC_SHIFT));
	reg_write32(SMMU_CR2, CR2_PTM | CR2_RECINVSID);
	reg_write32(SMMU_IRQ_CTRL, 0); /* polled; see the file header */
	reg_write32(SMMU_GERRORN, reg_read32(SMMU_GERROR));

	reg_write64(SMMU_STRTAB_BASE,
	            (strtab_phys & STRTAB_BASE_MASK) | STRTAB_BASE_RA);
	reg_write32(SMMU_STRTAB_BASE_CFG,
	            (STRTAB_BASE_CFG_FMT_LINEAR << 16) | strtab_log2);

	reg_write64(SMMU_CMDQ_BASE,
	            (cmdq_phys & Q_BASE_ADDR_MASK) | Q_BASE_RWA | CMDQ_LOG2);
	reg_write32(SMMU_CMDQ_PROD, 0);
	reg_write32(SMMU_CMDQ_CONS, 0);
	if (cr0_write_wait(CR0_CMDQEN) != 0) {
		console_write("smmuv3: command queue did not come up\n");
		return;
	}

	/* The unit's caches hold whatever was in them before this kernel ran. */
	cmdq_push(CMD_CFGI_ALL, 31ULL /* Range: every StreamID */);
	cmdq_push(CMD_TLBI_NSNH_ALL, 0);
	if (cmdq_sync() != 0) {
		console_write("smmuv3: command queue did not drain\n");
		return;
	}

	reg_write64(SMMU_EVTQ_BASE,
	            (evtq_phys & Q_BASE_ADDR_MASK) | Q_BASE_RWA | EVTQ_LOG2);
	reg_write32(SMMU_EVTQ_PROD, 0);
	reg_write32(SMMU_EVTQ_CONS, 0);
	evtq_cons = 0;
	evtq_wrap = 0;
	if (cr0_write_wait(CR0_CMDQEN | CR0_EVTQEN) != 0) {
		console_write("smmuv3: event queue did not come up\n");
		return;
	}

	/* A StreamID the stream table cannot describe aborts rather than sailing
	 * past the unit. GBPA governs only those; the table itself says bypass
	 * for every StreamID it does describe. */
	reg_write32(SMMU_GBPA, GBPA_UPDATE | GBPA_ABORT);

	if (cr0_write_wait(CR0_CMDQEN | CR0_EVTQEN | CR0_SMMUEN) != 0) {
		console_write("smmuv3: unit did not acknowledge being enabled\n");
		return;
	}

	smmu_enabled = 1;

	{
		char line[160];

		snprintf(line, sizeof(line),
		         "smmuv3: translation on, every stream bypassing, "
		         "%u wired interrupts (evtq SPI %u, polled)\n",
		         (unsigned)fdt_smmuv3_irq_count(),
		         (unsigned)fdt_smmuv3_irq(0));
		console_write(line);
	}
}

/* ── self-test ──────────────────────────────────────────────────────── */

static void smmu_report(const char *name, int ok, u64 detail)
{
	console_write(ok ? "M100E-SMOKE: ok " : "M100E-SMOKE: FAIL ");
	console_write(name);
	if (!ok) {
		console_write(" detail=0x");
		console_write_hex64(detail);
	}
	console_write("\n");
}

void smmuv3_selftest(void)
{
	u64 frame, iova;
	u32 cr0ack, cons_before, cons_after;
	int map_ok, unmap_ok, on;

	if (!bootinfo_has_flag("b1nix.test=1"))
		return;
	if (!fdt_smmuv3_base()) {
		console_write("M100E-SMOKE: skip smmuv3 (no SMMUv3 on this machine)\n");
		return;
	}
	if (!smmu_enabled) {
		smmu_report("smmuv3-enable", 0, 1);
		return;
	}

	/* The unit says it is on, and it is pointing at our stream table. */
	cr0ack = reg_read32(SMMU_CR0ACK);
	on = (cr0ack & CR0_SMMUEN) && (cr0ack & CR0_CMDQEN) &&
	     (cr0ack & CR0_EVTQEN) &&
	     (reg_read64(SMMU_STRTAB_BASE) & STRTAB_BASE_MASK) ==
	         (strtab_phys & STRTAB_BASE_MASK);
	smmu_report("smmuv3-enable", on, cr0ack);

	/* The command queue is the only way this unit is told anything, so a sync
	 * it never consumes would mean every invalidation above was a guess. */
	cons_before = reg_read32(SMMU_CMDQ_CONS) & 0xFFFFFu;
	cmdq_push(CMD_PREFETCH_CFG, 0);
	(void)cmdq_sync();
	cons_after = reg_read32(SMMU_CMDQ_CONS) & 0xFFFFFu;
	smmu_report("smmuv3-command-queue", cons_after != cons_before, cons_after);

	/* A mapping is what the tables the unit walks say, and unmapping removes
	 * it — read back through the descriptor format, not from a copy of what
	 * smmuv3_map() was asked for. */
	frame = pmm_alloc_frame();
	if (!frame) {
		smmu_report("smmuv3-map", 0, 2);
		return;
	}
	iova = 0x30000000ULL;
	map_ok = smmuv3_map(iova, frame, PAGE_SIZE, 1) == 0 &&
	         smmuv3_translate(iova) == frame &&
	         smmuv3_translate(iova + 0x80) == frame + 0x80;
	smmuv3_unmap(iova, PAGE_SIZE);
	unmap_ok = smmuv3_translate(iova) == 0;
	pmm_free_frame(frame);
	smmu_report("smmuv3-map", map_ok, iova);
	smmu_report("smmuv3-unmap", unmap_ok, 0);

	console_write("M100E-SMOKE: done\n");
}
