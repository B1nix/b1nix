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

int virtio_init_device(void *dev) { (void)dev; return -1; }
void virtio_set_guest_features(void *dev, u64 features) { (void)dev; (void)features; }
void virtio_set_status(void *dev, u8 status) { (void)dev; (void)status; }
u8 virtio_get_status(void *dev) { (void)dev; return 0; }
int virtq_init(void *dev, int qidx, void *vq) { (void)dev; (void)qidx; (void)vq; return -1; }
void virtq_kick(void *vq) { (void)vq; }

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

