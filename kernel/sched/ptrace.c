/* ptrace(2) — process tracing.
 *
 * A tracer stops its tracee, reads and writes its registers and memory, and
 * lets it continue. Two design choices make that possible without walking a
 * foreign kernel stack:
 *
 *  - A tracee stops inside the signal-delivery path, where its user register
 *    frame is already in hand. The frame is snapshotted into this file's table
 *    at that moment, so PTRACE_GETREGS reads a stable copy; PTRACE_SETREGS
 *    writes the copy back into the live frame when the tracee resumes.
 *  - Memory access goes through the tracee's page tables via the direct map
 *    (paging_user_frame), so POKETEXT can patch a read-only text page — a
 *    breakpoint would be impossible otherwise.
 *
 * The tracer need not be the tracee's parent: waitpid matches a task's tracer
 * as well as its parent (kernel/sched/scheduler.c), so an attached tracer sees
 * the stops it has to drive. Attaching still requires the right to signal the
 * target, exactly as Linux's ptrace_may_access does.
 */

#include <b1nix/arch.h>
#include <b1nix/errno.h>
#include <b1nix/mm.h>
#include <b1nix/ptrace.h>
#include <b1nix/sched.h>
#include <b1nix/uidgid.h>
#include <b1nix/spinlock.h>
#include <b1nix/syscall.h>
#include <b1nix/user.h>
#include <string.h>

#define PTRACE_MAX 32
#define X86_RFLAGS_TF (1ULL << 8)

struct ptrace_link {
  int used;
  struct task *tracee;
  /* The tracee's pid at attach time. A task slot is recycled, so matching on
   * the struct pointer alone would let a stale link (one whose tracee died
   * without running its cleanup) capture the next task to land in that slot. */
  usize tracee_pid;
  usize tracer_pid;
  int stopped;                    /* tracee is parked in the stop path */
  int single_step;                /* resume with TF set */
  int inject_signal;              /* signal to deliver on resume, 0 = none */
  int regs_dirty;                 /* SETREGS wrote the snapshot */
  int stop_signal;                /* signal that caused the current stop */
  u32 options;                    /* PTRACE_SETOPTIONS mask */
  int seized;                     /* attached with PTRACE_SEIZE */
  int listening;                  /* PTRACE_LISTEN: parked, not inspectable */
  int pending_event;              /* event to attach to the next stop */
  int syscall_trace;              /* resume was PTRACE_SYSCALL */
  int in_syscall;                 /* the entry stop of a pair has been reported */
  int frame_valid;                /* snapshot holds a real ring-3 frame */
  int stop_event;                 /* event code of the current stop */
  u64 event_msg;                  /* PTRACE_GETEVENTMSG value */
  struct interrupt_frame snapshot; /* registers as of the stop */
  struct interrupt_frame *live;   /* the tracee's frame while it is stopped */
};

static struct ptrace_link g_links[PTRACE_MAX];
/* Number of live links. Read without the lock by ptrace_any_traced(), whose
 * only job is to keep the syscall hot path free of locking when nothing on the
 * system is being traced. */
static volatile int g_link_count;
static spinlock_t g_ptrace_lock = SPINLOCK_INIT;

/* Declared tracers (prctl(PR_SET_PTRACER)) and the yama-style attach scope.
 * Kept in a small table rather than in struct task: adding fields to that
 * struct has broken unrelated paging invariants before (see sched.h). */
struct ptrace_declared {
  int used;
  usize target_pid;
  usize tracer_pid;
};
static struct ptrace_declared g_declared[PTRACE_MAX];
static int g_ptrace_scope; /* 0 = ownership only, 1 = ancestor/declared only */

static struct ptrace_link *link_of(struct task *t) {
  if (!t)
    return 0;
  for (usize i = 0; i < PTRACE_MAX; i++)
    if (g_links[i].used && g_links[i].tracee == t &&
        g_links[i].tracee_pid == t->id)
      return &g_links[i];
  return 0;
}

static struct ptrace_link *link_alloc(struct task *t, usize tracer_pid) {
  for (usize i = 0; i < PTRACE_MAX; i++) {
    if (g_links[i].used)
      continue;
    memset(&g_links[i], 0, sizeof(g_links[i]));
    g_links[i].used = 1;
    __atomic_fetch_add(&g_link_count, 1, __ATOMIC_RELAXED);
    g_links[i].tracee = t;
    g_links[i].tracee_pid = t->id;
    g_links[i].tracer_pid = tracer_pid;
    return &g_links[i];
  }
  return 0;
}

usize ptrace_tracer_pid(struct task *t) {
  if (!t)
    return 0;
  u64 flags;
  spin_lock_irqsave(&g_ptrace_lock, &flags);
  struct ptrace_link *l = link_of(t);
  usize pid = l ? l->tracer_pid : 0;
  spin_unlock_irqrestore(&g_ptrace_lock, flags);
  return pid;
}

int ptrace_is_traced(struct task *t) {
  u64 flags;
  spin_lock_irqsave(&g_ptrace_lock, &flags);
  int traced = link_of(t) != 0;
  spin_unlock_irqrestore(&g_ptrace_lock, flags);
  return traced;
}

void ptrace_task_cleanup(struct task *t) {
  if (!t)
    return;
  u64 flags;
  spin_lock_irqsave(&g_ptrace_lock, &flags);
  usize kill_pids[PTRACE_MAX];
  usize nkill = 0;
  for (usize i = 0; i < PTRACE_MAX; i++) {
    if (!g_links[i].used)
      continue;
    if (g_links[i].tracee == t) {
      g_links[i].used = 0;
      __atomic_fetch_sub(&g_link_count, 1, __ATOMIC_RELAXED);
      continue;
    }
    if (g_links[i].tracer_pid == t->id) {
      /* PTRACE_O_EXITKILL: the tracee dies with the tracer that asked for it.
       * A crash handler sets this so a process it stopped can never be left
       * parked forever if the handler itself dies. */
      if ((g_links[i].options & PTRACE_O_EXITKILL) && g_links[i].tracee)
        kill_pids[nkill++] = g_links[i].tracee->id;
      g_links[i].used = 0;
      __atomic_fetch_sub(&g_link_count, 1, __ATOMIC_RELAXED);
    }
  }
  spin_unlock_irqrestore(&g_ptrace_lock, flags);
  for (usize i = 0; i < nkill; i++)
    scheduler_kill(kill_pids[i], SIGKILL);
}

/* ── attach permission (Linux ptrace_may_access + yama ptrace_scope) ── */
int ptrace_set_declared_tracer(struct task *t, usize tracer_pid) {
  if (!t)
    return -ESRCH;
  u64 flags;
  spin_lock_irqsave(&g_ptrace_lock, &flags);
  struct ptrace_declared *free_slot = 0;
  for (usize i = 0; i < PTRACE_MAX; i++) {
    if (g_declared[i].used && g_declared[i].target_pid == t->id) {
      if (tracer_pid == 0)
        g_declared[i].used = 0;
      else
        g_declared[i].tracer_pid = tracer_pid;
      spin_unlock_irqrestore(&g_ptrace_lock, flags);
      return 0;
    }
    if (!g_declared[i].used && !free_slot)
      free_slot = &g_declared[i];
  }
  if (tracer_pid == 0) { /* withdrawing a declaration that was never made */
    spin_unlock_irqrestore(&g_ptrace_lock, flags);
    return 0;
  }
  if (!free_slot) {
    spin_unlock_irqrestore(&g_ptrace_lock, flags);
    return -ENOMEM;
  }
  free_slot->used = 1;
  free_slot->target_pid = t->id;
  free_slot->tracer_pid = tracer_pid;
  spin_unlock_irqrestore(&g_ptrace_lock, flags);
  return 0;
}

static usize declared_tracer_of(usize target_pid) {
  u64 flags;
  spin_lock_irqsave(&g_ptrace_lock, &flags);
  usize pid = 0;
  for (usize i = 0; i < PTRACE_MAX; i++)
    if (g_declared[i].used && g_declared[i].target_pid == target_pid) {
      pid = g_declared[i].tracer_pid;
      break;
    }
  spin_unlock_irqrestore(&g_ptrace_lock, flags);
  return pid;
}

int ptrace_scope_get(void) { return g_ptrace_scope; }

int ptrace_scope_set(int scope) {
  if (scope != 0 && scope != 1)
    return -EINVAL;
  g_ptrace_scope = scope;
  return 0;
}

/* Is `ancestor_pid` somewhere up `t`'s parent chain? The walk is bounded by
 * PTRACE_MAX*2 steps so a corrupted parent cycle can never hang the caller. */
static int is_ancestor_of(usize ancestor_pid, struct task *t) {
  usize pid = t ? t->parent_id : 0;
  for (int i = 0; i < 64 && pid; i++) {
    if (pid == ancestor_pid)
      return 1;
    struct task *p = scheduler_task_by_pid(pid);
    if (!p || p->id == p->parent_id)
      break;
    pid = p->parent_id;
  }
  return 0;
}

int ptrace_may_access(struct task *t) {
  if (!t)
    return 0;
  if (t == current_task)
    return 1;
  const struct cred *c = scheduler_get_current_cred();
  const struct cred *tc = t->cred;
  if (c && tc && c->euid != ROOT_UID && c->euid != tc->uid &&
      !cred_has_cap(c, CAP_SYS_PTRACE))
    return 0;
  if (!g_ptrace_scope)
    return 1;
  /* Restricted scope: ownership is not enough — the tracer must be an ancestor
   * of the target or the tracer the target itself nominated. CAP_SYS_PTRACE
   * still overrides, as it does under Linux's yama. */
  if (c && cred_has_cap(c, CAP_SYS_PTRACE))
    return 1;
  usize me = scheduler_get_pid();
  if (is_ancestor_of(me, t))
    return 1;
  usize declared = declared_tracer_of(t->id);
  return declared == PTRACE_ANY_TRACER || (declared != 0 && declared == me);
}

/* ── ptrace events ───────────────────────────────────────────────────────────
 * An event is reported by arming the tracee with a pending event code and a
 * SIGTRAP: the stop itself happens where every ptrace stop happens, at the next
 * return to ring 3, where the register frame is complete. */
static void ptrace_arm_event(struct task *t, int event, u64 msg) {
  u64 flags;
  spin_lock_irqsave(&g_ptrace_lock, &flags);
  struct ptrace_link *l = link_of(t);
  if (l) {
    l->pending_event = event;
    l->event_msg = msg;
  }
  spin_unlock_irqrestore(&g_ptrace_lock, flags);
  if (l)
    scheduler_post_signal(t->id, SIGTRAP);
}

int ptrace_any_traced(void) {
  return __atomic_load_n(&g_link_count, __ATOMIC_RELAXED) != 0;
}

int ptrace_stop_event(struct task *t) {
  if (!t)
    return 0;
  u64 flags;
  spin_lock_irqsave(&g_ptrace_lock, &flags);
  struct ptrace_link *l = link_of(t);
  int ev = (l && l->stopped) ? l->stop_event : 0;
  spin_unlock_irqrestore(&g_ptrace_lock, flags);
  return ev;
}

void ptrace_event_child(struct task *parent, struct task *child, int event) {
  if (!parent || !child)
    return;
  u32 want = (event == PTRACE_EVENT_FORK)    ? PTRACE_O_TRACEFORK
             : (event == PTRACE_EVENT_VFORK) ? PTRACE_O_TRACEVFORK
                                             : PTRACE_O_TRACECLONE;
  u64 flags;
  spin_lock_irqsave(&g_ptrace_lock, &flags);
  struct ptrace_link *pl = link_of(parent);
  int interested = pl && (pl->options & want);
  usize tracer = pl ? pl->tracer_pid : 0;
  u32 inherited = pl ? (pl->options & ~(u32)PTRACE_O_EXITKILL) : 0;
  struct ptrace_link *cl = 0;
  if (interested) {
    cl = link_alloc(child, tracer);
    if (cl) {
      cl->options = inherited;
      cl->seized = pl->seized;
    }
  }
  spin_unlock_irqrestore(&g_ptrace_lock, flags);
  if (!interested || !cl)
    return;
  /* The child is born traced and stops before it runs any of its own code, so
   * the tracer can set breakpoints in it before it starts — the whole point of
   * PTRACE_O_TRACEFORK. */
  scheduler_post_signal(child->id, SIGSTOP);
  ptrace_arm_event(parent, event, (u64)child->id);
}

void ptrace_event_exec(struct task *t) {
  if (!t)
    return;
  u64 flags;
  spin_lock_irqsave(&g_ptrace_lock, &flags);
  struct ptrace_link *l = link_of(t);
  int interested = l && (l->options & PTRACE_O_TRACEEXEC);
  spin_unlock_irqrestore(&g_ptrace_lock, flags);
  if (interested)
    ptrace_arm_event(t, PTRACE_EVENT_EXEC, (u64)t->id);
}

/* ── tracee memory access ────────────────────────────────────────────────────
 * Read/write one 64-bit word in another task's address space. The word may
 * straddle two pages, so each half is handled separately. Writes deliberately
 * ignore the PTE's write bit: patching an instruction in a read-only text page
 * is the whole point of POKETEXT. */
static int ptrace_access(struct task *t, u64 addr, u64 *value, int write) {
  if (!t || !t->pml4_phys)
    return -ESRCH;
  extern u64 vmm_direct_map_base(void);
  u64 direct = vmm_direct_map_base();
  u8 buf[8];
  if (write)
    memcpy(buf, value, 8);

  for (usize i = 0; i < 8; i++) {
    u64 va = addr + i;
    u64 phys = paging_user_phys(t->pml4_phys, va);
    if (!phys)
      return -EIO;
    u8 *p = (u8 *)(usize)(phys + direct);
    if (write)
      *p = buf[i];
    else
      buf[i] = *p;
  }
  if (!write)
    memcpy(value, buf, 8);
  return 0;
}

/* Read from the task's loaded-image backing copy for an address whose page is
 * not resident. Returns the number of bytes copied (0 when the address is not
 * covered by any segment that still has its data). */
static usize ptrace_read_from_image(struct task *t, u64 va, void *dst,
                                    usize len) {
  struct user_loaded_image *img = (struct user_loaded_image *)t->user_image;
  if (!img)
    return 0;
  for (usize i = 0; i < img->segment_count; i++) {
    const struct user_image_segment *seg = &img->segments[i];
    if (!seg->data || !seg->filesz)
      continue;
    if (va < seg->vaddr || va >= seg->vaddr + seg->filesz)
      continue;
    usize off = (usize)(va - seg->vaddr);
    usize avail = (usize)(seg->filesz - off);
    usize n = len < avail ? len : avail;
    memcpy(dst, (const u8 *)seg->data + off, n);
    return n;
  }
  return 0;
}

/* Byte-granular version of the same walk, for /proc/<pid>/mem and
 * /proc/<pid>/auxv. Stops at the first unmapped page: a short count is the
 * honest answer for a range that runs off the end of a mapping, and 0 bytes at
 * the very first page is -EIO, exactly as read(2) on Linux's /proc/pid/mem. */
static isize ptrace_copy_range(struct task *t, u64 addr, void *buf, usize len,
                               int write) {
  if (!t || !t->pml4_phys)
    return -ESRCH;
  if (len == 0)
    return 0;
  extern u64 vmm_direct_map_base(void);
  u64 direct = vmm_direct_map_base();
  u8 *p = (u8 *)buf;
  usize done = 0;
  while (done < len) {
    u64 va = addr + done;
    u64 page = va & ~(u64)(PAGE_SIZE - 1);
    u64 phys = paging_user_phys(t->pml4_phys, va);
    usize off = (usize)(va - page);
    usize chunk = PAGE_SIZE - off;
    if (chunk > len - done)
      chunk = len - done;
    if (!phys) {
      /* Not resident. A read may still be satisfiable from the loaded image's
       * own copy of the segment covering this address: the page has never been
       * touched, so the image copy IS its contents. This is what lets a
       * debugger read a program's ELF headers — pages nothing ever faulted in —
       * which is exactly where a crash reporter starts. Writes have nowhere to
       * go and stop here. */
      if (write)
        break;
      usize got = ptrace_read_from_image(t, va, p + done, chunk);
      if (!got)
        break;
      done += got;
      continue;
    }
    u8 *kva = (u8 *)(usize)(phys + direct);
    if (write)
      memcpy(kva, p + done, chunk);
    else
      memcpy(p + done, kva, chunk);
    done += chunk;
  }
  if (done == 0)
    return -EIO;
  return (isize)done;
}

isize ptrace_copy_from_task(struct task *t, u64 addr, void *dst, usize len) {
  return ptrace_copy_range(t, addr, dst, len, 0);
}

isize ptrace_copy_to_task(struct task *t, u64 addr, const void *src,
                          usize len) {
  return ptrace_copy_range(t, addr, (void *)(usize)src, len, 1);
}

/* ── register exchange ── */
/* NT_X86_XSTATE in the XSAVE layout: the 512-byte legacy FXSAVE region
 * followed by the 64-byte XSAVE header. b1nix's context switch uses FXSAVE, so
 * the only components a task carries are x87 and SSE — XSTATE_BV says exactly
 * that (bits 0 and 1), and no extended area follows. A debugger reading this
 * gets the truthful register set rather than a padded-out AVX area whose upper
 * halves the kernel never saved. */
static usize ptrace_xstate_size(struct task *t) {
  return task_xsave_area(t) ? arch_xsave_area_size() : B1NIX_XSTATE_SIZE;
}

/* `out` must have room for ptrace_xstate_size(t) bytes (<= ARCH_XSAVE_MAX_SIZE). */
static void ptrace_build_xstate(struct task *t, u8 *out) {
  void *area = task_xsave_area(t);
  if (area) {
    /* The task really is managed with XSAVE: hand back the area as it stands,
     * XSTATE_BV and all, so a debugger sees exactly which components exist. */
    memcpy(out, area, arch_xsave_area_size());
    return;
  }
  memset(out, 0, B1NIX_XSTATE_SIZE);
  memcpy(out, t->fpu_state, 512);
  u64 xstate_bv = 0x3; /* XFEATURE_MASK_FP | XFEATURE_MASK_SSE */
  memcpy(out + 512, &xstate_bv, sizeof(xstate_bv));
}

/* The legacy 512-byte FXSAVE region, wherever this task keeps it. */
static const void *ptrace_fxsave_region(struct task *t) {
  void *area = task_xsave_area(t);
  return area ? area : (const void *)t->fpu_state;
}

static void ptrace_write_fxsave_region(struct task *t, const void *src) {
  void *area = task_xsave_area(t);
  if (area) {
    memcpy(area, src, 512);
    /* Declare x87+SSE present so xrstor actually loads what was just written. */
    u64 bv;
    memcpy(&bv, (u8 *)area + 512, sizeof(bv));
    bv |= (arch_xsave_mask() & 0x3);
    memcpy((u8 *)area + 512, &bv, sizeof(bv));
  } else {
    /* No XSAVE area: the task's own save area IS the arch's register set, so
     * copy exactly that much (528 bytes on aarch64, 512 on x86_64). */
    memcpy(t->fpu_state, src, sizeof(struct user_fpregs_struct));
  }
  t->fpu_initialized = 1;
}

#if defined(__x86_64__)
typedef struct user_regs_struct native_regs_t;
#elif defined(__aarch64__)
typedef struct b1nix_user_pt_regs native_regs_t;
#endif

static void frame_to_uregs(const struct interrupt_frame *f, struct task *t,
                           native_regs_t *u) {
  memset(u, 0, sizeof(*u));
#if defined(__x86_64__)
  /* fs_base is the tracee's TLS pointer. A crash reporter needs it to find the
   * thread's own bookkeeping (it is where the thread pointer lives on x86_64),
   * so report the value the kernel restores on switch-in rather than 0. */
  if (t)
    u->fs_base = task_tls_base(t);
  u->r15 = f->r15; u->r14 = f->r14; u->r13 = f->r13; u->r12 = f->r12;
  u->rbp = f->rbp; u->rbx = f->rbx; u->r11 = f->r11; u->r10 = f->r10;
  u->r9 = f->r9;   u->r8 = f->r8;   u->rax = f->rax; u->rcx = f->rcx;
  u->rdx = f->rdx; u->rsi = f->rsi; u->rdi = f->rdi;
  u->orig_rax = f->rax;
  u->rip = f->rip; u->cs = f->cs; u->eflags = f->rflags;
  u->rsp = f->rsp; u->ss = f->ss;
#elif defined(__aarch64__)
  u->regs[0] = f->x0; u->regs[1] = f->x1; u->regs[2] = f->x2; u->regs[3] = f->x3;
  u->regs[4] = f->x4; u->regs[5] = f->x5; u->regs[6] = f->x6; u->regs[7] = f->x7;
  u->regs[8] = f->x8; u->regs[9] = f->x9; u->regs[10] = f->x10; u->regs[11] = f->x11;
  u->regs[12] = f->x12; u->regs[13] = f->x13; u->regs[14] = f->x14; u->regs[15] = f->x15;
  u->regs[16] = f->x16; u->regs[17] = f->x17; u->regs[18] = f->x18; u->regs[19] = f->x19;
  u->regs[20] = f->x20; u->regs[21] = f->x21; u->regs[22] = f->x22; u->regs[23] = f->x23;
  u->regs[24] = f->x24; u->regs[25] = f->x25; u->regs[26] = f->x26; u->regs[27] = f->x27;
  u->regs[28] = f->x28; u->regs[29] = f->x29; u->regs[30] = f->x30;
  u->sp = f->sp_el0;
  u->pc = f->elr;
  u->pstate = f->spsr;
#endif
}

static void uregs_to_frame(const native_regs_t *u,
                           struct interrupt_frame *f) {
#if defined(__x86_64__)
  f->r15 = u->r15; f->r14 = u->r14; f->r13 = u->r13; f->r12 = u->r12;
  f->rbp = u->rbp; f->rbx = u->rbx; f->r11 = u->r11; f->r10 = u->r10;
  f->r9 = u->r9;   f->r8 = u->r8;   f->rax = u->rax; f->rcx = u->rcx;
  f->rdx = u->rdx; f->rsi = u->rsi; f->rdi = u->rdi;
  f->rip = u->rip;
  /* cs/ss stay kernel-chosen: a tracer must not be able to pick its tracee's
   * privilege level. Of eflags, only the flags userspace may set itself are
   * taken, with TF owned by PTRACE_SINGLESTEP. */
  f->rflags = (f->rflags & ~0xcd5ULL) | (u->eflags & 0xcd5ULL);
  f->rsp = u->rsp;
#elif defined(__aarch64__)
  f->x0 = u->regs[0]; f->x1 = u->regs[1]; f->x2 = u->regs[2]; f->x3 = u->regs[3];
  f->x4 = u->regs[4]; f->x5 = u->regs[5]; f->x6 = u->regs[6]; f->x7 = u->regs[7];
  f->x8 = u->regs[8]; f->x9 = u->regs[9]; f->x10 = u->regs[10]; f->x11 = u->regs[11];
  f->x12 = u->regs[12]; f->x13 = u->regs[13]; f->x14 = u->regs[14]; f->x15 = u->regs[15];
  f->x16 = u->regs[16]; f->x17 = u->regs[17]; f->x18 = u->regs[18]; f->x19 = u->regs[19];
  f->x20 = u->regs[20]; f->x21 = u->regs[21]; f->x22 = u->regs[22]; f->x23 = u->regs[23];
  f->x24 = u->regs[24]; f->x25 = u->regs[25]; f->x26 = u->regs[26]; f->x27 = u->regs[27];
  f->x28 = u->regs[28]; f->x29 = u->regs[29]; f->x30 = u->regs[30];
  f->sp_el0 = u->sp;
  f->elr = u->pc;
  f->spsr = u->pstate;
#endif
}

/* ── crash record (M80 crash capture) ────────────────────────────────────────
 * The CPU-fault handler records what killed a task before the signal is
 * delivered: the signal number, the faulting address (CR2 for a page fault) and
 * a Linux si_code. A crash reporter reads it back two ways — from its own
 * SA_SIGINFO handler (si_addr) and, after attaching, via PTRACE_GETSIGINFO. */
struct ptrace_fault {
  int used;
  usize pid;
  int signo;
  int code;
  u64 addr;
};
static struct ptrace_fault g_faults[PTRACE_MAX];

void ptrace_record_fault(struct task *t, int signo, u64 addr, int code) {
  if (!t)
    return;
  u64 flags;
  spin_lock_irqsave(&g_ptrace_lock, &flags);
  struct ptrace_fault *slot = 0;
  for (usize i = 0; i < PTRACE_MAX; i++) {
    if (g_faults[i].used && g_faults[i].pid == t->id) {
      slot = &g_faults[i];
      break;
    }
    if (!g_faults[i].used && !slot)
      slot = &g_faults[i];
  }
  if (slot) {
    slot->used = 1;
    slot->pid = t->id;
    slot->signo = signo;
    slot->code = code;
    slot->addr = addr;
  }
  spin_unlock_irqrestore(&g_ptrace_lock, flags);
}

int ptrace_fault_info(struct task *t, int *signo, u64 *addr, int *code) {
  if (!t)
    return 0;
  u64 flags;
  int found = 0;
  spin_lock_irqsave(&g_ptrace_lock, &flags);
  for (usize i = 0; i < PTRACE_MAX; i++)
    if (g_faults[i].used && g_faults[i].pid == t->id) {
      if (signo) *signo = g_faults[i].signo;
      if (addr)  *addr = g_faults[i].addr;
      if (code)  *code = g_faults[i].code;
      found = 1;
      break;
    }
  spin_unlock_irqrestore(&g_ptrace_lock, flags);
  return found;
}

/* Park the tracee and tell its tracer. Called with no locks held. */
static void ptrace_do_stop(struct task *t, struct ptrace_link *l, int signo,
                           struct interrupt_frame *frame) {
  u64 flags;
  spin_lock_irqsave(&g_ptrace_lock, &flags);
  l->snapshot = *frame;
  l->live = frame;
  l->stopped = 1;
  l->regs_dirty = 0;
  l->stop_signal = signo;
  l->stop_event = l->pending_event;
  l->pending_event = 0;
  spin_unlock_irqrestore(&g_ptrace_lock, flags);

  t->last_stop_signal = signo;
  t->stop_report_pending = 1;
  t->state = TASK_STOPPED;
  /* Wake BOTH waiters: the real parent, and the tracer when it is somebody
   * else — the tracer is the one that has to see this stop, and it is
   * sleeping in waitpid on its own wait event. */
  scheduler_notify_wait_event(t->parent_id);
  if (l->tracer_pid && l->tracer_pid != t->parent_id)
    scheduler_notify_wait_event(l->tracer_pid);

  while (1) {
    spin_lock_irqsave(&g_ptrace_lock, &flags);
    int still = l->used && l->stopped;
    spin_unlock_irqrestore(&g_ptrace_lock, flags);
    if (!still)
      break;
    /* A SIGKILL outranks the stop — re-parking a task the kernel is killing
     * would leave it STOPPED forever (and queued on a runqueue it can never be
     * picked from). */
    if (__atomic_load_n(&t->pending_signals, __ATOMIC_ACQUIRE) &
        (1ULL << (SIGKILL - 1)))
      break;
    t->state = TASK_STOPPED;
    scheduler_yield();
  }

  /* However the stop ended — the tracer continued us, detached, or a SIGKILL
   * outran it — this task is executing again, so it must not be left marked
   * STOPPED (nothing would ever pick it, and a task wedged mid-exit never
   * reaps). */
  t->state = TASK_RUNNING;
  t->stop_report_pending = 0;

  /* Resuming: adopt whatever the tracer left behind. */
  spin_lock_irqsave(&g_ptrace_lock, &flags);
  if (l->used && l->regs_dirty) {
    *frame = l->snapshot;
    l->regs_dirty = 0;
    spin_unlock_irqrestore(&g_ptrace_lock, flags);
    spin_lock_irqsave(&g_ptrace_lock, &flags);
  }
  if (l->used) {
#ifdef __x86_64__
    if (l->single_step)
      frame->rflags |= X86_RFLAGS_TF;
    else
      frame->rflags &= ~X86_RFLAGS_TF;
#endif
    l->live = 0;
  }
  spin_unlock_irqrestore(&g_ptrace_lock, flags);
}

int ptrace_signal_stop(struct task *t, int signo,
                       struct interrupt_frame *frame) {
  /* SIGKILL is never interceptable, and SIGCONT is how a stop is lifted — a
   * tracee must not re-stop on the signal that just resumed it. */
  if (!t || !frame || signo == SIGKILL || signo == SIGCONT)
    return 0;
  /* Only ever park on a return to ring 3: a kernel-mode frame may live on a
   * per-CPU/IST stack that another task reuses while this one sleeps, and
   * resuming from it would jump into whatever overwrote it. */
#if defined(__aarch64__)
  if ((frame->spsr & 0xF) != 0)
    return 0;
#else
  if (frame->cs != 0x1B && frame->cs != 0x23)
    return 0;
#endif
  u64 flags;
  spin_lock_irqsave(&g_ptrace_lock, &flags);
  struct ptrace_link *l = link_of(t);
  if (!l || l->stopped) {
    spin_unlock_irqrestore(&g_ptrace_lock, flags);
    return 0;
  }
  spin_unlock_irqrestore(&g_ptrace_lock, flags);

  ptrace_do_stop(t, l, signo, frame);

  /* The tracer decides what the tracee sees: PTRACE_CONT with a signal
   * re-injects it, PTRACE_CONT with 0 swallows it. */
  spin_lock_irqsave(&g_ptrace_lock, &flags);
  int inject = l->used ? l->inject_signal : 0;
  if (l->used)
    l->inject_signal = 0;
  spin_unlock_irqrestore(&g_ptrace_lock, flags);
  if (inject > 0 && inject != signo)
    scheduler_kill(t->id, inject);
  return inject == 0 ? 1 : (inject == signo ? 0 : 1);
}

/* ── syscall-entry / syscall-exit stops (PTRACE_SYSCALL) ─────────────────── */
void ptrace_syscall_stop(struct task *t, struct interrupt_frame *frame,
                         int is_exit) {
  if (!t || !frame)
    return;
  /* Only ever park on a frame that really came from ring 3 (see the same guard
   * in ptrace_signal_stop). */
#if defined(__aarch64__)
  if ((frame->spsr & 0xF) != 0)
    return;
#else
  if (frame->cs != 0x1B && frame->cs != 0x23)
    return;
#endif
  u64 flags;
  spin_lock_irqsave(&g_ptrace_lock, &flags);
  struct ptrace_link *l = link_of(t);
  if (!l || l->stopped) {
    spin_unlock_irqrestore(&g_ptrace_lock, flags);
    return;
  }
  /* Record the last ring-3 frame unconditionally: PTRACE_O_TRACEEXIT reports
   * these registers, and a dying task no longer has a frame of its own. */
  l->snapshot = *frame;
  l->frame_valid = 1;
  int want = l->syscall_trace;
  int sysgood = (l->options & PTRACE_O_TRACESYSGOOD) != 0;
  if (want) {
    /* Entry and exit alternate; a resume between them keeps the pairing. */
    l->in_syscall = !is_exit;
  }
  spin_unlock_irqrestore(&g_ptrace_lock, flags);
  if (!want)
    return;
  /* Linux marks a syscall stop by setting bit 7 of the reported signal when
   * PTRACE_O_TRACESYSGOOD is on; that is how a tracer distinguishes it from a
   * real SIGTRAP. */
  ptrace_do_stop(t, l, sysgood ? (SIGTRAP | 0x80) : SIGTRAP, frame);
}

/* ── PTRACE_O_TRACEEXIT ──────────────────────────────────────────────────── */
void ptrace_exit_stop(struct task *t, int exit_code) {
  if (!t)
    return;
  u64 flags;
  spin_lock_irqsave(&g_ptrace_lock, &flags);
  struct ptrace_link *l = link_of(t);
  if (!l || l->stopped || !(l->options & PTRACE_O_TRACEEXIT)) {
    spin_unlock_irqrestore(&g_ptrace_lock, flags);
    return;
  }
  l->stopped = 1;
  l->regs_dirty = 0;
  l->stop_signal = SIGTRAP;
  l->stop_event = PTRACE_EVENT_EXIT;
  l->pending_event = 0;
  /* Linux reports the exit event's message in wait-status form, so a tracer can
   * feed it straight to WIFEXITED/WEXITSTATUS. */
  l->event_msg = (exit_code & TASK_EXIT_SIGNALED)
                     ? (u64)(u32)(exit_code & 0x7f)
                     : (u64)(u32)((exit_code & 0xff) << 8);
  l->live = 0; /* the task is on its way out: nothing may write its frame */
  usize tracer = l->tracer_pid;
  spin_unlock_irqrestore(&g_ptrace_lock, flags);

  t->last_stop_signal = SIGTRAP;
  t->stop_report_pending = 1;
  t->state = TASK_STOPPED;
  scheduler_notify_wait_event(t->parent_id);
  if (tracer && tracer != t->parent_id)
    scheduler_notify_wait_event(tracer);

  /* Park until the tracer releases us. SIGKILL still outranks the stop — a
   * task the kernel is killing must not be held here. */
  while (1) {
    spin_lock_irqsave(&g_ptrace_lock, &flags);
    int still = l->used && l->stopped;
    spin_unlock_irqrestore(&g_ptrace_lock, flags);
    if (!still)
      break;
    if (__atomic_load_n(&t->pending_signals, __ATOMIC_ACQUIRE) &
        (1ULL << (SIGKILL - 1)))
      break;
    t->state = TASK_STOPPED;
    scheduler_yield();
  }
  t->state = TASK_RUNNING;
  t->stop_report_pending = 0;
  spin_lock_irqsave(&g_ptrace_lock, &flags);
  if (l->used)
    l->stop_event = 0;
  spin_unlock_irqrestore(&g_ptrace_lock, flags);
}

int ptrace_handle_debug_trap(struct interrupt_frame *frame) {
  struct task *t = current_task;
  if (!t || !frame)
    return 0;
  u64 flags;
  spin_lock_irqsave(&g_ptrace_lock, &flags);
  struct ptrace_link *l = link_of(t);
  int mine = l && l->single_step;
  spin_unlock_irqrestore(&g_ptrace_lock, flags);
  if (!mine)
    return 0;

#ifdef __x86_64__
  frame->rflags &= ~X86_RFLAGS_TF;
#endif
  ptrace_do_stop(t, l, SIGTRAP, frame);
  return 1;
}

isize ptrace_request(long request, usize pid, u64 addr, u64 data,
                     u64 *out_peek) {
  u64 flags;

  if (request == PTRACE_TRACEME) {
    if (!current_task)
      return -EINVAL;
    spin_lock_irqsave(&g_ptrace_lock, &flags);
    if (link_of(current_task)) {
      spin_unlock_irqrestore(&g_ptrace_lock, flags);
      return -EPERM;
    }
    struct ptrace_link *l = link_alloc(current_task, current_task->parent_id);
    spin_unlock_irqrestore(&g_ptrace_lock, flags);
    return l ? 0 : -ENOMEM;
  }

  struct task *t = scheduler_task_by_pid(pid);
  if (!t)
    return -ESRCH;

  if (request == PTRACE_ATTACH || request == PTRACE_SEIZE) {
    /* Any task may attach to another it is allowed to inspect; waitpid reports
     * a tracee's stops to its tracer as well as to its parent
     * (scheduler_waitpid). Attaching to yourself, or to an already-traced
     * task, is refused. PTRACE_SEIZE differs from PTRACE_ATTACH in exactly one
     * way here: it does not stop the tracee — the tracer stops it later with
     * PTRACE_INTERRUPT, which is what a crash handler wants when it must first
     * decide whether this process is one it is responsible for. */
    if (t == current_task)
      return -EPERM;
    if (!ptrace_may_access(t))
      return -EPERM;
    spin_lock_irqsave(&g_ptrace_lock, &flags);
    if (link_of(t)) {
      spin_unlock_irqrestore(&g_ptrace_lock, flags);
      return -EPERM;
    }
    struct ptrace_link *l = link_alloc(t, scheduler_get_pid());
    if (l) {
      l->seized = (request == PTRACE_SEIZE);
      /* PTRACE_SEIZE takes its options in `data`, as Linux does. */
      if (l->seized && data) {
        if (data & ~(u64)PTRACE_O_SUPPORTED) {
          l->used = 0;
          __atomic_fetch_sub(&g_link_count, 1, __ATOMIC_RELAXED);
          spin_unlock_irqrestore(&g_ptrace_lock, flags);
          return -EINVAL;
        }
        l->options = (u32)data;
      }
    }
    spin_unlock_irqrestore(&g_ptrace_lock, flags);
    if (!l)
      return -ENOMEM;
    /* Leave SIGSTOP pending instead of forcing the state: the tracee stops
     * itself at its next return to ring 3, where its register frame is
     * complete and safe to snapshot. */
    if (request == PTRACE_ATTACH)
      scheduler_post_signal(pid, SIGSTOP);
    return 0;
  }

  spin_lock_irqsave(&g_ptrace_lock, &flags);
  struct ptrace_link *l = link_of(t);
  int owned = l && l->tracer_pid == scheduler_get_pid();
  int stopped = l && l->stopped;
  spin_unlock_irqrestore(&g_ptrace_lock, flags);
  if (!l || !owned)
    return -ESRCH;

  /* PTRACE_LISTEN leaves the tracee parked but out of ptrace-stop: it is not
   * inspectable until PTRACE_INTERRUPT brings it back, exactly as on Linux. */
  u64 lflags;
  spin_lock_irqsave(&g_ptrace_lock, &lflags);
  int listening = l->listening;
  spin_unlock_irqrestore(&g_ptrace_lock, lflags);
  if (listening && request != PTRACE_INTERRUPT && request != PTRACE_DETACH &&
      request != PTRACE_KILL && request != PTRACE_CONT)
    return -ESRCH;

  switch (request) {
  case PTRACE_SETOPTIONS: {
    if (data & ~(u64)PTRACE_O_SUPPORTED)
      return -EINVAL;
    spin_lock_irqsave(&g_ptrace_lock, &flags);
    l->options = (u32)data;
    spin_unlock_irqrestore(&g_ptrace_lock, flags);
    return 0;
  }
  case PTRACE_GETEVENTMSG: {
    spin_lock_irqsave(&g_ptrace_lock, &flags);
    u64 msg = l->event_msg;
    spin_unlock_irqrestore(&g_ptrace_lock, flags);
    if (syscall_copyout((void *)(usize)data, &msg, sizeof(msg)) < 0)
      return -EFAULT;
    return 0;
  }
  case PTRACE_LISTEN: {
    /* Only meaningful for a seized tracee that is currently stopped — Linux
     * answers EIO otherwise. */
    spin_lock_irqsave(&g_ptrace_lock, &flags);
    int ok = l->seized && l->stopped;
    if (ok)
      l->listening = 1;
    spin_unlock_irqrestore(&g_ptrace_lock, flags);
    return ok ? 0 : -EIO;
  }
  case PTRACE_PEEKTEXT:
  case PTRACE_PEEKDATA: {
    if (!stopped)
      return -ESRCH; /* the tracee must be stopped to be inspected */
    u64 word = 0;
    int rc = ptrace_access(t, addr, &word, 0);
    if (rc < 0)
      return rc;
    if (out_peek)
      *out_peek = word;
    return 0;
  }
  case PTRACE_POKETEXT:
  case PTRACE_POKEDATA: {
    if (!stopped)
      return -ESRCH;
    u64 word = data;
    return ptrace_access(t, addr, &word, 1);
  }
  case PTRACE_GETREGS: {
    if (!stopped)
      return -ESRCH;
    native_regs_t u;
    spin_lock_irqsave(&g_ptrace_lock, &flags);
    struct interrupt_frame snap = l->snapshot;
    spin_unlock_irqrestore(&g_ptrace_lock, flags);
    frame_to_uregs(&snap, t, &u);
    if (syscall_copyout((void *)(usize)data, &u, sizeof(u)) < 0)
      return -EFAULT;
    return 0;
  }
  case PTRACE_GETFPREGS: {
    if (!stopped)
      return -ESRCH;
    /* A stopped tracee has been switched out at least once, so its FPU/SSE
     * state lives in the task's FXSAVE area — which IS the Linux
     * user_fpregs_struct layout, byte for byte. */
    if (syscall_copyout((void *)(usize)data, ptrace_fxsave_region(t),
                        sizeof(struct user_fpregs_struct)) < 0)
      return -EFAULT;
    return 0;
  }
  case PTRACE_SETFPREGS: {
    if (!stopped)
      return -ESRCH;
    struct user_fpregs_struct f;
    if (syscall_copyin(&f, (const void *)(usize)data, sizeof(f)) < 0)
      return -EFAULT;
    /* Make sure the (possibly never-run) tracee restores what was just written
     * instead of a clean FPU image on its next switch-in. */
    ptrace_write_fxsave_region(t, &f);
    return 0;
  }
  case PTRACE_GETREGSET:
  case PTRACE_SETREGSET: {
    /* addr selects the note type, data points at a struct iovec describing the
     * caller's buffer. iov_len is updated to the number of bytes actually
     * transferred, as Linux does. */
    if (!stopped)
      return -ESRCH;
    struct ptrace_iovec iov;
    if (syscall_copyin(&iov, (const void *)(usize)data, sizeof(iov)) < 0)
      return -EFAULT;
    u8 buf[ARCH_XSAVE_MAX_SIZE];
    usize full;
    if (addr == NT_PRSTATUS) {
      full = sizeof(native_regs_t);
    } else if (addr == NT_PRFPREG) {
      full = sizeof(struct user_fpregs_struct);
    } else if (addr == NT_X86_XSTATE) {
      full = ptrace_xstate_size(t);
#if defined(__aarch64__)
    } else if (addr == NT_ARM_TLS) {
      full = sizeof(u64);
#endif
    } else {
      return -EINVAL; /* no other register set exists on this port */
    }
    usize n = iov.iov_len < full ? (usize)iov.iov_len : full;
    if (n == 0)
      return -EINVAL;

    if (request == PTRACE_GETREGSET) {
      if (addr == NT_PRSTATUS) {
        native_regs_t u;
        spin_lock_irqsave(&g_ptrace_lock, &flags);
        struct interrupt_frame snap = l->snapshot;
        spin_unlock_irqrestore(&g_ptrace_lock, flags);
        frame_to_uregs(&snap, t, &u);
        memcpy(buf, &u, sizeof(u));
      } else if (addr == NT_X86_XSTATE) {
        ptrace_build_xstate(t, buf);
#if defined(__aarch64__)
      } else if (addr == NT_ARM_TLS) {
        u64 tls = task_tls_base(t);
        memcpy(buf, &tls, sizeof(tls));
#endif
      } else {
        memcpy(buf, ptrace_fxsave_region(t), sizeof(struct user_fpregs_struct));
      }
      if (syscall_copyout((void *)(usize)iov.iov_base, buf, n) < 0)
        return -EFAULT;
    } else {
      /* A short SETREGSET writes only the prefix the caller supplied, so start
       * from the current values rather than from zeroes. */
      if (addr == NT_PRSTATUS) {
        native_regs_t u;
        spin_lock_irqsave(&g_ptrace_lock, &flags);
        struct interrupt_frame snap = l->snapshot;
        spin_unlock_irqrestore(&g_ptrace_lock, flags);
        frame_to_uregs(&snap, t, &u);
        memcpy(buf, &u, sizeof(u));
      } else if (addr == NT_X86_XSTATE) {
        ptrace_build_xstate(t, buf);
#if defined(__aarch64__)
      } else if (addr == NT_ARM_TLS) {
        u64 tls = task_tls_base(t);
        memcpy(buf, &tls, sizeof(tls));
#endif
      } else {
        ptrace_build_xstate(t, buf);
      }
      if (syscall_copyin(buf, (const void *)(usize)iov.iov_base, n) < 0)
        return -EFAULT;
      if (addr == NT_PRSTATUS) {
        native_regs_t u;
        memcpy(&u, buf, sizeof(u));
        spin_lock_irqsave(&g_ptrace_lock, &flags);
        uregs_to_frame(&u, &l->snapshot);
        l->regs_dirty = 1;
        spin_unlock_irqrestore(&g_ptrace_lock, flags);
#if defined(__aarch64__)
      } else if (addr == NT_ARM_TLS) {
        u64 tls;
        memcpy(&tls, buf, sizeof(tls));
        task_set_tls_base(t, tls);
#endif
      } else if (addr == NT_X86_XSTATE && task_xsave_area(t)) {
        /* A full XSAVE area was written: adopt it wholesale, so a debugger can
         * set AVX registers and not just the legacy half. */
        memcpy(task_xsave_area(t), buf, arch_xsave_area_size());
        t->fpu_initialized = 1;
      } else {
        /* Legacy region only — the whole of the state this task carries. */
        ptrace_write_fxsave_region(t, buf);
      }
    }
    iov.iov_len = n;
    if (syscall_copyout((void *)(usize)data, &iov, sizeof(iov)) < 0)
      return -EFAULT;
    return 0;
  }
  case PTRACE_GETSIGINFO: {
    if (!stopped)
      return -ESRCH;
    spin_lock_irqsave(&g_ptrace_lock, &flags);
    int signo = l->stop_signal;
    spin_unlock_irqrestore(&g_ptrace_lock, flags);
    struct ptrace_siginfo si;
    memset(&si, 0, sizeof(si));
    si.si_signo = signo;
    /* If a CPU fault produced this signal, report the address it faulted on and
     * the matching si_code — that is the payload a crash reporter writes into
     * its dump. Otherwise it came from kill(2): SI_USER (0). */
    int fsig = 0, fcode = 0;
    u64 faddr = 0;
    if (ptrace_fault_info(t, &fsig, &faddr, &fcode) && fsig == signo) {
      si.si_code = fcode;
      si.si_addr = faddr;
    }
    if (syscall_copyout((void *)(usize)data, &si, sizeof(si)) < 0)
      return -EFAULT;
    return 0;
  }
  case PTRACE_INTERRUPT: {
    /* Stop a seized-but-running tracee. The stop itself happens on its next
     * return to ring 3 (ptrace_signal_stop), exactly like ATTACH's SIGSTOP. */
    if (listening) {
      /* Coming back out of PTRACE_LISTEN: the tracee never resumed, so all
       * that is needed is a fresh trace-stop report for the tracer. */
      spin_lock_irqsave(&g_ptrace_lock, &flags);
      l->listening = 0;
      l->stop_event = PTRACE_EVENT_STOP;
      spin_unlock_irqrestore(&g_ptrace_lock, flags);
      t->last_stop_signal = SIGTRAP;
      t->stop_report_pending = 1;
      scheduler_notify_wait_event(scheduler_get_pid());
      scheduler_notify_wait_event(t->parent_id);
      return 0;
    }
    if (stopped)
      return 0;
    scheduler_post_signal(pid, SIGSTOP);
    return 0;
  }
  case PTRACE_SETREGS: {
    if (!stopped)
      return -ESRCH;
    native_regs_t u;
    if (syscall_copyin(&u, (const void *)(usize)data, sizeof(u)) < 0)
      return -EFAULT;
    spin_lock_irqsave(&g_ptrace_lock, &flags);
    uregs_to_frame(&u, &l->snapshot);
    l->regs_dirty = 1;
    spin_unlock_irqrestore(&g_ptrace_lock, flags);
    return 0;
  }
  case PTRACE_CONT:
  case PTRACE_SYSCALL:
  case PTRACE_SINGLESTEP: {
    if (!stopped)
      return -ESRCH;
    spin_lock_irqsave(&g_ptrace_lock, &flags);
    l->syscall_trace = (request == PTRACE_SYSCALL);
    l->single_step = (request == PTRACE_SINGLESTEP);
    l->inject_signal = (int)data;
    l->listening = 0;
    l->stop_event = 0;
    l->stopped = 0;
    spin_unlock_irqrestore(&g_ptrace_lock, flags);
    t->stop_report_pending = 0;
    scheduler_kill(pid, SIGCONT); /* the scheduler's own stop->ready path */
    return 0;
  }
  case PTRACE_DETACH: {
    spin_lock_irqsave(&g_ptrace_lock, &flags);
    l->single_step = 0;
    l->inject_signal = (int)data;
    l->listening = 0;
    l->stop_event = 0;
    l->stopped = 0;
    l->syscall_trace = 0;
    l->used = 0;
    __atomic_fetch_sub(&g_link_count, 1, __ATOMIC_RELAXED);
    spin_unlock_irqrestore(&g_ptrace_lock, flags);
    scheduler_kill(pid, SIGCONT); /* the scheduler's own stop->ready path */
    return 0;
  }
  case PTRACE_KILL:
    /* Resume the stopped tracee through the normal signal wake path before
     * dropping the link. Clearing the link first leaves a task parked in the
     * ptrace stop with SIGKILL pending; its real parent can then wait forever
     * because the tracee never reaches exit_current(). */
    spin_lock_irqsave(&g_ptrace_lock, &flags);
    l->stopped = 0;
    spin_unlock_irqrestore(&g_ptrace_lock, flags);
    scheduler_kill(pid, SIGKILL);
    spin_lock_irqsave(&g_ptrace_lock, &flags);
    if (l->used) {
      l->used = 0;
      __atomic_fetch_sub(&g_link_count, 1, __ATOMIC_RELAXED);
    }
    spin_unlock_irqrestore(&g_ptrace_lock, flags);
    return 0;
  default:
    return -EIO;
  }
}
