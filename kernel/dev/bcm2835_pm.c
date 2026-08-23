/*
 * The BCM2835/BCM2711 Power Management and Watchdog block.
 *
 * Used for software-triggered resets and power management on Broadcom SoCs
 * (Raspberry Pi boards).
 */

#include <b1nix/bcm2835.h>
#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/types.h>

#if defined(__aarch64__)

#define PM_RSTC                   0x1cu
#define PM_RSTS                   0x20u
#define PM_WDOG                   0x24u

#define PM_PASSWORD               0x5a000000u
#define PM_RSTC_WRCFG_FULL_RESET  0x00000020u
#define PM_RSTS_PARTITION_DEFAULT 0x00000555u

static u64 g_pm;

static inline u32 pm_read(u32 off)
{
	return *(volatile u32 *)(usize)(g_pm + off);
}

static inline void pm_write(u32 off, u32 val)
{
	*(volatile u32 *)(usize)(g_pm + off) = val;
}

void bcm2835_pm_init(void)
{
	g_pm = fdt_pm_base();
	if (!g_pm)
		return;
	console_write("pm: BCM2835 power management at 0x");
	console_write_hex64(g_pm);
	console_write("\n");
}

int bcm2835_pm_ready(void)
{
	return g_pm != 0;
}

void bcm2835_pm_reset(void)
{
	if (!g_pm)
		return;

	/* Clear partition flags, set full reset and arm watchdog */
	u32 rsts = pm_read(PM_RSTS);
	pm_write(PM_RSTS, PM_PASSWORD | (rsts & ~PM_RSTS_PARTITION_DEFAULT));
	pm_write(PM_WDOG, PM_PASSWORD | 10); /* 10 ticks */
	pm_write(PM_RSTC, PM_PASSWORD | PM_RSTC_WRCFG_FULL_RESET);

	while (1) {
		__asm__ volatile("wfe");
	}
}

void bcm2835_pm_poweroff(void)
{
	if (!g_pm)
		return;

	/* On Broadcom SoCs without PSCI, halt the CPU */
	while (1) {
		__asm__ volatile("wfi");
	}
}

#else

void bcm2835_pm_init(void) {}
int bcm2835_pm_ready(void) { return 0; }
void bcm2835_pm_reset(void) {}
void bcm2835_pm_poweroff(void) {}

#endif
