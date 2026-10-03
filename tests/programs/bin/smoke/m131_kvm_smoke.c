/* SPDX-License-Identifier: GPL-2.0-only */
/* m131_kvm_smoke — a virtual machine through /dev/kvm (M131).
 *
 * The smallest thing a hypervisor can be asked to do, with nothing but the
 * KVM ioctls: make a VM, give it one page of memory holding a few real-mode
 * instructions, make a vCPU, and run it. The guest adds two registers, writes
 * the digit to a serial port and halts, so the exits KVM_RUN reports -- two
 * port writes, then HLT -- are the guest's own work: '4' can only come from a
 * CPU that executed the code with the registers this test set.
 *
 * Each step is its own marker, so a failure says how far it got. A kernel
 * built without KVM, or a CPU without VMX, has no /dev/kvm; that is reported
 * as such and is the lane's decision, not a pass.
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/kvm.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

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

/* mov $0x3f8, %dx; add %bl, %al; add $'0', %al; out %al, (%dx);
 * mov $'\n', %al; out %al, (%dx); hlt */
static const uint8_t guest_code[] = {
	0xba, 0xf8, 0x03,
	0x00, 0xd8,
	0x04, '0',
	0xee,
	0xb0, '\n',
	0xee,
	0xf4,
};

int main(void)
{
	struct kvm_userspace_memory_region region;
	struct kvm_sregs sregs;
	struct kvm_regs regs;
	struct kvm_run *run;
	char out[8];
	int kvm, vm, vcpu, ver, mmap_size, nout = 0, halted = 0;
	uint8_t *mem;

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

	vm = ioctl(kvm, KVM_CREATE_VM, 0UL);
	if (vm < 0) {
		bad("create-vm", "KVM_CREATE_VM", vm);
		printf("M131-KVM: done\n");
		return 1;
	}
	ok("create-vm");

	mem = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (mem == MAP_FAILED) {
		bad("memslot", "mmap of guest memory", -1);
		return 1;
	}
	memcpy(mem, guest_code, sizeof(guest_code));
	memset(&region, 0, sizeof(region));
	region.slot = 0;
	region.guest_phys_addr = 0x1000;
	region.memory_size = 0x1000;
	region.userspace_addr = (uint64_t)(uintptr_t)mem;
	if (ioctl(vm, KVM_SET_USER_MEMORY_REGION, &region) < 0)
		bad("memslot", "KVM_SET_USER_MEMORY_REGION", -1);
	else
		ok("memslot");

	vcpu = ioctl(vm, KVM_CREATE_VCPU, 0UL);
	mmap_size = ioctl(kvm, KVM_GET_VCPU_MMAP_SIZE, NULL);
	if (vcpu < 0 || mmap_size < (int)sizeof(*run)) {
		bad("vcpu", "KVM_CREATE_VCPU / KVM_GET_VCPU_MMAP_SIZE", vcpu < 0 ? vcpu : mmap_size);
		printf("M131-KVM: done\n");
		return 1;
	}
	run = mmap(NULL, (size_t)mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu, 0);
	if (run == MAP_FAILED) {
		bad("vcpu", "mmap of the vCPU's run area", -1);
		printf("M131-KVM: done\n");
		return 1;
	}
	ok("vcpu");

	/* Real mode, CS based at 0, starting at the code's guest address. */
	if (ioctl(vcpu, KVM_GET_SREGS, &sregs) < 0) {
		bad("setup", "KVM_GET_SREGS", -1);
		return 1;
	}
	sregs.cs.base = 0;
	sregs.cs.selector = 0;
	if (ioctl(vcpu, KVM_SET_SREGS, &sregs) < 0) {
		bad("setup", "KVM_SET_SREGS", -1);
		return 1;
	}
	memset(&regs, 0, sizeof(regs));
	regs.rip = 0x1000;
	regs.rax = 2;
	regs.rbx = 2;
	regs.rflags = 0x2;
	if (ioctl(vcpu, KVM_SET_REGS, &regs) < 0) {
		bad("setup", "KVM_SET_REGS", -1);
		return 1;
	}
	ok("setup");

	for (int round = 0; round < 16 && !halted; round++) {
		if (ioctl(vcpu, KVM_RUN, NULL) < 0) {
			bad("run", "KVM_RUN", -1);
			break;
		}
		switch (run->exit_reason) {
		case KVM_EXIT_IO:
			if (run->io.direction == KVM_EXIT_IO_OUT && run->io.port == 0x3f8 &&
			    run->io.size == 1 && nout < (int)sizeof(out) - 1)
				out[nout++] = *((char *)run + run->io.data_offset);
			break;
		case KVM_EXIT_HLT:
			halted = 1;
			break;
		default:
			printf("M131-KVM: exit reason %u\n", run->exit_reason);
			bad("run", "unexpected exit", (long)run->exit_reason);
			round = 16;
			break;
		}
	}
	out[nout] = 0;
	printf("M131-KVM: guest wrote %d byte(s):", nout);
	for (int i = 0; i < nout; i++)
		printf(" 0x%02x", (unsigned char)out[i]);
	printf("%s\n", halted ? ", then halted" : "");
	if (nout == 2 && out[0] == '4' && out[1] == '\n')
		ok("guest-io");
	else
		bad("guest-io", "the guest's port writes are not \"4\\n\"", nout);
	if (halted)
		ok("guest-hlt");
	else
		bad("guest-hlt", "no HLT exit", 0);

	close(vcpu);
	close(vm);
	close(kvm);
	printf("M131-KVM: done\n");
	fflush(stdout);
	return fails ? 1 : 0;
}
