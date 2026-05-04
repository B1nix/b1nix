#include <b1nix/console.h>
#include <b1nix/io.h>
#include <b1nix/mm.h>
#include <b1nix/panic.h>
#include <b1nix/sched.h>
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

struct interrupt_frame {
	u64 rax;
	u64 rbx;
	u64 rcx;
	u64 rdx;
	u64 rbp;
	u64 rdi;
	u64 rsi;
	u64 r8;
	u64 r9;
	u64 r10;
	u64 r11;
	u64 r12;
	u64 r13;
	u64 r14;
	u64 r15;
	u64 vector;
	u64 error_code;
	u64 rip;
	u64 cs;
	u64 rflags;
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

static volatile u64 timer_ticks;

static u64 read_cr2(void)
{
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

static void idt_set_gate(u8 vector, void (*handler)(void))
{
	u64 address = (u64)handler;

	idt[vector].offset_low = (u16)(address & 0xffff);
	idt[vector].selector = KERNEL_CODE_SELECTOR;
	idt[vector].ist = 0;
	idt[vector].type_attr = IDT_INTERRUPT_GATE;
	idt[vector].offset_mid = (u16)((address >> 16) & 0xffff);
	idt[vector].offset_high = (u32)((address >> 32) & 0xffffffff);
	idt[vector].zero = 0;
}

void x86_idt_init(void)
{
	void (*handlers[])(void) = {
		isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7,
		isr8, isr9, isr10, isr11, isr12, isr13, isr14, isr15,
		isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23,
		isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31,
	};

	for (u8 i = 0; i < 32; i++) {
		idt_set_gate(i, handlers[i]);
	}
	idt_set_gate(32, isr32);
	idt_set_gate(33, isr33);

	struct idt_pointer pointer = {
		.limit = sizeof(idt) - 1,
		.base = (u64)&idt,
	};

	__asm__ volatile("lidt %0" : : "m"(pointer));
}

void x86_pic_init(void)
{
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

	outb(PIC1_DATA, 0xfc); // Unmask IRQ0 and IRQ1
	outb(PIC2_DATA, 0xff);
}

void x86_timer_init(void)
{
	u16 divisor = (u16)(PIT_FREQUENCY / TIMER_HZ);

	outb(PIT_COMMAND, 0x36);
	outb(PIT_CHANNEL0, (u8)(divisor & 0xff));
	outb(PIT_CHANNEL0, (u8)((divisor >> 8) & 0xff));

	console_write("timer: pit 100hz initialized\n");
}

extern void ps2_kbd_interrupt_handler(void);

void x86_irq_handler(struct interrupt_frame *frame)
{
	if (frame->vector == 32) {
		timer_ticks++;
		outb(PIC1_COMMAND, PIC_EOI);
		scheduler_on_timer_tick();
		return;
	}

	if (frame->vector == 33) {
		ps2_kbd_interrupt_handler();
		outb(PIC1_COMMAND, PIC_EOI);
		return;
	}

	console_write("\nIRQ: unexpected vector 0x");
	console_write_hex64(frame->vector);
	console_write("\n");
	outb(PIC1_COMMAND, PIC_EOI);
}

void x86_exception_handler(struct interrupt_frame *frame)
{
	// Page fault handling for Demand Paging
	if (frame->vector == 14) {
		u64 fault_addr = read_cr2();
		u64 error_code = frame->error_code;
		
		if (vmm_handle_page_fault(fault_addr, error_code) == 0) {
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
		console_write("\ncr2:    0x");
		console_write_hex64(read_cr2());
	}
	console_write("\ncs:     0x");
	console_write_hex64(frame->cs);
	console_write("\nrflags: 0x");
	console_write_hex64(frame->rflags);
	console_write("\n");

	panic("unhandled CPU exception");
}
