/*
 * reboot2_smoke — reboot(2) with LINUX_REBOOT_CMD_RESTART2 and a reason.
 *
 * `reboot bootloader` on a phone is this call. It must reach the kernel's
 * restart path with the string: the kernel prints "reboot: restarting
 * (<reason>)" and resets, so under QEMU -no-reboot the machine exits. Before
 * it, the same call was EINVAL and nothing happened.
 *
 * Usage: reboot2_smoke [reason]   (default "bootloader")
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/reboot.h>

int main(int argc, char **argv)
{
	const char *reason = argc > 1 ? argv[1] : "bootloader";

	sync();
	long rc = syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
	                  LINUX_REBOOT_CMD_RESTART2, reason);
	/* Only reached when the kernel refused: a reset does not return. */
	printf("REBOOT2-SMOKE: FAIL restart2 returned %ld (%s)\n", rc, strerror(errno));
	return 1;
}
