#include <b1nix/arch_x86_64.h>
#include <b1nix/arch.h>
#include <b1nix/console.h>
#include <b1nix/user.h>
#include <b1nix/bootinfo.h>
#include <b1nix/gdbstub.h>
#include <b1nix/io.h>
#include <b1nix/ioapic.h>
#include <b1nix/irq.h>
#include <b1nix/klog.h>

/* M35: ELF core dump on fatal fault (kernel/arch/x86_64/coredump.c). */
void coredump_write(struct interrupt_frame *frame, int sig);
#include <b1nix/lapic.h>
#include <b1nix/mm.h>
#include <b1nix/serial.h>
#include <b1nix/panic.h>
#include <b1nix/ptrace.h>
#include <b1nix/rseq.h>
#include <b1nix/sched.h>
#include <b1nix/vfs.h>
#include <b1nix/spinlock.h>
#include <b1nix/serial_tty.h>
#include <b1nix/watchdog.h>
#include <b1nix/net.h>
#include <b1nix/tlb.h>
#include <b1nix/types.h>
#include <stdio.h>
#include <string.h>

/* ── Page-fault profile ─────────────────────────────────────────────────────
 * How many demand-paging faults a run takes and what they cost, printed beside
 * the system-call profile. Off unless b1nix.sysprof asked for it. */
static u64 g_pf_count;
static u64 g_pf_cycles;

/* kernel/arch/x86_64/kprof.c — tick-driven kernel profile. */
void kprof_tick(u64 rip, int in_user, int in_idle, int cpu);

static inline u64 pf_prof_now(void) {
  u32 lo, hi;

  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return ((u64)hi << 32) | lo;
}

static int pf_prof_enabled(void) {
  static int on = -1;

  if (on < 0)
    on = bootinfo_has_flag("b1nix.sysprof") ? 1 : 0;
  return on;
}

static void pf_prof_account(u64 cycles) {
  __atomic_fetch_add(&g_pf_count, 1, __ATOMIC_RELAXED);
  __atomic_fetch_add(&g_pf_cycles, cycles, __ATOMIC_RELAXED);
}

void pf_prof_dump(void) {
  if (!pf_prof_enabled())
    return;
  console_write("pfprof: faults=");
  console_write_dec(__atomic_load_n(&g_pf_count, __ATOMIC_RELAXED));
  console_write(" Mcycles=");
  console_write_dec(__atomic_load_n(&g_pf_cycles, __ATOMIC_RELAXED) / 1000000);
  console_write("\n");
}


#define IDT_ENTRY_COUNT 256
#define KERNEL_CODE_SELECTOR 0x08
#define IDT_INTERRUPT_GATE 0x8e
#define PIC1_COMMAND 0x20
#define PIC1_DATA 0x21
#define PIC2_COMMAND 0xa0
#define PIC2_DATA 0xa1
#define PIC_EOI 0x20
#define PIT_CHANNEL0 0x40
#define PIT_COMMAND 0x43
#define PIT_FREQUENCY 1193182
#define TIMER_HZ 100

struct idt_entry {
  u16 offset_low;
  u16 selector;
  u8 ist;
  u8 type_attr;
  u16 offset_mid;
  u32 offset_high;
  u32 zero;
} __attribute__((packed));

struct idt_pointer {
  u16 limit;
  u64 base;
} __attribute__((packed));

static struct idt_entry idt[IDT_ENTRY_COUNT];

extern void isr0(void);
extern void isr1(void);
extern void isr2(void);
extern void isr3(void);
extern void isr4(void);
extern void isr5(void);
extern void isr6(void);
extern void isr7(void);
extern void isr8(void);
extern void isr9(void);
extern void isr10(void);
extern void isr11(void);
extern void isr12(void);
extern void isr13(void);
extern void isr14(void);
extern void isr15(void);
extern void isr16(void);
extern void isr17(void);
extern void isr18(void);
extern void isr19(void);
extern void isr20(void);
extern void isr21(void);
extern void isr22(void);
extern void isr23(void);
extern void isr24(void);
extern void isr25(void);
extern void isr26(void);
extern void isr27(void);
extern void isr28(void);
extern void isr29(void);
extern void isr30(void);
extern void isr31(void);
extern void isr32(void);
extern void isr33(void);
extern void isr34(void);
extern void isr35(void);
extern void isr36(void);
extern void isr37(void);
extern void isr38(void);
extern void isr39(void);
extern void isr40(void);
extern void isr41(void);
extern void isr42(void);
extern void isr43(void);
extern void isr44(void);
extern void isr45(void);
extern void isr46(void);
extern void isr47(void);
extern void isr48(void);   /* M98: MSI/MSI-X vectors 48..63 */
extern void isr49(void);
extern void isr50(void);
extern void isr51(void);
extern void isr52(void);
extern void isr53(void);
extern void isr54(void);
extern void isr55(void);
extern void isr56(void);
extern void isr57(void);
extern void isr58(void);
extern void isr59(void);
extern void isr60(void);
extern void isr61(void);
extern void isr62(void);
extern void isr63(void);
extern void isr64(void);   /* LAPIC timer — per-CPU scheduler tick */
extern void isr65(void);   /* TLB shootdown IPI */
extern void isr66(void);   /* Reschedule IPI — wake from sti;hlt */
extern void isr255(void);  /* LAPIC spurious — no-EOI no-op */

static volatile u64 timer_ticks;

static u64 read_cr2(void) {
  u64 value;

  __asm__ volatile("movq %%cr2, %0" : "=r"(value));
  return value;
}

/* Lock-free hex dump straight to the serial port for the fatal fault path.
 * Uses no locks and minimal stack so it works even when the console lock is
 * wedged. */
static void serial_emerg_hex(u64 v) {
  static const char d[] = "0123456789abcdef";
  serial_putc('0');
  serial_putc('x');
  for (int i = 60; i >= 0; i -= 4)
    serial_putc(d[(v >> i) & 0xf]);
}

static const char *exception_names[] = {
    "divide error",
    "debug",
    "non-maskable interrupt",
    "breakpoint",
    "overflow",
    "bound range exceeded",
    "invalid opcode",
    "device not available",
    "double fault",
    "coprocessor segment overrun",
    "invalid tss",
    "segment not present",
    "stack-segment fault",
    "general protection fault",
    "page fault",
    "reserved",
    "x87 floating-point exception",
    "alignment check",
    "machine check",
    "simd floating-point exception",
    "virtualization exception",
    "control protection exception",
    "reserved",
    "reserved",
    "reserved",
    "reserved",
    "reserved",
    "reserved",
    "hypervisor injection exception",
    "vmm communication exception",
    "security exception",
    "reserved",
};

static void idt_set_gate(u8 vector, void (*handler)(void)) {
  u64 address = (u64)handler;

  idt[vector].offset_low = (u16)(address & 0xffff);
  idt[vector].selector = KERNEL_CODE_SELECTOR;
  idt[vector].ist = 0;
  idt[vector].type_attr = IDT_INTERRUPT_GATE;
  idt[vector].offset_mid = (u16)((address >> 16) & 0xffff);
  idt[vector].offset_high = (u32)((address >> 32) & 0xffffffff);
  idt[vector].zero = 0;
}

void x86_idt_init(void) {
  void (*handlers[])(void) = {
      isr0,  isr1,  isr2,  isr3,  isr4,  isr5,  isr6,  isr7,
      isr8,  isr9,  isr10, isr11, isr12, isr13, isr14, isr15,
      isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23,
      isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31,
  };

  for (u8 i = 0; i < 32; i++) {
    idt_set_gate(i, handlers[i]);
  }
  /* #BP and #OF are the two exceptions a program raises on purpose, with int3
   * and into. Their gates need DPL=3 or the instruction faults as #GP with the
   * IDT index in the error code instead of trapping — which is what chromium's
   * BreakDebugger looked like here: a general protection fault at error 0x1a,
   * naming IDT vector 3, with no hint that the program had asked for a
   * breakpoint. Linux gives these two gates DPL=3 for the same reason. */
  idt[3].type_attr = IDT_INTERRUPT_GATE | 0x60;
  idt[4].type_attr = IDT_INTERRUPT_GATE | 0x60;
  /* #DF runs on IST1, a per-CPU stack the TSS points at (x86_tss_init_cpu).
   * Without it, a fault that hit BECAUSE the kernel stack was unusable (stack
   * overflow, corrupted RSP) re-faults while pushing the #DF frame onto that
   * same broken stack — a triple fault, which the CPU answers by resetting.
   * QEMU with -no-reboot then just exits, so the failure looked like a silent
   * hang: no panic, no backtrace, the serial log simply stops. On IST1 the
   * handler gets a known-good stack and can print the exception + registers. */
  idt[8].ist = 1;
  idt_set_gate(32, isr32);
  idt_set_gate(33, isr33);
  idt_set_gate(34, isr34);
  idt_set_gate(35, isr35);
  idt_set_gate(36, isr36);
  idt_set_gate(37, isr37);
  idt_set_gate(38, isr38);
  idt_set_gate(39, isr39);
  idt_set_gate(40, isr40);
  idt_set_gate(41, isr41);
  idt_set_gate(42, isr42);
  idt_set_gate(43, isr43);
  idt_set_gate(44, isr44);
  idt_set_gate(45, isr45);
  idt_set_gate(46, isr46);
  idt_set_gate(47, isr47);

  /* M98: the MSI/MSI-X vector range. Separate from 32..47 because a message
   * interrupt carries no line: the device writes the vector straight to the
   * local APIC, so there is no IOAPIC entry to mask and no line to share with
   * a legacy device. */
  {
    void (*const msi_stubs[16])(void) = {
        isr48, isr49, isr50, isr51, isr52, isr53, isr54, isr55,
        isr56, isr57, isr58, isr59, isr60, isr61, isr62, isr63,
    };
    for (u8 i = 0; i < 16; i++)
      idt_set_gate((u8)(MSI_VECTOR_BASE + i), msi_stubs[i]);
  }

  /* LAPIC timer (per-CPU) and LAPIC spurious gates. Installed unconditionally:
   * the BSP arms the LAPIC timer from lapic_timer_start_periodic_ms after
   * calibration, and APs arm it as they enter the cooperative phase. */
  idt_set_gate(64, isr64);
  /* TLB shootdown IPI vector — wired on every CPU so tlb_shootdown_*
   * can target every other core. */
  idt_set_gate(65, isr65);
  /* Reschedule IPI — wires the wake-from-sti;hlt path so a new task on
   * the global runqueue doesn't wait for the next 10 ms LAPIC tick on
   * each idle AP. Handler is a no-op (just EOI). */
  idt_set_gate(66, isr66);
  idt_set_gate(255, isr255);

  struct idt_pointer pointer = {
      .limit = sizeof(idt) - 1,
      .base = (u64)&idt,
  };

  __asm__ volatile("lidt %0" : : "m"(pointer));
}

/* Load the (already-populated) shared IDT on the calling CPU. Used by APs from
 * x86_ap_arch_init so exceptions/page faults are handled on every core. */
void x86_idt_load(void) {
  struct idt_pointer pointer = {
      .limit = sizeof(idt) - 1,
      .base = (u64)&idt,
  };
  __asm__ volatile("lidt %0" : : "m"(pointer));
}

void x86_pic_init(void) {
  outb(PIC1_COMMAND, 0x11);
  io_wait();
  outb(PIC2_COMMAND, 0x11);
  io_wait();

  outb(PIC1_DATA, 0x20);
  io_wait();
  outb(PIC2_DATA, 0x28);
  io_wait();

  outb(PIC1_DATA, 0x04);
  io_wait();
  outb(PIC2_DATA, 0x02);
  io_wait();

  outb(PIC1_DATA, 0x01);
  io_wait();
  outb(PIC2_DATA, 0x01);
  io_wait();

  outb(PIC1_DATA, 0xf8); // Unmask IRQ0, IRQ1 and IRQ2(cascade to PIC2)
  outb(PIC2_DATA, 0xff);
}

/* ── Generic device-IRQ dispatch (M70) ──────────────────────────────────────
 * A small table keyed by legacy IRQ line (0..15). Block/DMA drivers register a
 * completion handler here instead of bolting a vector into the dispatcher. PCI
 * lines are shared, so each line holds a few sharers and every registered
 * handler is called; the handler reports whether its device raised the IRQ. */
#define IRQ_LINES 16
#define IRQ_SHARERS 4
struct irq_action {
  irq_handler_fn fn;
  void *ctx;
};
static struct irq_action g_irq_actions[IRQ_LINES][IRQ_SHARERS];
/* Serialises register/unregister (writers) so two drivers claiming a slot on the
 * same line cannot clobber each other. The dispatcher (reader) stays lock-free —
 * it only does atomic ACQUIRE loads of the fn field — so it never contends with,
 * or deadlocks against, a writer even when an IRQ fires mid-registration. */
static spinlock_t g_irq_lock = SPINLOCK_INIT;

int irq_register_handler(u8 irq, irq_handler_fn fn, void *ctx) {
  if (irq >= IRQ_LINES || fn == 0)
    return -1;
  u64 flags;
  spin_lock_irqsave(&g_irq_lock, &flags);
  for (int i = 0; i < IRQ_SHARERS; i++) {
    if (g_irq_actions[irq][i].fn == 0) {
      g_irq_actions[irq][i].ctx = ctx;
      /* Publish ctx before fn so the dispatcher (possibly on another CPU)
       * never sees a handler with a stale/NULL context. */
      __atomic_store_n(&g_irq_actions[irq][i].fn, fn, __ATOMIC_RELEASE);
      spin_unlock_irqrestore(&g_irq_lock, flags);
      return 0;
    }
  }
  spin_unlock_irqrestore(&g_irq_lock, flags);
  return -1;
}

/* M98: MSI/MSI-X vector table. One owner per vector — a message interrupt is
 * point-to-point, so there is no sharing to arbitrate. Same publication order
 * as the line table: ctx before fn, so a dispatch that races registration
 * never sees a handler with a stale context. */
static struct irq_action g_msi_actions[MSI_VECTOR_COUNT];

int msi_alloc_vector(irq_handler_fn fn, void *ctx) {
  if (fn == 0)
    return -1;
  u64 flags;
  spin_lock_irqsave(&g_irq_lock, &flags);
  for (u32 i = 0; i < MSI_VECTOR_COUNT; i++) {
    if (g_msi_actions[i].fn == 0) {
      g_msi_actions[i].ctx = ctx;
      __atomic_store_n(&g_msi_actions[i].fn, fn, __ATOMIC_RELEASE);
      spin_unlock_irqrestore(&g_irq_lock, flags);
      return (int)(MSI_VECTOR_BASE + i);
    }
  }
  spin_unlock_irqrestore(&g_irq_lock, flags);
  return -1;
}

void msi_free_vector(int vector) {
  if (vector < (int)MSI_VECTOR_BASE ||
      vector >= (int)(MSI_VECTOR_BASE + MSI_VECTOR_COUNT))
    return;
  u64 flags;
  spin_lock_irqsave(&g_irq_lock, &flags);
  __atomic_store_n(&g_msi_actions[vector - (int)MSI_VECTOR_BASE].fn,
                   (irq_handler_fn)0, __ATOMIC_RELEASE);
  g_msi_actions[vector - (int)MSI_VECTOR_BASE].ctx = 0;
  spin_unlock_irqrestore(&g_irq_lock, flags);
}

int msi_dispatch(int vector) {
  if (vector < (int)MSI_VECTOR_BASE ||
      vector >= (int)(MSI_VECTOR_BASE + MSI_VECTOR_COUNT))
    return 0;
  struct irq_action *a = &g_msi_actions[vector - (int)MSI_VECTOR_BASE];
  irq_handler_fn fn = __atomic_load_n(&a->fn, __ATOMIC_ACQUIRE);
  if (!fn)
    return 0;
  fn(a->ctx);
  return 1;
}

/* Remove a previously-registered (fn, ctx) handler from `irq`. Returns 0 if a
 * matching slot was found and cleared, -1 otherwise. The fn is cleared with a
 * RELEASE store so a concurrent dispatcher sees the old handler in full or sees
 * it gone — never a torn entry. */
int irq_unregister_handler(u8 irq, irq_handler_fn fn, void *ctx) {
  if (irq >= IRQ_LINES || fn == 0)
    return -1;
  u64 flags;
  spin_lock_irqsave(&g_irq_lock, &flags);
  for (int i = 0; i < IRQ_SHARERS; i++) {
    if (g_irq_actions[irq][i].fn == fn && g_irq_actions[irq][i].ctx == ctx) {
      __atomic_store_n(&g_irq_actions[irq][i].fn, (irq_handler_fn)0,
                       __ATOMIC_RELEASE);
      g_irq_actions[irq][i].ctx = 0;
      spin_unlock_irqrestore(&g_irq_lock, flags);
      return 0;
    }
  }
  spin_unlock_irqrestore(&g_irq_lock, flags);
  return -1;
}

int irq_dispatch(int irq) {
  if (irq < 0 || irq >= IRQ_LINES)
    return 0;
  int handled = 0;
  for (int i = 0; i < IRQ_SHARERS; i++) {
    irq_handler_fn fn = __atomic_load_n(&g_irq_actions[irq][i].fn, __ATOMIC_ACQUIRE);
    if (fn)
      handled |= fn(g_irq_actions[irq][i].ctx);
  }
  return handled;
}

void irq_unmask(u8 irq) { x86_pic_unmask(irq); }

void x86_pic_unmask(u8 irq) {
  /* IOAPIC mode: program a redirection entry instead of poking the (now
   * masked) 8259. Default to PCI semantics — level-triggered, active-low —
   * which matches every dynamic IRQ a driver actually wants to register
   * (NIC and friends). Any ACPI ISO override still takes precedence inside
   * ioapic_route_irq. */
  if (ioapic_active()) {
    ioapic_route_irq(irq, (u8)(32 + irq), (u8)lapic_id(), /*level_low=*/1);
    return;
  }
  u16 port;
  u8 value;
  if (irq < 8) {
    port = PIC1_DATA;
  } else {
    port = PIC2_DATA;
    irq -= 8;
  }
  value = inb(port) & ~(1 << irq);
  outb(port, value);
}

void x86_timer_init(void) {
  u16 divisor = (u16)(PIT_FREQUENCY / TIMER_HZ);

  outb(PIT_COMMAND, 0x36);
  outb(PIT_CHANNEL0, (u8)(divisor & 0xff));
  outb(PIT_CHANNEL0, (u8)((divisor >> 8) & 0xff));

  console_write("timer: pit 100hz initialized\n");
}

extern void ps2_kbd_interrupt_handler(void);
extern void virtio_input_poll(void);
extern void usb_kbd_poll(void);
extern void ps2_mouse_interrupt_handler(void);

extern void fb_console_blink_cursor(void);

/* Centralised EOI: LAPIC EOI when running through the IOAPIC, 8259 EOI
 * (both PICs for IRQs 8..15) when still on the legacy PIC. Vectors below 32
 * are CPU exceptions and don't go through this path. */
static inline void irq_eoi(u64 vector) {
  if (ioapic_active()) {
    lapic_eoi();
    return;
  }
  int irq = (int)vector - 32;
  if (irq >= 8) outb(PIC2_COMMAND, PIC_EOI);
  outb(PIC1_COMMAND, PIC_EOI);
}

static void x86_irq_handler_inner(struct interrupt_frame *frame) {
  /* LAPIC spurious interrupt (vector 0xFF). Per Intel SDM 10.9, the handler
   * must NOT issue an EOI. Just return. */
  if (frame->vector == 255) {
    return;
  }

  /* TLB shootdown IPI (vector 0x41 = 65). Forwarded to the shootdown machinery
   * in kernel/arch/x86_64/tlb.c which invlpg's the published vaddr (or reloads
   * CR3) and decrements the pending counter the initiator polls. EOI happens
   * inside the handler. */
  if (frame->vector == 65) {
    tlb_shootdown_handler();
    return;
  }

  /* Reschedule IPI (vector 0x42 = 66). Pure wake-up: the target was sitting in
   * `sti; hlt` and we want it to re-poll the runqueue immediately. No state to
   * touch — just EOI. */
  if (frame->vector == 66) {
    lapic_eoi();
    return;
  }

  /* LAPIC timer (vector 0x40 = 64) — per-CPU scheduler tick. Drives the global
   * scheduler bookkeeping (scheduler_ticks++, wake_sleepers) on the BSP only so
   * wall-clock ticks aren't multiplied by g_max_cpus. APs receive the tick as a
   * per-CPU preemption opportunity (cooperative for now — preemption from ISR is
   * still gated on the VFS chain-walk rwlock audit, M28 item 3). */
  if (frame->vector == 64) {
    struct percpu *pcpu = get_percpu();
    int is_bsp = pcpu ? (pcpu->cpu_id == 0) : 1;
    /* T8 (M28 #8): EOI BEFORE scheduler_on_timer_tick. With preemptive
     * yields enabled, scheduler_on_timer_tick may context-switch away —
     * the task that returns here later finishes the EOI path long after
     * we wanted the LAPIC to consider this interrupt done. Delaying EOI
     * past the yield wedges the LAPIC: it thinks the timer is still in
     * service and never delivers another tick. Issue EOI first so the
     * LAPIC unblocks immediately, then run the (preemptible) tick work. */
    lapic_eoi();
    /* Record the user RIP the tick preempted, so the silence watchdog's task
     * dump can name the exact user function a wedged thread group spins in
     * (a thread group burning CPU in the same address forever is a lockup;
     * the RIP distinguishes that from mere slow progress). */
    if (frame->cs == 0x1B || frame->cs == 0x23) {
      task_set_user_rip(current_task, frame->rip);
    }
    /* Sample where this tick landed. The distribution (user/kernel/idle) is
     * always kept; the kernel-RIP histogram only under b1nix.sysprof. This is
     * the per-CPU LAPIC timer, so every core contributes — vector 32 is the
     * BSP's PIT and would have collected a sixth of the picture. */
    {
      int in_user = (frame->cs == 0x1B || frame->cs == 0x23);
      int in_idle = (pcpu && pcpu->idle_task &&
                     (struct task *)pcpu->cur_task == (struct task *)pcpu->idle_task);

      kprof_tick(frame->rip, in_user, in_idle, pcpu ? (int)pcpu->cpu_id : 0);
    }
    if (is_bsp) {
      timer_ticks++;
      if (timer_ticks % 50 == 0) {
        fb_console_blink_cursor();
      }
      /* M107: /dev/watchdog is a real deadline, checked once per tick. */
      watchdog_tick();

      /* Drain input BEFORE scheduler_on_timer_tick: the tick may context-
       * switch away (T8) and delay anything placed after it. */
      /* Poll i8042 as a fallback as well as handling IRQ1. QEMU/macOS and
       * some real IOAPIC setups can leave bytes pending without delivering
       * another edge; draining an empty controller is harmless. */
      ps2_kbd_interrupt_handler();
      virtio_input_poll(); /* drain virtio-tablet absolute-pointer events */
      serial_tty_tick(); /* M39: drain UART RX for open /dev/ttySn sessions */

      scheduler_on_timer_tick();
      usb_kbd_poll(); /* M37: drain the USB HID keyboard's interrupt endpoint */
    }
    if (frame->cs == 0x1B || frame->cs == 0x23) {
      /* rseq(2): the tick may have preempted (and the task may have come back
       * on another CPU), so refresh the registered cpu ids and restart a
       * critical section we would otherwise resume in the middle of. */
      rseq_on_return_to_user(frame);
      /* A pending signal is delivered here too. Without it a task that never
       * makes a syscall — a compute loop, or a ptrace tracee spinning between
       * breakpoints — could only be signalled by forcing its state from
       * another CPU, which races with its own context save. */
      arch_check_and_deliver_signals(frame);
    }
    return;
  }

  if (frame->vector == 32) {
    /* Legacy PIT IRQ0 fallback. With M28-A the LAPIC timer drives the scheduler
     * tick on all cores and PIT IRQ0 is masked at the IOAPIC; this branch is
     * kept so an unsuspected stray (e.g. calibration failed → IOAPIC mask
     * skipped) still progresses the BSP tick instead of console-spamming. */
    serial_tty_tick(); /* M39: drain UART RX for open /dev/ttySn sessions */
    timer_ticks++;
    if (timer_ticks % 50 == 0) {
      fb_console_blink_cursor();
    }
    irq_eoi(frame->vector);
    scheduler_on_timer_tick();
    return;
  }

  if (frame->vector == 33) {
    ps2_kbd_interrupt_handler();
    irq_eoi(frame->vector);
    return;
  }

  if (frame->vector == 44) {
    ps2_mouse_interrupt_handler();
    irq_eoi(frame->vector);
    return;
  }
  /* M98: MSI/MSI-X. One owner, no IOAPIC entry behind it, so EOI goes straight
   * to the local APIC — irq_eoi's legacy-PIC branch would be wrong here (the
   * 8259 never saw this interrupt). */
  if (frame->vector >= MSI_VECTOR_BASE &&
      frame->vector < MSI_VECTOR_BASE + MSI_VECTOR_COUNT) {
    msi_dispatch((int)frame->vector);
    lapic_eoi();
    return;
  }

  if (frame->vector >= 32 && frame->vector <= 47) {
    int irq = frame->vector - 32;
    int handled = 0;
    /* NICs first: every registered interface on this line gets its cause
     * register read, which is what releases a shared level-triggered INTx. */
    handled |= net_handle_irq(irq);
    /* M70: registered block/DMA device handlers (AHCI, virtio-blk, NVMe).
     * Lines are shared, so always consult the table even after net claimed it. */
    handled |= irq_dispatch(irq);
    irq_eoi(frame->vector);
    /* A level-triggered PCI line that no driver claimed is a benign shared-IRQ
     * artifact — stay silent rather than spam the console. Genuinely stray
     * vectors outside the device range still get the diagnostic below. */
    (void)handled;
    return;
  }

  console_write("\nIRQ: unexpected vector 0x");
  console_write_hex64(frame->vector);
  console_write("\n");
  irq_eoi(frame->vector);

  if (frame->cs == 0x1B || frame->cs == 0x23) {
    arch_check_and_deliver_signals(frame);
  }
}

/* Interrupt entry runs BKL-free (M28 #7). The hot IPI/timer vectors are handled
 * inline below; device IRQs and exceptions are each independently SMP-safe (see
 * x86_irq_handler / x86_exception_handler).
 *
 * **TLB shootdown IPI (vector 65)** is dispatched before anything else: the
 * initiator spins (IRQs off) waiting for every target to ACK, so the handler
 * must run even while this CPU is deep in other kernel work. It is trivial
 * (invlpg + atomic decrement + EOI) and runs with IRQs implicitly disabled at
 * the LAPIC level.
 */
static int addr_is_kernel_text(u64 addr); /* defined below; used by the fault dump */
static void x86_irq_handler_dispatch(struct interrupt_frame *frame);
void x86_irq_handler(struct interrupt_frame *frame) {
  /* M86: an IRQ taken in ring 3 closes that task's user-time interval; the
   * handler's own cost is charged to it as system time (Unix accounts device
   * interrupts to whoever was interrupted). The two IPI vectors below return
   * before any of that — they can arrive at any point in kernel code, and a
   * shootdown ACK is not the interrupted task's CPU time in any useful sense.
   * They only ever fire with the CPU already in ring 0, so skipping them costs
   * no accuracy. */
  int from_user = (frame->cs == 0x1B || frame->cs == 0x23);
  if (from_user)
    sched_acct_enter_kernel();
  x86_irq_handler_dispatch(frame);
  if (from_user)
    sched_acct_leave_kernel();
}

static void x86_irq_handler_dispatch(struct interrupt_frame *frame) {
  if (frame->vector == 65) {
    tlb_shootdown_handler();
    return;
  }
  /* Reschedule IPI (M28 #6): same BKL bypass as TLB shootdown — the handler
   * is a pure no-op wake-up, so taking BKL would just add unnecessary
   * contention without adding correctness. */
  if (frame->vector == 66) {
    lapic_eoi();
    return;
  }
  /* T3 (M28 #7): LAPIC timer (vector 64) bypasses the BKL.
   * scheduler_on_timer_tick mutates only:
   *  - scheduler_ticks (BSP-only writer, single-writer safe non-atomic),
   *  - wake_sleepers (atomic CAS SLEEPING->READY via F4 + IPI),
   *  - cursor blink + ipi_reschedule_all.
   * All SMP-safe via the F-tier foundation, so the BKL is needless serialisation
   * here. The inner handler does its own EOI for this vector. */
  if (frame->vector == 64) {
    x86_irq_handler_inner(frame);
    return;
  }
  /* M28 #7 (final teardown): device IRQs run WITHOUT the BKL. The hot vectors
   * above already bypassed it; the remaining device handlers are each
   * independently SMP-safe — the PS/2 keyboard ring is a release/acquire SPSC
   * (kbd_push vs ps2_kbd_getc), the mouse event flag is an atomic exchange and
   * its packet assembly is single-producer (a device IRQ is delivered to one
   * CPU at a time and is not re-entrant), and the net handler serialises on its
   * own net_rx_lock/net_tx_lock. A device IRQ touches no other cross-CPU state
   * the BKL was protecting. This removes the last bkl_lock() call site. */
  x86_irq_handler_inner(frame);
}

static void x86_exception_handler_inner(struct interrupt_frame *frame) {
  /* M36: route #BP (int3, vector 3) and #DB (single-step, vector 1) to the
   * GDB serial stub when the kernel was booted with b1nix.gdb. Off by default
   * so an ordinary/test boot never blocks waiting on a host debugger. */
  /* #DB from a PTRACE_SINGLESTEP: the tracee stops and its tracer sees the
   * SIGTRAP, before the kernel-debugging stub gets a look at the vector. */
  if (frame->vector == 1 && (frame->cs == 0x1B || frame->cs == 0x23) &&
      ptrace_handle_debug_trap(frame))
    return;

  if ((frame->vector == 3 || frame->vector == 1) &&
      bootinfo_has_flag("b1nix.gdb")) {
    gdb_stub_enter(frame);
    return;
  }

  // Page fault handling for Demand Paging
  if (frame->vector == 14) {
    u64 fault_addr = read_cr2();
    u64 error_code = frame->error_code;
    /* Handle the fault with interrupts as the faulting code had them.
     *
     * The gate clears IF, and demand paging is not a short handler: it takes
     * the page-cache lock and can wait on a disk read. Spinning for that lock
     * with IF clear on every CPU is a deadlock with no way out — the CPU that
     * holds it is blocked on I/O and can never be rescheduled, because no CPU
     * can take a timer tick. Observed with two vCPUs both stopped in
     * page_cache_get_page, RFLAGS=0x2.
     *
     * CR2 is already read, so nothing here depends on the fault registers
     * staying untouched. A fault taken with IF already clear came from code
     * holding an irqsave lock, and that state is left exactly as it was.
     * Interrupts go back off before the return so the iretq below restores
     * the frame's own flags rather than ours. */
    int restore_irqs = (frame->rflags & (1ull << 9)) != 0;

    if (restore_irqs)
      __asm__ volatile("sti");

    /* Demand paging is the other half of the start-up bill. Counted beside
     * the system calls so the two can be compared rather than guessed at. */
    /* The futex watchpoint reports the write it was armed for, then steps
     * aside: the fault is resolved by the normal path below. */
    if (error_code & PF_WRITE) {
      extern int futex_watch_hit(u64 fault_addr, u64 pml4_phys, u64 rip,
                                 usize task_id);

      if (current_task)
        (void)futex_watch_hit(fault_addr, current_task->pml4_phys, frame->rip,
                              current_task->id);
    }

    u64 pf_t0 = pf_prof_enabled() ? pf_prof_now() : 0;
    int handled = vmm_handle_page_fault(fault_addr, error_code) == 0;

    if (pf_t0)
      pf_prof_account(pf_prof_now() - pf_t0);

    if (restore_irqs)
      __asm__ volatile("cli");

    if (handled) {
      if (frame->cs == 0x1B || frame->cs == 0x23) {
        arch_check_and_deliver_signals(frame);
      }
      return; // Successfully handled
    }
  }

  const char *name = "unknown exception";
  if (frame->vector < 32) {
    name = exception_names[frame->vector];
  }

  /* Lock-free emergency line FIRST: a single direct-serial dump of the fatal
   * fault, using minimal stack and no locks, so it survives even if the console
   * lock is wedged or a follow-on dump triple-faults. */
  {
    serial_write("\n#EXC vec=");
    serial_emerg_hex(frame->vector);
    serial_write(" err=");
    serial_emerg_hex(frame->error_code);
    serial_write(" rip=");
    serial_emerg_hex(frame->rip);
    serial_write(" cs=");
    serial_emerg_hex(frame->cs);
    serial_write(" cr2=");
    serial_emerg_hex(read_cr2());
    serial_write("\n");

    /* Name a boot-stack overflow for what it is.
     *
     * The page below the boot stack is unmapped (see boot_stack_guard in
     * boot.S), so running off the bottom faults there. On #PF the address is
     * in CR2; on #DF it is CR2 as well, because the double fault is the CPU
     * failing to push the #PF frame onto the same dead stack. Either way the
     * reader would otherwise be looking at a fault on an address that no
     * source file mentions, which is exactly the sort of report that gets
     * blamed on whatever change happened to be under test. */
    if (boot_stack_is_guard_addr(read_cr2()) ||
        boot_stack_is_guard_addr(frame->rsp)) {
      serial_write("#EXC boot-stack overflow: the boot CPU ran off the bottom "
                   "of its kernel stack (guard page)\n");
    }
  }

  /* SMP-FRAME: the things a ring-3 fault report is only true *because of*.
   *
   * Every fact this handler prints about a user fault -- the faulting RIP, the
   * bytes at it, the mapping it fell in -- is read out of a frame the CPU
   * pushed at TSS.rsp0, through the address space CR3 names, using
   * `current_task` for the identity. If any of those three is not the running
   * task's own, the report is internally consistent and about the wrong
   * process, and nothing in it says so. A #UD on an instruction whose bytes
   * are then dumped and are correct is exactly what that looks like, so the
   * invariants get asserted rather than assumed:
   *
   *   rsp0    must be this task's kernel_stack_ptr, and the frame must sit
   *           inside that task's kernel stack;
   *   cr3     must be this task's PML4, or the fetch that faulted went through
   *           another process's tables;
   *   cs/ss   must be the ring-3 pair the kernel installs.
   *
   * One snprintf and one serial_write, because two CPUs faulting at once
   * interleave anything built from several writes -- which would itself
   * produce a report that pairs one fault's RIP with another's bytes. */
  if ((frame->cs & 3) == 3) {
    struct percpu *fp = get_percpu();
    struct task *ft = current_task;
    int fcpu = fp ? (int)fp->cpu_id : -1;
    u64 cr3 = 0, cr4 = 0, xcr0 = 0;
    u64 rsp0 = arch_kernel_stack_of_cpu(fcpu);
    u64 ksp = ft ? ft->kernel_stack_ptr : 0;
    u64 klo = ft && ft->stack ? (u64)(usize)ft->stack : 0;
    u64 fa = (u64)(usize)frame;
    char b[320];

    __asm__ volatile("movq %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("movq %%cr4, %0" : "=r"(cr4));
    if (cr4 & (1ULL << 18)) {
      u32 lo, hi;
      __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
      xcr0 = ((u64)hi << 32) | lo;
    }
    snprintf(b, sizeof(b),
             "SMP-FRAME cpu=%d pid=%u vec=%u cs=%x ss=%x rfl=%x "
             "cr3=%p pml4=%p rsp0=%p ksp=%p frame=%p kstack=%p "
             "xcr0=%x cr4=%x%s%s%s%s\n",
             fcpu, (unsigned)(ft ? ft->id : 0), (unsigned)frame->vector,
             (unsigned)frame->cs, (unsigned)frame->ss,
             (unsigned)frame->rflags, (void *)(usize)cr3,
             (void *)(usize)(ft ? ft->pml4_phys : 0), (void *)(usize)rsp0,
             (void *)(usize)ksp, (void *)(usize)fa, (void *)(usize)klo,
             (unsigned)xcr0, (unsigned)cr4,
             (ft && cr3 != ft->pml4_phys) ? " BAD-CR3" : "",
             (ft && rsp0 != ksp) ? " BAD-RSP0" : "",
             (klo && !(fa > klo && fa < klo + 64u * 1024u)) ? " BAD-FRAME" : "",
             (frame->cs != 0x23 || frame->ss != 0x1B) ? " BAD-SEL" : "");
    serial_write(b);
  }

  /* And, for a fault that claims the instruction itself was refused, the bytes
   * the CPU could have fetched -- read twice.
   *
   * Once through the faulting address as userspace sees it, which is what the
   * dump below does and what a stale translation would answer wrongly; and
   * once through the kernel's direct map of the physical frame the page tables
   * actually name, which no TLB entry of the user address can affect. Two
   * different answers say the translation lied. The same answer, decoding to a
   * legal instruction, says the frame did. */
  if ((frame->cs & 3) == 3 && (frame->vector == 6 || frame->vector == 13) &&
      current_task && current_task->pml4_phys) {
    u64 pte = paging_user_pte(current_task->pml4_phys,
                              frame->rip & ~(u64)(PAGE_SIZE - 1));
    char b[320];
    char va[64], pa[64];
    unsigned char op[16];
    extern int syscall_copyin(void *dst, const void *user_src,
                              unsigned long size);

    va[0] = pa[0] = 0;
    if (syscall_copyin(op, (void *)(usize)frame->rip, sizeof(op)) == 0)
      for (unsigned i = 0; i < sizeof(op); i++)
        snprintf(va + i * 2, sizeof(va) - i * 2, "%02x", op[i]);
    if ((pte & VMM_PRESENT) &&
        (pte & 0x000ffffffffff000ULL) < DIRECT_MAP_SIZE) {
      const unsigned char *d =
          (const unsigned char *)(usize)((pte & 0x000ffffffffff000ULL) +
                                         DIRECT_MAP_BASE +
                                         (frame->rip & (PAGE_SIZE - 1)));
      for (unsigned i = 0; i < sizeof(op); i++)
        snprintf(pa + i * 2, sizeof(pa) - i * 2, "%02x", d[i]);
    }
    snprintf(b, sizeof(b), "SMP-CODE rip=%p pte=%p va=[%s] phys=[%s]%s\n",
             (void *)(usize)frame->rip, (void *)(usize)pte, va, pa,
             (va[0] && pa[0] && strcmp(va, pa)) ? " MISMATCH" : "");
    serial_write(b);

    /* When the two disagree, say WHICH flush repairs it -- that is the whole
     * diagnosis, not a detail.
     *
     * A reload of CR3 evicts every translation this CPU holds except the ones
     * marked GLOBAL; INVLPG evicts the entry for one page whether it is global
     * or not. So: if the reload leaves the wrong bytes and the INVLPG fixes
     * them, the entry was global, and no context switch and no
     * tlb_shootdown_all() (which asks its targets to reload CR3) could ever
     * have removed it. If the reload already fixes them, it was an ordinary
     * stale entry and what is missing is a shootdown at whoever changed the
     * mapping. The two have completely different repairs, and nothing else
     * distinguishes them from the outside.
     *
     * The task is being killed either way, so re-reading its instruction after
     * two flushes costs nothing and changes nothing else. */
    if (va[0] && pa[0] && strcmp(va, pa) != 0) {
      char after_cr3[64], after_invlpg[64];
      char c[320];

      after_cr3[0] = after_invlpg[0] = 0;
      paging_reload_cr3();
      if (syscall_copyin(op, (void *)(usize)frame->rip, sizeof(op)) == 0)
        for (unsigned i = 0; i < sizeof(op); i++)
          snprintf(after_cr3 + i * 2, sizeof(after_cr3) - i * 2, "%02x", op[i]);
      __asm__ volatile("invlpg (%0)"
                       :
                       : "r"(frame->rip & ~(u64)(PAGE_SIZE - 1))
                       : "memory");
      if (syscall_copyin(op, (void *)(usize)frame->rip, sizeof(op)) == 0)
        for (unsigned i = 0; i < sizeof(op); i++)
          snprintf(after_invlpg + i * 2, sizeof(after_invlpg) - i * 2, "%02x",
                   op[i]);
      snprintf(c, sizeof(c), "SMP-CODE after-cr3=[%s] after-invlpg=[%s] %s\n",
               after_cr3, after_invlpg,
               (strcmp(after_cr3, pa) != 0 && strcmp(after_invlpg, pa) == 0)
                   ? "GLOBAL-ENTRY (survived a cr3 reload)"
                   : (strcmp(after_cr3, pa) == 0
                          ? "STALE-ENTRY (a cr3 reload cleared it)"
                          : "still-wrong-after-both"));
      serial_write(c);
    }
  }

  /* Fatal path. Bust the console lock first: this fault may have interrupted
   * code on this CPU that held it (e.g. a log/backtrace mid-print), so the
   * diagnostic console_write()s below would otherwise self-deadlock (spinlocks
   * mask IRQs but NOT CPU exceptions). bust_spinlocks pattern. */
  console_bust_lock();

  /* Recursion guard around the risky diagnostic dump (backtrace + userspace
   * stack walk both dereference possibly-bad memory). If that dump itself
   * faults, we re-enter here with the flag set — print a marker and panic
   * instead of looping forever. Cleared right after the dump, before the
   * (fault-safe) signal logic, so sequential faults from different processes
   * don't trip it. */
  /* Per-CPU, because a fault on another CPU is not a nested one.
   *
   * With one flag for the whole machine, an ordinary ring-3 fault taken on a
   * second CPU while this one was printing counted as a recursion and brought
   * the kernel down — two userspace programs crashing at the same time is not a
   * kernel fault, and on an SMP boot with several processes it happens easily.
   */
  static volatile int in_fault_dump[MAX_CPUS];
  u32 dump_cpu = get_percpu()->cpu_id;

  if (dump_cpu >= MAX_CPUS)
    dump_cpu = 0;
  if (in_fault_dump[dump_cpu]) {
    console_write("\n[nested fault while dumping exception — aborting]\n");
    panic("nested exception in fault handler");
  }
  in_fault_dump[dump_cpu] = 1;
  console_write(name);
  console_write("\nvector: 0x");
  console_write_hex64(frame->vector);
  console_write("\nerror:  0x");
  console_write_hex64(frame->error_code);
  console_write("\nrip:    0x");
  console_write_hex64(frame->rip);
  if (frame->vector == 14) {
    u64 cr2_val = read_cr2();
    console_write("\ncr2:    0x");
    console_write_hex64(cr2_val);
    console_write("\n");
    paging_dump_entries(cr2_val);
  }
  console_write("\ncs:     0x");
  console_write_hex64(frame->cs);
  console_write("\nrflags: 0x");
  console_write_hex64(frame->rflags);
  console_write("\nrax=0x"); console_write_hex64(frame->rax);
  console_write(" rbx=0x"); console_write_hex64(frame->rbx);
  console_write(" rcx=0x"); console_write_hex64(frame->rcx);
  console_write("\nrdx=0x"); console_write_hex64(frame->rdx);
  console_write(" rsi=0x"); console_write_hex64(frame->rsi);
  console_write(" rdi=0x"); console_write_hex64(frame->rdi);
  console_write("\nrbp=0x"); console_write_hex64(frame->rbp);
  console_write(" r8=0x");  console_write_hex64(frame->r8);
  console_write(" r9=0x");  console_write_hex64(frame->r9);
  console_write("\nr12=0x"); console_write_hex64(frame->r12);
  console_write(" r13=0x"); console_write_hex64(frame->r13);
  console_write(" r14=0x"); console_write_hex64(frame->r14);
  console_write(" rsp=0x"); console_write_hex64(frame->rsp);
  console_write("\n");

  /* Who faulted, and what does a wild kernel address actually point at? A rip
   * outside kernel text means we jumped through corrupted memory; naming the
   * containing heap block (live 64 KiB block = a task kernel stack, freed block
   * = use-after-free) is what turns that into an actionable report. */
  {
    struct task *ft = current_task;
    console_write("faulting task: pid=");
    console_write_dec(ft ? (u64)ft->id : 0);
    console_write(" name=");
    console_write(ft && ft->name ? ft->name : "(none)");
    console_write(" kstack=0x");
    console_write_hex64(ft ? (u64)(usize)ft->stack : 0);
    console_write("\n");
    kheap_describe(frame->rip, "rip  ->");
    if (frame->vector == 14)
      kheap_describe(read_cr2(), "cr2  ->");
    kheap_describe(frame->rsp, "rsp  ->");
    /* Raw bytes at the faulting rip: identifies WHAT the "code" we jumped into
     * really is (a struct's fields, a string, a stale pointer table). */
    if (frame->rip >= 0xffff800000000000ULL) {
      const u64 *w = (const u64 *)(usize)(frame->rip & ~7ULL);
      console_write("bytes@rip:");
      for (int i = -2; i < 6; i++) {
        console_write(" 0x");
        console_write_hex64(w[i]);
      }
      console_write("\n");
    }
    /* What the fault handler itself saw. The table walk above runs after the
     * handler returned, so it can report a leaf that permits the access that
     * was just refused — the tables moved on. This is the decision as made. */
    if (frame->vector == 14) {
      int seen = 0;
      u64 val = 0;
      const char *why = 0;

      paging_last_fault_leaf(&seen, &val, &why);
      console_write("at fault time: leaf ");
      if (seen) {
        console_write("0x");
        console_write_hex64(val);
      } else {
        console_write("(not read)");
      }
      console_write(" — ");
      console_write(why ? why : "?");
      console_write("\n");
    }

    /* Ring 3: the call chain, followed rather than guessed.
     *
     * Scanning the stack for values that look like code finds data that
     * happens to lie in a mapped range and reports it as a frame — every such
     * walk here produced library names that were never on the stack. A frame
     * pointer chain has no such ambiguity: rbp points at the saved rbp, and
     * the return address is the word above it. Abort paths keep their frame
     * pointer (they push rbp before the call that never returns), which is
     * exactly the case that needs naming. Each link is read only after its
     * page is confirmed present, so a broken chain stops the walk instead of
     * faulting inside the fault handler. */
    if ((frame->cs & 3) == 3 && ft && ft->pml4_phys) {
      u64 fp = frame->rbp;
      console_write("user call chain (rbp):");
      for (int depth = 0; depth < 12 && fp; depth++) {
        if (fp & 7)
          break;
        u64 pte = paging_user_pte(ft->pml4_phys, fp & ~(u64)(PAGE_SIZE - 1));
        if (!(pte & VMM_PRESENT))
          break;
        /* Both words of the frame must be on the same present page for this
         * read to be safe without a second lookup. */
        if ((fp & (PAGE_SIZE - 1)) > PAGE_SIZE - 16)
          break;
        const u64 *f = (const u64 *)(usize)fp;
        u64 ret = f[1];
        if (!ret)
          break;
        console_write(" 0x");
        console_write_hex64(ret);
        u64 next = f[0];
        if (next <= fp) /* frames grow upward; anything else is a broken chain */
          break;
        fp = next;
      }
      console_write("\n");
    }

    /* Raw kernel stack around rsp: shows what overwrote a return address. */
    if (frame->cs == 0x08 && frame->rsp >= 0xffff800000000000ULL) {
      const u64 *sp = (const u64 *)(usize)frame->rsp;
      for (int i = 0; i < 24; i += 4) {
        console_write("stack +0x");
        console_write_hex64((u64)(i * 8));
        console_write(":");
        for (int j = 0; j < 4; j++) {
          console_write(" 0x");
          console_write_hex64(sp[i + j]);
        }
        console_write("\n");
      }
    }
  }

  /* Fault-stack scan: leaf functions like memcpy don't set up a frame pointer,
   * so the rbp-based backtrace stops at the faulting rip. Scan the actual
   * faulting stack (frame->rsp upward) for kernel-text values — the first one is
   * the return address into the CALLER. Bounded, read-only, fault-tolerant. */
  if (frame->cs == 0x08 && frame->rsp >= 0xffff800000000000ULL) {
    console_write("fault-stack callers:");
    u64 *sp = (u64 *)(usize)frame->rsp;
    int shown = 0;
    for (int i = 0; i < 48 && shown < 8; i++) {
      u64 v = sp[i];
      if (addr_is_kernel_text(v)) {
        console_write(" 0x");
        console_write_hex64(v);
        ksym_print(v);
        shown++;
      }
    }
    console_write("\n");
  }

  arch_backtrace(frame->rbp, frame->rip);

  if (frame->cs == 0x1B || frame->cs == 0x23) {
    /* Which mapping the faulting address is in, by name and offset.
     *
     * Everything a dynamic program runs lives above 0x700000000000, so a raw
     * user rip identifies nothing: the loader, libc and every library share the
     * same neighbourhood. The mapping knows which file it came from, and the
     * offset is what a symbol table on the host can be asked about. */
    struct task *ft = current_task;

    for (struct vm_area *v = ft ? ft->vma_list : 0; v; v = v->next) {
      if (frame->rip < v->start || frame->rip >= v->end)
        continue;
      console_write("\nrip in mapping ");
      console_write(v->node && v->node->name[0] ? v->node->name : "<anonymous>");
      console_write(" + 0x");
      console_write_hex64(frame->rip - v->start + (u64)v->offset);
      console_write(" (base 0x");
      console_write_hex64(v->start);
      console_write(")\n");
      break;
    }

    /* The bytes at the faulting instruction.
     *
     * On an invalid-opcode fault this is the whole question, and it has two
     * completely different answers. Either the program really does contain an
     * instruction this CPU does not implement — in which case the bytes decode
     * to it and the fault is the program's — or the bytes are not the
     * instruction the program's own file holds at that offset, and what
     * faulted is the mapping: a page that is not the page it should be. The
     * two are indistinguishable from the address alone, and telling them apart
     * by disassembling the library on the host and comparing by hand is a
     * round trip per fault. */
    if (frame->vector == 6) {
      extern int syscall_copyin(void *dst, const void *user_src,
                                unsigned long size);
      unsigned char op[16];

      console_write("opcode bytes at rip:");
      if (syscall_copyin(op, (void *)(usize)frame->rip, sizeof(op)) == 0) {
        for (unsigned i = 0; i < sizeof(op); i++) {
          console_write(" ");
          console_write_hex64(op[i]);
        }
        console_write("\n");
      } else {
        console_write(" unreadable\n");
      }
    }

    /*
     * What the registers were pointing at.
     *
     * A fault on a null pointer says which field was read, not why it was null.
     * The structure it came from is still there, in a register, and its bytes
     * are the evidence: all-zero says the memory was replaced wholesale — a
     * page that lost its contents — while plausible-looking values with one
     * field wrong says something wrote through it. The mapping name says which
     * of the two the kernel could be responsible for.
     */
    {
      static const char *const names[] = {"rax", "rbx", "rcx", "rdx",
                                          "rsi", "rdi", "r8",  "r9"};
      const u64 vals[] = {frame->rax, frame->rbx, frame->rcx, frame->rdx,
                          frame->rsi, frame->rdi, frame->r8,  frame->r9};

      for (unsigned i = 0; i < sizeof(vals) / sizeof(vals[0]); i++) {
        u64 a = vals[i] & ~7ULL;
        struct vm_area *hit = 0;

        if (a < 0x1000)
          continue;
        for (struct vm_area *v = ft ? ft->vma_list : 0; v; v = v->next) {
          if (a >= v->start && a + 32 <= v->end) {
            hit = v;
            break;
          }
        }
        if (!hit || !(paging_leaf_pte(a) & 1))
          continue;
        console_write(names[i]);
        console_write(" -> ");
        console_write(hit->node && hit->node->name[0] ? hit->node->name
                                                      : "<anonymous>");
        console_write(":");
        for (unsigned k = 0; k < 4; k++) {
          console_write(" 0x");
          console_write_hex64(((const u64 *)(usize)a)[k]);
        }
        console_write("\n");
      }
    }

    /*
     * The whole map, once, when a user program dies.
     *
     * A single mapping names the code that faulted only when it is file-backed,
     * and a dynamic loader leaves plenty that is not. The neighbours identify it
     * instead: the library above and below are named, and the offset into the
     * region is what a symbol table on the other side resolves.
     */
    {
      unsigned shown = 0;

      /* Only the first few faults get the whole map.
       *
       * A process that faults in a loop produced 1566 reports, and at 600
       * mappings each that is most of a million lines onto a serial console —
       * printed with locks held, which stalled the CPUs holding them until a
       * spinlock lockup was declared. The diagnostic became the outage. The
       * early reports carry the same information, so print those in full and
       * cap the rest. */
      static u64 full_reports;
      unsigned cap = (++full_reports <= 4) ? 600 : 12;

      console_write("brk heap: 0x");
      console_write_hex64(ft ? ft->heap_start : 0);
      console_write("-0x");
      console_write_hex64(ft ? ft->user_brk : 0);
      console_write("\nmappings:\n");
      /* All of them, and with their protection.
       *
       * Forty-eight was enough to name a faulting library and not enough to
       * show the heap: a dynamic program has well over a hundred mappings and
       * the interesting ones — the brk heap, the guard page musl puts in front
       * of its allocator metadata — sit at the end of the list. The prot bits
       * are here because two VMAs covering one address is a bug this list is
       * the only witness to. */
      for (struct vm_area *v = ft ? ft->vma_list : 0; v && shown < cap;
           v = v->next, shown++) {
        console_write("  0x");
        console_write_hex64(v->start);
        console_write("-0x");
        console_write_hex64(v->end);
        console_write(v->prot & PROT_READ ? " r" : " -");
        console_write(v->prot & PROT_WRITE ? "w" : "-");
        console_write(v->prot & PROT_EXEC ? "x" : "-");
        console_write(v->flags & MAP_SHARED ? "s " : "p ");
        console_write(v->node && v->node->name[0] ? v->node->name
                                                  : "<anonymous>");
        console_write("\n");
      }
      /* And whether any two of those mappings are the same memory.
       *
       * Pixels from a client's buffer turned up inside the compositor's heap,
       * which cannot happen unless one physical page is reachable twice. The
       * map above shows what the process asked for; this says what it got. */
      if (bootinfo_has_flag("b1nix.frame-alias")) {
        extern void vmm_report_frame_aliases(struct task *t);
        if (ft)
          vmm_report_frame_aliases(ft);
      }
    }

    console_write("\nuserspace stack dump (rsp=0x");
    console_write_hex64(frame->rsp);
    console_write("):");
    extern int syscall_copyin(void *dst, const void *user_src, unsigned long size);
    for (int i = 0; i < 32; i++) {
      u64 val = 0;
      if (syscall_copyin(&val, (void *)(frame->rsp + i * 8), 8) == 0) {
        console_write("\n  +0x");
        console_write_hex64(i * 8);
        console_write(": 0x");
        console_write_hex64(val);
        if (val >= 0x2000000ULL && val <= 0x3000000ULL) {
          console_write(" (code?)");
        }
      } else {
        console_write("\n  (invalid page)");
        break;
      }
    }
    console_write("\n");
  }

  /* RIP=0 crash diagnostic: dump first 16 stack qwords + GOT entries. */
  if ((frame->cs == 0x1B || frame->cs == 0x23) && frame->rip == 0) {
    extern int syscall_copyin(void *dst, const void *user_src, unsigned long size);
    console_write("\nrip0-stk:");
    for (int i = 0; i < 16; i++) {
      u64 v = 0;
      if (syscall_copyin(&v, (void *)(frame->rsp + i * 8), 8) != 0) break;
      console_write(" "); console_write_hex64(v);
    }
    console_write("\n");

    /* Dump .got.plt entries to see if PLT resolver filled them.
     * Find the DYNAMIC segment, read DT_PLTGOT, then dump 8 GOT qwords. */
    if (current_task && current_task->user_image) {
      struct user_loaded_image *img = (struct user_loaded_image *)current_task->user_image;
      console_write("\nrip0-segs:");
      for (usize s = 0; s < img->segment_count && s < 8; s++) {
        console_write(" s"); console_write_dec(s);
        console_write(":v="); console_write_hex64(img->segments[s].vaddr);
        console_write(",m="); console_write_hex64(img->segments[s].memsz);
        console_write(",f="); console_write_hex64(img->segments[s].flags);
      }
      console_write("\n");
      /* Scan ALL RW segments for DT_PLTGOT (tag=3) in the DYNAMIC section.
       * Both ld-musl and app segments are RW; ld-musl's DYNAMIC won't have
       * a PLTGOT that points into app address space. */
      for (usize s = 0; s < img->segment_count && s < 8; s++) {
        u64 flags = img->segments[s].flags;
        if ((flags & 6) != 6) continue;
        u64 vaddr = img->segments[s].vaddr;
        u64 memsz = img->segments[s].memsz;
        for (u64 off = 0; off < 0x2000 && off + 16 <= memsz; off += 8) {
          u64 tag = 0, val = 0;
          if (syscall_copyin(&tag, (void *)(vaddr + off), 8) != 0) break;
          if (tag == 3) {
            if (syscall_copyin(&val, (void *)(vaddr + off + 8), 8) != 0) break;
            /* The in-memory DYNAMIC d_un values are file-relative for a PIE
             * (ld.so does not relocate its own view we read here). Rebase
             * low values using the containing segment's 2MB-aligned base.
             * Only dump the app's GOT (segments above the ld.so window are
             * skipped via the 0x7000...  interp base check). */
            if (vaddr >= 0x0000700000000000ULL) break; /* ld.so's own DYNAMIC */
            u64 got = val;
            if (got < 0x40000000ULL)
              got += vaddr & ~((u64)0x1FFFFF);
            console_write("\ngot-plt @0x");
            console_write_hex64(got);
            console_write(":");
            for (int g = 0; g < 64; g++) {
              u64 gv = 0;
              if (syscall_copyin(&gv, (void *)(got + g * 8), 8) != 0) break;
              console_write(" "); console_write_hex64(gv);
            }
            console_write("\n");
            goto got_done;
          }
        }
      }
      got_done: (void)0;
    }
  }

  /* Diagnostic dump done (it's the only fault-prone part) — clear the guard so
   * the next independent fault dumps normally. */
  in_fault_dump[dump_cpu] = 0;

  /* If exception happened in userspace (CS == 0x1B), send signal instead of
   * panic */
  if (frame->cs == 0x1B || frame->cs == 0x23) {
    /*
     * A #UD on an instruction that is not invalid.
     *
     * Weston and unrelated processes were killed by SIGILL on more than one
     * CPU, at instructions whose bytes the kernel then dumped and which were
     * exactly right -- a PLT `jmp *(%rip)` once. Correct bytes in an
     * executable mapping and a #UD from the CPU is what a stale instruction
     * translation looks like: the CPU fetched from a page the entry no longer
     * names. An instruction fetch does not fault when the translation is
     * merely old, so nothing here can notice it the way the page-fault handler
     * notices a spurious data fault.
     *
     * So test it rather than guess: drop this CPU's translations for the
     * faulting page, drop them everywhere, and let the instruction run again.
     * If it was a real invalid opcode the second attempt faults at the same
     * RIP and the signal goes out as before, one instruction later. If it
     * executes, the memory was always right and the translation was not --
     * which is the whole question, and the retry says so in the log.
     *
     * Bounded to one retry per RIP so a genuinely invalid instruction cannot
     * loop, and counted so a run that leans on this cannot look clean.
     *
     * Behind b1nix.ud-retry: this is a measurement, not a repair. Swallowing
     * one #UD would hide a real invalid opcode, and a kernel that silently
     * retries faults is a kernel whose faults mean less.
     *
     * Result so far: the retry does NOT rescue the instruction -- it faults
     * again at the same RIP after a full CR3 reload and a global shootdown.
     * So the translation is not stale, which is what this was built to find
     * out.
     */
    if (frame->vector == 6 && current_task &&
        bootinfo_has_flag("b1nix.ud-retry")) {
      static volatile u64 ud_retry_rip[MAX_CPUS];
      static volatile u64 ud_retries;
      int c = (int)dump_cpu;

      if (c >= 0 && c < (int)MAX_CPUS && ud_retry_rip[c] != frame->rip) {
        extern void tlb_shootdown_all(void);
        u64 n = __atomic_add_fetch(&ud_retries, 1, __ATOMIC_RELAXED);

        ud_retry_rip[c] = frame->rip;
        __asm__ volatile("invlpg (%0)" : : "r"(frame->rip & ~0xFFFULL)
                         : "memory");
        paging_reload_cr3();
        tlb_shootdown_all();
        if (n <= 16) {
          char b[128];

          snprintf(b, sizeof(b),
                   "UD-RETRY: #UD at rip=0x%lx (%s) — flushed and retrying"
                   " [%lu]\n",
                   (unsigned long)frame->rip,
                   current_task->name ? current_task->name : "?",
                   (unsigned long)n);
          console_write(b);
        }
        return;
      }
    }
    int sig = 0;
    switch (frame->vector) {
    case 0:
      sig = SIGFPE;
      break; /* #DE divide error */
    case 4:
      sig = SIGILL;
      break; /* #OF overflow */
    case 3:
      sig = SIGTRAP;
      break; /* #BP int3 — a debugger trap, not a fault */
    case 5:
      sig = SIGSEGV;
      break; /* #BR bound range */
    case 6:
      sig = SIGILL;
      break; /* #UD invalid opcode */
    case 8:
      sig = SIGSEGV;
      break; /* #DF double fault */
    case 11:
      sig = SIGSEGV;
      break; /* #NP segment not present */
    case 12:
      sig = SIGSEGV;
      break; /* #SS stack segment */
    case 13:
      sig = SIGSEGV;
      break; /* #GP general protection */
    case 14:
      sig = SIGSEGV;
      break; /* #PF page fault */
    default:
      sig = SIGTERM;
      break;
    }
    /* A signal generated synchronously by a CPU fault must be acted upon
     * before we resume userspace. If the faulting process installed a handler
     * for it and hasn't blocked it, deliver to that handler (e.g. a debugger or
     * an ICE reporter). Otherwise — SIG_DFL, SIG_IGN, or currently blocked —
     * returning to the faulting instruction would just re-fault forever, so we
     * force the default terminate action (matching Linux force_sig()). This
     * also covers the case where the signal is re-raised inside its own handler
     * (where it is blocked): the second fault terminates instead of looping. */
    /* M80 crash capture: record what faulted before anything else can run.
     * A page fault's address is CR2 and its si_code says whether the page was
     * absent (SEGV_MAPERR) or present but inaccessible (SEGV_ACCERR); every
     * other fault reports the instruction pointer with SI_KERNEL. The record is
     * what the task's own SA_SIGINFO handler and a tracer's PTRACE_GETSIGINFO
     * both read back. */
    if (frame->vector == 14)
      ptrace_record_fault(current_task, sig, read_cr2(),
                          (frame->error_code & 1) ? B1NIX_SEGV_ACCERR
                                                  : B1NIX_SEGV_MAPERR);
    else
      ptrace_record_fault(current_task, sig, frame->rip, B1NIX_SI_KERNEL);

    usize pid = scheduler_get_pid();
    struct sigaction *sa = &current_task->sigactions[sig - 1];
    int is_blocked = (current_task->blocked_signals >> (sig - 1)) & 1ULL;
    int has_handler =
        (sa->sa_handler != SIG_DFL && sa->sa_handler != SIG_IGN);

    /* A traced task's tracer gets first refusal on a fatal fault, exactly as
     * Linux's force_sig does: delivery parks the task in ptrace_signal_stop
     * with its register frame snapshotted, so a debugger or crash reporter sees
     * the crash instead of a corpse. Without this a traced process that faults
     * with no handler would be torn down before its tracer ever woke up. */
    if (!has_handler && ptrace_is_traced(current_task)) {
      scheduler_kill(pid, sig);
      arch_check_and_deliver_signals(frame);
      scheduler_yield();
      return;
    }

    if (has_handler && !is_blocked) {
      if (bootinfo_has_flag("b1nix.user-bt"))
        arch_user_backtrace(frame);
      console_write("delivering signal ");
      console_write_dec(sig);
      console_write(" to handler in pid ");
      console_write_hex64(pid);
      console_write("\n");
      scheduler_kill(pid, sig);
      arch_check_and_deliver_signals(frame);
      scheduler_yield();
      return;
    }

    /* Most actionable line in a crash log — name the signal, name the task,
     * and the userspace address that took the fault. Mirrors what Linux
     * dmesg ("Segmentation fault in <comm>[<pid>], ip=<addr>") and FreeBSD
     * ("pid X (comm), signal Y") emit. */
    static const char *sig_name[] = {
        /* 0 */ "?",
        /* 1 */ "SIGHUP",   /* 2 */ "SIGINT",    /* 3 */ "SIGQUIT",
        /* 4 */ "SIGILL",   /* 5 */ "SIGTRAP",   /* 6 */ "SIGABRT",
        /* 7 */ "SIGBUS",   /* 8 */ "SIGFPE",    /* 9 */ "SIGKILL",
        /* 10 */ "SIGUSR1", /* 11 */ "SIGSEGV",  /* 12 */ "SIGUSR2",
        /* 13 */ "SIGPIPE", /* 14 */ "SIGALRM",  /* 15 */ "SIGTERM",
        /* 16 */ "SIGSTKFLT", /* 17 */ "SIGCHLD", /* 18 */ "SIGCONT",
        /* 19 */ "SIGSTOP", /* 20 */ "SIGTSTP",  /* 21 */ "SIGTTIN",
        /* 22 */ "SIGTTOU", /* 23 */ "SIGURG",   /* 24 */ "SIGXCPU",
        /* 25 */ "SIGXFSZ", /* 26 */ "SIGVTALRM", /* 27 */ "SIGPROF",
        /* 28 */ "SIGWINCH", /* 29 */ "SIGIO",   /* 30 */ "SIGPWR",
        /* 31 */ "SIGSYS",
    };
    const char *sname = (sig > 0 && sig < (int)(sizeof(sig_name)/sizeof(*sig_name)))
                          ? sig_name[sig] : "?";
    arch_user_backtrace(frame);
    console_write("[FATAL] task '");
    console_write(current_task && current_task->name ? current_task->name : "?");
    console_write("' (pid ");
    console_write_dec(pid);
    console_write("): unhandled ");
    console_write(sname);
    console_write(" (signal ");
    console_write_dec(sig);
    console_write(") at rip=0x");
    console_write_hex64(frame->rip);
    /* For a fault signal, the address is the whole story: rip says which
     * instruction, and only the faulting address and its page-table entry say
     * why it was refused. A write to a page that is present but read-only and
     * a write to nothing at all are the same signal and different bugs. */
    /* ...but ONLY for a page fault.
     *
     * CR2 is written by #PF and by nothing else, and the error code's
     * present/write/user/fetch bits mean those things only in a #PF frame. A
     * #GP raised in ring 3 -- an unaligned SSE access, a non-canonical address
     * -- also becomes SIGSEGV here, and printing this block for it reports the
     * CR2 of some unrelated earlier fault as the faulting address, together
     * with a page-table entry for it and a decoding of a #GP selector as
     * "not-present, read, kernel". Every word of that is false, and it is
     * exactly the kind of confident wrong answer that costs a day. */
    if ((sig == SIGSEGV || sig == SIGBUS) && frame->vector != 14) {
      console_write(" vector=");
      console_write_dec(frame->vector);
      console_write(" (");
      console_write(frame->vector < 32 && exception_names[frame->vector]
                        ? exception_names[frame->vector]
                        : "?");
      console_write(") err=0x");
      console_write_hex64(frame->error_code);
      console_write(" (not a page fault: no faulting address)");
    } else if (sig == SIGSEGV || sig == SIGBUS) {
      extern u64 vmm_query_leaf_pte(u64 vaddr);
      u64 cr2;

      __asm__ volatile("movq %%cr2, %0" : "=r"(cr2));
      console_write(" addr=0x");
      console_write_hex64(cr2);
      console_write(" pte=0x");
      console_write_hex64(vmm_query_leaf_pte(cr2 & ~(u64)0xfff));
      /* And what the CPU actually objected to. A present page that refused a
       * write and a page that was not there are the same signal and different
       * bugs, and the entry alone cannot tell them apart — the error code is
       * the half that says which. */
      console_write(" err=0x");
      console_write_hex64(frame->error_code);
      console_write(" (");
      console_write((frame->error_code & 1) ? "protection" : "not-present");
      console_write((frame->error_code & 2) ? ", write" : ", read");
      console_write((frame->error_code & 4) ? ", user" : ", kernel");
      if (frame->error_code & 16)
        console_write(", fetch");
      console_write(")");
      /* And why the demand-paging path declined it.
       *
       * The error code says what the CPU objected to; it does not say what the
       * kernel decided afterwards. A page marked lazy that never materialises
       * and an address with no VMA behind it produce the identical line, and
       * they are different bugs. The handler already records its reason for
       * every fault -- printing it here costs nothing on a boot that does not
       * fault fatally, and saves a rebuild on one that does. */
      {
        int leaf_seen = 0;
        u64 leaf_val = 0;
        const char *why = 0;

        paging_last_fault_leaf(&leaf_seen, &leaf_val, &why);
        console_write(" handler=");
        console_write(why ? why : "(none)");
        console_write(" leaf_seen=");
        console_write_dec((u64)leaf_seen);
        console_write(" leaf=0x");
        console_write_hex64(leaf_val);
      }
    }
    console_write(" — terminating\n");
    /* M35: dump an ELF core for the fault-generating signals before the task
     * is torn down (its address space is still live here). */
    if (sig == SIGSEGV || sig == SIGABRT || sig == SIGILL || sig == SIGFPE ||
        sig == SIGBUS) {
      coredump_write(frame, sig);
      console_write("coredump: wrote /tmp/core\n");
    }
    scheduler_exit_current(TASK_EXIT_SIGNALED | sig);
    /* scheduler_exit_current never returns */
    arch_halt();
  }

  console_write("[PANIC] unhandled CPU exception\n");
  arch_halt();
}

/* Exception entry runs WITHOUT the Big Kernel Lock (M28 #7, final teardown
 * step). The two things an exception does are both independently SMP-safe now:
 *  - demand paging / CoW / swap-in: vmm_handle_page_fault is self-locking under
 *    vmm_lock (prepare-then-commit with a leaf re-validation), so concurrent
 *    faults on different CPUs no longer race on the page tables / PMM;
 *  - fatal faults: signal delivery (pending_signals atomics) and
 *    scheduler_exit_current (stack_released lease) are already SMP-safe.
 * The faulting task runs on its own CPU and kernel stack, so there is no
 * cross-CPU state the BKL was protecting here. */
void x86_exception_handler(struct interrupt_frame *frame) {
  /* M86: a fault taken in ring 3 ends a user-time interval and starts a
   * kernel-time one (page faults on demand-paged user memory are a real and
   * frequent part of a process's system time). */
  int from_user = (frame->cs == 0x1B || frame->cs == 0x23);
  if (from_user)
    sched_acct_enter_kernel();
  x86_exception_handler_inner(frame);
  if (from_user)
    sched_acct_leave_kernel();
}

/* ── Stack Backtrace ──────────────────────────────────────────── */
#define MAX_BACKTRACE_FRAMES 32

extern char __kernel_text_start[];
extern char __kernel_text_end[];

static int addr_is_kernel_text(u64 addr) {
  /* The kernel is linked higher-half (KERNEL_VMA), so its .text lives at
   * 0xffffffff801xxxxx — the old hard-coded 0x100000-0x200000 window only ever
   * matched the pre-relocation .boot stub, which is why every supervisor-mode
   * fault printed an EMPTY "fault-stack callers:" line and resolved every
   * address to long_mode_low. Use the linker-provided bounds instead.
   * The low window is kept for the .boot stub, and the 0x2000000-0x3000000
   * userspace window for backtracing non-PIE user binaries. */
  if ((u64)(usize)__kernel_text_start && addr >= (u64)(usize)__kernel_text_start &&
      addr < (u64)(usize)__kernel_text_end)
    return 1;
  return (addr >= 0x100000ULL && addr <= 0x200000ULL) ||
         (addr >= 0x2000000ULL && addr <= 0x3000000ULL);
}

/* A frame pointer is safe to dereference only if it is canonical and lands in a
   region we actually map. A corrupt frame (e.g. after a bad fork return) can
   carry a NON-canonical rbp; dereferencing it would itself #GP inside the
   exception handler and mask the original fault. */
static int fp_is_safe(u64 fp) {
  if (fp < 0x1000ULL || fp == (u64)-1)
    return 0;
  /* Reject the non-canonical hole [2^47, 0xffff800000000000). */
  if (fp >= 0x0000800000000000ULL && fp < 0xffff800000000000ULL)
    return 0;
  if (fp < 0x0000800000000000ULL)
    /* Low identity-mapped region: kernel image + heap/stacks live below 4 GiB. */
    return fp + 16 <= 0x0000000100000000ULL;
  /* Higher-half direct map. */
  return fp + 16 <= 0xffff800100000000ULL;
}

void arch_backtrace(u64 rbp, u64 rip) {
  int frames = 0;
  console_write("\n--- Kernel Backtrace ---\n");

  if (rip) {
    console_write("  [0] 0x");
    console_write_hex64(rip);
    ksym_print(rip);
    frames++;
  }

  /* Phase 1: Try RBP-based unwinding */
  for (int i = 0; i < MAX_BACKTRACE_FRAMES && rbp; i++) {
    if (!fp_is_safe(rbp))
      break;

    u64 next_rbp = 0;
    u64 ret_addr = 0;

    next_rbp = *(volatile u64 *)rbp;
    ret_addr = *(volatile u64 *)(rbp + 8);

    if (ret_addr == 0 || !addr_is_kernel_text(ret_addr))
      break;

    console_write("\n  [");
    console_write_dec(frames);
    console_write("] 0x");
    console_write_hex64(ret_addr);
    ksym_print(ret_addr);
    frames++;

    rbp = next_rbp;
  }

  /* Phase 2: If RBP unwinding gave nothing, try scanning the stack for
     return addresses using the current frame pointer (if in exception handler) */
  if (frames <= 1) {
    u64 scan_rbp = 0;
    __asm__ volatile("movq %%rbp, %0" : "=r"(scan_rbp));

    for (int i = 0; i < MAX_BACKTRACE_FRAMES && scan_rbp; i++) {
      if (!fp_is_safe(scan_rbp))
        break;

      u64 ret = *(volatile u64 *)(scan_rbp + 8);
      if (ret && addr_is_kernel_text(ret)) {
        console_write("\n  [");
        console_write_dec(frames);
        console_write("] 0x");
        console_write_hex64(ret);
        ksym_print(ret);
        frames++;
      }
      scan_rbp = *(volatile u64 *)scan_rbp;
    }
  }

  if (frames == 0)
    console_write("  (no frames)");

  console_write("\n--- End Backtrace ---\n");
}

/* ── User-mode backtrace ────────────────────────────────────────────────────
 * A ring-3 crash reports one address: the instruction that faulted. That names
 * the victim, never the culprit — a detected stack smash faults inside the C
 * library's __stack_chk_fail, and the function whose canary was overwritten is
 * one frame further up. Scan the crashing thread's own stack for words that
 * land in an executable mapping and attribute each to the module it belongs
 * to. The offset printed is the address's offset within the named file, which
 * is what llvm-addr2line wants for a position-independent object.
 *
 * Everything here reads user memory through syscall_copyin (which validates
 * the pointer) and never dereferences a user address directly, so a fault
 * inside the report cannot mask the fault being reported. */
int syscall_copyin(void *dst, const void *user_src, unsigned long size);

#define USER_BT_STACK_WORDS 192
#define USER_BT_MAX_FRAMES  24

/* Name the mapping an address falls in, and its offset within it. Returns 0
 * when the address is not in an executable user mapping — i.e. it is not a
 * return address, just stack data. */
static const char *user_module_at(struct task *t, u64 addr, u64 *off) {
  if (!t)
    return 0;
  for (struct vm_area *v = t->vma_list; v; v = v->next) {
    if (addr < v->start || addr >= v->end)
      continue;
    if (!(v->prot & 0x4))
      return 0; /* mapped, but not code */
    if (v->node && v->node->name[0]) {
      *off = (u64)v->offset + (addr - v->start);
      return v->node->name;
    }
    break;
  }
  /* The executable's and the interpreter's own segments are mapped by the
   * kernel loader and carry no backing node, so they have to be named from
   * the image the way /proc/<pid>/maps names them. */
  struct user_loaded_image *img = (struct user_loaded_image *)t->user_image;
  if (!img)
    return 0;
  for (usize k = 0; k < img->segment_count; k++) {
    const struct user_image_segment *seg = &img->segments[k];
    if (!seg->memsz)
      continue;
    if (addr < seg->vaddr || addr >= seg->vaddr + seg->memsz)
      continue;
    if (img->interp_base && seg->vaddr >= img->interp_base &&
        img->interp_path[0]) {
      *off = addr - img->interp_base;
      return img->interp_path;
    }
    *off = seg->file_offset + (addr - seg->vaddr);
    return img->path ? img->path : "[exe]";
  }
  return 0;
}

static void user_bt_line(const char *tag, int idx, u64 addr, struct task *t) {
  u64 off = 0;
  const char *mod = user_module_at(t, addr, &off);
  if (!mod)
    return;
  console_write("  ");
  console_write(tag);
  if (idx >= 0) {
    console_write("[");
    console_write_dec((usize)idx);
    console_write("]");
  }
  console_write(" 0x");
  console_write_hex64(addr);
  console_write(" ");
  console_write(mod);
  console_write("+0x");
  console_write_hex64(off);
  console_write("\n");
}

/* Print what ring 3 was doing when it faulted: the faulting instruction, then
 * every plausible return address still on its stack, innermost first. */
void arch_user_backtrace(struct interrupt_frame *frame) {
  struct task *t = current_task;
  if (!t || !frame)
    return;

  console_write("--- User Backtrace (");
  console_write(t->name ? t->name : "?");
  console_write(" pid ");
  console_write_dec(scheduler_get_pid());
  console_write(") ---\n");

  user_bt_line("rip", -1, frame->rip, t);
  /* The raw words too: an address that resolves to nothing is still evidence
   * (a canary, a length, a poison value), and a report that silently drops
   * them cannot be re-read later for something it was not looking for. */
  console_write("  rsp=0x");
  console_write_hex64(frame->rsp);
  console_write(":");
  for (int i = 0; i < 8; i++) {
    u64 word = 0;
    if (syscall_copyin(&word, (const void *)(frame->rsp + (u64)i * 8), 8) != 0)
      break;
    console_write(" ");
    console_write_hex64(word);
  }
  console_write("\n");
  /* The return address of the call that faulted sits at the top of the stack
   * when the callee has not pushed a frame yet — which is exactly the case for
   * __stack_chk_fail. */
  int frames = 0;
  for (int i = 0; i < USER_BT_STACK_WORDS && frames < USER_BT_MAX_FRAMES; i++) {
    u64 word = 0;
    if (syscall_copyin(&word, (const void *)(frame->rsp + (u64)i * 8), 8) != 0)
      break;
    u64 off = 0;
    if (!user_module_at(t, word, &off))
      continue;
    user_bt_line("ret", frames, word, t);
    frames++;
  }
  if (frames == 0)
    console_write("  (no return addresses on the stack)\n");
  console_write("--- End User Backtrace ---\n");
}
