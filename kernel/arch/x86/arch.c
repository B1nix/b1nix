#include <b1nix/arch.h>
#include <b1nix/console.h>
#include <b1nix/lapic.h>
#include <b1nix/types.h>
#include <string.h>

#define X86_TSS_SELECTOR 0x28

struct x86_32_tss {
  u32 prev_tss;
  u32 esp0;
  u32 ss0;
  u32 esp1;
  u32 ss1;
  u32 esp2;
  u32 ss2;
  u32 cr3;
  u32 eip;
  u32 eflags;
  u32 eax;
  u32 ecx;
  u32 edx;
  u32 ebx;
  u32 esp;
  u32 ebp;
  u32 esi;
  u32 edi;
  u32 es;
  u32 cs;
  u32 ss;
  u32 ds;
  u32 fs;
  u32 gs;
  u32 ldt;
  u16 trap;
  u16 iomap_base;
} __attribute__((packed));

static struct x86_32_tss x86_tss_arr[MAX_CPUS] __attribute__((aligned(16)));

struct gdt_entry {
  u16 limit_low;
  u16 base_low;
  u8 base_middle;
  u8 access;
  u8 granularity;
  u8 base_high;
} __attribute__((packed));

struct gdt_ptr {
  u16 limit;
  u32 base;
} __attribute__((packed));

static struct gdt_entry g_cpu_gdts[MAX_CPUS][8] __attribute__((aligned(16)));
struct gdt_ptr g_cpu_gdt_ptrs[MAX_CPUS];

extern struct gdt_entry gdt32[];
extern char x86_syscall_stack_top[];

void x86_idt_init(void);
void x86_idt_load(void);
void x86_pic_init(void);
void x86_timer_init(void);
void rtc_init(void);

static void x86_set_gdt_entry(struct gdt_entry *entry, u32 base, u32 limit, u8 access, u8 granularity) {
  entry->limit_low = limit & 0xffff;
  entry->base_low = base & 0xffff;
  entry->base_middle = (base >> 16) & 0xff;
  entry->access = access;
  entry->granularity = ((limit >> 16) & 0x0f) | (granularity & 0xf0);
  entry->base_high = (base >> 24) & 0xff;
}

static void x86_gdt_init_cpu(int cpu) {
  /* Copy template GDT */
  memcpy(g_cpu_gdts[cpu], gdt32, 8 * sizeof(struct gdt_entry));
  
  u32 *src = (u32 *)&gdt32[1];
  console_write("gdt32[1]: 0x");
  console_write_hex32(src[1]);
  console_write_hex32(src[0]);
  console_write("\n");

  u32 *desc = (u32 *)&g_cpu_gdts[cpu][1];
  console_write("g_cpu_gdts[0][1]: 0x");
  console_write_hex32(desc[1]);
  console_write_hex32(desc[0]);
  console_write("\n");
  
  /* Set up TSS descriptor in entry 5 */
  struct x86_32_tss *t = &x86_tss_arr[cpu];
  x86_set_gdt_entry(&g_cpu_gdts[cpu][5], (u32)t, sizeof(*t) - 1, 0x89, 0x00);
  
  /* Set up Kernel per-CPU descriptor in entry 7 */
  extern struct percpu boot_cpu_data;
  extern struct percpu *ap_cpu_data[MAX_CPUS];
  struct percpu *pcpu = (cpu == 0) ? &boot_cpu_data : ap_cpu_data[cpu];
  pcpu->self_ptr = (u32)pcpu;
  x86_set_gdt_entry(&g_cpu_gdts[cpu][7], (u32)pcpu, sizeof(struct percpu) - 1, 0x92, 0xcf);
  
  /* Set up User TLS in entry 6 */
  x86_set_gdt_entry(&g_cpu_gdts[cpu][6], 0, 0xfffff, 0xf2, 0xcf);

  g_cpu_gdt_ptrs[cpu].limit = 8 * sizeof(struct gdt_entry) - 1;
  g_cpu_gdt_ptrs[cpu].base = (u32)&g_cpu_gdts[cpu];

  __asm__ volatile("lgdt %0" : : "m"(g_cpu_gdt_ptrs[cpu]) : "memory");
  __asm__ volatile("ltr %0" : : "r"((u16)0x28) : "memory");
  __asm__ volatile("movw $0x38, %%ax\n\t"
                   "movw %%ax, %%fs\n\t"
                   : : : "ax", "memory");
}

static void x86_enable_write_protect(void) {
  u32 cr0;
  __asm__ volatile("movl %%cr0, %0" : "=r"(cr0));
  cr0 |= (1UL << 16);
  __asm__ volatile("movl %0, %%cr0" : : "r"(cr0) : "memory");
}

static void x86_tss_init(void) {
  struct x86_32_tss *t = &x86_tss_arr[0];
  t->esp0 = (u32)x86_syscall_stack_top;
  t->ss0 = 0x10;
  t->iomap_base = sizeof(*t);

  x86_gdt_init_cpu(0);
}

void arch_set_kernel_stack(u64 stack_top) {
  struct percpu *p = get_percpu();
  int cpu = p ? (int)p->cpu_id : 0;
  x86_tss_arr[cpu].esp0 = (u32)stack_top;
}

void arch_set_fs_base(u64 base) {
  struct percpu *p = get_percpu();
  int cpu = p ? (int)p->cpu_id : 0;
  
  x86_set_gdt_entry(&g_cpu_gdts[cpu][6], (u32)base, 0xfffff, 0xf2, 0xcf);
  
  /* Reload GS to apply TLS change */
  __asm__ volatile("movw $0x33, %ax\n\t"
                   "movw %ax, %gs\n\t");
}

void arch_set_fs_base_percpu(u32 base) {
  struct percpu *p = get_percpu();
  int cpu = p ? (int)p->cpu_id : 0;
  x86_set_gdt_entry(&g_cpu_gdts[cpu][7], base, sizeof(struct percpu) - 1, 0x92, 0xcf);
  if (cpu == 0) {
    x86_set_gdt_entry(&gdt32[7], base, sizeof(struct percpu) - 1, 0x92, 0xcf);
  }
  __asm__ volatile("movw $0x38, %%ax\n\t"
                   "movw %%ax, %%fs\n\t"
                   : : : "ax", "memory");
}

u32 arch_get_fs_base_percpu(void) {
  u16 selector;
  __asm__ volatile("movw %%fs, %0" : "=r"(selector));
  if (selector != 0x38) return 0;
  u32 base;
  __asm__ volatile("movl %%fs:0x0c, %0" : "=r"(base) : : "memory");
  return base;
}

static void x86_enable_sse(void) {
  u32 cr0;
  __asm__ volatile("movl %%cr0, %0" : "=r"(cr0));
  cr0 &= ~(1UL << 2);
  cr0 |= (1UL << 1);
  cr0 |= (1UL << 5);
  __asm__ volatile("movl %0, %%cr0" : : "r"(cr0) : "memory");

  u32 cr4;
  __asm__ volatile("movl %%cr4, %0" : "=r"(cr4));
  cr4 |= (1UL << 9);
  cr4 |= (1UL << 10);
  __asm__ volatile("movl %0, %%cr4" : : "r"(cr4) : "memory");
}

void arch_init(void) {
  x86_tss_init();
  x86_idt_init();
  x86_pic_init();
  x86_timer_init();
  rtc_init();
  x86_enable_write_protect();
  x86_enable_sse();
  __asm__ volatile("sti");
  console_write("arch: x86 32-bit initialized\n");
}

void x86_ap_arch_init(int cpu) {
  x86_gdt_init_cpu(cpu);

  __asm__ volatile("movw $0x10, %%ax\n\t"
                   "movw %%ax, %%ds\n\t"
                   "movw %%ax, %%es\n\t"
                   "movw %%ax, %%ss\n\t"
                   "pushl $0x08\n\t"
                   "pushl $1f\n\t"
                   "lret\n\t"
                   "1:\n\t"
                   : : : "ax", "memory");

  x86_idt_load();
  x86_enable_sse();
  x86_enable_write_protect();
  lapic_init_local();
}

void arch_halt(void) {
  __asm__ volatile("outb %0, %1" : : "a"((u8)0), "Nd"((u16)0xf4));
  for (;;) {
    __asm__ volatile("hlt");
  }
}
