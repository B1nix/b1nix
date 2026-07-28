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
#include <b1nix/spinlock.h>
#include <b1nix/serial_tty.h>
#include <b1nix/net.h>
#include <b1nix/tlb.h>
#include <b1nix/types.h>
#include <stdio.h>
#include <string.h>

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
    scheduler_charge_tick(frame->cs == 0x1B || frame->cs == 0x23);
    if (is_bsp) {
      timer_ticks++;
      if (timer_ticks % 50 == 0) {
        fb_console_blink_cursor();
      }

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
    scheduler_charge_tick(frame->cs == 0x1B || frame->cs == 0x23);
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
  if (frame->vector >= 32 && frame->vector <= 47) {
    int irq = frame->vector - 32;
    int handled = 0;
    if (irq == net_get_irq()) {
      net_interrupt_handler();
      handled = 1;
    }
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
void x86_irq_handler(struct interrupt_frame *frame) {
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

    if (vmm_handle_page_fault(fault_addr, error_code) == 0) {
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
  static volatile int in_fault_dump;
  if (in_fault_dump) {
    console_write("\n[nested fault while dumping exception — aborting]\n");
    panic("nested exception in fault handler");
  }
  in_fault_dump = 1;
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
  in_fault_dump = 0;

  /* If exception happened in userspace (CS == 0x1B), send signal instead of
   * panic */
  if (frame->cs == 0x1B || frame->cs == 0x23) {
    int sig = 0;
    switch (frame->vector) {
    case 0:
      sig = SIGFPE;
      break; /* #DE divide error */
    case 4:
      sig = SIGILL;
      break; /* #OF overflow */
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
    usize pid = scheduler_get_pid();
    struct sigaction *sa = &current_task->sigactions[sig - 1];
    int is_blocked = (current_task->blocked_signals >> (sig - 1)) & 1ULL;
    int has_handler =
        (sa->sa_handler != SIG_DFL && sa->sa_handler != SIG_IGN);

    if (has_handler && !is_blocked) {
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
        "?",      "SIGABRT", "SIGALRM", "SIGBUS",  "SIGCHLD", "SIGCONT",
        "SIGFPE", "SIGHUP",  "SIGILL",  "SIGINT",  "SIGKILL", "SIGPIPE",
        "SIGQUIT","SIGSEGV", "SIGSTOP", "SIGTERM", "SIGTSTP", "SIGTTIN",
        "SIGTTOU"
    };
    const char *sname = (sig > 0 && sig < (int)(sizeof(sig_name)/sizeof(*sig_name)))
                          ? sig_name[sig] : "?";
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
  x86_exception_handler_inner(frame);
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
