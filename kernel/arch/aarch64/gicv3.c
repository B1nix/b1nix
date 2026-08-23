/* GICv3: distributor, per-CPU redistributor, and the system-register CPU
 * interface.
 *
 * Why this exists at all: GICv2's CPU interface is a memory-mapped block, and
 * its MSI story is an optional GICv2m frame that QEMU's `virt` does not give
 * us. GICv3 moves the CPU interface into system registers (ICC_*) and pairs
 * with an ITS, which is the only way a device on this board can raise a
 * message-signalled interrupt. So this file is the groundwork for MSI, not a
 * rewrite for its own sake — a machine that reports GICv2 keeps using
 * kernel/arch/aarch64/interrupts.c's v2 path unchanged.
 *
 * What v3 needs that v2 did not:
 *   - Affinity Routing (GICD_CTLR.ARE_NS) instead of the 8-bit target-CPU
 *     bitmaps, so an SPI is routed by writing an affinity value to GICD_IROUTER
 *     rather than by setting a bit in GICD_ITARGETSR.
 *   - A redistributor per CPU, which owns that CPU's SGIs and PPIs (and, later,
 *     its LPIs). Each one has to be woken out of its reset "processor asleep"
 *     state before the CPU can take anything at all.
 *   - ICC_SRE_EL1.SRE set before any other ICC_* register is touched: with SRE
 *     clear those registers trap, and on a machine that only implements the
 *     system-register interface the memory-mapped one does not exist.
 */

#include <b1nix/arch.h>
#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/gicv3.h>
#include <b1nix/types.h>

/* Distributor. Only the registers that differ from v2's use are named here;
 * the shared ones (ISENABLER, IPRIORITYR, ICFGR) keep their v2 offsets. */
#define GICD_CTLR_OFF        0x0000
#define GICD_TYPER_OFF       0x0004
#define GICD_IGROUPR_OFF     0x0080
#define GICD_ISENABLER_OFF   0x0100
#define GICD_ICENABLER_OFF   0x0180
#define GICD_IPRIORITYR_OFF  0x0400
#define GICD_IROUTER_OFF     0x6000

#define GICD_CTLR_ARE_NS     (1u << 4)
#define GICD_CTLR_ENABLE_G1NS (1u << 1)
#define GICD_CTLR_RWP        (1u << 31)

/* Redistributor: two 64 KiB frames per CPU — RD_base then SGI_base. */
#define GICR_STRIDE          0x20000
#define GICR_CTLR_OFF        0x0000
#define GICR_TYPER_OFF       0x0008
#define GICR_WAKER_OFF       0x0014
#define GICR_WAKER_PS        (1u << 1)  /* ProcessorSleep */
#define GICR_WAKER_CA        (1u << 2)  /* ChildrenAsleep */
/* SGI frame, 64 KiB into the redistributor. */
#define GICR_PROPBASER_OFF   0x0070
#define GICR_PENDBASER_OFF   0x0078
#define GICR_CTLR_ENABLE_LPIS (1u << 0)
#define GICR_SGI_OFF         0x10000
#define GICR_IGROUPR0_OFF    (GICR_SGI_OFF + 0x0080)
#define GICR_ISENABLER0_OFF  (GICR_SGI_OFF + 0x0100)
#define GICR_ICENABLER0_OFF  (GICR_SGI_OFF + 0x0180)
#define GICR_IPRIORITYR_OFF  (GICR_SGI_OFF + 0x0400)

static u64 g_gicd;
static u64 g_gicr;      /* base of the redistributor region */
static u64 g_gicr_size;
static int g_present;
static u32 g_lines = 32;

static inline volatile u32 *d32(u64 off) {
	return (volatile u32 *)(usize)(g_gicd + off);
}

static inline volatile u64 *d64(u64 off) {
	return (volatile u64 *)(usize)(g_gicd + off);
}

static void gicd_wait_rwp(void) {
	/* A write to CTLR/IROUTER is not in force until RWP clears. Bounded: a
	 * distributor that never clears it is broken, and spinning forever there
	 * would look like a hang at boot with nothing said. */
	for (int i = 0; i < 1000000; i++) {
		if (!(*d32(GICD_CTLR_OFF) & GICD_CTLR_RWP))
			return;
		cpu_relax();
	}
	console_write("gicv3: distributor RWP never cleared\n");
}

int gicv3_present(void) { return g_present; }

/* The redistributor belonging to the CPU this runs on.
 *
 * The frames are laid out in the region in an implementation-defined order, so
 * they are searched by the affinity value each one reports in GICR_TYPER
 * rather than indexed by CPU number — QEMU orders them by CPU index today, and
 * a kernel that assumed that would break on the first board that does not. */
static u64 gicr_for_this_cpu(void) {
	u64 mpidr;
	__asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));

	u64 affinity = ((mpidr >> 32) & 0xffULL) << 32 | (mpidr & 0x00ffffffULL);

	for (u64 off = 0; off + GICR_STRIDE <= g_gicr_size; off += GICR_STRIDE) {
		u64 frame = g_gicr + off;
		u64 typer = *(volatile u64 *)(usize)(frame + GICR_TYPER_OFF);

		if ((typer >> 32) == affinity)
			return frame;
		if (typer & (1ULL << 4)) /* Last: no further frames */
			break;
	}
	return 0;
}

/* Wake this CPU's redistributor and program the CPU interface. Called once per
 * CPU, like the v2 gic_cpu_init(). */
void gicv3_cpu_init(void) {
	u64 rd = gicr_for_this_cpu();

	if (!rd) {
		console_write("gicv3: no redistributor for this CPU\n");
		return;
	}

	volatile u32 *waker = (volatile u32 *)(usize)(rd + GICR_WAKER_OFF);

	*waker &= ~GICR_WAKER_PS;
	for (int i = 0; i < 1000000 && (*waker & GICR_WAKER_CA); i++)
		cpu_relax();

	/* SGIs and PPIs belong to this redistributor, not to the distributor:
	 * group 1 non-secure, a priority that the mask below lets through, and
	 * disabled until a driver asks for one. */
	*(volatile u32 *)(usize)(rd + GICR_IGROUPR0_OFF) = 0xffffffffu;
	*(volatile u32 *)(usize)(rd + GICR_ICENABLER0_OFF) = 0xffffffffu;
	for (u32 i = 0; i < 8; i++)
		*(volatile u32 *)(usize)(rd + GICR_IPRIORITYR_OFF + i * 4) = 0xa0a0a0a0u;

	/* SRE first: every other ICC_* access traps without it. */
	u64 sre;
	__asm__ volatile("mrs %0, S3_0_C12_C12_5" : "=r"(sre)); /* ICC_SRE_EL1 */
	if (!(sre & 1)) {
		sre |= 1;
		__asm__ volatile("msr S3_0_C12_C12_5, %0\n\tisb" : : "r"(sre));
	}

	__asm__ volatile("msr S3_0_C4_C6_0, %0" : : "r"((u64)0xf0)); /* ICC_PMR_EL1 */
	__asm__ volatile("msr S3_0_C12_C12_7, %0" : : "r"((u64)0));  /* ICC_IGRPEN1_EL1 = 0 */
	__asm__ volatile("msr S3_0_C12_C12_4, %0" : : "r"((u64)0));  /* ICC_CTLR_EL1: EOImode 0 */
	__asm__ volatile("isb");
	__asm__ volatile("msr S3_0_C12_C12_7, %0\n\tisb" : : "r"((u64)1)); /* enable group 1 */
}

/* Distributor bring-up, once for the machine. Returns 0 when the device tree
 * describes no GICv3 — the caller then keeps the v2 path. */
int gicv3_init(void) {
	if (!fdt_gic_is_v3())
		return -1;

	g_gicd = fdt_gicd_base();
	g_gicr = fdt_gicr_base();
	g_gicr_size = fdt_gicr_size();
	if (!g_gicd || !g_gicr)
		return -1;

	*d32(GICD_CTLR_OFF) = 0;
	gicd_wait_rwp();

	g_lines = ((*d32(GICD_TYPER_OFF) & 0x1f) + 1) * 32;
	if (g_lines > 1020)
		g_lines = 1020;

	/* SPIs: group 1 non-secure, disabled, a priority the mask lets through.
	 * SGIs and PPIs (the first 32) live in the redistributor instead. */
	for (u32 i = 32; i < g_lines; i += 32) {
		*d32(GICD_IGROUPR_OFF + (i / 32) * 4) = 0xffffffffu;
		*d32(GICD_ICENABLER_OFF + (i / 32) * 4) = 0xffffffffu;
	}
	for (u32 i = 32; i < g_lines; i += 4)
		*d32(GICD_IPRIORITYR_OFF + i) = 0xa0a0a0a0u;

	/* Affinity routing on, group 1 non-secure enabled. ARE has to be set
	 * before IROUTER means anything. */
	*d32(GICD_CTLR_OFF) = GICD_CTLR_ARE_NS;
	gicd_wait_rwp();

	/* Every SPI to the boot CPU. Interrupt affinity is a policy this kernel
	 * does not have yet (the v2 path targets CPU 0 the same way), and a line
	 * routed to a CPU that never came up is delivered to nobody. */
	u64 mpidr;
	__asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
	u64 route = (mpidr & 0x00ffffffULL) | (((mpidr >> 32) & 0xffULL) << 32);
	for (u32 i = 32; i < g_lines; i++)
		*d64(GICD_IROUTER_OFF + i * 8) = route;

	*d32(GICD_CTLR_OFF) = GICD_CTLR_ARE_NS | GICD_CTLR_ENABLE_G1NS;
	gicd_wait_rwp();

	g_present = 1;
	gicv3_cpu_init();

	console_write("gicv3: dist 0x");
	console_write_hex64(g_gicd);
	console_write(" redist 0x");
	console_write_hex64(g_gicr);
	console_write(" lines ");
	console_write_dec(g_lines);
	console_write("\n");
	return 0;
}

/* Enable one interrupt. SGIs and PPIs are the redistributor's, SPIs the
 * distributor's — the split v2 did not have. */
void gicv3_enable_irq(u32 irq) {
	if (irq < 32) {
		u64 rd = gicr_for_this_cpu();

		if (rd)
			*(volatile u32 *)(usize)(rd + GICR_ISENABLER0_OFF) = 1u << irq;
		return;
	}
	if (irq >= g_lines)
		return;
	*d32(GICD_ISENABLER_OFF + (irq / 32) * 4) = 1u << (irq % 32);
	gicd_wait_rwp();
}

/* This CPU's redistributor, for the ITS: MAPC targets a redistributor by
 * address, and SYNC waits on one. */
u64 gicv3_rdbase(void) { return gicr_for_this_cpu(); }

/* Hand the redistributor the LPI configuration and pending tables and turn
 * LPIs on. Both are physical addresses of memory the caller owns; the
 * configuration table is shared between CPUs, the pending table is not.
 *
 * GICR_CTLR.EnableLPIs is write-once on many implementations — once set, the
 * tables cannot be moved — so this is called exactly once per CPU, from the
 * ITS bring-up. */
int gicv3_lpi_enable(u64 prop_table, u32 id_bits, u64 pend_table) {
	u64 rd = gicr_for_this_cpu();

	if (!rd)
		return -1;

	volatile u32 *ctlr = (volatile u32 *)(usize)(rd + GICR_CTLR_OFF);
	volatile u64 *propbaser = (volatile u64 *)(usize)(rd + GICR_PROPBASER_OFF);
	volatile u64 *pendbaser = (volatile u64 *)(usize)(rd + GICR_PENDBASER_OFF);

	if (*ctlr & GICR_CTLR_ENABLE_LPIS)
		return 0; /* already on: the tables are whatever they were */

	*propbaser = (prop_table & 0x0000fffffffff000ULL) | (u64)(id_bits - 1) |
	             (1ULL << 10) /* InnerShareable */ | (5ULL << 7) /* RaWaWb */;
	*pendbaser = (pend_table & 0x0000fffffffff000ULL) |
	             (1ULL << 10) | (5ULL << 7);
	__asm__ volatile("dsb ishst" ::: "memory");

	*ctlr |= GICR_CTLR_ENABLE_LPIS;
	__asm__ volatile("dsb ish" ::: "memory");
	return 0;
}

/* The highest-priority pending interrupt, without acknowledging it — for
 * diagnostics that want to say what arrived. */
/* Diagnostic: this CPU's SGI/PPI enable bits. */
/* The processor number this CPU's redistributor reports (GICR_TYPER[23:8]).
 * The ITS addresses a redistributor either by physical address or by this
 * number, depending on GITS_TYPER.PTA. */
u32 gicv3_processor_number(void) {
	u64 rd = gicr_for_this_cpu();

	if (!rd)
		return 0;
	return (u32)((*(volatile u64 *)(usize)(rd + GICR_TYPER_OFF) >> 8) & 0xffff);
}

u32 gicv3_isenabler0(void) {
	u64 rd = gicr_for_this_cpu();

	return rd ? *(volatile u32 *)(usize)(rd + GICR_ISENABLER0_OFF) : 0;
}

u32 gicv3_ack_peek(void) {
	u64 v;

	__asm__ volatile("mrs %0, S3_0_C12_C12_2" : "=r"(v)); /* ICC_HPPIR1_EL1 */
	return (u32)v;
}

u32 gicv3_ack(void) {
	u64 iar;

	__asm__ volatile("mrs %0, S3_0_C12_C12_0" : "=r"(iar)); /* ICC_IAR1_EL1 */
	return (u32)iar;
}

void gicv3_eoi(u32 iar) {
	__asm__ volatile("msr S3_0_C12_C12_1, %0" : : "r"((u64)iar)); /* ICC_EOIR1_EL1 */
}
