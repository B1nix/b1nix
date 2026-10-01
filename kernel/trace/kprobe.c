/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * kprobes — a breakpoint over a kernel function's first instruction.
 *
 * HOW IT WORKS
 *
 * Arming writes 0xCC (int3) over the symbol's first byte and keeps the byte it
 * replaced. When the function is entered the CPU traps: the #BP handler sees
 * that RIP-1 is a probe's address, reports the hit to the tracepoint the probe
 * belongs to, puts the original byte back, rewinds RIP by one and sets TF so
 * the instruction that was displaced executes exactly as written. The #DB that
 * TF produces immediately afterwards writes the int3 back and clears TF. The
 * function therefore runs unmodified; the only cost is two traps per call of a
 * probed function, and nothing at all for an unprobed one.
 *
 * WHAT IT DOES NOT DO
 *
 * A return probe (`r:name symbol`) is refused. A kretprobe needs a trampoline
 * planted on the return address of each live invocation, one instance per
 * frame; nothing here keeps that book, and a probe that silently fired at entry
 * while claiming to be a return probe would report the wrong thing.
 *
 * The single-step window is per-CPU and the patched byte is not: while one CPU
 * steps the displaced instruction, another CPU entering the same function runs
 * it without trapping, so a hit can be missed under contention. Linux avoids
 * that with per-CPU instruction buffers (an out-of-line single step); the
 * simpler scheme here costs an occasional undercount and never a wrong answer.
 * The counter a probe reports is therefore a lower bound on a busy SMP machine,
 * which is why `perf stat` on a kprobe is honest about being a sample of the
 * calls rather than a tally of them.
 */

#include <b1nix/kprobe.h>

#include <b1nix/arch.h>
#include <b1nix/errno.h>
#include <b1nix/klog.h>
#include <b1nix/lapic.h>
#include <b1nix/spinlock.h>
#include <b1nix/vfs.h>
#include <b1nix/sched.h>
#include <b1nix/mm.h>
#include <b1nix/tracepoint.h>
#include <b1nix/user.h>

#include <stdio.h>
#include <string.h>

#define KPROBE_MAX 32
#define KPROBE_INT3 0xCCu

/* The trap mechanism here is x86's int3 and TF. aarch64 has BRK and a
 * single-step bit that would do the same job, but nothing here plants them yet,
 * so a probe on that architecture is refused rather than pretended. */
#if !defined(__x86_64__) && !defined(__aarch64__)
int kprobe_symbol_ok(const char *symbol) { (void)symbol; return 0; }
const char *kprobe_symbol_of(u16 id) { (void)id; return "?"; }
int kprobe_arm(const char *symbol, u16 id, int is_return) {
  (void)symbol; (void)id; (void)is_return;
  return -EOPNOTSUPP;
}
int kprobe_disarm(u16 id) { (void)id; return -ENOENT; }
int kprobe_handle_bp(struct interrupt_frame *frame) { (void)frame; return 0; }
int kprobe_handle_db(struct interrupt_frame *frame) { (void)frame; return 0; }
const void *kprobe_current_frame(void) { return 0; }
int uprobe_arm(u16 id, const void *inode, u64 offset) {
  (void)id; (void)inode; (void)offset;
  return -EOPNOTSUPP;
}
int uprobe_disarm(u16 id) { (void)id; return -ENOENT; }
void uprobe_page_mapped(u64 va) { (void)va; }
int uprobe_handle_bp(struct interrupt_frame *frame) { (void)frame; return 0; }
int uprobe_handle_db(struct interrupt_frame *frame) { (void)frame; return 0; }
#elif defined(__x86_64__)

/* The trap frame of the probe being reported on each CPU, for the whole of
 * the report: an attached program reads the probed context's registers from
 * it (bpf_run_trace). */
static const struct interrupt_frame *g_kp_frame[MAX_CPUS];
static unsigned kprobe_this_cpu(void);

const void *kprobe_current_frame(void) {
  unsigned cpu = kprobe_this_cpu();

  return cpu < MAX_CPUS ? g_kp_frame[cpu] : 0;
}

struct kprobe {
  u64 addr;    /* the patched byte's address */
  u8 orig;     /* what was there */
  u8 patched;  /* is the int3 in place right now? */
  u16 id;      /* the tracepoint this probe is */
  char symbol[64];
  int used;
};

static struct kprobe g_kp[KPROBE_MAX];
static spinlock_t g_kp_lock = SPINLOCK_INIT;

/* The probe each CPU is in the middle of single-stepping, +1 so that zero
 * means "none". A CPU steps one instruction at a time, so one slot each. */
static volatile u32 g_stepping[MAX_CPUS];
/* Whether interrupts were on where the probe hit. The step runs with them off:
 * an interrupt between the return from the int3 and the stepped instruction
 * could preempt the task and resume it on another CPU, whose #DB then finds
 * nothing in its g_stepping slot -- a "debug" panic in the probed function one
 * byte in, seen with bpftrace's kprobe on vfs_find_node. Linux single-steps a
 * kprobe with IF clear for the same reason. */
static volatile u8 g_step_if[MAX_CPUS];

static unsigned kprobe_this_cpu(void) {
  struct percpu *pc = get_percpu();

  return pc ? (unsigned)pc->cpu_id : 0;
}

/* The one place that changes the text. A byte store is atomic on x86 and the
 * instruction stream is coherent with it on the same core; another core may be
 * executing the old byte, which is what makes the int3 (one byte, never
 * straddling an instruction boundary) the only safe thing to write here. */
static void kprobe_poke(u64 addr, u8 value) {
  *(volatile u8 *)(usize)addr = value;
  __asm__ volatile("" ::: "memory");
}


int kprobe_symbol_ok(const char *symbol) {
  return symbol && *symbol && ksym_addr_of(symbol) != 0;
}

const char *kprobe_symbol_of(u16 id) {
  for (usize i = 0; i < KPROBE_MAX; i++)
    if (g_kp[i].used && g_kp[i].id == id)
      return g_kp[i].symbol;
  return "?";
}

int kprobe_arm(const char *symbol, u16 id, int is_return) {
  u64 flags;
  u64 addr;
  int slot = -1;

  if (is_return)
    return -EOPNOTSUPP; /* no return trampoline: see the file comment */
  addr = symbol ? ksym_addr_of(symbol) : 0;
  if (!addr)
    return -ENOENT;
  spin_lock_irqsave(&g_kp_lock, &flags);
  for (usize i = 0; i < KPROBE_MAX; i++) {
    if (g_kp[i].used && g_kp[i].id == id) {
      /* Already known: just put the byte back in. */
      if (!g_kp[i].patched) {
        g_kp[i].orig = *(volatile u8 *)(usize)g_kp[i].addr;
        kprobe_poke(g_kp[i].addr, KPROBE_INT3);
        g_kp[i].patched = 1;
      }
      spin_unlock_irqrestore(&g_kp_lock, flags);
      return 0;
    }
    if (g_kp[i].used && g_kp[i].addr == addr) {
      spin_unlock_irqrestore(&g_kp_lock, flags);
      return -EEXIST; /* two probes on one byte cannot both keep the original */
    }
    if (!g_kp[i].used && slot < 0)
      slot = (int)i;
  }
  if (slot < 0) {
    spin_unlock_irqrestore(&g_kp_lock, flags);
    return -ENOSPC;
  }
  g_kp[slot].addr = addr;
  g_kp[slot].orig = *(volatile u8 *)(usize)addr;
  g_kp[slot].id = id;
  strncpy(g_kp[slot].symbol, symbol, sizeof(g_kp[slot].symbol) - 1);
  g_kp[slot].symbol[sizeof(g_kp[slot].symbol) - 1] = '\0';
  g_kp[slot].used = 1;
  kprobe_poke(addr, KPROBE_INT3);
  g_kp[slot].patched = 1;
  spin_unlock_irqrestore(&g_kp_lock, flags);
  return 0;
}

int kprobe_disarm(u16 id) {
  u64 flags;
  int rc = -ENOENT;

  spin_lock_irqsave(&g_kp_lock, &flags);
  for (usize i = 0; i < KPROBE_MAX; i++) {
    if (!g_kp[i].used || g_kp[i].id != id)
      continue;
    if (g_kp[i].patched) {
      kprobe_poke(g_kp[i].addr, g_kp[i].orig);
      g_kp[i].patched = 0;
    }
    /* The row stays: a CPU may still be stepping this probe, and it needs the
     * address to find its way back. Freed by a later arm of the same id or
     * simply reused once nothing steps it. */
    g_kp[i].used = 1;
    rc = 0;
    break;
  }
  spin_unlock_irqrestore(&g_kp_lock, flags);
  return rc;
}

/* ── the traps ───────────────────────────────────────────────────────────── */

int kprobe_handle_bp(struct interrupt_frame *frame) {
  u64 hit;
  unsigned cpu;

  if (!frame || (frame->cs & 3) == 3)
    return 0; /* a user int3 is not ours */
  hit = frame->rip - 1;
  for (usize i = 0; i < KPROBE_MAX; i++) {
    if (!g_kp[i].used || !g_kp[i].patched || g_kp[i].addr != hit)
      continue;
    /* The hit itself. The site's three words are what a probe can know without
     * a per-function description: where it was, and the first two arguments in
     * the SysV registers. */
    cpu = kprobe_this_cpu();
    if (cpu < MAX_CPUS)
      g_kp_frame[cpu] = frame;
    /* The frame says where the probe is: the int3 has moved rip past it. */
    {
      u64 saved = frame->rip;

      frame->rip = hit;
      TRACEPOINT_FIRE(g_kp[i].id, hit, frame->rdi, frame->rsi);
      frame->rip = saved;
    }
    if (cpu < MAX_CPUS)
      g_kp_frame[cpu] = 0;
    /* Step the displaced instruction with the original byte in place. */
    kprobe_poke(g_kp[i].addr, g_kp[i].orig);
    g_kp[i].patched = 0;
    frame->rip = hit;
    frame->rflags |= 0x100ull; /* TF */
    cpu = kprobe_this_cpu();
    if (cpu < MAX_CPUS) {
      g_step_if[cpu] = (frame->rflags & 0x200ull) ? 1 : 0;
      frame->rflags &= ~0x200ull; /* IF off for the one instruction */
      g_stepping[cpu] = (u32)i + 1;
    }
    return 1;
  }
  return 0;
}

int kprobe_handle_db(struct interrupt_frame *frame) {
  unsigned cpu;
  u32 slot;

  if (!frame)
    return 0;
  cpu = kprobe_this_cpu();
  if (cpu >= MAX_CPUS)
    return 0;
  slot = g_stepping[cpu];
  if (!slot)
    return 0;
  g_stepping[cpu] = 0;
  slot--;
  frame->rflags &= ~0x100ull; /* TF off; the step is done */
  if (g_step_if[cpu])
    frame->rflags |= 0x200ull; /* and interrupts back as they were */
  /* Put the breakpoint back, unless the probe was turned off meanwhile. */
  {
    struct b1nix_tracepoint *tp = tracepoint_by_id(g_kp[slot].id);

    if (g_kp[slot].used && tp && tp->enabled && !g_kp[slot].patched) {
      kprobe_poke(g_kp[slot].addr, KPROBE_INT3);
      g_kp[slot].patched = 1;
    }
  }
  /* DR6 is ours to clear: a stale single-step bit makes the next #DB from
   * anywhere look like this one. */
  {
    u64 dr6;

    __asm__ volatile("mov %%dr6, %0" : "=r"(dr6));
    __asm__ volatile("mov %0, %%dr6" ::"r"(dr6 & ~0xfull));
  }
  return 1;
}

/* ── uprobes ─────────────────────────────────────────────────────────────── */

#define UPROBE_MAX 16
#define UPROBE_STEP_MAX 64

struct uprobe_site {
  int used;
  u16 id;
  const void *inode; /* the file's inode: every mapping of it is probed */
  u64 off;           /* file offset of the probed instruction */
  u8 orig;           /* the byte the breakpoint replaced */
  int have_orig;
};

/* A task between its hit and the single step of the original instruction:
 * per task, not per CPU, because it may run the step somewhere else. */
struct uprobe_step {
  usize tid;
  u64 va;
  u16 id;
  int had_tf;
};

static struct uprobe_site g_up[UPROBE_MAX];
static struct uprobe_step g_up_step[UPROBE_STEP_MAX];
static spinlock_t g_up_lock = SPINLOCK_INIT;
static volatile int g_up_armed;

/* Where the site's instruction is in one of a task's mappings, or 0. */
#define UP_ELF_PF_X 0x1 /* ELF p_flags: executable segment */

static u64 up_va_in(const struct task *t, const struct vm_area *v,
                    const struct uprobe_site *u) {
  if (!v || !(v->prot & PROT_EXEC))
    return 0;
  if (v->node) {
    if (!v->node->inode || v->node->inode != u->inode || v->offset < 0 ||
        u->off < (u64)v->offset || u->off - (u64)v->offset >= v->end - v->start)
      return 0;
    return v->start + (u->off - (u64)v->offset);
  }
  /* The loader copies a segment it cannot share from the page cache into
   * private frames, and that mapping carries no node; the image's segment
   * table still says which file and offset it came from. */
  const struct user_loaded_image *img = t->user_image;
  if (!img || !img->exe_file || img->exe_file->inode != u->inode)
    return 0;
  for (usize k = 0; k < img->segment_count; k++) {
    const struct user_image_segment *seg = &img->segments[k];
    u64 vstart = seg->vaddr & ~(u64)(PAGE_SIZE - 1);
    u64 fbase = seg->file_offset & ~(u64)(PAGE_SIZE - 1);

    if (seg->from_interp || !(seg->flags & UP_ELF_PF_X) || vstart != v->start ||
        u->off < fbase || u->off >= seg->file_offset + seg->filesz)
      continue;
    u64 va = vstart + (u->off - fbase);
    return va < v->end ? va : 0;
  }
  return 0;
}

static int up_byte(u64 pml4, u64 va, u8 *out) {
  u64 phys = paging_user_phys(pml4, va);

  if (!phys)
    return -EFAULT;
  *out = *(const u8 *)(usize)(phys + vmm_direct_map_base());
  return 0;
}

/* Plant (or lift) the site's breakpoint wherever `t` maps it and the page is
 * in; a page not in yet gets it when it comes in. */
static void up_apply_task(struct task *t, struct uprobe_site *u, int plant) {
  if (!t || !t->pml4_phys)
    return;
  for (struct vm_area *v = t->vma_list; v; v = v->next) {
    u64 va = up_va_in(t, v, u);
    u8 cur, old;

    if (!va || up_byte(t->pml4_phys, va, &cur) < 0)
      continue;
    if (plant && cur != KPROBE_INT3) {
      if (paging_user_poke_text(t->pml4_phys, va, KPROBE_INT3, &old) == 0 &&
          !u->have_orig) {
        u->orig = old;
        u->have_orig = 1;
      }
    } else if (!plant && cur == KPROBE_INT3 && u->have_orig) {
      paging_user_poke_text(t->pml4_phys, va, u->orig, &old);
    }
  }
}

static void up_apply_all(struct uprobe_site *u, int plant) {
  usize n = scheduler_task_slots();

  for (usize i = 0; i < n; i++) {
    struct task *t = scheduler_task_slot(i);

    if (t && t->state != TASK_UNUSED && t->state != TASK_DEAD &&
        t->state != TASK_REAPING)
      up_apply_task(t, u, plant);
  }
}

int uprobe_arm(u16 id, const void *inode, u64 offset) {
  u64 flags;
  struct uprobe_site *u = 0;

  if (!inode)
    return -EINVAL;
  spin_lock_irqsave(&g_up_lock, &flags);
  for (usize i = 0; i < UPROBE_MAX && !u; i++)
    if (!g_up[i].used) {
      u = &g_up[i];
      memset(u, 0, sizeof(*u));
      u->used = 1;
      u->id = id;
      u->inode = inode;
      u->off = offset;
    }
  spin_unlock_irqrestore(&g_up_lock, flags);
  if (!u)
    return -ENOSPC;
  __atomic_add_fetch(&g_up_armed, 1, __ATOMIC_RELEASE);
  up_apply_all(u, 1);
  return 0;
}

int uprobe_disarm(u16 id) {
  struct uprobe_site *u = 0;

  for (usize i = 0; i < UPROBE_MAX && !u; i++)
    if (g_up[i].used && g_up[i].id == id)
      u = &g_up[i];
  if (!u)
    return -ENOENT;
  up_apply_all(u, 0);
  u->used = 0;
  __atomic_sub_fetch(&g_up_armed, 1, __ATOMIC_RELEASE);
  return 0;
}

void uprobe_page_mapped(u64 va) {
  struct task *t = current_task;
  u64 page = va & ~(u64)(PAGE_SIZE - 1);

  if (!__atomic_load_n(&g_up_armed, __ATOMIC_ACQUIRE) || !t || !t->pml4_phys)
    return;
  for (usize i = 0; i < UPROBE_MAX; i++) {
    if (!g_up[i].used)
      continue;
    for (struct vm_area *v = t->vma_list; v; v = v->next) {
      u64 at = up_va_in(t, v, &g_up[i]);
      u8 cur, old;

      if (!at || (at & ~(u64)(PAGE_SIZE - 1)) != page ||
          up_byte(t->pml4_phys, at, &cur) < 0 || cur == KPROBE_INT3)
        continue;
      if (paging_user_poke_text(t->pml4_phys, at, KPROBE_INT3, &old) == 0 &&
          !g_up[i].have_orig) {
        g_up[i].orig = old;
        g_up[i].have_orig = 1;
      }
    }
  }
}

int uprobe_handle_bp(struct interrupt_frame *frame) {
  struct task *t = current_task;
  u64 va;
  u64 flags;

  if (!frame || (frame->cs & 3) != 3 || !t || !t->pml4_phys ||
      !__atomic_load_n(&g_up_armed, __ATOMIC_ACQUIRE))
    return 0;
  va = frame->rip - 1;
  for (usize i = 0; i < UPROBE_MAX; i++) {
    struct uprobe_site *u = &g_up[i];
    struct vm_area *hit = 0;
    unsigned cpu;
    u8 old;

    if (!u->used || !u->have_orig)
      continue;
    for (struct vm_area *v = t->vma_list; v && !hit; v = v->next)
      if (up_va_in(t, v, u) == va)
        hit = v;
    if (!hit)
      continue;
    /* The hit, with the task's registers as they are at the instruction. */
    cpu = kprobe_this_cpu();
    frame->rip = va;
    if (cpu < MAX_CPUS)
      g_kp_frame[cpu] = frame;
    TRACEPOINT_FIRE(u->id, va, frame->rdi, frame->rsi);
    if (cpu < MAX_CPUS)
      g_kp_frame[cpu] = 0;
    /* Run the original instruction in place, one step, then put the
     * breakpoint back (uprobe_handle_db). */
    if (paging_user_poke_text(t->pml4_phys, va, u->orig, &old) < 0)
      return 0;
    spin_lock_irqsave(&g_up_lock, &flags);
    for (usize k = 0; k < UPROBE_STEP_MAX; k++)
      if (!g_up_step[k].tid) {
        g_up_step[k].tid = t->id;
        g_up_step[k].va = va;
        g_up_step[k].id = u->id;
        g_up_step[k].had_tf = (frame->rflags & 0x100ull) != 0;
        break;
      }
    spin_unlock_irqrestore(&g_up_lock, flags);
    frame->rflags |= 0x100ull; /* TF */
    return 1;
  }
  return 0;
}

int uprobe_handle_db(struct interrupt_frame *frame) {
  struct task *t = current_task;
  struct uprobe_step st = {0};
  u64 flags;

  if (!frame || (frame->cs & 3) != 3 || !t)
    return 0;
  spin_lock_irqsave(&g_up_lock, &flags);
  for (usize k = 0; k < UPROBE_STEP_MAX; k++)
    if (g_up_step[k].tid == t->id) {
      st = g_up_step[k];
      g_up_step[k].tid = 0;
      break;
    }
  spin_unlock_irqrestore(&g_up_lock, flags);
  if (!st.tid)
    return 0;
  /* The step is done: the breakpoint goes back if the probe is still there. */
  for (usize i = 0; i < UPROBE_MAX; i++)
    if (g_up[i].used && g_up[i].id == st.id) {
      u8 old;

      paging_user_poke_text(t->pml4_phys, st.va, KPROBE_INT3, &old);
      break;
    }
  if (!st.had_tf)
    frame->rflags &= ~0x100ull;
  return 1;
}

#endif /* __x86_64__ */

#if defined(__aarch64__)
/* ── aarch64 (M135) ─────────────────────────────────────────────────────────
 *
 * The same scheme on this architecture's own tools, as Linux arm64 does it:
 * a BRK in place of int3 (immediate 0x004 for a kprobe, 0x005 for a uprobe, so
 * one is never mistaken for the other or for a debugger's), the displaced
 * instruction run in place with the original word back, and the hardware
 * single step (MDSCR_EL1.SS with PSTATE.SS; KDE for a step at EL1) standing in
 * for TF. The step runs with IRQs masked, so it cannot be preempted onto
 * another CPU whose MDSCR is not set up for it.
 */

#define KPROBE_BRK_IMM 0x004u
#define UPROBE_BRK_IMM 0x005u
#define BRK_INSN(imm) (0xD4200000u | ((u32)(imm) << 5))
#define SPSR_SS (1ULL << 21)
#define SPSR_D (1ULL << 9)
#define SPSR_I (1ULL << 7)
#define MDSCR_SS (1ULL << 0)
#define MDSCR_KDE (1ULL << 13)

extern void aarch64_sync_icache(u64 addr);
extern int paging_user_poke_text32(u64 pml4_phys, u64 va, u32 val, u32 *old);

static const struct interrupt_frame *g_kp_frame[MAX_CPUS];

static unsigned kprobe_this_cpu(void) {
  struct percpu *pc = get_percpu();

  return pc ? (unsigned)pc->cpu_id : 0;
}

const void *kprobe_current_frame(void) {
  unsigned cpu = kprobe_this_cpu();

  return cpu < MAX_CPUS ? g_kp_frame[cpu] : 0;
}

static u64 mdscr_read(void) {
  u64 v;

  __asm__ volatile("mrs %0, mdscr_el1" : "=r"(v));
  return v;
}

static void mdscr_write(u64 v) {
  __asm__ volatile("msr mdscr_el1, %0\n\tisb" ::"r"(v) : "memory");
}

static u32 brk_imm(const struct interrupt_frame *f) {
  return (u32)(f->esr & 0xffffu);
}

struct kprobe {
  u64 addr;
  u32 orig;
  u8 patched;
  u16 id;
  char symbol[64];
  int used;
};

static struct kprobe g_kp[KPROBE_MAX];
static spinlock_t g_kp_lock = SPINLOCK_INIT;
static volatile u32 g_stepping[MAX_CPUS];
static volatile u8 g_step_i[MAX_CPUS];

/* The kernel image is mapped writable here; the word is written and the
 * instruction caches told. */
static void kprobe_poke(u64 addr, u32 value) {
  *(volatile u32 *)(usize)addr = value;
  aarch64_sync_icache(addr);
}

int kprobe_symbol_ok(const char *symbol) {
  return symbol && *symbol && ksym_addr_of(symbol) != 0;
}

const char *kprobe_symbol_of(u16 id) {
  for (usize i = 0; i < KPROBE_MAX; i++)
    if (g_kp[i].used && g_kp[i].id == id)
      return g_kp[i].symbol;
  return "?";
}

int kprobe_arm(const char *symbol, u16 id, int is_return) {
  u64 flags;
  u64 addr;
  int slot = -1;

  if (is_return)
    return -EOPNOTSUPP;
  addr = symbol ? ksym_addr_of(symbol) : 0;
  if (!addr || (addr & 3))
    return -ENOENT;
  spin_lock_irqsave(&g_kp_lock, &flags);
  for (usize i = 0; i < KPROBE_MAX; i++) {
    if (g_kp[i].used && g_kp[i].id == id) {
      if (!g_kp[i].patched) {
        g_kp[i].orig = *(volatile u32 *)(usize)g_kp[i].addr;
        kprobe_poke(g_kp[i].addr, BRK_INSN(KPROBE_BRK_IMM));
        g_kp[i].patched = 1;
      }
      spin_unlock_irqrestore(&g_kp_lock, flags);
      return 0;
    }
    if (g_kp[i].used && g_kp[i].addr == addr) {
      spin_unlock_irqrestore(&g_kp_lock, flags);
      return -EEXIST;
    }
    if (!g_kp[i].used && slot < 0)
      slot = (int)i;
  }
  if (slot < 0) {
    spin_unlock_irqrestore(&g_kp_lock, flags);
    return -ENOSPC;
  }
  g_kp[slot].addr = addr;
  g_kp[slot].orig = *(volatile u32 *)(usize)addr;
  g_kp[slot].id = id;
  strncpy(g_kp[slot].symbol, symbol, sizeof(g_kp[slot].symbol) - 1);
  g_kp[slot].symbol[sizeof(g_kp[slot].symbol) - 1] = '\0';
  g_kp[slot].used = 1;
  kprobe_poke(addr, BRK_INSN(KPROBE_BRK_IMM));
  g_kp[slot].patched = 1;
  spin_unlock_irqrestore(&g_kp_lock, flags);
  return 0;
}

int kprobe_disarm(u16 id) {
  u64 flags;
  int rc = -ENOENT;

  spin_lock_irqsave(&g_kp_lock, &flags);
  for (usize i = 0; i < KPROBE_MAX; i++) {
    if (!g_kp[i].used || g_kp[i].id != id)
      continue;
    if (g_kp[i].patched) {
      kprobe_poke(g_kp[i].addr, g_kp[i].orig);
      g_kp[i].patched = 0;
    }
    rc = 0;
    break;
  }
  spin_unlock_irqrestore(&g_kp_lock, flags);
  return rc;
}

/* Step the instruction at ELR once, with IRQs masked, and come back. */
static void step_arm(struct interrupt_frame *frame, int kernel) {
  frame->spsr = (frame->spsr | SPSR_SS | SPSR_I) & ~SPSR_D;
  mdscr_write(mdscr_read() | MDSCR_SS | (kernel ? MDSCR_KDE : 0));
}

static void step_disarm(struct interrupt_frame *frame, int had_i) {
  mdscr_write(mdscr_read() & ~(MDSCR_SS | MDSCR_KDE));
  frame->spsr &= ~SPSR_SS;
  if (!had_i)
    frame->spsr &= ~SPSR_I;
}

int kprobe_handle_bp(struct interrupt_frame *frame) {
  u64 hit;
  unsigned cpu;

  if (!frame || (frame->spsr & 0xFULL) == 0 || brk_imm(frame) != KPROBE_BRK_IMM)
    return 0;
  hit = frame->elr; /* BRK leaves the PC on itself */
  for (usize i = 0; i < KPROBE_MAX; i++) {
    if (!g_kp[i].used || !g_kp[i].patched || g_kp[i].addr != hit)
      continue;
    cpu = kprobe_this_cpu();
    if (cpu < MAX_CPUS)
      g_kp_frame[cpu] = frame;
    TRACEPOINT_FIRE(g_kp[i].id, hit, frame->x0, frame->x1);
    if (cpu < MAX_CPUS)
      g_kp_frame[cpu] = 0;
    kprobe_poke(g_kp[i].addr, g_kp[i].orig);
    g_kp[i].patched = 0;
    if (cpu < MAX_CPUS) {
      g_step_i[cpu] = (frame->spsr & SPSR_I) ? 1 : 0;
      g_stepping[cpu] = (u32)i + 1;
    }
    step_arm(frame, 1);
    return 1;
  }
  return 0;
}

int kprobe_handle_db(struct interrupt_frame *frame) {
  unsigned cpu;
  u32 slot;

  if (!frame)
    return 0;
  cpu = kprobe_this_cpu();
  if (cpu >= MAX_CPUS || !(slot = g_stepping[cpu]))
    return 0;
  g_stepping[cpu] = 0;
  slot--;
  step_disarm(frame, g_step_i[cpu]);
  {
    struct b1nix_tracepoint *tp = tracepoint_by_id(g_kp[slot].id);

    if (g_kp[slot].used && tp && tp->enabled && !g_kp[slot].patched) {
      kprobe_poke(g_kp[slot].addr, BRK_INSN(KPROBE_BRK_IMM));
      g_kp[slot].patched = 1;
    }
  }
  return 1;
}

/* ── uprobes ─────────────────────────────────────────────────────────────── */

#define UPROBE_MAX 16
#define UPROBE_STEP_MAX 64
#define UP_ELF_PF_X 0x1

struct uprobe_site {
  int used;
  u16 id;
  const void *inode;
  u64 off;
  u32 orig;
  int have_orig;
};

struct uprobe_step {
  usize tid;
  u64 va;
  u16 id;
  int had_i;
};

static struct uprobe_site g_up[UPROBE_MAX];
static struct uprobe_step g_up_step[UPROBE_STEP_MAX];
static spinlock_t g_up_lock = SPINLOCK_INIT;
static volatile int g_up_armed;

static u64 up_va_in(const struct task *t, const struct vm_area *v,
                    const struct uprobe_site *u) {
  if (!v || !(v->prot & PROT_EXEC))
    return 0;
  if (v->node) {
    if (!v->node->inode || v->node->inode != u->inode || v->offset < 0 ||
        u->off < (u64)v->offset || u->off - (u64)v->offset >= v->end - v->start)
      return 0;
    return v->start + (u->off - (u64)v->offset);
  }
  const struct user_loaded_image *img = t->user_image;
  if (!img || !img->exe_file || img->exe_file->inode != u->inode)
    return 0;
  for (usize k = 0; k < img->segment_count; k++) {
    const struct user_image_segment *seg = &img->segments[k];
    u64 vstart = seg->vaddr & ~(u64)(PAGE_SIZE - 1);
    u64 fbase = seg->file_offset & ~(u64)(PAGE_SIZE - 1);

    if (seg->from_interp || !(seg->flags & UP_ELF_PF_X) || vstart != v->start ||
        u->off < fbase || u->off >= seg->file_offset + seg->filesz)
      continue;
    u64 va = vstart + (u->off - fbase);
    return va < v->end ? va : 0;
  }
  return 0;
}

static int up_word(u64 pml4, u64 va, u32 *out) {
  u64 phys = paging_user_phys(pml4, va);

  if (!phys)
    return -EFAULT;
  *out = *(const u32 *)(usize)(phys + vmm_direct_map_base());
  return 0;
}

static void up_apply_task(struct task *t, struct uprobe_site *u, int plant) {
  if (!t || !t->pml4_phys)
    return;
  for (struct vm_area *v = t->vma_list; v; v = v->next) {
    u64 va = up_va_in(t, v, u);
    u32 cur, old;

    if (!va || (va & 3) || up_word(t->pml4_phys, va, &cur) < 0)
      continue;
    if (plant && cur != BRK_INSN(UPROBE_BRK_IMM)) {
      if (paging_user_poke_text32(t->pml4_phys, va, BRK_INSN(UPROBE_BRK_IMM),
                                  &old) == 0 &&
          !u->have_orig) {
        u->orig = old;
        u->have_orig = 1;
      }
    } else if (!plant && cur == BRK_INSN(UPROBE_BRK_IMM) && u->have_orig) {
      paging_user_poke_text32(t->pml4_phys, va, u->orig, &old);
    }
  }
}

static void up_apply_all(struct uprobe_site *u, int plant) {
  usize n = scheduler_task_slots();

  for (usize i = 0; i < n; i++) {
    struct task *t = scheduler_task_slot(i);

    if (t && t->state != TASK_UNUSED && t->state != TASK_DEAD &&
        t->state != TASK_REAPING)
      up_apply_task(t, u, plant);
  }
}

int uprobe_arm(u16 id, const void *inode, u64 offset) {
  u64 flags;
  struct uprobe_site *u = 0;

  if (!inode || (offset & 3))
    return -EINVAL;
  spin_lock_irqsave(&g_up_lock, &flags);
  for (usize i = 0; i < UPROBE_MAX && !u; i++)
    if (!g_up[i].used) {
      u = &g_up[i];
      memset(u, 0, sizeof(*u));
      u->used = 1;
      u->id = id;
      u->inode = inode;
      u->off = offset;
    }
  spin_unlock_irqrestore(&g_up_lock, flags);
  if (!u)
    return -ENOSPC;
  __atomic_add_fetch(&g_up_armed, 1, __ATOMIC_RELEASE);
  up_apply_all(u, 1);
  return 0;
}

int uprobe_disarm(u16 id) {
  struct uprobe_site *u = 0;

  for (usize i = 0; i < UPROBE_MAX && !u; i++)
    if (g_up[i].used && g_up[i].id == id)
      u = &g_up[i];
  if (!u)
    return -ENOENT;
  up_apply_all(u, 0);
  u->used = 0;
  __atomic_sub_fetch(&g_up_armed, 1, __ATOMIC_RELEASE);
  return 0;
}

void uprobe_page_mapped(u64 va) {
  struct task *t = current_task;
  u64 page = va & ~(u64)(PAGE_SIZE - 1);

  if (!__atomic_load_n(&g_up_armed, __ATOMIC_ACQUIRE) || !t || !t->pml4_phys)
    return;
  for (usize i = 0; i < UPROBE_MAX; i++) {
    if (!g_up[i].used)
      continue;
    for (struct vm_area *v = t->vma_list; v; v = v->next) {
      u64 at = up_va_in(t, v, &g_up[i]);
      u32 cur, old;

      if (!at || (at & ~(u64)(PAGE_SIZE - 1)) != page ||
          up_word(t->pml4_phys, at, &cur) < 0 ||
          cur == BRK_INSN(UPROBE_BRK_IMM))
        continue;
      if (paging_user_poke_text32(t->pml4_phys, at, BRK_INSN(UPROBE_BRK_IMM),
                                  &old) == 0 &&
          !g_up[i].have_orig) {
        g_up[i].orig = old;
        g_up[i].have_orig = 1;
      }
    }
  }
}

int uprobe_handle_bp(struct interrupt_frame *frame) {
  struct task *t = current_task;
  u64 va, flags;

  if (!frame || (frame->spsr & 0xFULL) != 0 || !t || !t->pml4_phys ||
      brk_imm(frame) != UPROBE_BRK_IMM ||
      !__atomic_load_n(&g_up_armed, __ATOMIC_ACQUIRE))
    return 0;
  va = frame->elr;
  for (usize i = 0; i < UPROBE_MAX; i++) {
    struct uprobe_site *u = &g_up[i];
    struct vm_area *hit = 0;
    unsigned cpu;
    u32 old;

    if (!u->used || !u->have_orig)
      continue;
    for (struct vm_area *v = t->vma_list; v && !hit; v = v->next)
      if (up_va_in(t, v, u) == va)
        hit = v;
    if (!hit)
      continue;
    cpu = kprobe_this_cpu();
    if (cpu < MAX_CPUS)
      g_kp_frame[cpu] = frame;
    TRACEPOINT_FIRE(u->id, va, frame->x0, frame->x1);
    if (cpu < MAX_CPUS)
      g_kp_frame[cpu] = 0;
    if (paging_user_poke_text32(t->pml4_phys, va, u->orig, &old) < 0)
      return 0;
    spin_lock_irqsave(&g_up_lock, &flags);
    for (usize k = 0; k < UPROBE_STEP_MAX; k++)
      if (!g_up_step[k].tid) {
        g_up_step[k].tid = t->id;
        g_up_step[k].va = va;
        g_up_step[k].id = u->id;
        g_up_step[k].had_i = (frame->spsr & SPSR_I) != 0;
        break;
      }
    spin_unlock_irqrestore(&g_up_lock, flags);
    step_arm(frame, 0);
    return 1;
  }
  return 0;
}

int uprobe_handle_db(struct interrupt_frame *frame) {
  struct task *t = current_task;
  struct uprobe_step st = {0};
  u64 flags;

  if (!frame || (frame->spsr & 0xFULL) != 0 || !t)
    return 0;
  spin_lock_irqsave(&g_up_lock, &flags);
  for (usize k = 0; k < UPROBE_STEP_MAX; k++)
    if (g_up_step[k].tid == t->id) {
      st = g_up_step[k];
      g_up_step[k].tid = 0;
      break;
    }
  spin_unlock_irqrestore(&g_up_lock, flags);
  if (!st.tid)
    return 0;
  for (usize i = 0; i < UPROBE_MAX; i++)
    if (g_up[i].used && g_up[i].id == st.id) {
      u32 old;

      paging_user_poke_text32(t->pml4_phys, st.va, BRK_INSN(UPROBE_BRK_IMM),
                              &old);
      break;
    }
  step_disarm(frame, st.had_i);
  return 1;
}
#endif /* __aarch64__ */
