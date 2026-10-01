/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ACPI events (M135): the SCI, the fixed events, GPEs and Notify().
 *
 * The platform tells the OS about a button, a lid, an adapter, a battery or a
 * temperature by raising the System Control Interrupt. Behind it are two
 * kinds of source, both read out of the FADT:
 *
 *   - the fixed events in PM1: the power button, the sleep button, the RTC
 *     alarm. A status bit each, an enable bit each, write 1 to clear;
 *   - the general-purpose events in the GPE blocks: one status and one enable
 *     bit per event, and for each an AML method the firmware wrote to service
 *     it, \_GPE._Lxx for a level-triggered event and \_GPE._Exx for an edge.
 *
 * A GPE's method usually ends in Notify(device, value), which is the firmware
 * telling the OS "look at this device again". What the OS then does depends on
 * the device: a battery's state is re-read and userspace told it changed, a
 * lid's _LID is read and reported as a switch, a control-method button is
 * pressed.
 *
 * The interrupt handler only reads and clears status and masks what fired;
 * methods run in the kacpid thread, because the interpreter allocates and an
 * evaluation can be long. A level GPE is left masked until its method has run
 * and its status is clear, an edge one is cleared first -- the same order as
 * Linux's ACPICA, and the only order in which neither kind is lost nor storms.
 *
 * x86 only: an arm64 ACPI platform is hardware-reduced, with no PM1 or GPE
 * registers and its events through a GED device instead.
 */

#include <b1nix/acpi_event.h>

#include <b1nix/acpi.h>
#include <b1nix/acpi_power.h>
#include <b1nix/aml.h>
#include <b1nix/console.h>
#include <b1nix/cpufreq.h>
#include <b1nix/errno.h>
#include <b1nix/input.h>
#include <b1nix/irq.h>
#include <b1nix/kprintf.h>
#include <b1nix/mm.h>
#include <b1nix/pci.h>
#include <b1nix/sched.h>
#include <b1nix/spinlock.h>
#include <b1nix/suspend.h>
#include <b1nix/thermal.h>
#include <b1nix/sysfs_attr.h>
#include <b1nix/types.h>
#include <b1nix/uevent.h>
#if defined(__x86_64__)
#include <b1nix/arch.h>
#include <b1nix/io.h>
#endif

#include <stdio.h>
#include <string.h>

/* ── Notify(), from the interpreter ─────────────────────────────────────── */

#define NOTIFY_Q 32
#define NOTIFY_PATH 64

struct notify_rec {
  char path[NOTIFY_PATH];
  u64 value;
};

static struct notify_rec g_nq[NOTIFY_Q];
static u32 g_nq_head, g_nq_tail;
static spinlock_t g_nq_lock = SPINLOCK_INIT;
static u64 g_nq_dropped;
/* What kacpid sleeps on; everything that gives it work wakes it. */
static int g_kacpid_chan;
static volatile int g_kacpid_running;

void acpi_event_notify(const char *path, u64 value) {
  u64 flags;

  if (!path)
    return;
  spin_lock_irqsave(&g_nq_lock, &flags);
  if (g_nq_tail - g_nq_head < NOTIFY_Q) {
    struct notify_rec *n = &g_nq[g_nq_tail % NOTIFY_Q];

    strncpy(n->path, path, NOTIFY_PATH - 1);
    n->path[NOTIFY_PATH - 1] = '\0';
    n->value = value;
    g_nq_tail++;
  } else {
    g_nq_dropped++;
  }
  spin_unlock_irqrestore(&g_nq_lock, flags);
  if (__atomic_load_n(&g_kacpid_running, __ATOMIC_ACQUIRE))
    scheduler_wake_all(&g_kacpid_chan);
}


#if defined(__x86_64__)

static int notify_pop(struct notify_rec *out) {
  u64 flags;
  int got = 0;

  spin_lock_irqsave(&g_nq_lock, &flags);
  if (g_nq_head != g_nq_tail) {
    *out = g_nq[g_nq_head % NOTIFY_Q];
    g_nq_head++;
    got = 1;
  }
  spin_unlock_irqrestore(&g_nq_lock, flags);
  return got;
}

/* ── the registers, from the FADT ──────────────────────────────────────── */

#define FADT_OFF_SCI_INT      46
#define FADT_OFF_SMI_CMD      48
#define FADT_OFF_ACPI_ENABLE  52
#define FADT_OFF_PM1A_EVT     56
#define FADT_OFF_PM1B_EVT     60
#define FADT_OFF_GPE0_BLK     80
#define FADT_OFF_GPE1_BLK     84
#define FADT_OFF_PM1_EVT_LEN  88
#define FADT_OFF_GPE0_LEN     92
#define FADT_OFF_GPE1_LEN     93
#define FADT_OFF_GPE1_BASE    94
#define FADT_OFF_FLAGS        112

#define FADT_F_PWR_BUTTON  (1u << 4)  /* set: no FIXED power button */
#define FADT_F_SLP_BUTTON  (1u << 5)  /* set: no FIXED sleep button */
#define FADT_F_HW_REDUCED  (1u << 20)

#define PM1_TMR    (1u << 0)
#define PM1_GBL    (1u << 5)
#define PM1_PWRBTN (1u << 8)
#define PM1_SLPBTN (1u << 9)
#define PM1_RTC    (1u << 10)
#define PM1_CNT_SCI_EN (1u << 0)
#define PM1_CNT_SLP_TYP_SHIFT 10
#define PM1_CNT_SLP_EN (1u << 13)

#define GPE_MAX 128

static u16 g_pm1a_evt, g_pm1b_evt;
static u8 g_pm1_len;
static u16 g_gpe_blk[2];
static u8 g_gpe_regs[2]; /* status registers per block (= enable registers) */
static u8 g_gpe_base[2]; /* the number of the block's first event */
static int g_ngpe;
static u8 g_gpe_kind[GPE_MAX]; /* 0: no method, 'L' level, 'E' edge */
static volatile u8 g_gpe_on[GPE_MAX]; /* armed: enabled when not in service */
static volatile u32 g_gpe_pending[GPE_MAX / 32];
static volatile u32 g_fixed_pending;
static u16 g_fixed_armed; /* PM1 enable bits this file set */
static int g_sci_irq = -1;
static int g_ready;

/* /sys/firmware/acpi/interrupts: one count per source, as Linux keeps them. */
static u64 g_cnt_gpe[GPE_MAX];
static u64 g_cnt_sci, g_cnt_sci_not, g_cnt_gpe_all, g_cnt_error;
static u64 g_cnt_ff_tmr, g_cnt_ff_gbl, g_cnt_ff_pwr, g_cnt_ff_slp, g_cnt_ff_rtc;

static u16 pm1_sts(u16 blk) { return blk ? inw(blk) : 0; }
static u16 pm1_en_port(u16 blk) { return (u16)(blk + g_pm1_len / 2); }

static u16 pm1_read_en(void) {
  u16 en = 0;

  if (g_pm1a_evt)
    en |= inw(pm1_en_port(g_pm1a_evt));
  if (g_pm1b_evt)
    en |= inw(pm1_en_port(g_pm1b_evt));
  return en;
}

static void pm1_write_en(u16 en) {
  if (g_pm1a_evt)
    outw(pm1_en_port(g_pm1a_evt), en);
  if (g_pm1b_evt)
    outw(pm1_en_port(g_pm1b_evt), en);
}

static void pm1_clear(u16 bits) {
  if (g_pm1a_evt)
    outw(g_pm1a_evt, bits);
  if (g_pm1b_evt)
    outw(g_pm1b_evt, bits);
}

/* Where event `n` lives: its block, its register and its bit. */
static int gpe_locate(int n, u16 *sts, u16 *en, u8 *bit) {
  for (int b = 0; b < 2; b++) {
    int first = g_gpe_base[b];
    int count = g_gpe_regs[b] * 8;

    if (!g_gpe_blk[b] || n < first || n >= first + count)
      continue;
    *sts = (u16)(g_gpe_blk[b] + (n - first) / 8);
    *en = (u16)(g_gpe_blk[b] + g_gpe_regs[b] + (n - first) / 8);
    *bit = (u8)(1u << ((n - first) % 8));
    return 0;
  }
  return -1;
}

/* Read-modify-write of one enable bit. The SCI handler and kacpid both touch
 * the enable registers, so under a lock the handler may take. */
static spinlock_t g_gpe_lock = SPINLOCK_INIT;

static void gpe_set_enabled(int n, int on) {
  u16 sts, en;
  u8 bit;
  u64 flags;

  if (gpe_locate(n, &sts, &en, &bit) < 0)
    return;
  spin_lock_irqsave(&g_gpe_lock, &flags);
  u8 v = inb(en);
  outb(en, on ? (u8)(v | bit) : (u8)(v & ~bit));
  spin_unlock_irqrestore(&g_gpe_lock, flags);
}

static void gpe_clear(int n) {
  u16 sts, en;
  u8 bit;

  if (gpe_locate(n, &sts, &en, &bit) == 0)
    outb(sts, bit);
}

static void gpe_status(int n, int *enabled, int *pending) {
  u16 sts, en;
  u8 bit;

  *enabled = *pending = 0;
  if (gpe_locate(n, &sts, &en, &bit) < 0)
    return;
  *enabled = (inb(en) & bit) != 0;
  *pending = (inb(sts) & bit) != 0;
}

/* ── the interrupt ─────────────────────────────────────────────────────── */

static int acpi_sci_irq(void *ctx) {
  int handled = 0;
  u16 fired;

  (void)ctx;
  fired = (u16)((pm1_sts(g_pm1a_evt) | pm1_sts(g_pm1b_evt)) & pm1_read_en() &
                (PM1_TMR | PM1_GBL | PM1_PWRBTN | PM1_SLPBTN | PM1_RTC));
  if (fired) {
    pm1_clear(fired);
    if (fired & PM1_TMR)
      g_cnt_ff_tmr++;
    if (fired & PM1_GBL)
      g_cnt_ff_gbl++;
    if (fired & PM1_RTC)
      g_cnt_ff_rtc++;
    if (fired & PM1_PWRBTN)
      g_cnt_ff_pwr++;
    if (fired & PM1_SLPBTN)
      g_cnt_ff_slp++;
    __atomic_fetch_or(&g_fixed_pending, fired & (PM1_PWRBTN | PM1_SLPBTN),
                      __ATOMIC_RELEASE);
    if (fired & (PM1_PWRBTN | PM1_SLPBTN))
      suspend_wake_event("acpi-button");
    handled = 1;
  }

  for (int b = 0; b < 2; b++) {
    for (int r = 0; g_gpe_blk[b] && r < g_gpe_regs[b]; r++) {
      u16 sts = (u16)(g_gpe_blk[b] + r);
      u16 en = (u16)(g_gpe_blk[b] + g_gpe_regs[b] + r);
      u8 hit;

      spin_lock(&g_gpe_lock);
      hit = (u8)(inb(sts) & inb(en));
      if (hit)
        outb(en, (u8)(inb(en) & ~hit)); /* masked until serviced */
      spin_unlock(&g_gpe_lock);
      for (int k = 0; hit && k < 8; k++) {
        int n;

        if (!(hit & (1u << k)))
          continue;
        n = g_gpe_base[b] + r * 8 + k;
        if (n >= GPE_MAX)
          continue;
        g_cnt_gpe[n]++;
        g_cnt_gpe_all++;
        if (g_gpe_kind[n] == 'E')
          outb(sts, (u8)(1u << k)); /* an edge is cleared before its method */
        __atomic_fetch_or(&g_gpe_pending[n / 32], 1u << (n % 32),
                          __ATOMIC_RELEASE);
        handled = 1;
      }
    }
  }

  if (handled) {
    g_cnt_sci++;
    scheduler_wake_all(&g_kacpid_chan);
  } else {
    g_cnt_sci_not++;
  }
  return handled;
}

/* ── the devices Notify() can name ─────────────────────────────────────── */

enum acpi_dev_kind {
  AK_PWRBTN = 1,
  AK_SLPBTN,
  AK_LID,
  AK_BATTERY,
  AK_AC,
  AK_THERMAL,
};

#define ACPI_DEV_MAX 16

struct acpi_dev {
  char path[NOTIFY_PATH];
  int kind;
  int index; /* battery / adapter / zone number */
};

static struct acpi_dev g_devs[ACPI_DEV_MAX];
static int g_ndevs;

static void dev_add(const char *path, int kind, int index) {
  if (g_ndevs >= ACPI_DEV_MAX)
    return;
  strncpy(g_devs[g_ndevs].path, path, NOTIFY_PATH - 1);
  g_devs[g_ndevs].kind = kind;
  g_devs[g_ndevs].index = index;
  g_ndevs++;
}

/* A _HID or _CID as the firmware wrote it: a compressed EISA id (an
 * integer) or a string. */
static u32 eisa_id(const char *s) {
  u32 v = 0;

  if (strlen(s) != 7)
    return 0;
  v |= (u32)((s[0] - 0x40) & 0x1f) << 26;
  v |= (u32)((s[1] - 0x40) & 0x1f) << 21;
  v |= (u32)((s[2] - 0x40) & 0x1f) << 16;
  for (int i = 3; i < 7; i++) {
    char c = s[i];
    u32 d = (c >= '0' && c <= '9') ? (u32)(c - '0')
            : (c >= 'A' && c <= 'F') ? (u32)(c - 'A' + 10)
                                     : 0;
    v |= d << (4 * (6 - i));
  }
  /* The firmware stores it byte-swapped. */
  return ((v & 0xff) << 24) | ((v & 0xff00) << 8) | ((v >> 8) & 0xff00) |
         (v >> 24);
}

static int id_matches(const char *base, const char *leaf, const char *want) {
  char p[NOTIFY_PATH + 8];
  struct aml_result r;

  snprintf(p, sizeof(p), "%s.%s", base, leaf);
  if (aml_evaluate(p, 0, 0, &r) != AML_OK)
    return 0;
  if (r.type == AML_T_INTEGER)
    return eisa_id(want) != 0 && (u32)r.integer == eisa_id(want);
  if (r.type == AML_T_STRING) {
    usize n = strlen(want);

    return r.bytes_copied == n && memcmp(r.bytes, want, n) == 0;
  }
  return 0;
}

static int hid_is(const char *path, const char *want) {
  return id_matches(path, "_HID", want) || id_matches(path, "_CID", want);
}

struct scan_ctx {
  char (*paths)[NOTIFY_PATH];
  int n, max;
};

static void scan_devices(void *ctx, const char *path, int type) {
  struct scan_ctx *sc = (struct scan_ctx *)ctx;

  if (type != AML_T_DEVICE || sc->n >= sc->max || strlen(path) >= NOTIFY_PATH)
    return;
  strcpy(sc->paths[sc->n++], path);
}

/* ── what an event does ────────────────────────────────────────────────── */

static void button_press(int dev, u16 key) {
  input_event_push(dev, B1NIX_EV_KEY, key, 1);
  input_event_sync(dev);
  input_event_push(dev, B1NIX_EV_KEY, key, 0);
  input_event_sync(dev);
}

/* _LID: 0 closed, anything else open. -1 when the firmware will not say. */
static int lid_closed(const char *path) {
  char p[NOTIFY_PATH + 8];
  struct aml_result r;

  snprintf(p, sizeof(p), "%s._LID", path);
  if (aml_evaluate(p, 0, 0, &r) != AML_OK || r.type != AML_T_INTEGER)
    return -1;
  return r.integer == 0;
}

/* A power_supply or thermal zone whose state moved: the same "change" uevent
 * Linux's battery, ac and thermal drivers send, so upower and friends re-read
 * the files rather than polling them. */
static void class_changed(const char *cls, const char *name) {
  char devpath[64];

  snprintf(devpath, sizeof(devpath), "/class/%s/%s", cls, name);
  uevent_post("change", devpath, cls, 0, 0, -1, -1);
}

/* Notify(slot, device check) on a device under a PCI root bridge: what
 * ACPI PCI hotplug says when a card has been plugged in (QEMU's _E01 on the
 * pc machine, a laptop's dock). The slot is the device's _ADR, and what is new
 * in it is published and announced. Bus check (0) means the same for our one
 * bus. */
static int pci_slot_notify(const char *path, u64 value) {
  char parent[NOTIFY_PATH], p[NOTIFY_PATH + 8];
  const char *dot = strrchr(path, '.');
  struct aml_result r;
  int added;

  if (value > 1 || !dot || (usize)(dot - path) >= sizeof(parent))
    return 0;
  memcpy(parent, path, (usize)(dot - path));
  parent[dot - path] = '\0';
  if (!hid_is(parent, "PNP0A03") && !hid_is(parent, "PNP0A08"))
    return 0;
  snprintf(p, sizeof(p), "%s._ADR", path);
  if (aml_evaluate(p, 0, 0, &r) != AML_OK || r.type != AML_T_INTEGER)
    return 0;
  added = pci_hotplug_slot(0, (u8)((r.integer >> 16) & 0x1f));
  k_info("acpi", "%s: %s check, %d new PCI function(s)", path,
         value ? "device" : "bus", added);
  return 1;
}

static void notify_dispatch(const struct notify_rec *n) {
  for (int i = 0; i < g_ndevs; i++) {
    const struct acpi_dev *d = &g_devs[i];
    char name[16];

    if (strcmp(d->path, n->path) != 0)
      continue;
    switch (d->kind) {
    case AK_PWRBTN:
      if (n->value == 0x80)
        button_press(INPUT_DEV_PWRBTN, B1NIX_KEY_POWER);
      return;
    case AK_SLPBTN:
      if (n->value == 0x80)
        button_press(INPUT_DEV_SLPBTN, B1NIX_KEY_SLEEP);
      return;
    case AK_LID: {
      int closed = lid_closed(d->path);

      if (closed >= 0) {
        input_event_push(INPUT_DEV_LID, B1NIX_EV_SW, B1NIX_SW_LID, closed);
        input_event_sync(INPUT_DEV_LID);
      }
      return;
    }
    case AK_BATTERY:
      snprintf(name, sizeof(name), "BAT%d", d->index);
      class_changed("power_supply", name);
      return;
    case AK_AC:
      class_changed("power_supply", "AC0");
      return;
    case AK_THERMAL:
      snprintf(name, sizeof(name), "thermal_zone%d", d->index);
      class_changed("thermal", name);
      thermal_notify(d->index, n->value);
      return;
    default:
      return;
    }
  }
  /* 0x80 on a processor is "the performance states changed": _PPC moved. */
  if (n->value == 0x80 && cpufreq_is_pss_node(n->path)) {
    cpufreq_ppc_changed();
    return;
  }
  if (pci_slot_notify(n->path, n->value))
    return;
  k_info("acpi", "Notify(%s, 0x%llx) for no device this kernel drives",
         n->path, (unsigned long long)n->value);
}

/* ── kacpid ────────────────────────────────────────────────────────────── */

static int kacpid_has_work(void) {
  if (__atomic_load_n(&g_fixed_pending, __ATOMIC_ACQUIRE))
    return 1;
  for (int i = 0; i < GPE_MAX / 32; i++)
    if (__atomic_load_n(&g_gpe_pending[i], __ATOMIC_ACQUIRE))
      return 1;
  return __atomic_load_n(&g_nq_head, __ATOMIC_ACQUIRE) !=
         __atomic_load_n(&g_nq_tail, __ATOMIC_ACQUIRE);
}

static void run_gpe(int n) {
  char method[16];
  struct aml_result r;

  snprintf(method, sizeof(method), "\\_GPE._%c%02X", g_gpe_kind[n], n);
  if (aml_evaluate(method, 0, 0, &r) != AML_OK)
    g_cnt_error++;
  /* A level event is cleared once its method has dealt with the source;
   * cleared before, it would only be raised again at once. */
  if (g_gpe_kind[n] == 'L')
    gpe_clear(n);
  if (g_gpe_on[n])
    gpe_set_enabled(n, 1);
}

static void kacpid(void *arg) {
  (void)arg;
  __atomic_store_n(&g_kacpid_running, 1, __ATOMIC_RELEASE);
  for (;;) {
    struct notify_rec nr;
    u32 fixed;

    scheduler_wait_prepare(&g_kacpid_chan);
    if (!kacpid_has_work()) {
      scheduler_wait_commit();
      continue;
    }
    scheduler_wait_cancel();

    fixed = __atomic_exchange_n(&g_fixed_pending, 0, __ATOMIC_ACQ_REL);
    if (fixed & PM1_PWRBTN)
      button_press(INPUT_DEV_PWRBTN, B1NIX_KEY_POWER);
    if (fixed & PM1_SLPBTN)
      button_press(INPUT_DEV_SLPBTN, B1NIX_KEY_SLEEP);

    for (int w = 0; w < GPE_MAX / 32; w++) {
      u32 bits = __atomic_exchange_n(&g_gpe_pending[w], 0, __ATOMIC_ACQ_REL);

      for (int k = 0; bits && k < 32; k++)
        if (bits & (1u << k))
          run_gpe(w * 32 + k);
    }
    while (notify_pop(&nr))
      notify_dispatch(&nr);
  }
}

/* ── /sys/firmware/acpi/interrupts ─────────────────────────────────────── */

#define CTR_GPE(n) (n)
#define CTR_FF_TMR (GPE_MAX + 0)
#define CTR_FF_GBL (GPE_MAX + 1)
#define CTR_FF_PWR (GPE_MAX + 2)
#define CTR_FF_SLP (GPE_MAX + 3)
#define CTR_FF_RTC (GPE_MAX + 4)
#define CTR_SCI (GPE_MAX + 5)
#define CTR_SCI_NOT (GPE_MAX + 6)
#define CTR_GPE_ALL (GPE_MAX + 7)
#define CTR_ERROR (GPE_MAX + 8)

static u64 *counter_of(int id) {
  switch (id) {
  case CTR_FF_TMR: return &g_cnt_ff_tmr;
  case CTR_FF_GBL: return &g_cnt_ff_gbl;
  case CTR_FF_PWR: return &g_cnt_ff_pwr;
  case CTR_FF_SLP: return &g_cnt_ff_slp;
  case CTR_FF_RTC: return &g_cnt_ff_rtc;
  case CTR_SCI: return &g_cnt_sci;
  case CTR_SCI_NOT: return &g_cnt_sci_not;
  case CTR_GPE_ALL: return &g_cnt_gpe_all;
  case CTR_ERROR: return &g_cnt_error;
  default: return id < GPE_MAX ? &g_cnt_gpe[id] : 0;
  }
}

static u16 fixed_bit(int id) {
  switch (id) {
  case CTR_FF_TMR: return PM1_TMR;
  case CTR_FF_GBL: return PM1_GBL;
  case CTR_FF_PWR: return PM1_PWRBTN;
  case CTR_FF_SLP: return PM1_SLPBTN;
  case CTR_FF_RTC: return PM1_RTC;
  default: return 0;
  }
}

/* The layout of Linux's counter_show: the count, then for an event the
 * EN/STS columns and whether it has a handler and is enabled. */
static isize counter_show(void *ctx, char *buf, usize cap) {
  int id = (int)(usize)ctx;
  u64 *c = counter_of(id);
  int n = snprintf(buf, cap, "%8lu", (unsigned long)(c ? *c : 0));
  int enabled = 0, pending = 0, handler = 0;

  if (id < GPE_MAX) {
    gpe_status(id, &enabled, &pending);
    handler = g_gpe_kind[id] != 0;
    enabled = enabled || (handler && g_gpe_on[id]);
  } else if (fixed_bit(id)) {
    u16 bit = fixed_bit(id);

    enabled = (pm1_read_en() & bit) != 0;
    pending = ((pm1_sts(g_pm1a_evt) | pm1_sts(g_pm1b_evt)) & bit) != 0;
    handler = (g_fixed_armed & bit) != 0;
  } else {
    n += snprintf(buf + n, cap > (usize)n ? cap - (usize)n : 0, "\n");
    return n;
  }
  n += snprintf(buf + n, cap > (usize)n ? cap - (usize)n : 0, "%s%s%s%s\n",
                enabled ? "  EN" : "    ", pending ? " STS" : "    ",
                !handler ? " invalid     "
                : enabled ? " enabled     "
                          : " disabled    ",
                " unmasked");
  return n;
}

/* "disable", "enable" and "clear", as Linux's counter_set takes them. */
static isize counter_store(void *ctx, const char *buf, usize len) {
  int id = (int)(usize)ctx;
  int on;

  if (len >= 7 && strncmp(buf, "disable", 7) == 0)
    on = 0;
  else if (len >= 6 && strncmp(buf, "enable", 6) == 0)
    on = 1;
  else if (len >= 5 && strncmp(buf, "clear", 5) == 0)
    on = -1;
  else
    return -EINVAL;
  if (id < GPE_MAX) {
    if (!g_gpe_kind[id])
      return -EINVAL;
    if (on < 0) {
      gpe_clear(id);
    } else {
      g_gpe_on[id] = (u8)on;
      gpe_set_enabled(id, on);
    }
  } else if (fixed_bit(id)) {
    u16 bit = fixed_bit(id);

    if (on < 0)
      pm1_clear(bit);
    else
      pm1_write_en(on ? (u16)(pm1_read_en() | bit)
                      : (u16)(pm1_read_en() & ~bit));
  } else {
    return -EINVAL;
  }
  return (isize)len;
}

static void publish_counters(void) {
  struct sysfs_dir *d = sysfs_reg_dir(
      sysfs_reg_dir(sysfs_reg_dir(0, "firmware"), "acpi"), "interrupts");
  static const struct { const char *name; int id; } fixed[] = {
      {"ff_pmtimer", CTR_FF_TMR}, {"ff_gbl_lock", CTR_FF_GBL},
      {"ff_pwr_btn", CTR_FF_PWR}, {"ff_slp_btn", CTR_FF_SLP},
      {"ff_rt_clk", CTR_FF_RTC},  {"sci", CTR_SCI},
      {"sci_not", CTR_SCI_NOT},   {"gpe_all", CTR_GPE_ALL},
      {"error", CTR_ERROR},
  };

  if (!d)
    return;
  for (int n = 0; n < g_ngpe && n < GPE_MAX; n++) {
    char name[8];

    snprintf(name, sizeof(name), "gpe%02X", n);
    sysfs_reg_attr(d, name, 0644, counter_show, counter_store,
                   (void *)(usize)CTR_GPE(n), 0);
  }
  for (usize i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++)
    sysfs_reg_attr(d, fixed[i].name, 0644, counter_show,
                   fixed_bit(fixed[i].id) ? counter_store : 0,
                   (void *)(usize)fixed[i].id, 0);
}

/* ── bring-up ──────────────────────────────────────────────────────────── */

/* Leave legacy (SMI) mode if the firmware did not: without SCI_EN nothing in
 * PM1 or the GPE blocks is delivered as an SCI. */
static void acpi_enable_mode(void) {
  u16 smi_cmd = (u16)acpi_fadt_read(FADT_OFF_SMI_CMD, 4);
  u8 enable = (u8)acpi_fadt_read(FADT_OFF_ACPI_ENABLE, 1);
  u16 cnt = acpi_pm1a_cnt_port();

  if (!cnt || (inw(cnt) & PM1_CNT_SCI_EN) || !smi_cmd || !enable)
    return;
  outb(smi_cmd, enable);
  for (int i = 0; i < 1000000 && !(inw(cnt) & PM1_CNT_SCI_EN); i++)
    ;
}

/* Everything off and acknowledged, then on again for what has a handler:
 * the state after a boot, and again after an S3 resets the chipset. */
static void arm_events(void) {
  u64 flags;

  acpi_enable_mode();
  pm1_write_en(0);
  pm1_clear(0xffff);
  spin_lock_irqsave(&g_gpe_lock, &flags);
  for (int b = 0; b < 2; b++)
    for (int r = 0; g_gpe_blk[b] && r < g_gpe_regs[b]; r++) {
      outb((u16)(g_gpe_blk[b] + g_gpe_regs[b] + r), 0);
      outb((u16)(g_gpe_blk[b] + r), 0xff);
    }
  spin_unlock_irqrestore(&g_gpe_lock, flags);
  for (int n = 0; n < g_ngpe && n < GPE_MAX; n++)
    if (g_gpe_on[n])
      gpe_set_enabled(n, 1);
  pm1_write_en(g_fixed_armed);
}

static int acpi_event_resume(void *ctx) {
  (void)ctx;
  if (g_ready)
    arm_events();
  return 0;
}

static int buttons_armed(void *ctx) {
  (void)ctx;
  return (g_fixed_armed & (PM1_PWRBTN | PM1_SLPBTN)) != 0;
}

static void find_devices(void) {
  struct scan_ctx sc;

  sc.max = 256;
  sc.n = 0;
  sc.paths = kzalloc((usize)sc.max * NOTIFY_PATH);
  if (!sc.paths)
    return;
  aml_walk(scan_devices, &sc);
  for (int i = 0; i < sc.n; i++) {
    const char *p = sc.paths[i];

    if (hid_is(p, "PNP0C0C")) {
      dev_add(p, AK_PWRBTN, 0);
      input_register_optional(INPUT_DEV_PWRBTN, 0);
    } else if (hid_is(p, "PNP0C0E")) {
      dev_add(p, AK_SLPBTN, 0);
      input_register_optional(INPUT_DEV_SLPBTN, 0);
    } else if (hid_is(p, "PNP0C0D")) {
      int closed = lid_closed(p);

      dev_add(p, AK_LID, 0);
      input_register_optional(INPUT_DEV_LID, closed > 0);
    }
  }
  kfree(sc.paths);
  /* Batteries, adapters and zones are the ones acpi_power already found. */
  for (int i = 0; i < acpi_power_battery_count(); i++)
    dev_add(acpi_power_battery_path(i), AK_BATTERY, i);
  for (int i = 0; i < acpi_power_ac_count(); i++)
    dev_add(acpi_power_ac_path(i), AK_AC, i);
  for (int i = 0; i < acpi_power_thermal_count(); i++)
    dev_add(acpi_power_thermal_path(i), AK_THERMAL, i);
}

void acpi_event_init(void) {
  u32 fflags;

  if (g_ready || !acpi_find_table("FACP") || !aml_ready())
    return;
  fflags = (u32)acpi_fadt_read(FADT_OFF_FLAGS, 4);
  if (fflags & FADT_F_HW_REDUCED)
    return;
  g_sci_irq = (int)acpi_fadt_read(FADT_OFF_SCI_INT, 2);
  g_pm1a_evt = (u16)acpi_fadt_read(FADT_OFF_PM1A_EVT, 4);
  g_pm1b_evt = (u16)acpi_fadt_read(FADT_OFF_PM1B_EVT, 4);
  g_pm1_len = (u8)acpi_fadt_read(FADT_OFF_PM1_EVT_LEN, 1);
  if (!g_pm1a_evt || g_pm1_len < 4 || g_sci_irq <= 0 || g_sci_irq > 15)
    return;
  g_gpe_blk[0] = (u16)acpi_fadt_read(FADT_OFF_GPE0_BLK, 4);
  g_gpe_regs[0] = (u8)(acpi_fadt_read(FADT_OFF_GPE0_LEN, 1) / 2);
  g_gpe_blk[1] = (u16)acpi_fadt_read(FADT_OFF_GPE1_BLK, 4);
  g_gpe_regs[1] = (u8)(acpi_fadt_read(FADT_OFF_GPE1_LEN, 1) / 2);
  g_gpe_base[1] = (u8)acpi_fadt_read(FADT_OFF_GPE1_BASE, 1);
  g_ngpe = g_gpe_blk[0] ? g_gpe_regs[0] * 8 : 0;
  if (g_gpe_blk[1] && g_gpe_base[1] + g_gpe_regs[1] * 8 > g_ngpe)
    g_ngpe = g_gpe_base[1] + g_gpe_regs[1] * 8;
  if (g_ngpe > GPE_MAX)
    g_ngpe = GPE_MAX;

  /* A GPE is serviced when the firmware wrote a method for it. */
  for (int n = 0; n < g_ngpe; n++) {
    char m[16];

    snprintf(m, sizeof(m), "\\_GPE._L%02X", n);
    if (aml_exists(m)) {
      g_gpe_kind[n] = 'L';
    } else {
      snprintf(m, sizeof(m), "\\_GPE._E%02X", n);
      if (aml_exists(m))
        g_gpe_kind[n] = 'E';
    }
    g_gpe_on[n] = g_gpe_kind[n] != 0;
  }

  /* The fixed buttons, when the FADT says they are there. */
  if (!(fflags & FADT_F_PWR_BUTTON)) {
    g_fixed_armed |= PM1_PWRBTN;
    input_register_optional(INPUT_DEV_PWRBTN, 0);
  }
  if (!(fflags & FADT_F_SLP_BUTTON)) {
    g_fixed_armed |= PM1_SLPBTN;
    input_register_optional(INPUT_DEV_SLPBTN, 0);
  }
  find_devices();

  if (kthread_create("kacpid", kacpid, 0) < 0) {
    k_warn("acpi", "no kacpid thread; ACPI events stay off");
    return;
  }
  irq_register_handler((u32)g_sci_irq, acpi_sci_irq, 0);
  g_ready = 1;
  arm_events();
  irq_unmask((u32)g_sci_irq);
  suspend_register_device("acpi-events", acpi_event_resume, 0);
  suspend_register_wake_source_flags("acpi-button", buttons_armed, 0,
                                     SUSPEND_WAKE_EVENTS);
  publish_counters();
  {
    int armed = 0;

    for (int n = 0; n < g_ngpe; n++)
      armed += g_gpe_on[n] ? 1 : 0;
    k_info("acpi", "SCI on IRQ %d: %d GPEs with methods,%s%s %d devices",
           g_sci_irq, armed,
           (g_fixed_armed & PM1_PWRBTN) ? " power button," : "",
           (g_fixed_armed & PM1_SLPBTN) ? " sleep button," : "", g_ndevs);
  }
}

/* ── power-off ─────────────────────────────────────────────────────────── */

void acpi_poweroff(void) {
  struct aml_result r;
  u64 arg = 5;
  u16 a = acpi_pm1a_cnt_port(), b = acpi_pm1b_cnt_port();
  u8 typa, typb;

  if (!a || !aml_ready() || aml_evaluate("\\_S5_", 0, 0, &r) != AML_OK ||
      r.type != AML_T_PACKAGE || r.elems < 1)
    return;
  typa = (u8)(r.elem_int[0] & 7);
  typb = (u8)(r.elems > 1 ? (r.elem_int[1] & 7) : 0);
  /* Prepare To Sleep: the firmware's own last word before the state. */
  if (aml_exists("\\_PTS"))
    (void)aml_evaluate("\\_PTS", &arg, 1, &r);
  interrupts_disable();
  if (g_ready) {
    pm1_write_en(0);
    for (int n = 0; n < g_ngpe; n++)
      gpe_set_enabled(n, 0);
  }
  /* SLP_TYP first, then SLP_EN, as ACPICA writes it. */
  outw(a, (u16)((inw(a) & ~(7u << PM1_CNT_SLP_TYP_SHIFT)) |
                ((u16)typa << PM1_CNT_SLP_TYP_SHIFT)));
  if (b)
    outw(b, (u16)((inw(b) & ~(7u << PM1_CNT_SLP_TYP_SHIFT)) |
                  ((u16)typb << PM1_CNT_SLP_TYP_SHIFT)));
  outw(a, (u16)(inw(a) | PM1_CNT_SLP_EN));
  if (b)
    outw(b, (u16)(inw(b) | PM1_CNT_SLP_EN));
  for (int i = 0; i < 10000000; i++)
    __asm__ volatile("pause");
}

#else /* !__x86_64__ */

void acpi_event_init(void) {}
void acpi_poweroff(void) {}

#endif
