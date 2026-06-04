#include <b1nix/arch_x86.h>
#include <b1nix/arch.h>
#include <b1nix/console.h>
#include <b1nix/bootinfo.h>
#include <b1nix/gdbstub.h>
#include <b1nix/io.h>
#include <b1nix/ioapic.h>
#include <b1nix/klog.h>

void coredump_write(struct interrupt_frame *frame, int sig);
#include <b1nix/lapic.h>
#include <b1nix/mm.h>
#include <b1nix/panic.h>
#include <b1nix/sched.h>
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
  u8 zero;
  u8 type_attr;
  u16 offset_high;
} __attribute__((packed));

struct idt_pointer {
  u16 limit;
  u32 base;
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

static u32 read_cr2(void) {
  u32 value;
  __asm__ volatile("movl %%cr2, %0" : "=r"(value));
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
  u32 address = (u32)handler;

  idt[vector].offset_low = (u16)(address & 0xffff);
  idt[vector].selector = KERNEL_CODE_SELECTOR;
  idt[vector].zero = 0;
  idt[vector].type_attr = IDT_INTERRUPT_GATE;
  idt[vector].offset_high = (u16)((address >> 16) & 0xffff);
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

  /* LAPIC timer (per-CPU) and LAPIC spurious gates. Installed unconditionally */
  idt_set_gate(64, isr64);
  idt_set_gate(65, isr65);
  idt_set_gate(66, isr66);
  idt_set_gate(255, isr255);

  /* Set DPL 3 for vector 0x80 (syscall) gate so userspace can trigger int $0x80 */
  extern void x86_syscall_entry(void);
  idt_set_gate(0x80, x86_syscall_entry);
  idt[0x80].type_attr = 0xee; // DPL = 3, type = interrupt gate

  struct idt_pointer pointer = {
      .limit = sizeof(idt) - 1,
      .base = (u32)&idt,
  };

  __asm__ volatile("lidt %0" : : "m"(pointer));
}

void x86_idt_load(void) {
  struct idt_pointer pointer = {
      .limit = sizeof(idt) - 1,
      .base = (u32)&idt,
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
extern void usb_kbd_poll(void);
extern void ps2_mouse_interrupt_handler(void);
extern void fb_console_blink_cursor(void);

static inline void irq_eoi(u32 vector) {
  if (ioapic_active()) {
    lapic_eoi();
    return;
  }
  int irq = (int)vector - 32;
  if (irq >= 8) outb(PIC2_COMMAND, PIC_EOI);
  outb(PIC1_COMMAND, PIC_EOI);
}

static void x86_irq_handler_inner(struct interrupt_frame *frame) {
  if (frame->vector == 255) {
    return;
  }

  if (frame->vector == 65) {
    tlb_shootdown_handler();
    return;
  }

  if (frame->vector == 66) {
    lapic_eoi();
    return;
  }

  if (frame->vector == 64) {
    struct percpu *pcpu = get_percpu();
    int is_bsp = pcpu ? (pcpu->cpu_id == 0) : 1;
    lapic_eoi();
    if (is_bsp) {
      /* Poll the i8042 from the timer tick as a keyboard fallback. The
       * scheduler runs off the LAPIC timer, so a live scheduler does NOT prove
       * the IOAPIC delivers ISA IRQs — on some bare-metal boxes (e.g. Acer
       * Aspire One ZG5) IRQ1 never arrives even though the device is fine.
       * Draining here makes the keyboard work regardless; if IRQ1 does fire,
       * this just finds an empty buffer. Both paths run on the BSP in ISR
       * context (non-reentrant), so the single-producer kbd ring is safe. */
      ps2_kbd_interrupt_handler();
      usb_kbd_poll(); /* M37: drain the USB HID keyboard's interrupt endpoint */
      timer_ticks++;
      if (timer_ticks % 50 == 0) {
        fb_console_blink_cursor();
      }
      scheduler_on_timer_tick();
    }
    return;
  }

  if (frame->vector == 32) {
    ps2_kbd_interrupt_handler(); /* i8042 poll fallback — see vector 64 above */
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

void x86_irq_handler(struct interrupt_frame *frame) {
  if (frame->vector == 65) {
    tlb_shootdown_handler();
    return;
  }
  if (frame->vector == 66) {
    lapic_eoi();
    return;
  }
  if (frame->vector == 64) {
    x86_irq_handler_inner(frame);
    return;
  }
  x86_irq_handler_inner(frame);
}

static void x86_exception_handler_inner(struct interrupt_frame *frame) {
  if ((frame->vector == 3 || frame->vector == 1) &&
      bootinfo_has_flag("b1nix.gdb")) {
    gdb_stub_enter(frame);
    return;
  }

  if (frame->vector == 14) {
    u32 fault_addr = read_cr2();
    u32 error_code = frame->error_code;

    if (vmm_handle_page_fault(fault_addr, error_code) == 0) {
      if (frame->cs == 0x1B || frame->cs == 0x23) {
        arch_check_and_deliver_signals(frame);
      }
      return;
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
  console_write("\neip:    0x");
  console_write_hex64(frame->eip);
  if (frame->vector == 14) {
    u32 cr2_val = read_cr2();
    console_write("\ncr2:    0x");
    console_write_hex64(cr2_val);
    console_write("\n");
    paging_dump_entries(cr2_val);
  }
  console_write("\ncs:     0x");
  console_write_hex64(frame->cs);
  console_write("\neflags: 0x");
  console_write_hex64(frame->eflags);
  console_write("\n");

  arch_backtrace(frame->ebp, frame->eip);

  if (frame->cs == 0x1B || frame->cs == 0x23) {
    console_write("\nuserspace stack dump (esp=0x");
    console_write_hex64(frame->esp);
    console_write("):");
    extern int syscall_copyin(void *dst, const void *user_src, unsigned long size);
    for (int i = 0; i < 32; i++) {
      u32 val = 0;
      if (syscall_copyin(&val, (void *)(frame->esp + i * 4), 4) == 0) {
        console_write("\n  +0x");
        console_write_hex64(i * 4);
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

  if (frame->cs == 0x1B || frame->cs == 0x23) {
    int sig = 0;
    switch (frame->vector) {
    case 0:
      sig = SIGFPE;
      break;
    case 4:
      sig = SIGILL;
      break;
    case 5:
      sig = SIGSEGV;
      break;
    case 6:
      sig = SIGILL;
      break;
    case 8:
      sig = SIGSEGV;
      break;
    case 11:
      sig = SIGSEGV;
      break;
    case 12:
      sig = SIGSEGV;
      break;
    case 13:
      sig = SIGSEGV;
      break;
    case 14:
      sig = SIGSEGV;
      break;
    default:
      sig = SIGTERM;
      break;
    }
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
    console_write(") at eip=0x");
    console_write_hex64(frame->eip);
    console_write(" — terminating\n");
    if (sig == SIGSEGV || sig == SIGABRT || sig == SIGILL || sig == SIGFPE ||
        sig == SIGBUS) {
      coredump_write(frame, sig);
      console_write("coredump: wrote /tmp/core\n");
    }
    scheduler_exit_current(128 + sig);
    arch_halt();
  }

  console_write("[PANIC] unhandled CPU exception\n");
  arch_halt();
}

void x86_exception_handler(struct interrupt_frame *frame) {
  x86_exception_handler_inner(frame);
}

#define MAX_BACKTRACE_FRAMES 32

static int addr_is_kernel_text(u32 addr) {
  return (addr >= 0x100000ULL && addr <= 0x200000ULL) ||
         (addr >= 0x2000000ULL && addr <= 0x3000000ULL) ||
         (addr >= 0x80000000ULL && addr <= 0xc0000000ULL);
}

static int fp_is_safe(u32 fp) {
  if (fp < 0x1000ULL || fp == (u32)-1)
    return 0;
  if (fp + 8 > 0xffffffffULL)
    return 0;
  /* arch_backtrace dereferences [fp] and [fp+4] directly from kernel mode while
   * unwinding a *faulting* task, so fp is frequently garbage (a corrupted user
   * frame pointer). A range check alone is not enough — a plausible-but-unmapped
   * user address sails through and the dereference itself page-faults in the
   * kernel, turning a recoverable userspace SIGSEGV into a kernel panic. Verify
   * both words are actually mapped in the current address space first. */
  extern u64 vmm_virt_to_phys(void *ptr);
  if (vmm_virt_to_phys((void *)(usize)fp) == 0)
    return 0;
  if (vmm_virt_to_phys((void *)(usize)(fp + 4)) == 0)
    return 0;
  return 1;
}

void arch_backtrace(u64 rbp, u64 rip) {
  int frames = 0;
  console_write("\n--- Kernel Backtrace ---\n");

  u32 ebp = (u32)rbp;
  u32 eip = (u32)rip;

  if (eip) {
    console_write("  [0] 0x");
    console_write_hex64(eip);
    ksym_print(eip);
    frames++;
  }

  for (int i = 0; i < MAX_BACKTRACE_FRAMES && ebp; i++) {
    if (!fp_is_safe(ebp))
      break;

    u32 next_ebp = 0;
    u32 ret_addr = 0;

    next_ebp = *(volatile u32 *)ebp;
    ret_addr = *(volatile u32 *)(ebp + 4);

    if (ret_addr == 0 || !addr_is_kernel_text(ret_addr))
      break;

    console_write("\n  [");
    console_write_dec(frames);
    console_write("] 0x");
    console_write_hex64(ret_addr);
    ksym_print(ret_addr);
    frames++;

    ebp = next_ebp;
  }

  if (frames <= 1) {
    u32 scan_ebp = 0;
    __asm__ volatile("movl %%ebp, %0" : "=r"(scan_ebp));

    for (int i = 0; i < MAX_BACKTRACE_FRAMES && scan_ebp; i++) {
      if (!fp_is_safe(scan_ebp))
        break;

      u32 ret = *(volatile u32 *)(scan_ebp + 4);
      if (ret && addr_is_kernel_text(ret)) {
        console_write("\n  [");
        console_write_dec(frames);
        console_write("] 0x");
        console_write_hex64(ret);
        ksym_print(ret);
        frames++;
      }
      scan_ebp = *(volatile u32 *)scan_ebp;
    }
  }

  if (frames == 0)
    console_write("  (no frames)");

  console_write("\n--- End Backtrace ---\n");
}
