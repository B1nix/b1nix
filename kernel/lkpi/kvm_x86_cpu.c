// SPDX-License-Identifier: GPL-2.0-only
/*
 * The x86 CPU facilities Linux's KVM expects of its host (M131), on b1nix.
 *
 * What is here: the boot CPU's identity and capability words in Linux's
 * layout, IA32_FEAT_CTL set up the way Linux's feat_ctl.c does it, the MSR
 * probes, static calls, SMT detection, WBINVD, the TSC delay loop, the
 * emergency-reboot callback, the local-APIC sends and the host segment state
 * a VMCS records. The architecture-neutral part is kvm_cpu.c.
 *
 * The native half (CPU numbering, IPIs, the TSS and GS bases) is
 * kernel/virt/kvm_bridge.c; see <b1nix/kvm_bridge.h>.
 */
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/percpu.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/cpumask.h>
#include <linux/cpu.h>
#include <linux/static_call.h>
#include <linux/syscore_ops.h>
#include <asm/processor.h>
#include <asm/cpufeature.h>
#include <asm/msr.h>
#include <asm/apic.h>
#include <asm/reboot.h>
#include <asm/hypervisor.h>
#include <asm/nospec-branch.h>
#include <asm/spec-ctrl.h>
#include <asm/debugreg.h>
#include <asm/perf_event.h>
#include <asm/vmx.h>
#include <b1nix/kvm_bridge.h>

/* ── the boot CPU, in Linux's words ───────────────────────────────────── */

struct cpuinfo_x86 boot_cpu_data;

static void cpuid4(u32 leaf, u32 sub, u32 *a, u32 *b, u32 *c, u32 *d)
{
	__asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
			 : "a"(leaf), "c"(sub));
}

static void setcap(unsigned int bit) { set_cpu_cap(&boot_cpu_data, bit); }

/* IA32_FEAT_CTL as Linux's init_ia32_feat_ctl leaves it: VMX outside SMX
 * enabled and the MSR locked. Firmware that locked it with VMX off gets VMX
 * cleared, so KVM says "disabled by BIOS" instead of faulting on VMXON. */
static int feat_ctl_ok;

static void feat_ctl_cpu(void *unused)
{
	u64 v;

	(void)unused;
	if (lkpi_rdmsrq_safe(MSR_IA32_FEAT_CTL, &v)) {
		__atomic_store_n(&feat_ctl_ok, 0, __ATOMIC_RELAXED);
		return;
	}
	if (!(v & FEAT_CTL_LOCKED)) {
		v |= FEAT_CTL_LOCKED | FEAT_CTL_VMX_ENABLED_OUTSIDE_SMX;
		if (lkpi_wrmsrq_safe(MSR_IA32_FEAT_CTL, v)) {
			__atomic_store_n(&feat_ctl_ok, 0, __ATOMIC_RELAXED);
			return;
		}
	}
	if (!(v & FEAT_CTL_VMX_ENABLED_OUTSIDE_SMX))
		__atomic_store_n(&feat_ctl_ok, 0, __ATOMIC_RELAXED);
}

void lkpi_x86_cpu_init(void)
{
	static int done;
	struct cpuinfo_x86 *c = &boot_cpu_data;
	u32 a, b, cx, d, max, xmax;

	if (done)
		return;
	done = 1;
	memset(c, 0, sizeof(*c));

	cpuid4(0, 0, &max, &b, &cx, &d);
	c->cpuid_level = (int)max;
	memcpy(c->x86_vendor_id + 0, &b, 4);
	memcpy(c->x86_vendor_id + 4, &d, 4);
	memcpy(c->x86_vendor_id + 8, &cx, 4);
	if (!memcmp(c->x86_vendor_id, "GenuineIntel", 12))
		c->x86_vendor = X86_VENDOR_INTEL;
	else if (!memcmp(c->x86_vendor_id, "AuthenticAMD", 12))
		c->x86_vendor = X86_VENDOR_AMD;
	else if (!memcmp(c->x86_vendor_id, "HygonGenuine", 12))
		c->x86_vendor = X86_VENDOR_HYGON;
	else
		c->x86_vendor = X86_VENDOR_UNKNOWN;

	cpuid4(1, 0, &a, &b, &cx, &d);
	c->x86 = (u8)x86_family(a);
	c->x86_model = (u8)x86_model(a);
	c->x86_stepping = (u8)x86_stepping(a);
	c->x86_clflush_size = (u16)(((b >> 8) & 0xff) * 8);
	c->x86_capability[CPUID_1_EDX] = d;
	c->x86_capability[CPUID_1_ECX] = cx;

	cpuid4(0x80000000u, 0, &xmax, &b, &cx, &d);
	c->extended_cpuid_level = xmax;
	if (xmax >= 0x80000001u) {
		cpuid4(0x80000001u, 0, &a, &b, &cx, &d);
		c->x86_capability[CPUID_8000_0001_EDX] = d;
		c->x86_capability[CPUID_8000_0001_ECX] = cx;
	}
	if (xmax >= 0x80000007u) {
		cpuid4(0x80000007u, 0, &a, &b, &cx, &d);
		c->x86_capability[CPUID_8000_0007_EBX] = b;
		c->x86_power = (int)d;
		/* Invariant TSC: constant rate, and it keeps ticking in C-states. */
		if (d & (1u << 8)) {
			setcap(X86_FEATURE_CONSTANT_TSC);
			setcap(X86_FEATURE_NONSTOP_TSC);
		}
	}
	if (xmax >= 0x80000008u) {
		cpuid4(0x80000008u, 0, &a, &b, &cx, &d);
		c->x86_phys_bits = (u8)(a & 0xff);
		c->x86_virt_bits = (u8)((a >> 8) & 0xff);
		c->x86_capability[CPUID_8000_0008_EBX] = b;
	} else {
		c->x86_phys_bits = 36;
		c->x86_virt_bits = 48;
	}
	c->x86_cache_bits = c->x86_phys_bits;
	if (max >= 6) {
		cpuid4(6, 0, &a, &b, &cx, &d);
		c->x86_capability[CPUID_6_EAX] = a;
	}
	if (max >= 7) {
		u32 sub;

		cpuid4(7, 0, &sub, &b, &cx, &d);
		c->x86_capability[CPUID_7_0_EBX] = b;
		c->x86_capability[CPUID_7_ECX] = cx;
		c->x86_capability[CPUID_7_EDX] = d;
		if (sub >= 1) {
			cpuid4(7, 1, &a, &b, &cx, &d);
			c->x86_capability[CPUID_7_1_EAX] = a;
		}
		/* The Linux-defined speculation words, from the architectural
		 * bits Linux derives them from on Intel (7.0 EDX). */
		cpuid4(7, 0, &a, &b, &cx, &d);
		if (d & (1u << 26)) {          /* IBRS + IBPB */
			setcap(X86_FEATURE_IBRS);
			setcap(X86_FEATURE_IBPB);
			setcap(X86_FEATURE_MSR_SPEC_CTRL);
		}
		if (d & (1u << 27))
			setcap(X86_FEATURE_STIBP);
		if (d & (1u << 31))
			setcap(X86_FEATURE_MSR_SPEC_CTRL);
	}
	if (max >= 0xd) {
		cpuid4(0xd, 1, &a, &b, &cx, &d);
		c->x86_capability[CPUID_D_1_EAX] = a;
	}
	if (max >= 0xa) {
		cpuid4(0xa, 0, &a, &b, &cx, &d);
		if ((a & 0xff) > 0)
			setcap(X86_FEATURE_ARCH_PERFMON);
	}
	if (xmax >= 0x8000000au) {
		cpuid4(0x8000000au, 0, &a, &b, &cx, &d);
		c->x86_capability[CPUID_8000_000A_EDX] = d;
	}
	if (xmax >= 0x8000001fu) {
		cpuid4(0x8000001fu, 0, &a, &b, &cx, &d);
		c->x86_capability[CPUID_8000_001F_EAX] = a;
	}
	if (xmax >= 0x80000021u) {
		cpuid4(0x80000021u, 0, &a, &b, &cx, &d);
		c->x86_capability[CPUID_8000_0021_EAX] = a;
	}

	/* VMX needs IA32_FEAT_CTL configured on every CPU before VMXON. */
	if (boot_cpu_has(X86_FEATURE_VMX)) {
		feat_ctl_ok = 1;
		for (int cpu = 0; cpu < b1nix_kvm_cpu_count(); cpu++)
			b1nix_kvm_call_on_cpu(cpu, feat_ctl_cpu, NULL, 1);
		if (feat_ctl_ok)
			setcap(X86_FEATURE_MSR_IA32_FEAT_CTL);
		else
			clear_cpu_cap(c, X86_FEATURE_VMX);
	}
}

/* ── MSRs that may not exist ──────────────────────────────────────────── */

int lkpi_rdmsrq_safe(u32 msr, u64 *val)
{
	return b1nix_kvm_rdmsr_safe(msr, val) ? -EIO : 0;
}

int lkpi_wrmsrq_safe(u32 msr, u64 val)
{
	return b1nix_kvm_wrmsr_safe(msr, val) ? -EIO : 0;
}

/* ── speculation and mitigations: none applied (see asm/nospec-branch.h) ─ */

DEFINE_PER_CPU(u64, x86_spec_ctrl_current);
DEFINE_PER_CPU(bool, x86_ibpb_exit_to_user);
DEFINE_PER_CPU(unsigned long, cpu_dr7);
u64 x86_pred_cmd = PRED_CMD_IBPB;
DEFINE_STATIC_KEY_FALSE(switch_vcpu_ibpb);
DEFINE_STATIC_KEY_FALSE(cpu_buf_vm_clear);
DEFINE_STATIC_KEY_FALSE(cpu_buf_idle_clear);
enum l1tf_mitigations l1tf_mitigation = L1TF_MITIGATION_OFF;
enum vmx_l1d_flush_state l1tf_vmx_mitigation = VMENTER_L1D_FLUSH_NOT_REQUIRED;
bool itlb_multihit_kvm_mitigation;

/* SPEC_CTRL is never changed by b1nix, so the host value to put back after
 * a guest is whatever the CPU holds when the guest is entered. */
void x86_spec_ctrl_set_guest(u64 guest_virt_spec_ctrl)
{
	(void)guest_virt_spec_ctrl;
}

void x86_spec_ctrl_restore_host(u64 guest_virt_spec_ctrl)
{
	(void)guest_virt_spec_ctrl;
}

/* Debug registers are not used by the host: DR7 stays clear, and nothing
 * of b1nix's needs restoring after a guest touched them. */
void hw_breakpoint_restore(void)
{
}

/* Intel PT is not driven by the host. */
void intel_pt_handle_vmx(int on)
{
	(void)on;
}

/* No host perf counters to hand over around a guest's run. */
struct perf_guest_switch_msr *perf_guest_get_msrs(int *nr, void *data)
{
	(void)data;
	*nr = 0;
	return NULL;
}

/* ── the hypervisor under this kernel, if any ─────────────────────────── */

enum x86_hypervisor_type lkpi_x86_hypervisor_type(void)
{
	u32 a, b, c, d;
	char sig[13];

	if (!boot_cpu_has(X86_FEATURE_HYPERVISOR))
		return X86_HYPER_NATIVE;
	cpuid4(0x40000000u, 0, &a, &b, &c, &d);
	memcpy(sig + 0, &b, 4);
	memcpy(sig + 4, &c, 4);
	memcpy(sig + 8, &d, 4);
	sig[12] = 0;
	if (!memcmp(sig, "KVMKVMKVM", 9))
		return X86_HYPER_KVM;
	if (!memcmp(sig, "Microsoft Hv", 12))
		return X86_HYPER_MS_HYPERV;
	if (!memcmp(sig, "VMwareVMware", 12))
		return X86_HYPER_VMWARE;
	return X86_HYPER_NATIVE;
}

/* ── static calls ─────────────────────────────────────────────────────── */

void __static_call_nop(void)
{
}

long __static_call_return0(void)
{
	return 0;
}

/* Hyperthread siblings: CPUID 1 EBX[23:16] logical processors per package
 * against the cores CPUID 4 reports. */
bool sched_smt_active(void)
{
	u32 a, b, c, d, threads, cores;

	cpuid4(1, 0, &a, &b, &c, &d);
	if (!(d & (1u << 28)))
		return false;
	threads = (b >> 16) & 0xff;
	cpuid4(4, 0, &a, &b, &c, &d);
	cores = ((a >> 26) & 0x3f) + 1;
	return threads > cores;
}

static void wbinvd_this(void *unused)
{
	(void)unused;
	wbinvd();
}

void wbinvd_on_cpu(int cpu)
{
	b1nix_kvm_call_on_cpu(cpu, wbinvd_this, NULL, 1);
}

void wbinvd_on_cpus_mask(struct cpumask *cpus)
{
	for (int cpu = 0; cpu < b1nix_kvm_cpu_count(); cpu++)
		if ((cpus->bits[cpu / 64] >> (cpu % 64)) & 1)
			wbinvd_on_cpu(cpu);
}

void __delay(unsigned long loops)
{
	/* Upstream's loops are TSC cycles on a modern CPU. */
	u64 end = rdtsc() + loops;

	while (rdtsc() < end)
		cpu_relax();
}

static cpu_emergency_virt_cb *emergency_cb;

void cpu_emergency_register_virt_callback(cpu_emergency_virt_cb *callback)
{
	emergency_cb = callback;
}

void cpu_emergency_unregister_virt_callback(cpu_emergency_virt_cb *callback)
{
	if (emergency_cb == callback)
		emergency_cb = NULL;
}

/* ── the local APIC ───────────────────────────────────────────────────── */

int x2apic_mode;   /* b1nix drives the xAPIC through MMIO */

u32 default_cpu_present_to_apicid(int cpu)
{
	u32 id = b1nix_kvm_cpu_apic_id(cpu);

	return id == 0xffffffffu ? BAD_APICID : id;
}

u32 cpu_physical_id(int cpu)
{
	return b1nix_kvm_cpu_apic_id(cpu);
}

void __apic_send_IPI(int cpu, int vector)
{
	b1nix_kvm_send_ipi(cpu, (u32)vector);
}

void __apic_send_IPI_mask(const struct cpumask *mask, int vector)
{
	for (int cpu = 0; cpu < b1nix_kvm_cpu_count(); cpu++)
		if ((mask->bits[cpu / 64] >> (cpu % 64)) & 1)
			b1nix_kvm_send_ipi(cpu, (u32)vector);
}

void __apic_send_IPI_self(int vector)
{
	b1nix_kvm_send_ipi(b1nix_kvm_this_cpu(), (u32)vector);
}

/* Posted interrupts are not enabled (APICv is off), so the wakeup vector is
 * never raised; the handler is kept for when it is. */
static void (*pi_wakeup_handler)(void);

void kvm_set_posted_intr_wakeup_handler(void (*handler)(void))
{
	pi_wakeup_handler = handler;
}

/* ── the host segment state a VMCS records ────────────────────────────── */

unsigned long lkpi_x86_tss_base(int cpu)
{
	return (unsigned long)b1nix_kvm_cpu_tss_base(cpu);
}

unsigned long cpu_kernelmode_gs_base(int cpu)
{
	return (unsigned long)b1nix_kvm_cpu_gs_base(cpu);
}

/* Linux's entry stack sits in the cpu_entry_area; b1nix enters the kernel
 * on the TSS's RSP0, which KVM never needs directly. */
void *cpu_entry_stack(int cpu)
{
	(void)cpu;
	return NULL;
}

/* The user FS/GS selectors and bases of the task entering a guest. b1nix
 * does not swap GS, so MSR_KERNEL_GS_BASE is the "other" GS base, which KVM
 * lends to the guest and puts back.
 *
 * The selectors are reported as null. b1nix keeps the flat user-data
 * selector in %fs, whose descriptor contributes no base in 64-bit mode; the
 * base is the MSR alone, so a null selector addresses the same TLS. Reported
 * as it is, its RPL bits make KVM reload it on the way back from the guest,
 * and loading a selector resets the segment base: the task would return to
 * userspace with FS.base 0 and fault on its first %fs:N access. */
void current_save_fsgs(void)
{
	struct task_struct *t = current;

	t->thread.fsindex = 0;
	t->thread.gsindex = 0;
	t->thread.fsbase = native_rdmsrq(MSR_FS_BASE);
	t->thread.gsbase = native_rdmsrq(MSR_KERNEL_GS_BASE);
}

/* ── the vector-2 gate, for an NMI that arrives in guest mode ─────────── */
/* After a VM exit for an NMI, KVM calls the host's NMI entry with an
 * interrupt frame (vmx_do_nmi_irqoff); b1nix's is the IDT stub for vector 2,
 * which returns with IRETQ. */
__asm__(".text\n"
	".globl asm_exc_nmi_kvm_vmx\n"
	".type asm_exc_nmi_kvm_vmx, @function\n"
	"asm_exc_nmi_kvm_vmx:\n"
	"\tjmp isr2\n"
	".size asm_exc_nmi_kvm_vmx, . - asm_exc_nmi_kvm_vmx\n");

/* ── AMD SVM (M131) ─────────────────────────────────────────────────────── */

int param_set_bint(const char *val, const struct kernel_param *kp)
{
	(void)val; (void)kp;
	return -EINVAL;
}

/* ── start-up, x86's half (kvm_sched.c calls these) ───────────────────── */

int lkpi_initcall_kvm_x86_init(void);
int lkpi_initcall_vt_init(void);
int lkpi_initcall_svm_init(void);
void kvm_percpu_mirror_all(void);
extern bool enable_apicv;

int kvm_arch_lkpi_early(void)
{
	lkpi_x86_cpu_init();
	/* The rate b1nix's clock was calibrated to: KVM's TSC and kvmclock
	 * arithmetic all divide by it. */
	tsc_khz = b1nix_kvm_tsc_khz();
	return 0;
}

/* The generic x86 layer, then VMX -- or SVM when the CPU has no VMX -- as
 * loading kvm, kvm_intel and kvm_amd does. */
int kvm_arch_lkpi_start(void)
{
	int r;

	/* Posted interrupts need host vectors b1nix does not route to KVM yet;
	 * the in-kernel APIC is emulated without them. */
	enable_apicv = false;
	r = lkpi_initcall_kvm_x86_init();
	if (r)
		return r;
	/* Intel's module first; on a CPU without VMX it declines and AMD's
	 * takes the machine instead, as loading kvm_intel then kvm_amd does. */
	r = lkpi_initcall_vt_init();
	if (r) {
		int vmx = r;

		r = lkpi_initcall_svm_init();
		if (r)
			pr_info("kvm: neither vendor module loaded (VMX %d, SVM %d)\n", vmx, r);
		else
			pr_info("kvm: AMD SVM\n");
	} else {
		pr_info("kvm: Intel VMX\n");
	}
	if (!r)
		kvm_percpu_mirror_all();
	return r;
}
