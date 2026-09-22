/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ACPI S3 — suspend to RAM (M129).
 *
 * The difference between this and the suspend-to-idle beside it is that the
 * processor stops existing. S3 is the firmware's state: the OS writes the
 * address of real-mode code into the FACS, writes the sleep type the firmware
 * declared for `\_S3` into PM1_CNT, and the platform removes power from
 * everything but memory. On wake the firmware hands back a processor at its
 * power-on state — real mode, no paging, caches off — and jumps to that
 * address. So three things have to be true or the machine never comes back:
 *
 *   1. the wake-up code is where the firmware will look for it, below 1 MiB and
 *      written before the sleep (kernel/arch/x86_64/s3_wakeup.S);
 *   2. everything the processor was holding is in memory (the register frame
 *      pushed by x86_s3_sleep_and_wake, the control registers and MSRs saved
 *      here); and
 *   3. the devices are re-initialised on the way back, because the LAPIC, the
 *      IOAPIC and the timers come back at their reset state and an interrupt
 *      that is not routed is a machine that answers nothing.
 *
 * What this file does NOT do is decide when to sleep, which tasks to freeze or
 * how long to wait: that is kernel/dev/suspend.c, which calls in here once the
 * machine is quiet.
 */

#include <b1nix/acpi.h>
#include <b1nix/aml.h>
#include <b1nix/arch.h>
#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/io.h>
#include <b1nix/ioapic.h>
#include <b1nix/lapic.h>
#include <b1nix/ktime.h>
#include <b1nix/mm.h>
#include <b1nix/rtc.h>
#include <b1nix/suspend.h>
#include <b1nix/types.h>

#include <string.h>

#include "s3_wakeup.inc"
#include "s3_wakeup_offsets.h"

/* Where the blob is linked and where it is copied. Both pages below 1 MiB are
 * outside the frame allocator (see the note in pmm.c); 0x8000 belongs to the AP
 * trampoline, so this takes the next one. */
#define S3_WAKE_PHYS 0x9000ULL
#define S3_WAKE_MAGIC 0x53335741414B45ULL /* must match s3_wakeup.S */

/* PM1 control: the sleep type goes in bits 12:10, and bit 13 is the write that
 * enters it. PM1 status bit 15 is WAK_STS, which the firmware sets on the way
 * back out. */
#define PM1_CNT_SLP_TYP_SHIFT 10
#define PM1_CNT_SLP_EN (1u << 13)
#define PM1_STS_WAK_STS (1u << 15)

/* The PM1 status block sits immediately below the control block in every
 * chipset this runs on, and the FADT names it separately; read it from the
 * FADT rather than assuming. */
#define FADT_OFF_SMI_CMD     48
#define FADT_OFF_ACPI_ENABLE 52
#define FADT_OFF_PM1A_EVT    56
#define FADT_OFF_PM1B_EVT    60
#define FADT_OFF_PM1_EVT_LEN 88

/* PM1 enable bits (ACPI 4.8.3.1.2). RTC_EN is the one that matters here: it is
 * what tells the platform to treat the RTC alarm as a wake event, and without
 * it an S3 entered with the alarm armed is a machine that never comes back —
 * the alarm fires into a processor that is powered off and nothing in the
 * chipset is listening. */
#define PM1_EN_RTC (1u << 10)
/* PM1 control bit 0: the platform delivers ACPI events rather than SMIs. Set by
 * firmware on most machines; written here when it is not, because a platform
 * still in legacy mode answers no PM1 event at all. */
#define PM1_CNT_SCI_EN (1u << 0)

struct s3_cpu_state {
  u16 idt_limit;
  u64 idt_base;
  u64 gdt_base;
  u64 cr0, cr3, cr4, efer;
  u64 fs_base, gs_base, kernel_gs_base;
  u64 star, lstar, cstar, sfmask, pat;
  u64 xcr0;
  int have_xcr0;
  u16 tr;
};

static struct s3_cpu_state g_saved;
/* What the clocks read on the way down: the processor's counter, which stops
 * with the processor, and the hardware clock, which does not. The resume needs
 * both before it can let anything read the time. */
static u64 g_mono_before_ns;
static u64 g_wall_before_s;
static u64 g_resume_stack_slot;
static int g_blob_ready;
static u64 g_s3_count;
static u64 g_s3_last_ms;

int x86_s3_sleep_and_wake(u32 pm1a_port, u32 pm1b_port, u32 val_a, u32 val_b,
                          u64 *stack_slot);
void x86_s3_resume_point(void);

/* interrupts.c / arch.c / memtype.c, all of them the per-CPU set-up the boot
 * path runs once. A resume has to run them again, on a processor that has
 * forgotten everything but its memory. */
int paging_la57(void);
void x86_idt_load(void);
void x86_pic_init(void);
void x86_timer_init(void);
void pat_init_cpu(void);

static u64 rdmsr_raw(u32 msr) {
  u32 lo, hi;

  __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
  return ((u64)hi << 32) | lo;
}

static void wrmsr_raw(u32 msr, u64 v) {
  __asm__ volatile("wrmsr" : : "a"((u32)v), "d"((u32)(v >> 32)), "c"(msr));
}

/* One field out of the FADT, by offset and width. acpi.c has the same reader
 * for the three fields it publishes; the sleep path needs several more, and
 * naming each of them in a header nobody else uses would be worse than reading
 * the table here. */
static u64 fadt_read(u32 off, int width) {
  const struct acpi_sdt_header *fadt = acpi_find_table("FACP");
  const u8 *b = (const u8 *)fadt;
  u64 v = 0;

  if (!fadt || off + (u32)width > fadt->length)
    return 0;
  for (int i = 0; i < width; i++)
    v |= (u64)b[off + i] << (i * 8);
  return v;
}

static u8 fadt_u8(u32 off) { return (u8)fadt_read(off, 1); }

/* The enable half of the PM1 event block: the block is PM1_EVT_LEN bytes and
 * the specification splits it down the middle, status first. */
static u16 pm1a_en_port(void) {
  u16 evt = (u16)fadt_read(FADT_OFF_PM1A_EVT, 4);
  u8 len = fadt_u8(FADT_OFF_PM1_EVT_LEN);

  if (!evt)
    return 0;
  return (u16)(evt + (len ? len / 2 : 2));
}

static u16 pm1b_en_port(void) {
  u16 evt = (u16)fadt_read(FADT_OFF_PM1B_EVT, 4);
  u8 len = fadt_u8(FADT_OFF_PM1_EVT_LEN);

  if (!evt)
    return 0;
  return (u16)(evt + (len ? len / 2 : 2));
}

/* Put the platform into ACPI mode if the firmware left it in legacy mode: a
 * machine still delivering SMIs answers no PM1 event, so neither the sleep nor
 * the wake would work. Nothing to do on a platform with no SMI command port,
 * which is what a firmware that boots straight into ACPI mode declares. */
static void s3_enable_acpi_mode(void) {
  u16 smi_cmd = (u16)fadt_read(FADT_OFF_SMI_CMD, 4);
  u8 enable = fadt_u8(FADT_OFF_ACPI_ENABLE);
  u16 cnt = acpi_pm1a_cnt_port();

  if (!cnt || (inw(cnt) & PM1_CNT_SCI_EN))
    return;
  if (!smi_cmd || !enable) {
    console_write("s3: the platform is in legacy mode and declares no way out"
                  " of it\n");
    return;
  }
  outb(smi_cmd, enable);
  for (int i = 0; i < 1000000; i++) {
    if (inw(cnt) & PM1_CNT_SCI_EN)
      return;
  }
  console_write("s3: SCI_EN never came up after the ACPI enable\n");
}

/* The event block's first half is the status register. */
static u16 pm1a_sts_port(void) {
  return (u16)fadt_read(FADT_OFF_PM1A_EVT, 4);
}

static u16 pm1b_sts_port(void) {
  return (u16)fadt_read(FADT_OFF_PM1B_EVT, 4);
}

/* The sleep type the firmware declared for a state: `\_S3` is a package whose
 * first two elements are SLP_TYPa and SLP_TYPb. A machine that declares no
 * `\_S3` does not have the state, and this is the only honest way to know. */
static int s3_sleep_type(u8 *typa, u8 *typb) {
  struct aml_result r;

  if (!aml_ready() || aml_evaluate("\\_S3_", 0, 0, &r) != AML_OK)
    return -1;
  if (r.type != AML_T_PACKAGE || r.elems < 1)
    return -1;
  *typa = (u8)(r.elem_int[0] & 0x7);
  *typb = (u8)(r.elems > 1 ? (r.elem_int[1] & 0x7) : 0);
  return 0;
}

/* Put the wake-up blob where the firmware will jump to it, and patch in the
 * three things it cannot know: the page tables to load, the stack to resume on
 * and the address to jump to once it is in long mode. */
static int s3_place_blob(void) {
  u8 *dst = (u8 *)(usize)(S3_WAKE_PHYS + DIRECT_MAP_BASE);
  u64 magic;

  memcpy(dst, s3_wakeup_bin, sizeof(s3_wakeup_bin));
  memcpy(&magic, dst + S3W_MAGIC_OFF, sizeof(magic));
  if (magic != S3_WAKE_MAGIC) {
    console_write("s3: the wake-up blob and its offsets disagree; refusing\n");
    return -1;
  }
  return 0;
}

static void s3_patch_blob(void) {
  u8 *dst = (u8 *)(usize)(S3_WAKE_PHYS + DIRECT_MAP_BASE);
  u64 cr3, entry = (u64)(usize)x86_s3_resume_point;
  u64 la57 = paging_la57() ? 1 : 0;
  struct {
    u16 limit;
    u64 base;
  } __attribute__((packed)) gdtr;

  __asm__ volatile("movq %%cr3, %0" : "=r"(cr3));
  __asm__ volatile("sgdt %0" : "=m"(gdtr));
  memcpy(dst + S3W_CR3_OFF, &cr3, sizeof(cr3));
  memcpy(dst + S3W_ENTRY_OFF, &entry, sizeof(entry));
  memcpy(dst + S3W_LA57_OFF, &la57, sizeof(la57));
  memcpy(dst + S3W_GDTR_OFF, &gdtr, sizeof(gdtr));
  g_resume_stack_slot = S3_WAKE_PHYS + DIRECT_MAP_BASE + S3W_STACK_OFF;
}

/* The FACS waking vector: a 32-bit physical address of real-mode code. The
 * 64-bit X field must be zero when the 32-bit one is used — a firmware that
 * finds both set is entitled to take either. */
static int s3_arm_waking_vector(void) {
  u64 facs_phys = acpi_facs_address();
  u8 *facs;

  if (!facs_phys || facs_phys >= DIRECT_MAP_SIZE)
    return -1;
  facs = (u8 *)(usize)(facs_phys + DIRECT_MAP_BASE);
  if (facs[0] != 'F' || facs[1] != 'A' || facs[2] != 'C' || facs[3] != 'S')
    return -1;
  {
    u32 vec = (u32)S3_WAKE_PHYS;
    u64 zero = 0;

    memcpy(facs + 12, &vec, sizeof(vec));   /* firmware_waking_vector */
    memcpy(facs + 24, &zero, sizeof(zero)); /* x_firmware_waking_vector */
  }
  return 0;
}

static void s3_disarm_waking_vector(void) {
  u64 facs_phys = acpi_facs_address();
  u8 *facs;
  u32 zero = 0;

  if (!facs_phys || facs_phys >= DIRECT_MAP_SIZE)
    return;
  facs = (u8 *)(usize)(facs_phys + DIRECT_MAP_BASE);
  if (facs[0] == 'F' && facs[1] == 'A' && facs[2] == 'C' && facs[3] == 'S')
    memcpy(facs + 12, &zero, sizeof(zero));
}

static void s3_save_cpu(void) {
  struct {
    u16 limit;
    u64 base;
  } __attribute__((packed)) idtr;

  __asm__ volatile("sidt %0" : "=m"(idtr));
  g_saved.idt_limit = idtr.limit;
  g_saved.idt_base = idtr.base;
  {
    struct {
      u16 limit;
      u64 base;
    } __attribute__((packed)) gdtr;

    __asm__ volatile("sgdt %0" : "=m"(gdtr));
    g_saved.gdt_base = gdtr.base;
  }
  __asm__ volatile("movq %%cr0, %0" : "=r"(g_saved.cr0));
  __asm__ volatile("movq %%cr3, %0" : "=r"(g_saved.cr3));
  __asm__ volatile("movq %%cr4, %0" : "=r"(g_saved.cr4));
  __asm__ volatile("str %0" : "=m"(g_saved.tr));
  g_saved.efer = rdmsr_raw(0xC0000080u);
  g_saved.fs_base = rdmsr_raw(0xC0000100u);
  g_saved.gs_base = rdmsr_raw(0xC0000101u);
  g_saved.kernel_gs_base = rdmsr_raw(0xC0000102u);
  g_saved.star = rdmsr_raw(0xC0000081u);
  g_saved.lstar = rdmsr_raw(0xC0000082u);
  g_saved.cstar = rdmsr_raw(0xC0000083u);
  g_saved.sfmask = rdmsr_raw(0xC0000084u);
  g_saved.pat = rdmsr_raw(0x277u);
  g_saved.have_xcr0 = 0;
  if (g_saved.cr4 & (1ull << 18)) { /* OSXSAVE */
    u32 lo, hi;

    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    g_saved.xcr0 = ((u64)hi << 32) | lo;
    g_saved.have_xcr0 = 1;
  }
}

/* Called from the wake-up path with interrupts off and nothing but the GDT and
 * the stack in place. Everything here has to run before anything can fault,
 * interrupt or make a system call. */
void x86_s3_restore_cpu(void) {
  struct {
    u16 limit;
    u64 base;
  } __attribute__((packed)) idtr = { g_saved.idt_limit, g_saved.idt_base };

  /* The MSRs first: the per-CPU pointer lives in GS_BASE, and every function
   * below that touches per-CPU state reads it. */
  wrmsr_raw(0xC0000100u, g_saved.fs_base);
  wrmsr_raw(0xC0000101u, g_saved.gs_base);
  wrmsr_raw(0xC0000102u, g_saved.kernel_gs_base);
  wrmsr_raw(0xC0000080u, g_saved.efer);
  wrmsr_raw(0xC0000081u, g_saved.star);
  wrmsr_raw(0xC0000082u, g_saved.lstar);
  wrmsr_raw(0xC0000083u, g_saved.cstar);
  wrmsr_raw(0xC0000084u, g_saved.sfmask);
  wrmsr_raw(0x277u, g_saved.pat);

  /* CR4 as it was — SMEP, SMAP, PKE, OSXSAVE and the rest — then the extended
   * state mask, which is only meaningful once OSXSAVE is back. */
  __asm__ volatile("movq %0, %%cr4" : : "r"(g_saved.cr4) : "memory");
  if (g_saved.have_xcr0)
    __asm__ volatile("xsetbv"
                     :
                     : "a"((u32)g_saved.xcr0), "d"((u32)(g_saved.xcr0 >> 32)),
                       "c"(0));
  /* CR0 last of the three: write protection and the FPU bits. */
  __asm__ volatile("movq %0, %%cr0" : : "r"(g_saved.cr0) : "memory");

  /* The clock, before anything else can read it. The time-stamp counter came
   * back at zero, so every duration computed from it is an underflow — a
   * monotonic clock 160 years ahead, which makes the scheduler's stall watchdog
   * shoot the machine before the resume finishes. The hardware clock kept
   * counting through the sleep and is what measures it. */
  {
    u64 wall_after = rtc_hw_unix_seconds();
    u64 away_ns = (wall_after && g_wall_before_s && wall_after > g_wall_before_s)
                      ? (wall_after - g_wall_before_s) * 1000000000ull
                      : 0;

    ktime_resume(g_mono_before_ns, away_ns);
  }

  __asm__ volatile("lidt %0" : : "m"(idtr));
  if (g_saved.tr) {
    /* The TSS descriptor in the GDT still says BUSY: the processor set that bit
     * when this TSS was loaded, the bit lives in memory rather than in the
     * processor, and it survived the sleep. `ltr` on a busy descriptor is a
     * general-protection fault — which, with no usable IDT yet, is a triple
     * fault and a machine that reboots instead of resuming. So the bit is
     * cleared first, exactly as the boot path finds it. Type 9 is an available
     * 64-bit TSS, type 11 the same one busy; the type is the low nibble of the
     * descriptor's sixth byte. */
    u8 *desc = (u8 *)(usize)(g_saved.gdt_base + (g_saved.tr & ~7u));

    desc[5] = (u8)(desc[5] & ~0x2u);
    __asm__ volatile("ltr %0" : : "m"(g_saved.tr));
  }

  /* The interrupt hardware. The LAPIC comes back software-disabled, the IOAPIC
   * with every redirection entry masked, and the 8259s at their reset state, so
   * all three are programmed again and the routes this kernel had installed are
   * replayed. */
  x86_pic_init();
  /* lapic_resume, not lapic_init: the full one re-measures the clock and
   * re-anchors the counter, which would undo the anchor computed above. */
  lapic_resume();
  (void)ioapic_resume();
  x86_timer_init();
  console_write("s3: processor and interrupt hardware restored\n");
}

int arch_s3_supported(void) {
  u8 a = 0, b = 0;

  if (!acpi_pm1a_cnt_port() || !acpi_facs_address())
    return 0;
  if (s3_sleep_type(&a, &b) != 0)
    return 0;
  return 1;
}

const char *arch_s3_why_not(void) {
  u8 a = 0, b = 0;

  if (!acpi_pm1a_cnt_port())
    return "no PM1a control register in the FADT";
  if (!acpi_facs_address())
    return "no FACS, so there is nowhere to put the waking vector";
  if (s3_sleep_type(&a, &b) != 0)
    return "the firmware declares no \\_S3";
  return "";
}

/* Enter S3. Returns 1 when the machine slept and came back, 0 when the
 * platform refused the state, and a negative errno when this kernel refused to
 * try. Called with interrupts DISABLED and userspace frozen. */
int arch_s3_enter(void) {
  u8 typa = 0, typb = 0;
  u16 cnt_a, cnt_b, sts_a, sts_b, en_a = 0, en_b = 0;
  u16 saved_en_a = 0, saved_en_b = 0;
  u32 val_a, val_b;
  int slept;

  if (!arch_s3_supported())
    return -ENODEV;
  if (s3_sleep_type(&typa, &typb) != 0)
    return -ENODEV;
  cnt_a = acpi_pm1a_cnt_port();
  cnt_b = acpi_pm1b_cnt_port();
  sts_a = pm1a_sts_port();
  sts_b = pm1b_sts_port();

  if (!g_blob_ready) {
    if (s3_place_blob() != 0)
      return -EIO;
    g_blob_ready = 1;
  }
  s3_patch_blob();
  if (s3_arm_waking_vector() != 0) {
    console_write("s3: the FACS is not reachable; refusing to sleep\n");
    return -EIO;
  }

  /* ACPI mode, or nothing below is delivered. */
  s3_enable_acpi_mode();

  /* A status bit still set from an earlier wake would make the firmware treat
   * this sleep as already over. */
  if (sts_a)
    outw(sts_a, PM1_STS_WAK_STS);
  if (sts_b)
    outw(sts_b, PM1_STS_WAK_STS);

  /* Arm the RTC as a WAKE event, not merely as an interrupt. The alarm the
   * caller set raises IRQ 8, which is enough to end an idle suspend and useless
   * once the processor is off: it is this bit that makes the chipset bring the
   * machine back. Restored to what it was on the way out, because an RTC_EN
   * left set turns every later alarm into a spurious system event. */
  en_a = pm1a_en_port();
  en_b = pm1b_en_port();
  if (en_a) {
    saved_en_a = inw(en_a);
    outw(en_a, (u16)(saved_en_a | PM1_EN_RTC));
  }
  if (en_b) {
    saved_en_b = inw(en_b);
    outw(en_b, (u16)(saved_en_b | PM1_EN_RTC));
  }

  /* `_PTS(3)`, where the firmware has it: its chance to prepare, and on some
   * platforms the only way the embedded controller learns the machine is going
   * down. A refusal is recorded, not fatal — the sleep is the platform's to
   * accept or ignore. */
  if (aml_exists("\\_PTS")) {
    u64 arg = 3;
    struct aml_result r;

    if (aml_evaluate("\\_PTS", &arg, 1, &r) != AML_OK)
      console_write("s3: _PTS refused the sleep request\n");
  }

  s3_save_cpu();
  /* The COUNTER's own reading, not the kernel clock's: it is the counter that
   * is re-anchored on the way back, and both clocks are built on it. */
  g_mono_before_ns = arch_tsc_monotonic_ns();
  g_wall_before_s = rtc_hw_unix_seconds();

  val_a = ((u32)typa << PM1_CNT_SLP_TYP_SHIFT) | PM1_CNT_SLP_EN;
  val_b = ((u32)typb << PM1_CNT_SLP_TYP_SHIFT) | PM1_CNT_SLP_EN;
  /* Whatever the register held besides the sleep type — SCI_EN above all, which
   * must stay set or the firmware stops delivering the events that wake the
   * machine. */
  val_a |= inw(cnt_a) & ~(0x7u << PM1_CNT_SLP_TYP_SHIFT);
  if (cnt_b)
    val_b |= inw(cnt_b) & ~(0x7u << PM1_CNT_SLP_TYP_SHIFT);

  /* Said before the sleep, because after it there may be no machine to say it
   * on: a sleep that does not come back leaves this line as the last thing that
   * happened, and it names the register and the value the firmware asked for. */
  console_write("s3: SLP_TYP ");
  console_write_dec(typa);
  console_write(" -> PM1a 0x");
  console_write_hex64(cnt_a);
  console_write(" value 0x");
  console_write_hex64(val_a);
  console_write("\n");

  slept = x86_s3_sleep_and_wake(cnt_a, cnt_b, val_a, val_b,
                                (u64 *)(usize)g_resume_stack_slot);

  s3_disarm_waking_vector();
  if (en_a)
    outw(en_a, saved_en_a);
  if (en_b)
    outw(en_b, saved_en_b);
  if (sts_a)
    outw(sts_a, PM1_STS_WAK_STS);
  if (sts_b)
    outw(sts_b, PM1_STS_WAK_STS);

  if (slept) {
    g_s3_count++;
    console_write("s3: back from the sleep\n");
  }
  return slept;
}

/* `_WAK(3)`: the firmware's own resume hook, and where a platform reports what
 * it did with the request.
 *
 * Deliberately NOT called from inside arch_s3_enter, where interrupts are still
 * off. The method is a program — QEMU's own spends time in Sleep() and in
 * Notify() — and running it with no timer meant a resume that took two minutes
 * to get past the firmware's hook, all of it counted against the userspace
 * write to /sys/power/state. With interrupts on, a Sleep() inside it sleeps for
 * as long as it asks and no longer. */
void arch_s3_firmware_wake(void) {
  if (!aml_exists("\\_WAK"))
    return;
  {
    u64 arg = 3;
    struct aml_result r;

    if (aml_evaluate("\\_WAK", &arg, 1, &r) != AML_OK)
      console_write("s3: the firmware's _WAK refused\n");
  }
}

u64 arch_s3_count(void) { return g_s3_count; }
void arch_s3_note_ms(u64 ms) { g_s3_last_ms = ms; }
u64 arch_s3_last_ms(void) { return g_s3_last_ms; }
