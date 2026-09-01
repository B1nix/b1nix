#include <b1nix/arch.h>
#include <b1nix/console.h>
#include <b1nix/lapic.h>
#include <b1nix/irq.h>
#include <b1nix/gicv3.h>
#include <b1nix/spinlock.h>
#include <b1nix/mm.h>
#include <b1nix/panic.h>
#include <b1nix/sched.h>

extern void interrupts_init(void);

/* Per-CPU data, CPU enumeration and secondary bring-up: smp.c. */

/* arch_fpu_save/restore/init_current/capture_clean live in
 * kernel/arch/aarch64/fpu.S — they must name V registers, which this file
 * cannot under -mgeneral-regs-only. */

void tlb_shootdown_poll(void)
{
}

void arch_backtrace(u64 fp, u64 lr)
{
	(void)fp;
	(void)lr;
}

/* ps2_kbd_getc/ps2_kbd_has_data now come from kernel/dev/ps2_kbd.c, which
 * builds here for its scancode translation (the USB HID driver feeds it). */

void arch_set_fs_base(u64 base)
{
	/* Unlike x86_64 (where FS base is a privileged MSR only the kernel can
	 * write, requiring this per-switch call), aarch64's TLS thread-pointer
	 * register TPIDR_EL0 is directly writable from EL0 — musl's own crt
	 * startup sets it itself via `msr tpidr_el0`, no syscall involved. The
	 * kernel's job is just to not clobber it: TPIDR_EL0 is saved/restored
	 * per interrupt frame in isr.S's SAVE_REGS/RESTORE_REGS (same treatment
	 * as ELR/SPSR/SP_EL0), which correctly preserves it across any
	 * reschedule. A previous version of this function forced TPIDR_EL0 to
	 * task_tls_base() (a side-table that's only ever populated for real
	 * pthreads via set_tid_address/clone), which stomped the value musl set
	 * for every plain process back to 0 on the next context switch. */
	(void)base;
}


/* Defined in console.c now that there is a lock to bust. */

__attribute__((weak)) const char __kallsyms_start[1] = {0};
__attribute__((weak)) const char __kallsyms_end[1] = {0};

volatile int g_ap_userspace_enabled = 0;

void x86_clone_thread_jump(void)
{
}

/* Top of the current task's kernel stack. AArch64 has no TSS to pin SP_EL1 on
 * an EL0->EL1 entry, so the exception vectors read this and reset sp themselves
 * (EL0_KSTACK_RESET in kernel/arch/aarch64/isr.S). The scheduler keeps it
 * current by calling arch_set_kernel_stack() on every switch. */
/* Published into this CPU's own block (kernel/arch/aarch64/smp.c), which the
 * exception vectors reach through TPIDR_EL1. A single global was correct while
 * one CPU ran userspace; with two, they would reset SP_EL1 to each other's
 * stacks. */
u64 g_aarch64_kernel_stack_top;

void arch_set_kernel_stack(u64 stack)
{
	/* A kernel stack that is not inside the allocated part of the heap is the
	 * signature of a frame this arch has been burned by twice: the exception
	 * vectors write the entry frame at this address with no further checking,
	 * so a bad value surfaces much later as a translation fault at exactly
	 * heap.end, on an unrelated task. Catch it where it is published. */
	/* Zero means "no task scheduled yet" to EL0_KSTACK_RESET, which then keeps
	 * whatever SP_EL1 already held — so publishing 0 for a task that is about
	 * to run at EL0 leaves it entering the kernel on a garbage stack. Name the
	 * task instead of letting it surface as `interrupted SP_EL1:
	 * 0xfffffffffffffed0` (0 - sizeof(frame)) somewhere else entirely. */
	if (!stack && current_task && current_task->user_image) {
		console_write("kstack: publishing 0 for '");
		console_write(current_task->name ? current_task->name : "?");
		console_write("' pid ");
		console_write_dec((u64)current_task->id);
		console_write(" ksp=0x");
		console_write_hex64(current_task->kernel_stack_ptr);
		console_write(" stack=0x");
		console_write_hex64((u64)(usize)current_task->stack);
		console_write("\n");
		panic("aarch64: kernel stack top published as 0");
	}
	u64 hb = 0, hc = 0;
	kheap_bounds(&hb, &hc, 0);
	if (stack && hb && stack >= hb && stack > hc) {
		console_write("kstack: top=0x");
		console_write_hex64(stack);
		console_write(" past heap.current=0x");
		console_write_hex64(hc);
		console_write("\n");
		panic("kernel stack top outside allocated heap");
	}
	g_aarch64_kernel_stack_top = stack;
	aarch64_set_kstack_top(stack);
}

int virtio_net_probe(void *dev)
{
	(void)dev;
	return -1;
}

void arch_init(void)
{
	console_write("aarch64: arch_init\n");
	interrupts_init();
}

u32 arch_cpu_khz(void)
{
	u64 freq = 0;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
	return (u32)(freq / 1000);
}

/* The generic timer is the aarch64 monotonic clock: CNTVCT_EL0 counts at the
 * constant CNTFRQ_EL0 rate by architecture (there is no x86-style "is this TSC
 * invariant?" question to ask), and it counts from reset, so no base has to be
 * captured. Nanoseconds, without overflowing: divide first, then scale the
 * remainder. */
static inline u64 cntvct(void)
{
	u64 v;
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
	return v;
}

static inline u64 cntfrq(void)
{
	u64 v;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
	return v;
}

void arch_tsc_clock_init(void) { /* nothing to calibrate: CNTFRQ_EL0 says it */ }

int arch_tsc_clock_ready(void) { return cntfrq() != 0; }

u64 arch_tsc_monotonic_ns(void)
{
	u64 f = cntfrq();

	if (!f)
		return 0;

	u64 t = cntvct();
	return (t / f) * 1000000000ull + ((t % f) * 1000000000ull) / f;
}

/* Busy-wait a real number of microseconds against the generic timer.
 *
 * The x86 counterpart counts port-0x80 reads when it has no calibrated clock;
 * there are no I/O ports here and CNTFRQ_EL0 is architecturally constant, so
 * this is exact whenever the counter exists at all. */
void arch_udelay(u32 us)
{
	u64 f = cntfrq();

	if (!f) {
		/* No counter: spin a fixed number of relax hints per microsecond.
		 * Unmeasured, and deliberately so — the alternative is not waiting. */
		for (u32 i = 0; i < us * 50u; i++)
			cpu_relax();
		return;
	}

	u64 start = cntvct();
	u64 want = ((u64)us * f) / 1000000ull;

	while (cntvct() - start < want)
		cpu_relax();
}

/* The scheduler tick, as this arch programs it: kernel/arch/aarch64/interrupts.c
 * reloads CNTV_TVAL_EL0 with CNTFRQ_EL0/100 every time, so the rate is not a
 * guess here the way it is on x86 when the LAPIC will not calibrate. */
u32 sched_tick_hz(void) { return 100u; }

/* ── Boot-stack accounting ──────────────────────────────────────────────────
 *
 * boot.S reserves one 64 KiB stack for the boot CPU between stack_bottom and
 * stack_top. Only stack_top is global, so the far end is named by subtraction.
 * Painting must leave the frames already in use alone: this is called from
 * kernel_main, which is itself on this stack. */
#define BOOT_STACK_PAINT 0x5Au

/* Both ends, so the size is boot.S's alone. It used to be repeated here as a
 * literal, which is a second place to forget. */
extern u8 stack_bottom[];
extern u8 stack_top[];

static u64 boot_stack_size(void)
{
	return (u64)(usize)stack_top - (u64)(usize)stack_bottom;
}

void boot_stack_paint(void)
{
	u8 *bottom = stack_bottom;
	/* A margin below the current frame, so painting cannot reach into the
	 * frame doing the painting. */
	u64 limit = (u64)(usize)__builtin_frame_address(0) - 256;

	for (u8 *p = bottom; (u64)(usize)p < limit; p++)
		*p = BOOT_STACK_PAINT;
}

u64 boot_stack_size_bytes(void) { return boot_stack_size(); }

u64 boot_stack_peak_bytes(void)
{
	const u8 *b = stack_bottom;
	u64 size = boot_stack_size();
	u64 clean = 0;

	while (clean < size && b[clean] == BOOT_STACK_PAINT)
		clean++;
	return size - clean;
}

/* Secondary-CPU stack usage is not instrumented on this arch: the APs here do
 * not get painted stacks the way the x86 trampoline paints them. Reporting a
 * zero total is how the caller learns the figure is absent rather than small. */
u64 ap_stack_peak(u64 *total_out)
{
	if (total_out)
		*total_out = 0;
	return 0;
}

/* The address-space edit ring is x86-only for now: its storage and its two
 * dump functions live in kernel/arch/x86_64/paging.c. Recording is called from
 * shared code, so it needs a definition here; there is nowhere to put the
 * entry yet, and inventing a second ring that nothing prints would be worse
 * than an honest gap. */
void vma_trace_record(const char *what, u64 start, u64 end)
{
	(void)what; (void)start; (void)end;
}

/* MIDR_EL1 — the processor's own identity register (ARM ARM D17.2.100):
 * Implementer[31:24], Variant[23:20], Architecture[19:16], PartNum[15:4],
 * Revision[3:0]. There is nothing to measure or guess here. */
static u64 midr(void)
{
	u64 v;
	__asm__ volatile("mrs %0, midr_el1" : "=r"(v));
	return v;
}

static void copy_str(char *buf, usize len, const char *src)
{
	usize i = 0;
	for (; i + 1 < len && src[i]; i++)
		buf[i] = src[i];
	buf[i] = 0;
}

/* The implementer byte is an ASCII-derived JEP106 code; these are the ones a
 * b1nix guest realistically runs on. */
void arch_cpu_vendor(char *buf, usize len)
{
	if (!buf || len == 0)
		return;
	switch ((u32)(midr() >> 24) & 0xff) {
	case 0x41: copy_str(buf, len, "ARM"); return;
	case 0x42: copy_str(buf, len, "Broadcom"); return;
	case 0x43: copy_str(buf, len, "Cavium"); return;
	case 0x4e: copy_str(buf, len, "NVIDIA"); return;
	case 0x50: copy_str(buf, len, "Ampere"); return;
	case 0x51: copy_str(buf, len, "Qualcomm"); return;
	case 0x53: copy_str(buf, len, "Samsung"); return;
	case 0x56: copy_str(buf, len, "Marvell"); return;
	case 0x61: copy_str(buf, len, "Apple"); return;
	case 0x66: copy_str(buf, len, "Fujitsu"); return;
	case 0x69: copy_str(buf, len, "Intel"); return;
	default: copy_str(buf, len, "unknown"); return;
	}
}

/* Part numbers are per-implementer, so the table is keyed by both. An unknown
 * part still reports its real number rather than a made-up name. */
void arch_cpu_model(char *buf, usize len)
{
	if (!buf || len == 0)
		return;
	u64 id = midr();
	u32 impl = (u32)(id >> 24) & 0xff;
	u32 part = (u32)(id >> 4) & 0xfff;
	const char *name = 0;

	if (impl == 0x41) {
		switch (part) {
		case 0xd03: name = "ARM Cortex-A53"; break;
		case 0xd05: name = "ARM Cortex-A55"; break;
		case 0xd07: name = "ARM Cortex-A57"; break;
		case 0xd08: name = "ARM Cortex-A72"; break;
		case 0xd09: name = "ARM Cortex-A73"; break;
		case 0xd0a: name = "ARM Cortex-A75"; break;
		case 0xd0b: name = "ARM Cortex-A76"; break;
		case 0xd0c: name = "ARM Neoverse-N1"; break;
		case 0xd40: name = "ARM Neoverse-V1"; break;
		case 0xd49: name = "ARM Neoverse-N2"; break;
		default: break;
		}
	} else if (impl == 0x61) {
		/* Apple's part numbers are not published; report the family. */
		name = "Apple silicon";
	}

	if (name) {
		copy_str(buf, len, name);
		return;
	}
	char vendor[16];
	arch_cpu_vendor(vendor, sizeof(vendor));
	static const char hex[] = "0123456789abcdef";
	char tmp[40];
	usize n = 0;
	for (const char *v = vendor; *v && n < sizeof(tmp) - 12; v++)
		tmp[n++] = *v;
	tmp[n++] = ' ';
	tmp[n++] = 'p';
	tmp[n++] = 'a';
	tmp[n++] = 'r';
	tmp[n++] = 't';
	tmp[n++] = ' ';
	tmp[n++] = '0';
	tmp[n++] = 'x';
	tmp[n++] = hex[(part >> 8) & 0xf];
	tmp[n++] = hex[(part >> 4) & 0xf];
	tmp[n++] = hex[part & 0xf];
	tmp[n] = 0;
	copy_str(buf, len, tmp);
}

u32 arch_cpu_max_khz(void)
{
	return arch_cpu_khz();
}

int arch_xsave_enabled(void) { return 0; }
u64 arch_xsave_mask(void) { return 0; }
usize arch_xsave_area_size(void) { return 512; }
void arch_xsave(void *area, u64 mask) { (void)area; (void)mask; }
void arch_xrstor(void *area, u64 mask) { (void)area; (void)mask; }
void arch_xsave_capture_clean(void *area, u64 mask) { (void)area; (void)mask; }

/* PSCI (Power State Coordination Interface) — the firmware call every ARM
 * virtual machine and modern board uses to power off or reset. QEMU's `virt`
 * board advertises the HVC conduit in its device tree and implements
 * SYSTEM_OFF/SYSTEM_RESET, so a guest at EL1 just issues the call; without
 * this, SYS_REBOOT could only spin in a halt loop and the OpenRC smoke lane
 * (whose pass condition is a clean "reboot: powering off") could never end. */
#define PSCI_SYSTEM_OFF   0x84000008u
#define PSCI_SYSTEM_RESET 0x84000009u

static void psci_call(u32 fn)
{
	register u64 x0 __asm__("x0") = fn;
	/* Try HVC first (QEMU virt's conduit), then SMC for boards that use it.
	 * A successful call never returns. */
	__asm__ volatile("hvc #0" : "+r"(x0) : : "memory");
	__asm__ volatile("smc #0" : "+r"(x0) : : "memory");
}

#include <b1nix/bcm2835.h>
#include "platform.h"

void arch_psci_poweroff(void)
{
	if (bcm2835_pm_ready())
		bcm2835_pm_poweroff();
	psci_call(PSCI_SYSTEM_OFF);
}

void arch_psci_reset(void)
{
	if (bcm2835_pm_ready())
		bcm2835_pm_reset();
	psci_call(PSCI_SYSTEM_RESET);
}

void arch_halt(void)
{
	console_write("aarch64: arch_halt\n");
	while (1) {
		__asm__ volatile("wfi");
	}
}

/* ── x86-only subsystems the shared code links against ────────────────────
 * QEMU virt has no SMBus/CMOS RTC/watchdog/VT hardware and no DMA remapping
 * unit, and there is no sound driver on this arch yet. The generic callers
 * (kernel/fs/vfs.c's device-node registration, kernel/lkpi's DMA mapping)
 * are arch-independent, so satisfy them here rather than sprinkling #ifdefs
 * through shared files. iommu_active() returning 0 keeps every LKPI DMA
 * mapping on the direct-map path, which is what a machine without an IOMMU
 * does anyway. */
/* PCI interrupt plumbing. The bus itself works here (kernel/dev/pci.c reaches
 * configuration space through the memory-mapped ECAM window), but MSI on this
 * board is delivered through the GIC's ITS, which this port has no driver for
 * — devices use their legacy INTx line via the GIC instead. Report "no vector
 * available" so pci.c's MSI setup declines rather than programming a message
 * address that goes nowhere. lapic_id has no meaning without a LAPIC; the
 * boot CPU is 0. */
u32 lapic_id(void) { return 0; }
/* MSI vectors. The table is this file's; what makes a vector arrive is the ITS
 * (kernel/arch/aarch64/gicv3_its.c), which turns a device's write into the LPI
 * that corresponds to the vector. Same one-owner-per-vector rule as x86_64: a
 * message interrupt is point to point, so there is nothing to share. */
static struct {
	irq_handler_fn fn;
	void *ctx;
} g_msi_actions[MSI_VECTOR_COUNT];
static spinlock_t g_msi_lock = SPINLOCK_INIT;

int msi_alloc_vector(irq_handler_fn fn, void *ctx)
{
	u64 flags;

	if (!fn || !its_ready())
		return -1;

	spin_lock_irqsave(&g_msi_lock, &flags);
	for (u32 i = 0; i < MSI_VECTOR_COUNT; i++) {
		if (g_msi_actions[i].fn == 0) {
			g_msi_actions[i].ctx = ctx;
			__atomic_store_n(&g_msi_actions[i].fn, fn, __ATOMIC_RELEASE);
			spin_unlock_irqrestore(&g_msi_lock, flags);
			return (int)(MSI_VECTOR_BASE + i);
		}
	}
	spin_unlock_irqrestore(&g_msi_lock, flags);
	return -1;
}

void msi_free_vector(int vector)
{
	u64 flags;

	if (vector < (int)MSI_VECTOR_BASE ||
	    vector >= (int)(MSI_VECTOR_BASE + MSI_VECTOR_COUNT))
		return;
	spin_lock_irqsave(&g_msi_lock, &flags);
	__atomic_store_n(&g_msi_actions[vector - (int)MSI_VECTOR_BASE].fn,
	                 (irq_handler_fn)0, __ATOMIC_RELEASE);
	g_msi_actions[vector - (int)MSI_VECTOR_BASE].ctx = 0;
	spin_unlock_irqrestore(&g_msi_lock, flags);
}

int msi_dispatch(int vector)
{
	if (vector < (int)MSI_VECTOR_BASE ||
	    vector >= (int)(MSI_VECTOR_BASE + MSI_VECTOR_COUNT))
		return 0;

	u32 i = (u32)(vector - (int)MSI_VECTOR_BASE);
	irq_handler_fn fn = __atomic_load_n(&g_msi_actions[i].fn, __ATOMIC_ACQUIRE);

	return fn ? fn(g_msi_actions[i].ctx) : 0;
}

void i2c_init(void) {}
void i2c_register_nodes(void) {}

/* AMD-Vi: an x86 chipset unit. QEMU virt has no DMA remapping at all (its
 * IOMMU would be an ARM SMMUv3, which this port has no driver for), so report
 * "not present" — every LKPI DMA mapping then stays on the direct-map path,
 * which is what a machine without an IOMMU does anyway. */
int amdvi_active(void) { return 0; }
int amdvi_attach_device(u8 bus, u8 slot, u8 func) {
	(void)bus; (void)slot; (void)func; return -1;
}
void amdvi_detach_device(u8 bus, u8 slot, u8 func) {
	(void)bus; (void)slot; (void)func;
}
int amdvi_map(u64 iova, u64 phys, usize size, int writable) {
	(void)iova; (void)phys; (void)size; (void)writable; return -1;
}
int amdvi_unmap(u64 iova, usize size) { (void)iova; (void)size; return -1; }
u64 amdvi_translate(u64 iova) { return iova; }
u32 amdvi_fault_count(void) { return 0; }
void amdvi_fault_clear(void) {}

int iommu_active(void) { return 0; }
int iommu_attach_device(u8 bus, u8 slot, u8 func) {
	(void)bus; (void)slot; (void)func; return -1;
}
void iommu_detach_device(u8 bus, u8 slot, u8 func) {
	(void)bus; (void)slot; (void)func;
}
int iommu_map(u64 iova, u64 phys, usize size, int writable) {
	(void)iova; (void)phys; (void)size; (void)writable; return -1;
}
int iommu_unmap(u64 iova, usize size) { (void)iova; (void)size; return -1; }
int iommu_map_identity(u64 phys, usize size, int writable) {
	(void)phys; (void)size; (void)writable; return -1;
}
u64 iommu_translate(u64 iova) { return iova; }
u64 iommu_iova_alloc(usize size) { (void)size; return 0; }
void iommu_iova_free(u64 iova, usize size) { (void)iova; (void)size; }

/* Interrupt remapping and the fault log are part of the same absent unit. */
int iommu_ir_active(void) { return 0; }
int iommu_ir_alloc(u8 vector, u32 apic_id, u16 source) {
	(void)vector; (void)apic_id; (void)source; return -1;
}
void iommu_ir_free(int handle) { (void)handle; }
u64 iommu_ir_message_address(int handle) { (void)handle; return 0; }
u32 iommu_ir_message_data(int handle) { (void)handle; return 0; }
int iommu_ir_entry_read(int handle, u8 *vector, u32 *apic_id, u16 *source) {
	(void)handle; (void)vector; (void)apic_id; (void)source; return -1;
}
u32 iommu_fault_count(void) { return 0; }
void iommu_fault_clear(void) {}
void iommu_fault_last(u64 *addr, u16 *source, u8 *reason) {
	if (addr) *addr = 0;
	if (source) *source = 0;
	if (reason) *reason = 0;
}

/* cache_flush_range and the rest of the M98 memory-typing API now live in
 * kernel/arch/aarch64/memtype.c, next to the MAIR/write-combining code and the
 * set/way whole-hierarchy flush. The version here hardcoded a 64-byte line;
 * that one reads CTR_EL0.DminLine. */


/*
 * x86_64-only diagnostics, answered here so the shared callers link.
 *
 * acpi_cpu_count: no board this kernel runs on arm64 boots via ACPI — CPUs come
 * from the device tree, and bootinfo_cpu_count() already reports them.
 * pf_prof_dump: the page-fault profiler is built on x86_64's fault frame and
 * has no arm64 counterpart yet. kprof is no longer among these — it moved to
 * kernel/lib/kprof.c and samples ELR_EL1 off the CNTV tick.
 */
int acpi_cpu_count(void) { return 0; }
void pf_prof_dump(void) {}

/* ── Visual boot markers, C side ─────────────────────────────────────────────
 *
 * The companion to FBMARK in boot.S. Those five bands prove the assembly
 * prologue ran; this carries the same signal through kernel_main, which is
 * where a board with no serial cable otherwise goes dark.
 *
 * It draws ONE number, very large, overwriting the previous one: the index of
 * the last milestone reached. Bands and coloured squares were the first two
 * attempts and both failed the same way — reporting the result meant counting
 * shapes and naming colours, which is exactly the kind of question a person
 * standing over a phone should not have to answer precisely. A number is read,
 * not counted.
 *
 * The framebuffer is the one the bootloader was scanning out of, reached
 * through the direct map (identity mapped) — no driver, no allocation, nothing
 * that can itself fail. Compiled out unless the phone build defines
 * B1NIX_FB_BOOT_MARKERS.
 */
#ifdef B1NIX_FB_BOOT_MARKERS
#define FBD_PITCH  4320
#define FBD_PX     (FBD_PITCH / 4)
#define FBD_X      80   /* top-left of the readout */
#define FBD_Y      1400 /* well above screen bottom so all 4 rows are 100% visible */
#define FBD_SCALE  10   /* one font pixel becomes 10 screen pixels */
#define FBD_GLYPH_W 5
#define FBD_GLYPH_H 7

/* 5x7 digits, one byte per row, five significant bits. */
static const u8 fbd_digits[10][FBD_GLYPH_H] = {
	{0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, /* 0 */
	{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, /* 1 */
	{0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, /* 2 */
	{0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}, /* 3 */
	{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, /* 4 */
	{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, /* 5 */
	{0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, /* 6 */
	{0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, /* 7 */
	{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, /* 8 */
	{0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}, /* 9 */
};

static void fbd_fill(u32 x0, u32 y0, u32 w, u32 h, u32 colour)
{
	volatile u32 *fb = (volatile u32 *)(usize)(B1NIX_FB_BOOT_MARKERS);

	for (u32 y = y0; y < y0 + h; y++)
		for (u32 x = x0; x < x0 + w; x++)
			fb[(u64)y * FBD_PX + x] = colour;
}

static void fbd_digit(u32 x0, u32 y0, int d, u32 colour)
{
	for (u32 row = 0; row < FBD_GLYPH_H; row++)
		for (u32 col = 0; col < FBD_GLYPH_W; col++)
			if (fbd_digits[d][row] & (1u << (FBD_GLYPH_W - 1 - col)))
				fbd_fill(x0 + col * FBD_SCALE, y0 + row * FBD_SCALE,
				         FBD_SCALE, FBD_SCALE, colour);
}

/* Draw `value` (0..999) as digits on row `row`. Row 0 is the boot milestone;
 * the rows below carry facts that must stay on screen next to it, because a
 * milestone alone cannot say whether the kernel understood the machine it is
 * running on.
 *
 * Every row is repainted on every call. The facts are written once, very
 * early, and fb_console_init() later clears the whole framebuffer — so a
 * write-once readout is erased long before anyone reads it. Repainting from a
 * cached copy costs nothing and survives anything else that paints over the
 * screen. */
#define FBD_ROWS_MAX 4
static int fbd_value[FBD_ROWS_MAX];
static int fbd_shown[FBD_ROWS_MAX];

static void fbd_paint(int row, int value)
{
	u32 gw = FBD_GLYPH_W * FBD_SCALE;
	u32 gh = FBD_GLYPH_H * FBD_SCALE;
	u32 y = FBD_Y + (u32)row * (gh + FBD_SCALE * 2);
	int d[3];

	if (value < 0)
		value = 0;
	if (value > 999)
		value = 999;
	d[0] = (value / 100) % 10;
	d[1] = (value / 10) % 10;
	d[2] = value % 10;

	fbd_fill(FBD_X, y, (gw + FBD_SCALE) * 3, gh, 0xFF000000u);
	for (int i = 0; i < 3; i++) {
		if (i == 0 && value < 100)
			continue;
		if (i == 1 && value < 10)
			continue;
		fbd_digit(FBD_X + (u32)i * (gw + FBD_SCALE), y, d[i], 0xFFFFFFFFu);
	}
}

void fb_boot_num(int row, int value)
{
	if (row < 0 || row >= FBD_ROWS_MAX)
		return;
	fbd_value[row] = value;
	fbd_shown[row] = 1;
	for (int r = 0; r < FBD_ROWS_MAX; r++)
		if (fbd_shown[r])
			fbd_paint(r, fbd_value[r]);
	__asm__ volatile("dsb sy" ::: "memory");
}

void fb_boot_mark(int slot)
{
	fb_boot_num(0, slot);
}

#else
void fb_boot_mark(int slot) { (void)slot; }
/* Stubbed too: main.c reports the platform and memory facts through this on
 * every aarch64 build, not only the phone one. */
void fb_boot_num(int row, int value) { (void)row; (void)value; }
#endif

/* ── Qualcomm APSS watchdog ──────────────────────────────────────────────────
 *
 * The Snapdragon bootloader arms this before it hands over and expects the OS
 * to pet it forever after: the SM8150 tree asks for a pet every 9.36 s
 * (qcom,pet-time), barks at 11 s (qcom,bark-time) and bites shortly after.
 * Nothing in this kernel pets it, so the board reset itself roughly twenty
 * seconds into every boot — which during bring-up is indistinguishable from a
 * hang, and worse, it turns "where did it stop" into "how far did it get in
 * twenty seconds".
 *
 * ponytail: disabled outright rather than petted. Petting means a timer
 * callback and a real watchdog driver, and a watchdog is only worth having once
 * there is something for it to recover. Wire it to the existing
 * kernel/dev/watchdog.c when this board runs long enough to need one.
 */
#define QCOM_WDT_BASE 0x17C10000ULL
#define QCOM_WDT_EN   0x08

#define QCOM_WDT_RST  0x04
#define QCOM_WDT_BARK 0x10
#define QCOM_WDT_BITE 0x14

/* Returns what WDT_EN reads back afterwards: 0 means this kernel owns the
 * watchdog and it is off, 1 means the write was swallowed — on this SoC the
 * block can belong to TrustZone, and then it cannot be disabled from EL1 at
 * all and the only way to survive is to keep petting it. 999 means the board
 * is not one that has this device. */
u32 aarch64_platform_watchdog_disable(void)
{
	volatile u32 *wdt = (volatile u32 *)(usize)QCOM_WDT_BASE;

	if (platform_type() != PLATFORM_SM8150)
		return 999;

	/* Pet first, so the deadline moves away before anything else is touched,
	 * then push bark and bite as far out as the counters go, and only then
	 * try to disable. Each step alone is enough; together they survive a
	 * register the firmware only partly lets go of. */
	wdt[QCOM_WDT_RST / 4] = 1;
	wdt[QCOM_WDT_BARK / 4] = 0x7FFFFFFFu;
	wdt[QCOM_WDT_BITE / 4] = 0x7FFFFFFFu;
	wdt[QCOM_WDT_EN / 4] = 0;
	__asm__ volatile("dsb sy" ::: "memory");
	return wdt[QCOM_WDT_EN / 4];
}
