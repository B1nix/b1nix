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
 * governor that cannot move the clock must not claim it can.
 *
 * And a third, which is the one a machine whose processor hides its MSRs from
 * the operating system still has: ACPI's own `_PSS`. The firmware declares the
 * P-states it will honour — one inner package per state, giving the frequency
 * in MHz, the power in mW, the latencies, and the value to WRITE to ask for it
 * — and `_PCT` names the register to write it to. That is a table and a
 * register, not a model-specific feature, so it works where the MSRs do not;
 * it is also the only one of the three whose numbers are the platform's own
 * rather than this kernel's arithmetic on a bus ratio. See the ACPI
 * specification 8.4.6 (_PSS) and 8.4.5 (_PCT). */

#include <b1nix/cpufreq.h>

#include <b1nix/aml.h>
#include <b1nix/console.h>
#include <b1nix/types.h>

#include <stdio.h>
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

enum { DRV_NONE = 0, DRV_HWP, DRV_EIST, DRV_PSS };

static int g_driver;
static u32 g_min_khz, g_max_khz;
static u8 g_hwp_lowest, g_hwp_highest;
static const char *g_governor = "performance";

/* ── ACPI _PSS ────────────────────────────────────────────────────────────
 *
 * One entry per state the firmware declared, in the order it declared them —
 * which the specification requires to be fastest first. `control` is the value
 * to write to the register `_PCT` names; `status` is what that register's
 * companion reads back when the request has taken effect, and a firmware that
 * declares 0 there is saying "do not check".
 */
#define PSS_MAX_STATES 16
#define PSS_PATH_MAX   96

struct pss_state {
  u32 mhz;
  u32 power_mw;
  u32 latency_us;
  u32 control;
  u32 status;
};

static struct pss_state g_pss[PSS_MAX_STATES];
static int g_pss_n;
static char g_pss_path[PSS_PATH_MAX];
/* The two registers _PCT names. Space 0x7f is FunctionalFixedHW, which on this
 * architecture means the IA32_PERF_CTL/IA32_PERF_STATUS pair; 0x01 is an I/O
 * port, which is what a chipset-driven platform declares. */
static u8 g_pct_ctrl_space, g_pct_stat_space;
static u8 g_pct_ctrl_width, g_pct_stat_width;
static u64 g_pct_ctrl_addr, g_pct_stat_addr;
static int g_pss_sel = -1;    /* index of the state last asked for */
static int g_pss_write_ok;    /* did that request reach the register? */

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


/* ── the ACPI mechanism ──────────────────────────────────────────────────── */

#define PCT_SPACE_FFH 0x7fu

static void pss_path_join(char *out, usize cap, const char *base,
                          const char *leaf) {
  snprintf(out, cap, "%s.%s", base, leaf);
}

struct pss_scan {
  char (*node)[PSS_PATH_MAX];
  int n, max;
};

/* The walk may not evaluate anything — it runs with the interpreter's lock
 * held — so it only collects the nodes a _PSS could hang from. */
static void pss_scan_one(void *ctx, const char *path, int type) {
  struct pss_scan *sc = (struct pss_scan *)ctx;

  if (strlen(path) >= PSS_PATH_MAX || sc->n >= sc->max)
    return;
  if (type == AML_T_PROCESSOR || type == AML_T_DEVICE)
    strncpy(sc->node[sc->n++], path, PSS_PATH_MAX - 1);
}

/* One Generic Register descriptor out of a _PCT element: the resource
 * descriptor ACPI 6.4.3.7 defines, which is how firmware names a register
 * without knowing what the register is. */
static int pct_parse(const struct aml_result *r, u8 *space, u8 *width,
                     u64 *addr) {
  const u8 *b = r->bytes;

  if (r->type != AML_T_BUFFER || r->bytes_copied < 15 || b[0] != 0x82)
    return -1;
  *space = b[3];
  *width = b[4];
  *addr = 0;
  for (int i = 0; i < 8; i++)
    *addr |= (u64)b[7 + i] << (i * 8);
  return 0;
}

static int pss_read_states(const char *base) {
  char path[PSS_PATH_MAX + 8];
  struct aml_result r;
  int n = 0;

  pss_path_join(path, sizeof(path), base, "_PSS");
  if (aml_evaluate(path, 0, 0, &r) != AML_OK || r.type != AML_T_PACKAGE ||
      r.length == 0)
    return 0;
  for (u32 i = 0; i < r.length && n < PSS_MAX_STATES; i++) {
    struct aml_result st;

    if (aml_evaluate_element(path, 0, 0, i, &st) != AML_OK)
      break;
    /* Six integers, in the order the specification fixes. A package with
     * fewer is a firmware this kernel will not guess about. */
    if (st.type != AML_T_PACKAGE || st.elems < 6)
      break;
    if (!st.elem_int[0])
      continue; /* a state with no frequency describes nothing */
    g_pss[n].mhz = (u32)st.elem_int[0];
    g_pss[n].power_mw = (u32)st.elem_int[1];
    g_pss[n].latency_us = (u32)st.elem_int[2];
    g_pss[n].control = (u32)st.elem_int[4];
    g_pss[n].status = (u32)st.elem_int[5];
    n++;
  }
  return n;
}

static int pss_read_pct(const char *base) {
  char path[PSS_PATH_MAX + 8];
  struct aml_result ctrl, stat;

  pss_path_join(path, sizeof(path), base, "_PCT");
  if (aml_evaluate_element(path, 0, 0, 0, &ctrl) != AML_OK ||
      aml_evaluate_element(path, 0, 0, 1, &stat) != AML_OK)
    return -1;
  if (pct_parse(&ctrl, &g_pct_ctrl_space, &g_pct_ctrl_width,
                &g_pct_ctrl_addr) != 0 ||
      pct_parse(&stat, &g_pct_stat_space, &g_pct_stat_width,
                &g_pct_stat_addr) != 0)
    return -1;
  return 0;
}

/* Ask for a state by writing its control value where _PCT says. Returns 0 when
 * the write itself was accepted — a hypervisor that refuses the register is
 * reported, not worked around. */
static int pss_write_control(u32 control) {
  if (g_pct_ctrl_space == PCT_SPACE_FFH) {
    u64 v = 0;

    /* FunctionalFixedHW on this architecture is IA32_PERF_CTL, whose low 16
     * bits carry the request. The rest of the register belongs to the
     * platform and is left as it was found. */
    if (arch_rdmsr_safe(MSR_IA32_PERF_CTL, &v) != 0)
      return -1;
    v = (v & ~0xffffull) | (control & 0xffffu);
    return arch_wrmsr_safe(MSR_IA32_PERF_CTL, v);
  }
  if (g_pct_ctrl_space == AML_SPACE_IO) {
    u16 port = (u16)g_pct_ctrl_addr;

    if (g_pct_ctrl_width > 16)
      __asm__ volatile("outl %0, %1" : : "a"(control), "Nd"(port));
    else if (g_pct_ctrl_width > 8)
      __asm__ volatile("outw %0, %1" : : "a"((u16)control), "Nd"(port));
    else
      __asm__ volatile("outb %0, %1" : : "a"((u8)control), "Nd"(port));
    return 0;
  }
  return -1; /* an address space this kernel will not write blind */
}

static int pss_read_status(u32 *out) {
  if (g_pct_stat_space == PCT_SPACE_FFH) {
    u64 v = 0;

    if (arch_rdmsr_safe(MSR_IA32_PERF_STATUS, &v) != 0)
      return -1;
    *out = (u32)(v & 0xffffu);
    return 0;
  }
  if (g_pct_stat_space == AML_SPACE_IO) {
    u16 port = (u16)g_pct_stat_addr;
    u32 v = 0;

    if (g_pct_stat_width > 16)
      __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port));
    else if (g_pct_stat_width > 8) {
      u16 w = 0;
      __asm__ volatile("inw %1, %0" : "=a"(w) : "Nd"(port));
      v = w;
    } else {
      u8 bt = 0;
      __asm__ volatile("inb %1, %0" : "=a"(bt) : "Nd"(port));
      v = bt;
    }
    *out = v;
    return 0;
  }
  return -1;
}

/* Find a processor the firmware declared P-states for, and take them. */
static int pss_probe(void) {
  struct pss_scan sc;
  static char nodes[64][PSS_PATH_MAX];
  int found = 0;

  if (!aml_ready() || aml_table_count() == 0)
    return 0;
  memset(&sc, 0, sizeof(sc));
  sc.node = nodes;
  sc.max = 64;
  aml_walk(pss_scan_one, &sc);

  for (int i = 0; i < sc.n && !found; i++) {
    char path[PSS_PATH_MAX + 8];

    pss_path_join(path, sizeof(path), nodes[i], "_PSS");
    if (!aml_exists(path))
      continue;
    g_pss_n = pss_read_states(nodes[i]);
    if (g_pss_n < 1) {
      g_pss_n = 0;
      continue;
    }
    if (pss_read_pct(nodes[i]) != 0) {
      /* States without a register to request them are a description, not a
       * control. Reporting them as a driver would promise something this
       * kernel cannot do. */
      g_pss_n = 0;
      continue;
    }
    strncpy(g_pss_path, nodes[i], sizeof(g_pss_path) - 1);
    found = 1;
  }
  if (!found)
    return 0;

  g_max_khz = g_pss[0].mhz * 1000u;
  g_min_khz = g_pss[g_pss_n - 1].mhz * 1000u;
  for (int i = 0; i < g_pss_n; i++) {
    u32 khz = g_pss[i].mhz * 1000u;

    if (khz > g_max_khz)
      g_max_khz = khz;
    if (khz < g_min_khz)
      g_min_khz = khz;
  }
  return 1;
}

int cpufreq_state_count(void) { return g_driver == DRV_PSS ? g_pss_n : 0; }

u32 cpufreq_state_khz(int idx) {
  if (g_driver != DRV_PSS || idx < 0 || idx >= g_pss_n)
    return 0;
  return g_pss[idx].mhz * 1000u;
}

u32 cpufreq_state_control(int idx) {
  if (g_driver != DRV_PSS || idx < 0 || idx >= g_pss_n)
    return 0;
  return g_pss[idx].control;
}

const char *cpufreq_pss_path(void) {
  return (g_driver == DRV_PSS && g_pss_path[0]) ? g_pss_path : "";
}

const char *cpufreq_pct_space_name(void) {
  if (g_driver != DRV_PSS)
    return "none";
  if (g_pct_ctrl_space == PCT_SPACE_FFH)
    return "fixed-hw";
  if (g_pct_ctrl_space == AML_SPACE_IO)
    return "system-io";
  return "unsupported";
}

int cpufreq_request_state(int idx) {
  if (g_driver != DRV_PSS || idx < 0 || idx >= g_pss_n)
    return -1;
  g_pss_write_ok = pss_write_control(g_pss[idx].control) == 0;
  g_pss_sel = idx;
  return g_pss_write_ok ? 0 : -1;
}

int cpufreq_selected_state(void) { return g_driver == DRV_PSS ? g_pss_sel : -1; }
int cpufreq_request_took(void) { return g_driver == DRV_PSS ? g_pss_write_ok : 0; }

u32 cpufreq_status_value(void) {
  u32 v = 0;

  if (g_driver != DRV_PSS || pss_read_status(&v) != 0)
    return 0;
  return v;
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
  /* Last, because it is the least direct of the three and the other two say
   * what the processor itself will do. First in practice on any machine whose
   * hypervisor hides the power-management leaves and whose firmware still
   * declares P-states. */
  if (!g_driver && pss_probe())
    g_driver = DRV_PSS;
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
  console_write(" MHz");
  if (g_driver == DRV_PSS) {
    console_write(", ");
    console_write_dec((u64)g_pss_n);
    console_write(" states from ");
    console_write(g_pss_path);
    console_write("._PSS via ");
    console_write(cpufreq_pct_space_name());
  }
  console_write("\n");
}

const char *cpufreq_driver_name(void) {
  switch (g_driver) {
  case DRV_HWP:
    return "hwp";
  case DRV_EIST:
    return "acpi-perf";
  case DRV_PSS:
    /* Linux calls its own _PSS driver acpi-cpufreq, and a monitoring tool that
     * knows that name knows what these files mean. */
    return "acpi-cpufreq";
  default:
    return "none";
  }
}

u32 cpufreq_cur_khz(void) {
  u64 v = 0;

  if (g_driver == DRV_PSS) {
    u32 st = 0;

    /* The state the platform says it is in, matched against what the firmware
     * declared. A firmware that declares 0 for a state's status is saying the
     * register will not answer, and then the honest number is the state this
     * kernel last asked for. */
    if (pss_read_status(&st) == 0 && st) {
      for (int i = 0; i < g_pss_n; i++)
        if (g_pss[i].status && g_pss[i].status == st)
          return g_pss[i].mhz * 1000u;
    }
    if (g_pss_sel >= 0 && g_pss_sel < g_pss_n)
      return g_pss[g_pss_sel].mhz * 1000u;
    return 0;
  }
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

  if (g_driver == DRV_PSS) {
    /* _PSS is declared fastest first, so the two governors are its two ends.
     * A request the register refuses is a failure: the caller asked for a
     * frequency and did not get one. */
    if (cpufreq_request_state(perf ? 0 : g_pss_n - 1) != 0)
      return -1;
    g_governor = perf ? "performance" : "powersave";
    return 0;
  }
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

int cpufreq_state_count(void) { return 0; }
u32 cpufreq_state_khz(int idx) { (void)idx; return 0; }
u32 cpufreq_state_control(int idx) { (void)idx; return 0; }
const char *cpufreq_pss_path(void) { return ""; }
const char *cpufreq_pct_space_name(void) { return "none"; }
int cpufreq_request_state(int idx) { (void)idx; return -1; }
int cpufreq_selected_state(void) { return -1; }
int cpufreq_request_took(void) { return 0; }
u32 cpufreq_status_value(void) { return 0; }

#endif
