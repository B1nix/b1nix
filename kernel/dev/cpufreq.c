/* SPDX-License-Identifier: GPL-2.0-only */
/* Frequency scaling (M129).
 *
 * What /sys/devices/system/cpu/cpuN/cpufreq said before this was the clock the
 * TSC calibration measured, repeated three times under different names, with
 * a governor field that named a governor nothing implemented. That is fine as
 * a number for a monitoring tool to print and useless as a control: nothing
 * could ask the processor to run slower, and nothing could say what the
 * processor was actually capable of.
 *
 * Two mechanisms, both architectural, both read through the faulting-safe MSR
 * accessors because a hypervisor may advertise the feature and still refuse
 * the register:
 *
 *   - HWP (CPUID.06H:EAX bit 7), where the processor picks the frequency and
 *     the kernel gives it a window and a preference. This is what every Intel
 *     machine since Skylake wants used, and what Linux's intel_pstate uses in
 *     its default mode.
 *   - EIST (CPUID.01H:ECX bit 7), the older explicit request: a bus-ratio in
 *     IA32_PERF_CTL, the ratio in force in IA32_PERF_STATUS.
 *
 * A machine with neither — every QEMU guest here, since KVM does not pass the
 * power-management leaves through — reports no driver at all, and the sysfs
 * files keep saying what the clock measured. Saying "none" is the point: a
 * governor that cannot move the clock must not claim it can. */

#include <b1nix/cpufreq.h>

#include <b1nix/console.h>
#include <b1nix/types.h>

#include <string.h>

#if defined(__x86_64__)

int arch_rdmsr_safe(u32 msr, u64 *out);
int arch_wrmsr_safe(u32 msr, u64 value);

#define MSR_PLATFORM_INFO        0x0CEu
#define MSR_IA32_PERF_STATUS     0x198u
#define MSR_IA32_PERF_CTL        0x199u
#define MSR_IA32_PM_ENABLE       0x770u
#define MSR_IA32_HWP_CAPABILITIES 0x771u
#define MSR_IA32_HWP_REQUEST     0x774u

/* Every Intel processor with these interfaces clocks the ratio against a
 * 100 MHz reference. The older 133 MHz parts predate HWP and EIST as the
 * kernel uses them here. */
#define CPUFREQ_BUS_KHZ 100000u

enum { DRV_NONE = 0, DRV_HWP, DRV_EIST };

static int g_driver;
static u32 g_min_khz, g_max_khz;
static u8 g_hwp_lowest, g_hwp_highest;
static const char *g_governor = "performance";

static void cpuid_count(u32 leaf, u32 sub, u32 *a, u32 *b, u32 *c, u32 *d) {
  __asm__ volatile("cpuid"
                   : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                   : "a"(leaf), "c"(sub));
}

/* Ask the processor for the window HWP may choose from. */
static int hwp_probe(void) {
  u64 caps = 0;

  if (arch_wrmsr_safe(MSR_IA32_PM_ENABLE, 1) != 0)
    return 0;
  if (arch_rdmsr_safe(MSR_IA32_HWP_CAPABILITIES, &caps) != 0)
    return 0;
  g_hwp_highest = (u8)(caps & 0xff);
  g_hwp_lowest = (u8)((caps >> 24) & 0xff);
  if (!g_hwp_highest || !g_hwp_lowest)
    return 0;
  g_max_khz = (u32)g_hwp_highest * CPUFREQ_BUS_KHZ;
  g_min_khz = (u32)g_hwp_lowest * CPUFREQ_BUS_KHZ;
  return 1;
}

static int eist_probe(void) {
  u64 info = 0, status = 0;
  u8 max_ratio, min_ratio;

  if (arch_rdmsr_safe(MSR_IA32_PERF_STATUS, &status) != 0)
    return 0;
  if (arch_rdmsr_safe(MSR_PLATFORM_INFO, &info) != 0)
    return 0;
  max_ratio = (u8)((info >> 8) & 0xff);  /* maximum non-turbo */
  min_ratio = (u8)((info >> 40) & 0xff); /* maximum efficiency */
  if (!max_ratio)
    return 0;
  if (!min_ratio)
    min_ratio = max_ratio;
  g_max_khz = (u32)max_ratio * CPUFREQ_BUS_KHZ;
  g_min_khz = (u32)min_ratio * CPUFREQ_BUS_KHZ;
  return 1;
}

void cpufreq_init(void) {
  u32 a, b, c, d;

  if (g_driver)
    return;
  cpuid_count(6, 0, &a, &b, &c, &d);
  if ((a & (1u << 7)) && hwp_probe()) {
    g_driver = DRV_HWP;
  } else {
    cpuid_count(1, 0, &a, &b, &c, &d);
    if ((c & (1u << 7)) && eist_probe())
      g_driver = DRV_EIST;
  }
  if (!g_driver)
    return;
  /* Start where the machine was: nothing here has asked for anything yet, and
   * a boot that quietly clamped the processor would be a performance bug
   * nobody looks for. */
  cpufreq_set_governor("performance");
  console_write("cpufreq: driver ");
  console_write(cpufreq_driver_name());
  console_write(", ");
  console_write_dec(g_min_khz / 1000);
  console_write("-");
  console_write_dec(g_max_khz / 1000);
  console_write(" MHz\n");
}

const char *cpufreq_driver_name(void) {
  switch (g_driver) {
  case DRV_HWP:
    return "hwp";
  case DRV_EIST:
    return "acpi-perf";
  default:
    return "none";
  }
}

u32 cpufreq_cur_khz(void) {
  u64 v = 0;

  if (g_driver == DRV_EIST || g_driver == DRV_HWP) {
    /* IA32_PERF_STATUS carries the ratio in force under both mechanisms. */
    if (arch_rdmsr_safe(MSR_IA32_PERF_STATUS, &v) == 0) {
      u32 ratio = (u32)((v >> 8) & 0xff);

      if (ratio)
        return ratio * CPUFREQ_BUS_KHZ;
    }
  }
  return 0; /* the caller falls back to the measured clock */
}

u32 cpufreq_max_khz(void) { return g_max_khz; }
u32 cpufreq_min_khz(void) { return g_min_khz; }
const char *cpufreq_governor(void) { return g_driver ? g_governor : "none"; }

int cpufreq_set_governor(const char *name) {
  int perf;

  if (!g_driver)
    return -1;
  if (!strcmp(name, "performance"))
    perf = 1;
  else if (!strcmp(name, "powersave"))
    perf = 0;
  else
    return -1;

  if (g_driver == DRV_HWP) {
    /* min, max, desired, energy-performance preference. Performance pins the
     * window to the top and asks for performance; powersave opens the window
     * and lets the processor choose, which is what HWP is for. */
    u64 req = perf ? ((u64)g_hwp_highest | ((u64)g_hwp_highest << 8) |
                      ((u64)0x00 << 24))
                   : ((u64)g_hwp_lowest | ((u64)g_hwp_highest << 8) |
                      ((u64)0x80 << 24));

    if (arch_wrmsr_safe(MSR_IA32_HWP_REQUEST, req) != 0)
      return -1;
  } else {
    u64 ctl = 0;
    u32 ratio = perf ? (g_max_khz / CPUFREQ_BUS_KHZ)
                     : (g_min_khz / CPUFREQ_BUS_KHZ);

    if (arch_rdmsr_safe(MSR_IA32_PERF_CTL, &ctl) != 0)
      return -1;
    ctl = (ctl & ~0xff00ull) | ((u64)(ratio & 0xff) << 8);
    if (arch_wrmsr_safe(MSR_IA32_PERF_CTL, ctl) != 0)
      return -1;
  }
  g_governor = perf ? "performance" : "powersave";
  return 0;
}

#else /* !__x86_64__ */

/* The aarch64 boards here scale through firmware interfaces this kernel does
 * not speak (SCMI, or a PSCI-backed cpufreq); nothing is claimed. */
void cpufreq_init(void) {}
const char *cpufreq_driver_name(void) { return "none"; }
u32 cpufreq_cur_khz(void) { return 0; }
u32 cpufreq_max_khz(void) { return 0; }
u32 cpufreq_min_khz(void) { return 0; }
const char *cpufreq_governor(void) { return "none"; }
int cpufreq_set_governor(const char *name) {
  (void)name;
  return -1;
}

#endif
