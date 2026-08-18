#include <b1nix/arch.h>
#include <b1nix/console.h>
#include <b1nix/lapic.h>
#include <b1nix/memtype.h>
#include <b1nix/mm.h>
#include <b1nix/types.h>

#define X86_TSS_SELECTOR 0x28

struct x86_tss {
  u32 reserved0;
  u64 rsp0;
  u64 rsp1;
  u64 rsp2;
  u64 reserved1;
  u64 ist1;
  u64 ist2;
  u64 ist3;
  u64 ist4;
  u64 ist5;
  u64 ist6;
  u64 ist7;
  u64 reserved2;
  u16 reserved3;
  u16 iomap_base;
} __attribute__((packed));

/* One TSS per CPU. TSS.rsp0 is the ring-0 stack a CPU switches to on a ring-3
 * interrupt/exception; it is repointed at the running task's kernel stack on
 * every context switch (arch_set_kernel_stack), so each CPU needs its own TSS
 * to run userspace independently. */
static struct x86_tss x86_tss_arr[MAX_CPUS] __attribute__((aligned(16)));

/* Emergency stack for #DF (IST1). The BSP's is static because its TSS is set
 * up before the heap is usable; APs allocate theirs from the heap. See the
 * idt[8].ist comment in interrupts.c for why this exists. */
#define X86_DF_STACK_SIZE 8192
static u8 x86_df_stack_bsp[X86_DF_STACK_SIZE] __attribute__((aligned(16)));

void x86_idt_init(void);
void x86_idt_load(void); /* interrupts.c — load the shared IDT on this CPU */
void x86_pic_init(void);
void x86_timer_init(void);
void rtc_init(void);

extern void x86_syscall_entry(void);
extern u64 gdt64_tss[];      /* MAX_CPUS TSS descriptors (2 quads each) */
extern u8 gdt64_pointer[];   /* 10-byte GDT descriptor (limit:2 + base:8) */
extern char x86_syscall_stack_top[];

static void x86_enable_write_protect(void) {
  u64 cr0;
  __asm__ volatile("movq %%cr0, %0" : "=r"(cr0));
  cr0 |= (1ULL << 16);
  __asm__ volatile("movq %0, %%cr0" : : "r"(cr0) : "memory");
}

/* Build CPU `cpu`'s TSS descriptor in the shared GDT and load it (ltr). The
 * descriptor pair lives at gdt64_tss[cpu*2 .. cpu*2+1] (selector
 * X86_TSS_SELECTOR + cpu*16). */
static void x86_tss_init_cpu(int cpu) {
  struct x86_tss *t = &x86_tss_arr[cpu];
  u64 base = (u64)t;
  u32 limit = sizeof(*t) - 1;

  if (cpu == 0)
    t->rsp0 = (u64)x86_syscall_stack_top; /* boot value; updated per switch */

  if (t->ist1 == 0) {
    u8 *df = (cpu == 0) ? x86_df_stack_bsp : (u8 *)kzalloc(X86_DF_STACK_SIZE);
    if (df)
      t->ist1 = (u64)(df + X86_DF_STACK_SIZE);
  }
  t->iomap_base = sizeof(*t);

  gdt64_tss[cpu * 2 + 0] =
      ((u64)(limit & 0xffff)) | ((base & 0xffffff) << 16) | ((u64)0x89 << 40) |
      ((u64)((limit >> 16) & 0xf) << 48) | ((u64)((base >> 24) & 0xff) << 56);
  gdt64_tss[cpu * 2 + 1] = base >> 32;

  __asm__ volatile("ltr %0"
                   :
                   : "r"((u16)(X86_TSS_SELECTOR + cpu * 16))
                   : "memory");
}

static void x86_tss_init(void) { x86_tss_init_cpu(0); }

void arch_set_kernel_stack(u64 stack_top) {
  struct percpu *p = get_percpu();
  int cpu = p ? (int)p->cpu_id : 0;
  x86_tss_arr[cpu].rsp0 = stack_top;
}

/* M29: write IA32_FS_BASE (MSR 0xC0000100) for userspace TLS. The kernel
 * deliberately keeps %fs's selector pointing at the user-data descriptor
 * (see kernel/arch/x86_64/user_jump.S), so userspace `%fs:N` reads land at
 * (fs_base + N) — exactly the pthread TLS pattern. Called from the
 * scheduler on every context switch and from SYS_SET_TLS for live updates. */
/* Set when CR4.FSGSBASE is on, i.e. WRFSBASE may be used instead of WRMSR. */
static int g_fsgsbase_ready;

void arch_set_fs_base(u64 base) {
  /* WRFSBASE writes the same register WRMSR does, in a fraction of the time,
   * and this runs on every context switch — the scheduler restores the
   * outgoing thread's TLS pointer each time it swaps tasks. WRMSR is a heavy,
   * partially serialising instruction; WRFSBASE is an ordinary one.
   *
   * Only available once CR4.FSGSBASE is enabled, which x86_enable_fsgsbase
   * does after checking CPUID — hence the flag rather than a bare instruction.
   * Without it, the MSR write below is still correct, just slower. */
  if (g_fsgsbase_ready) {
    __asm__ volatile("wrfsbase %0" : : "r"(base));
    return;
  }

  u32 lo = (u32)base;
  u32 hi = (u32)(base >> 32);
  __asm__ volatile("wrmsr" : : "a"(lo), "d"(hi), "c"(0xC0000100));
}


void x86_syscall_init(void) {
  u32 lo, hi;
  /* Enable syscall/sysret by setting the SCE bit in the EFER MSR. */
  __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080));
  lo |= 1; /* SCE (System Call Enable) */
  /* M95: EFER.NXE (bit 11) — makes PTE bit 63 mean "no execute" instead of a
   * reserved bit that must stay zero. The module loader relies on it to map a
   * module's data pages non-executable while its text stays executable and
   * read-only. Gated on CPUID.80000001H:EDX[20]; without NX support bit 63
   * remains reserved and no mapping may set it. Runs per-CPU (the BSP through
   * arch_init, every AP through x86_ap_arch_init) because EFER is a per-core
   * MSR. */
  {
    u32 eax = 0, ebx = 0, ecx = 0, edx = 0;
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(0x80000000u));
    if (eax >= 0x80000001u) {
      __asm__ volatile("cpuid"
                       : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                       : "a"(0x80000001u));
      if (edx & (1u << 20))
        lo |= (1u << 11); /* NXE */
    }
  }
  __asm__ volatile("wrmsr" : : "a"(lo), "d"(hi), "c"(0xC0000080));

  /* STAR: SYSCALL enters 0x08/0x10, SYSRET returns to 0x20/0x18. */
  hi = (0x10u << 16) | 0x08u;
  __asm__ volatile("wrmsr" : : "a"(0), "d"(hi), "c"(0xC0000081));

  u64 entry = (u64)x86_syscall_entry;
  __asm__ volatile("wrmsr"
                   :
                   : "a"((u32)entry), "d"((u32)(entry >> 32)), "c"(0xC0000082));

  __asm__ volatile("wrmsr" : : "a"(0x200), "d"(0), "c"(0xC0000084));
}

/* ── XSAVE / AVX (M80) ───────────────────────────────────────────────────────
 * With FXSAVE alone the kernel saved x87+SSE and silently dropped everything
 * above it, so a userspace thread's AVX (YMM upper halves) was clobbered by any
 * context switch. Enabling XSAVE with an explicitly chosen XCR0 fixes that and
 * makes the state the kernel manages self-describing — which is also what
 * ptrace's NT_X86_XSTATE reports. The feature set is deliberately capped at
 * x87|SSE|AVX: it keeps the per-task area at a fixed, modest size and covers
 * everything the userspace toolchain emits. */
#define XCR0_X87 0x1
#define XCR0_SSE 0x2
#define XCR0_AVX 0x4

static int g_xsave_enabled;
static u64 g_xsave_mask;
static u32 g_xsave_size;

int arch_xsave_enabled(void) { return g_xsave_enabled; }
u64 arch_xsave_mask(void) { return g_xsave_mask; }
usize arch_xsave_area_size(void) { return (usize)g_xsave_size; }

static void cpuid_count(u32 leaf, u32 sub, u32 *a, u32 *b, u32 *c, u32 *d) {
  __asm__ volatile("cpuid"
                   : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                   : "a"(leaf), "c"(sub));
}

/* Measured processor frequency in kHz (0 = never measured). Written once by the
 * PIT calibration in lapic.c, read by /proc/cpuinfo and the sysfs cpufreq
 * files. */
static u32 g_cpu_khz;

void arch_set_cpu_khz(u32 khz) {
  /* Ignore an implausible measurement rather than publishing it: a value from
   * a window the hypervisor stretched is worse than no value. */
  if (khz >= 100000u && khz <= 20000000u)
    g_cpu_khz = khz;
}

u32 arch_cpu_khz(void) { return g_cpu_khz; }

/* ── A clock with better than 10 ms resolution ──────────────────────────────
 *
 * clock_gettime was derived from the 100 Hz tick, so every reading landed on a
 * 10 ms boundary. That is coarse enough that a program scheduling work in
 * milliseconds — which is what a browser's task queue is — cannot tell two
 * events apart, and measured durations come out as 0 or 10 ms and nothing
 * between. The cycle counter is already calibrated for CPU-time accounting;
 * this exposes it as the monotonic clock.
 *
 * Only when the CPU says the counter is fit for it: an invariant TSC ticks at
 * a constant rate regardless of core frequency or C-states (CPUID
 * 0x80000007:EDX bit 8). Without that guarantee the tick stays authoritative —
 * a clock that speeds up and slows down with the core is worse than a coarse
 * one. */
static u64 g_tsc_base;      /* counter value the monotonic clock starts from */
static int g_tsc_usable;    /* invariant TSC + a calibrated frequency */
static u64 g_tsc_last_ns;   /* last value handed out, for monotonicity */

static inline u64 arch_rdtsc_ordered(void) {
  u32 lo, hi;
  __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi));
  return ((u64)hi << 32) | lo;
}

void arch_tsc_clock_init(void) {
  u32 a, b, c, d;

  cpuid_count(0x80000000u, 0, &a, &b, &c, &d);
  u32 max_ext = a;
  u32 pm = 0;

  if (max_ext >= 0x80000007u) {
    cpuid_count(0x80000007u, 0, &a, &b, &c, &d);
    pm = d;
  }
  /* Say what the CPU answered. "no invariant TSC" is a conclusion, and when it
   * is wrong the only way to tell is to see the numbers it was drawn from —
   * a guest can be told it has the feature and still report otherwise. */
  console_write("tsc: max_ext=0x");
  console_write_hex64(max_ext);
  console_write(" leaf7_edx=0x");
  console_write_hex64(pm);
  console_write(" khz=");
  console_write_dec(g_cpu_khz);
  console_write("\n");

  if (max_ext < 0x80000007u)
    return;
  if (!(pm & (1u << 8)))
    return; /* not invariant — leave the tick clock in charge */
  if (!g_cpu_khz)
    return; /* never calibrated */
  g_tsc_base = arch_rdtsc_ordered();
  g_tsc_usable = 1;
}

int arch_tsc_clock_ready(void) { return g_tsc_usable; }

/* Nanoseconds since arch_tsc_clock_init(), or 0 when the counter is not fit to
 * be a clock (the caller then falls back to the tick). */
u64 arch_tsc_monotonic_ns(void) {
  if (!g_tsc_usable)
    return 0;

  u64 cycles = arch_rdtsc_ordered() - g_tsc_base;
  u32 khz = g_cpu_khz;

  /* cycles * 1000000 / khz without overflowing: split the division so the
   * multiply only ever sees the remainder. A bare multiply overflows a u64
   * after about five hours at 3 GHz. */
  u64 ns = (cycles / khz) * 1000000ull + ((cycles % khz) * 1000000ull) / khz;

  /* Never go backwards. Cores can start their counters at slightly different
   * values, and a thread that migrates mid-read would otherwise see time
   * reverse — which breaks every duration a program computes from it. */
  u64 last = __atomic_load_n(&g_tsc_last_ns, __ATOMIC_RELAXED);
  while (ns < last) {
    if (__atomic_compare_exchange_n(&g_tsc_last_ns, &last, last, 0,
                                    __ATOMIC_RELAXED, __ATOMIC_RELAXED))
      return last;
  }
  __atomic_store_n(&g_tsc_last_ns, ns, __ATOMIC_RELAXED);
  return ns;
}

/* The processor's nominal maximum, from CPUID leaf 16h when the CPU publishes
 * it (EBX = max frequency in MHz). Falls back to the measured rate. */
u32 arch_cpu_max_khz(void) {
  u32 a, b, c, d;
  cpuid_count(0, 0, &a, &b, &c, &d);
  if (a >= 0x16) {
    cpuid_count(0x16, 0, &a, &b, &c, &d);
    if (b)
      return b * 1000u;
  }
  return g_cpu_khz;
}

/* The TSC frequency as the processor states it, in kHz, or 0 if it does not.
 *
 * CPUID leaf 15h gives the ratio of the TSC to the core crystal clock —
 * EBX/EAX — and, on parts that fill it in, the crystal frequency itself in ECX.
 * Multiply them out and the result is the exact TSC rate, with none of the
 * error a timed measurement carries. Every field can be zero (older CPUs, and
 * hypervisors that leave ECX empty even while publishing the ratio), so all
 * three are checked before the multiply; a zero return means "ask the PIT". */
u32 arch_tsc_khz_from_cpuid(void) {
  u32 a, b, c, d;
  cpuid_count(0, 0, &a, &b, &c, &d);
  if (a < 0x15)
    return 0;

  cpuid_count(0x15, 0, &a, &b, &c, &d);
  if (!a || !b || !c)
    return 0;

  /* crystal_hz * ratio / 1000, in 64-bit so a 100 MHz crystal times a ratio of
   * a few dozen cannot wrap on the way to kHz. */
  return (u32)(((u64)c * (u64)b) / ((u64)a * 1000ull));
}

/* Turn on CR4.FSGSBASE (bit 16) when the CPU has it (CPUID.7.0:EBX[0]).
 *
 * This unlocks the RD/WRFSBASE instructions, which arch_set_fs_base uses in
 * place of a WRMSR on every context switch. Enabling the bit also makes them
 * available to userspace — that is what the bit means, and what every other
 * x86_64 kernel does with it. */
static void x86_enable_fsgsbase(void) {
  u32 a, b, c, d;
  cpuid_count(0, 0, &a, &b, &c, &d);
  if (a < 7)
    return;

  cpuid_count(7, 0, &a, &b, &c, &d);
  if (!(b & (1u << 0)))
    return;

  u64 cr4;
  __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
  cr4 |= (1ull << 16); /* FSGSBASE */
  __asm__ volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");

  g_fsgsbase_ready = 1;
}

/* Turn on CR4.SMEP (bit 20) when the CPU has it (CPUID.7.0:EBX[7]).
 *
 * SMEP makes the processor refuse to execute a page marked user-accessible
 * while it is in ring 0. The kernel never intends to do that, so the feature
 * costs nothing and turns a whole class of attacks — get the kernel to jump
 * into a page userspace controls — into an immediate fault.
 *
 * What blocked it before was the low identity map. Every address space carries
 * one, the code the kernel runs down there (the boot stub, and the trampoline
 * each AP starts on at 0x8000) lives inside it, and a page in that window that
 * userspace owns must be user-accessible. The two are separated by ownership,
 * not by address: the identity window is mapped with supervisor 2 MiB pages,
 * and where a process takes a page inside it the huge page is split and only
 * that one leaf becomes the process's. The 511 neighbours — the trampoline
 * among them — stay supervisor, so SMEP has nothing to complain about.
 *
 * Enabling is also late by design. This runs from arch_init, long after the
 * boot stub has jumped to the high half, and from x86_ap_arch_init, which an
 * AP reaches only once it is executing high-half C code — neither trampoline
 * ever runs with the bit set.
 *
 * The CR4 read-back is not decoration: it is the only evidence the bit is
 * really on. The BSP prints its own; every CPU that succeeds also records
 * itself in g_smep_cpus, which arch_smep_cpu_count reports once the APs are
 * up. An AP must not print here — its line lands in the middle of the one
 * smp_boot_aps is writing and eats the marker the smoke suite greps for. */
static volatile u32 g_smep_cpus;

static void x86_enable_smep(int announce) {
  u32 a, b, c, d;
  cpuid_count(0, 0, &a, &b, &c, &d);
  if (a < 7) {
    if (announce)
      console_write("smep: unavailable (no CPUID leaf 7)\n");
    return;
  }

  cpuid_count(7, 0, &a, &b, &c, &d);
  if (!(b & (1u << 7))) {
    if (announce)
      console_write("smep: unavailable (not reported by CPUID.7.0:EBX[7])\n");
    return;
  }

  u64 cr4;
  __asm__ volatile("movq %%cr4, %0" : "=r"(cr4));
  cr4 |= (1ull << 20); /* SMEP */
  __asm__ volatile("movq %0, %%cr4" : : "r"(cr4) : "memory");

  __asm__ volatile("movq %%cr4, %0" : "=r"(cr4));
  if (!(cr4 & (1ull << 20))) {
    if (announce) {
      console_write("smep: refused, cr4=0x");
      console_write_hex64(cr4);
      console_write("\n");
    }
    return;
  }

  {
    struct percpu *pc = get_percpu();
    int cpu = pc ? (int)pc->cpu_id : 0;

    if (cpu >= 0 && cpu < 32)
      __atomic_or_fetch(&g_smep_cpus, 1u << cpu, __ATOMIC_RELEASE);
  }

  if (announce) {
    console_write("smep: enabled, cr4=0x");
    console_write_hex64(cr4);
    console_write("\n");
  }
}

/* How many CPUs are running with CR4.SMEP set. Read after AP bring-up. */
int arch_smep_cpu_count(void) {
  u32 mask = __atomic_load_n(&g_smep_cpus, __ATOMIC_ACQUIRE);
  int n = 0;

  while (mask) {
    n += (int)(mask & 1u);
    mask >>= 1;
  }
  return n;
}

static void x86_enable_xsave(void) {
  u32 a, b, c, d;
  cpuid_count(1, 0, &a, &b, &c, &d);
  int has_xsave = (c & (1u << 26)) != 0;
  int has_avx = (c & (1u << 28)) != 0;
  if (!has_xsave)
    return; /* stay on FXSAVE: every save path falls back on its own */

  u64 cr4;
  __asm__ volatile("movq %%cr4, %0" : "=r"(cr4));
  cr4 |= (1ULL << 18); /* OSXSAVE — required before XGETBV/XSETBV */
  __asm__ volatile("movq %0, %%cr4" : : "r"(cr4) : "memory");

  /* Which components this CPU can be asked to manage comes from
   * CPUID.(EAX=0Dh,ECX=0):EDX:EAX — NOT from XGETBV, which merely reads back
   * the XCR0 the OS has already set (0x1 right after reset). Reading the wrong
   * one caps the mask at x87, and a later XRSTOR of an area that legitimately
   * declares SSE then #GPs. */
  cpuid_count(0x0D, 0, &a, &b, &c, &d);
  u64 supported = ((u64)d << 32) | a;
  u64 want = XCR0_X87 | XCR0_SSE;
  if (has_avx && (supported & XCR0_AVX))
    want |= XCR0_AVX;
  want &= supported;
  if ((want & (XCR0_X87 | XCR0_SSE)) != (XCR0_X87 | XCR0_SSE))
    return; /* x87+SSE is the floor; without both, stay on FXSAVE */

  __asm__ volatile("xsetbv" : : "a"((u32)want), "d"((u32)(want >> 32)), "c"(0));

  /* CPUID.(EAX=0Dh,ECX=0):EBX is the area size for the components currently
   * enabled in XCR0, which is exactly what was just written. */
  cpuid_count(0x0D, 0, &a, &b, &c, &d);
  u32 size = b;
  if (size < 576)
    size = 576; /* legacy region + XSAVE header, the minimum a CPU may report */
  if (size > ARCH_XSAVE_MAX_SIZE)
    return; /* larger than the per-task area the scheduler reserves: stay on FXSAVE */

  g_xsave_mask = want;
  g_xsave_size = size;
  g_xsave_enabled = 1;
}

static void x86_enable_sse(void) {
  u64 cr0;
  __asm__ volatile("movq %%cr0, %0" : "=r"(cr0));
  cr0 &= ~(1ULL << 2); // Clear EM (Coprocessor Emulation)
  cr0 |= (1ULL << 1);  // Set MP (Monitor Coprocessor)
  cr0 |= (1ULL << 5);  // Set NE (Numeric Error)
  __asm__ volatile("movq %0, %%cr0" : : "r"(cr0) : "memory");

  u64 cr4;
  __asm__ volatile("movq %%cr4, %0" : "=r"(cr4));
  cr4 |= (1ULL << 9);  // Set OSFXSR (FXSAVE/FXRSTOR Support)
  cr4 |= (1ULL << 10); // Set OSXMMEXCPT (SIMD Exception Support)
  __asm__ volatile("movq %0, %%cr4" : : "r"(cr4) : "memory");

  /* Per-CPU: every core must have OSXSAVE and the same XCR0, or a task moved
   * to another core would xrstor a state that core does not manage. */
  x86_enable_xsave();

  /* Also per-CPU, and for the same reason: CR4 is not shared between cores.
   * A thread that set its TLS pointer on one core and then ran on another
   * would fault on WRFSBASE if only the first core had the bit. */
  x86_enable_fsgsbase();
}

void arch_init(void) {
  x86_tss_init();
  x86_idt_init();
  x86_pic_init();
  x86_timer_init();
  rtc_init();
  x86_syscall_init();
  x86_enable_write_protect();
  x86_enable_smep(1);
  x86_enable_sse();
  /* M98: program this CPU's IA32_PAT so VMM_WC means write-combining. */
  pat_init_cpu();
  __asm__ volatile("sti");
  console_write("arch: x86_64 initialized (syscalls enabled)\n");
}

/* Per-CPU arch init for an Application Processor, run once from ap_main before
 * the AP may execute ring 3. The AP arrives on the trampoline's minimal GDT
 * (no user segments, no TSS) with no IDT and with the SYSCALL/SSE MSRs unset,
 * so it must replicate the BSP's arch_init for itself. */
void x86_ap_arch_init(int cpu) {
  /* Switch to the kernel GDT (it has the user code/data segments and every
   * CPU's TSS descriptor). Reload the data segments and CS, but NEVER %gs:
   * reloading a GS selector in long mode resets the GS base, which holds this
   * CPU's per-CPU pointer (the trampoline set it via wrmsr; b1nix uses no
   * SWAPGS). CS is reloaded with a far return to the kernel code selector. */
  __asm__ volatile("lgdt (%0)" : : "r"(gdt64_pointer) : "memory");
  __asm__ volatile("movw $0x10, %%ax\n\t"
                   "movw %%ax, %%ds\n\t"
                   "movw %%ax, %%es\n\t"
                   "movw %%ax, %%ss\n\t"
                   "movw %%ax, %%fs\n\t"
                   "pushq $0x08\n\t"          /* CS */
                   "leaq 1f(%%rip), %%rax\n\t"
                   "pushq %%rax\n\t"          /* RIP */
                   "lretq\n\t"
                   "1:\n\t"
                   :
                   :
                   : "rax", "memory");

  x86_idt_load();         /* shared kernel IDT — page faults/exceptions on the AP */
  x86_tss_init_cpu(cpu);  /* this CPU's TSS + ltr (ring-3 interrupts need rsp0) */
  x86_syscall_init();     /* per-CPU SYSCALL MSRs: EFER.SCE, STAR, LSTAR, FMASK */
  x86_enable_sse();       /* per-CPU CR0/CR4 for fxsave/fxrstor in ctx switch */
  pat_init_cpu();         /* per-CPU IA32_PAT: WC PTEs mean WC on this core too */
  x86_enable_write_protect();
  x86_enable_smep(0);     /* per-CPU CR4 bit; the AP is past its trampoline here */
  /* Software-enable this AP's LAPIC + TPR/LVT setup. Without this the AP's
   * LAPIC stays in its reset (software-disabled) state and every locally-
   * delivered vector — including the LAPIC timer we arm later in ap_main
   * (M28-A) and the TLB shootdown IPI (M28 #5) — is silently dropped.
   * Prior to this call landing the timer "worked" because no smoke check
   * actually depended on AP ticks doing anything visible; the shootdown
   * IPI does, which is how the gap surfaced. */
  lapic_init_local();
}

void arch_halt(void) {
  /* QEMU isa-debug-exit: exit with status (val << 1) | 1. */
  __asm__ volatile("outb %0, %1" : : "a"((u8)0), "Nd"((u16)0xf4));

  for (;;) {
    __asm__ volatile("hlt");
  }
}
