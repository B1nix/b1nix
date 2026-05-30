#include <b1nix/arch_x86.h>
#include <b1nix/arch.h>
#include <b1nix/bkl.h>
#include <b1nix/console.h>
#include <b1nix/io.h>
#include <b1nix/ioapic.h>
#include <b1nix/lapic.h>
#include <b1nix/mm.h>
#include <b1nix/panic.h>
#include <b1nix/sched.h>
#include <b1nix/net.h>
#include <b1nix/tlb.h>
#include <b1nix/types.h>

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
   * in kernel/arch/x86/tlb.c which invlpg's the published vaddr (or reloads
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
    if (is_bsp) {
      timer_ticks++;
      if (timer_ticks % 50 == 0) {
        fb_console_blink_cursor();
      }
      scheduler_on_timer_tick();
    }
    return;
  }

  if (frame->vector == 32) {
    /* Legacy PIT IRQ0 fallback. With M28-A the LAPIC timer drives the scheduler
     * tick on all cores and PIT IRQ0 is masked at the IOAPIC; this branch is
     * kept so an unsuspected stray (e.g. calibration failed → IOAPIC mask
     * skipped) still progresses the BSP tick instead of console-spamming. */
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
  if (frame->vector >= 32 && frame->vector <= 47) {
    int irq = frame->vector - 32;
    if (irq == net_get_irq()) {
      net_interrupt_handler();
      irq_eoi(frame->vector);
      return;
    }
  }

  console_write("\nIRQ: unexpected vector 0x");
  console_write_hex64(frame->vector);
  console_write("\n");
  irq_eoi(frame->vector);

  if (frame->cs == 0x1B || frame->cs == 0x23) {
    arch_check_and_deliver_signals(frame);
  }
}

/* Interrupt entry from a userspace core acquires the Big Kernel Lock; a nested
 * interrupt on a core already holding it (e.g. a timer tick mid-syscall) just
 * recurses the depth. The matching release happens on the normal return; paths
 * that never return (a fatal signal terminating the task via
 * scheduler_exit_current) hand the depth-1 lock off across the context switch,
 * so skipping bkl_unlock there is correct.
 *
 * **TLB shootdown IPI (vector 65) is the one exception** — the initiator of
 * the shootdown holds the BKL and synchronously waits for every target to
 * ACK; if a target tried to acquire the BKL, the initiator would never
 * release it and the system deadlocks. The handler is trivial (invlpg +
 * atomic decrement + EOI), touches no BKL-protected state, and runs with
 * IRQs implicitly disabled at the LAPIC level, so skipping BKL here is safe.
 */
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
  bkl_lock();
  x86_irq_handler_inner(frame);
  bkl_unlock();
}

static void x86_exception_handler_inner(struct interrupt_frame *frame) {
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

  console_write("\nEXCEPTION: ");
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
  console_write("\n");

  arch_backtrace(frame->rbp, frame->rip);

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

    console_write("fatal signal ");
    console_write_dec(sig);
    console_write(" in pid ");
    console_write_hex64(pid);
    console_write(" (no handler): terminating\n");
    scheduler_exit_current(128 + sig);
    /* scheduler_exit_current never returns */
    arch_halt();
  }

  console_write("[PANIC] unhandled CPU exception\n");
  arch_halt();
}

/* Exception entry takes the Big Kernel Lock (recursively if the faulting CPU
 * already held it, e.g. a demand-paging fault while copying a user buffer
 * mid-syscall). Released on normal return; a fault that terminates the task
 * (scheduler_exit_current) hands the lock off across the context switch. */
void x86_exception_handler(struct interrupt_frame *frame) {
  bkl_lock();
  x86_exception_handler_inner(frame);
  bkl_unlock();
}

/* ── Stack Backtrace ──────────────────────────────────────────── */
#define MAX_BACKTRACE_FRAMES 32

static int addr_is_kernel_text(u64 addr) {
  /* Kernel .text is identity-mapped in the 0x100000-0x200000 range
     (the linker starts at 1M, and the kernel is a few hundred KB).
     Also accept higher-half direct-map addresses. */
  return (addr >= 0x100000ULL && addr <= 0x200000ULL) ||
         (addr >= 0xffff800000000000ULL && addr <= 0xffff800100000000ULL);
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
        frames++;
      }
      scan_rbp = *(volatile u64 *)scan_rbp;
    }
  }

  if (frames == 0)
    console_write("  (no frames)");

  console_write("\n--- End Backtrace ---\n");
}
