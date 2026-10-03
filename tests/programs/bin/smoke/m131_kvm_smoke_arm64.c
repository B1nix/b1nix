/* SPDX-License-Identifier: GPL-2.0-only */
/* m131_kvm_smoke (arm64) — virtual machines through /dev/kvm (M131).
 *
 * The arm64 counterpart of the x86 test, with nothing but the KVM ioctls.
 *
 * The first VM gets one page of code and one vCPU with PSCI. The guest adds
 * x0 and x1, which this test set through KVM_SET_ONE_REG, stores the digit to
 * an address with no memory behind it and powers itself off with a PSCI
 * call. KVM_RUN reports the store as an MMIO exit and the call as a system
 * event, so '4' and the shutdown are the guest's own work.
 *
 * The second VM adds an in-kernel GICv3 (vGIC). Its guest programs the GIC
 * CPU interface, distributor and redistributor, arms its virtual timer and
 * waits in WFI with interrupts unmasked; the timer interrupt arrives through
 * the vGIC, the guest's handler acknowledges it and reports the INTID it
 * read, then powers off. That is the host's timer and GIC path end to end:
 * the virtual timer runs in the guest, KVM injects its interrupt, and the
 * guest's acknowledge and EOI go through the virtual CPU interface.
 *
 * Each step is its own marker. A kernel without KVM, or one not at EL2, has
 * no /dev/kvm; that is reported as such.
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/kvm.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define GUEST_BASE	0x80000000ULL
#define GUEST_SIZE	0x10000ULL
#define REPORT_ADDR	0x09000000ULL	/* no memory there: every store exits */
#define GICD_BASE	0x08000000ULL
#define GICR_BASE	0x080a0000ULL

#define PSCI_SYSTEM_OFF	0x84000008

static int fails;

static void ok(const char *what)
{
	printf("M131-KVM: ok %s\n", what);
	fflush(stdout);
}

static void bad(const char *what, const char *why, long v)
{
	printf("M131-KVM: FAIL %s — %s (%ld, errno=%d)\n", what, why, v, errno);
	fflush(stdout);
	fails++;
}

/* The guests, assembled here and copied into guest memory. */
extern const unsigned char guest_add[], guest_add_end[];
extern const unsigned char guest_timer[], guest_timer_end[];

__asm__(
	".pushsection .rodata\n"
	".balign 4\n"
	".globl guest_add\n"
	"guest_add:\n"
	"	add	x2, x0, x1\n"
	"	add	x2, x2, #'0'\n"
	"	movz	x3, #0x0900, lsl #16\n"
	"	strb	w2, [x3]\n"
	"	mov	w2, #'\\n'\n"
	"	strb	w2, [x3]\n"
	"	movz	x0, #0x0008\n"
	"	movk	x0, #0x8400, lsl #16\n"
	"	hvc	#0\n"
	"1:	b	1b\n"
	".globl guest_add_end\n"
	"guest_add_end:\n"

	/* Code at +0, vectors at +0x800 (VBAR_EL1 needs 2 KiB alignment, and
	 * the copy starts at guest_timer, so it is aligned too). */
	".balign 0x800\n"
	".globl guest_timer\n"
	"guest_timer:\n"
	"	adr	x0, 9f\n"
	"	msr	vbar_el1, x0\n"
	"	isb\n"
	/* GIC CPU interface: system registers, all priorities, group 1 on. */
	"	mov	x0, #1\n"
	"	msr	s3_0_c12_c12_5, x0\n"		/* ICC_SRE_EL1.SRE */
	"	isb\n"
	"	mov	x0, #0xff\n"
	"	msr	s3_0_c4_c6_0, x0\n"		/* ICC_PMR_EL1 */
	"	mov	x0, #1\n"
	"	msr	s3_0_c12_c12_7, x0\n"		/* ICC_IGRPEN1_EL1 */
	"	isb\n"
	/* Distributor: affinity routing, group 1 enabled. */
	"	movz	x4, #0x0800, lsl #16\n"
	"	mov	w0, #0x12\n"
	"	str	w0, [x4]\n"			/* GICD_CTLR = ARE_NS | EnableGrp1NS */
	/* Redistributor: awake, PPI 27 in group 1 and enabled. */
	"	movz	x5, #0x080a, lsl #16\n"
	"	ldr	w0, [x5, #0x14]\n"
	"	bic	w0, w0, #2\n"
	"	str	w0, [x5, #0x14]\n"		/* GICR_WAKER.ProcessorSleep = 0 */
	"	add	x6, x5, #0x10000\n"		/* SGI_base */
	"	mov	w0, #-1\n"
	"	str	w0, [x6, #0x80]\n"		/* GICR_IGROUPR0: all group 1 */
	"	mov	w0, #(1 << 27)\n"
	"	str	w0, [x6, #0x100]\n"		/* GICR_ISENABLER0 */
	/* The virtual timer, a few microseconds out, then wait for it. */
	"	mov	x0, #1000\n"
	"	msr	cntv_tval_el0, x0\n"
	"	mov	x0, #1\n"
	"	msr	cntv_ctl_el0, x0\n"
	"	isb\n"
	"	msr	daifclr, #2\n"
	"2:	wfi\n"
	"	b	2b\n"
	".balign 0x800\n"
	"9:\n"
	".skip 0x280\n"
	/* Current EL, SPx, IRQ: acknowledge, report the INTID, stop the timer,
	 * end the interrupt, power off. */
	"	mrs	x1, s3_0_c12_c12_0\n"		/* ICC_IAR1_EL1 */
	"	movz	x3, #0x0900, lsl #16\n"
	"	strb	w1, [x3]\n"
	"	msr	cntv_ctl_el0, xzr\n"
	"	msr	s3_0_c12_c12_1, x1\n"		/* ICC_EOIR1_EL1 */
	"	isb\n"
	"	movz	x0, #0x0008\n"
	"	movk	x0, #0x8400, lsl #16\n"
	"	hvc	#0\n"
	"3:	b	3b\n"
	".balign 4\n"
	".globl guest_timer_end\n"
	"guest_timer_end:\n"
	".popsection\n");

static uint64_t core_reg(size_t off)
{
	return KVM_REG_ARM64 | KVM_REG_SIZE_U64 | KVM_REG_ARM_CORE | (off / sizeof(uint32_t));
}

static int set_reg(int vcpu, uint64_t id, uint64_t val)
{
	struct kvm_one_reg reg = { .id = id, .addr = (uint64_t)(uintptr_t)&val };

	return ioctl(vcpu, KVM_SET_ONE_REG, &reg);
}

struct vm {
	int fd;
	int vcpu;
	struct kvm_run *run;
	uint8_t *mem;
};

/* A VM with GUEST_SIZE of memory at GUEST_BASE holding [code, end), and one
 * vCPU with PSCI 0.2 starting there. Before the vCPU is made, @before gets
 * the VM (the vGIC is created there). */
static int make_vm(int kvm, struct vm *v, const unsigned char *code,
		   const unsigned char *end, int (*before)(struct vm *))
{
	struct kvm_userspace_memory_region region;
	struct kvm_vcpu_init init;
	int mmap_size;

	v->fd = ioctl(kvm, KVM_CREATE_VM, 0UL);
	if (v->fd < 0) {
		bad("create-vm", "KVM_CREATE_VM", v->fd);
		return -1;
	}
	v->mem = mmap(NULL, GUEST_SIZE, PROT_READ | PROT_WRITE,
		      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (v->mem == MAP_FAILED) {
		bad("memslot", "mmap of guest memory", -1);
		return -1;
	}
	memcpy(v->mem, code, (size_t)(end - code));
	memset(&region, 0, sizeof(region));
	region.slot = 0;
	region.guest_phys_addr = GUEST_BASE;
	region.memory_size = GUEST_SIZE;
	region.userspace_addr = (uint64_t)(uintptr_t)v->mem;
	if (ioctl(v->fd, KVM_SET_USER_MEMORY_REGION, &region) < 0) {
		bad("memslot", "KVM_SET_USER_MEMORY_REGION", -1);
		return -1;
	}
	if (before && before(v) < 0)
		return -1;
	v->vcpu = ioctl(v->fd, KVM_CREATE_VCPU, 0UL);
	mmap_size = ioctl(kvm, KVM_GET_VCPU_MMAP_SIZE, NULL);
	if (v->vcpu < 0 || mmap_size < (int)sizeof(*v->run)) {
		bad("vcpu", "KVM_CREATE_VCPU / KVM_GET_VCPU_MMAP_SIZE",
		    v->vcpu < 0 ? v->vcpu : mmap_size);
		return -1;
	}
	v->run = mmap(NULL, (size_t)mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, v->vcpu, 0);
	if (v->run == MAP_FAILED) {
		bad("vcpu", "mmap of the vCPU's run area", -1);
		return -1;
	}
	memset(&init, 0, sizeof(init));
	if (ioctl(v->fd, KVM_ARM_PREFERRED_TARGET, &init) < 0) {
		bad("vcpu-init", "KVM_ARM_PREFERRED_TARGET", -1);
		return -1;
	}
	init.features[0] |= 1u << KVM_ARM_VCPU_PSCI_0_2;
	if (ioctl(v->vcpu, KVM_ARM_VCPU_INIT, &init) < 0) {
		bad("vcpu-init", "KVM_ARM_VCPU_INIT", -1);
		return -1;
	}
	if (set_reg(v->vcpu, core_reg(offsetof(struct kvm_regs, regs.pc)), GUEST_BASE) < 0) {
		bad("vcpu-init", "KVM_SET_ONE_REG pc", -1);
		return -1;
	}
	return 0;
}

/* Run until the guest powers off; every byte it stores to REPORT_ADDR goes
 * to @out. 1 when it powered off. */
static int run_vm(struct vm *v, char *out, int outlen, int *nout)
{
	for (int round = 0; round < 64; round++) {
		if (ioctl(v->vcpu, KVM_RUN, NULL) < 0) {
			if (errno == EINTR)
				continue;
			bad("run", "KVM_RUN", -1);
			return 0;
		}
		switch (v->run->exit_reason) {
		case KVM_EXIT_MMIO:
			if (v->run->mmio.is_write && v->run->mmio.phys_addr == REPORT_ADDR &&
			    v->run->mmio.len == 1 && *nout < outlen - 1)
				out[(*nout)++] = (char)v->run->mmio.data[0];
			break;
		case KVM_EXIT_SYSTEM_EVENT:
			return v->run->system_event.type == KVM_SYSTEM_EVENT_SHUTDOWN;
		default:
			bad("run", "unexpected exit reason", v->run->exit_reason);
			return 0;
		}
	}
	bad("run", "no power-off after 64 exits", 64);
	return 0;
}

static int vgic_fd = -1;

static int set_attr(int fd, uint32_t group, uint64_t attr, void *addr)
{
	struct kvm_device_attr a = {
		.group = group, .attr = attr, .addr = (uint64_t)(uintptr_t)addr,
	};

	return ioctl(fd, KVM_SET_DEVICE_ATTR, &a);
}

static int add_vgic(struct vm *v)
{
	struct kvm_create_device dev = { .type = KVM_DEV_TYPE_ARM_VGIC_V3 };
	uint64_t dist = GICD_BASE, redist = GICR_BASE;

	if (ioctl(v->fd, KVM_CREATE_DEVICE, &dev) < 0) {
		bad("vgic", "KVM_CREATE_DEVICE vGICv3", -1);
		return -1;
	}
	vgic_fd = (int)dev.fd;
	if (set_attr(vgic_fd, KVM_DEV_ARM_VGIC_GRP_ADDR, KVM_VGIC_V3_ADDR_TYPE_DIST, &dist) < 0 ||
	    set_attr(vgic_fd, KVM_DEV_ARM_VGIC_GRP_ADDR, KVM_VGIC_V3_ADDR_TYPE_REDIST, &redist) < 0) {
		bad("vgic", "KVM_DEV_ARM_VGIC_GRP_ADDR", -1);
		return -1;
	}
	return 0;
}

int main(void)
{
	struct vm a, t;
	char out[16];
	int kvm, ver, nout = 0;

	printf("M131-KVM: start\n");
	fflush(stdout);
	kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC);
	if (kvm < 0) {
		printf("M131-KVM: no /dev/kvm (errno=%d)\n", errno);
		printf("M131-KVM: done\n");
		return 1;
	}
	ver = ioctl(kvm, KVM_GET_API_VERSION, 0);
	if (ver == KVM_API_VERSION)
		ok("api-version");
	else
		bad("api-version", "KVM_GET_API_VERSION", ver);

	if (make_vm(kvm, &a, guest_add, guest_add_end, NULL) == 0) {
		ok("create-vm");
		ok("memslot");
		ok("vcpu");
		if (set_reg(a.vcpu, core_reg(offsetof(struct kvm_regs, regs.regs[0])), 2) < 0 ||
		    set_reg(a.vcpu, core_reg(offsetof(struct kvm_regs, regs.regs[1])), 2) < 0) {
			bad("setup", "KVM_SET_ONE_REG x0/x1", -1);
		} else {
			ok("setup");
			if (run_vm(&a, out, sizeof(out), &nout)) {
				out[nout] = 0;
				if (nout == 2 && out[0] == '4' && out[1] == '\n')
					ok("guest-mmio");
				else
					bad("guest-mmio", "the guest's stores were not 4 and a newline", nout);
				ok("guest-psci-off");
			}
		}
	}

	nout = 0;
	if (make_vm(kvm, &t, guest_timer, guest_timer_end, add_vgic) == 0) {
		if (set_attr(vgic_fd, KVM_DEV_ARM_VGIC_GRP_CTRL, KVM_DEV_ARM_VGIC_CTRL_INIT, NULL) < 0) {
			bad("vgic", "KVM_DEV_ARM_VGIC_CTRL_INIT", -1);
		} else {
			ok("vgic");
			if (run_vm(&t, out, sizeof(out), &nout)) {
				if (nout == 1 && out[0] == 27)
					ok("guest-timer-irq");
				else
					bad("guest-timer-irq", "the guest did not report INTID 27",
					    nout ? out[0] : -1);
			}
		}
	}

	printf("M131-KVM: %s\n", fails ? "FAIL" : "done");
	fflush(stdout);
	return fails ? 1 : 0;
}
