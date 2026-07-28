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
 * The tracer must be the tracee's parent. b1nix's waitpid reports a stopped
 * child to its parent only, and a tracer that cannot wait for its tracee's
 * stops has no way to drive it, so anything else is refused rather than
 * half-working.
 */

#include <b1nix/arch.h>
#include <b1nix/errno.h>
#include <b1nix/mm.h>
#include <b1nix/ptrace.h>
#include <b1nix/sched.h>
#include <b1nix/uidgid.h>
#include <b1nix/spinlock.h>
#include <b1nix/syscall.h>
#include <string.h>

#define PTRACE_MAX 32
#define X86_RFLAGS_TF (1ULL << 8)

struct ptrace_link {
  int used;
  struct task *tracee;
  usize tracer_pid;
  int stopped;                    /* tracee is parked in the stop path */
  int single_step;                /* resume with TF set */
  int inject_signal;              /* signal to deliver on resume, 0 = none */
  int regs_dirty;                 /* SETREGS wrote the snapshot */
  struct interrupt_frame snapshot; /* registers as of the stop */
  struct interrupt_frame *live;   /* the tracee's frame while it is stopped */
};

static struct ptrace_link g_links[PTRACE_MAX];
static spinlock_t g_ptrace_lock = SPINLOCK_INIT;

static struct ptrace_link *link_of(struct task *t) {
  for (usize i = 0; i < PTRACE_MAX; i++)
    if (g_links[i].used && g_links[i].tracee == t)
      return &g_links[i];
  return 0;
}

static struct ptrace_link *link_alloc(struct task *t, usize tracer_pid) {
  for (usize i = 0; i < PTRACE_MAX; i++) {
    if (g_links[i].used)
      continue;
    memset(&g_links[i], 0, sizeof(g_links[i]));
    g_links[i].used = 1;
    g_links[i].tracee = t;
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
  for (usize i = 0; i < PTRACE_MAX; i++)
    if (g_links[i].used &&
        (g_links[i].tracee == t || g_links[i].tracer_pid == t->id))
      g_links[i].used = 0;
  spin_unlock_irqrestore(&g_ptrace_lock, flags);
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
    u64 frame = paging_user_frame(t->pml4_phys, va & ~(u64)(PAGE_SIZE - 1));
    if (!frame)
      return -EIO;
    u8 *p = (u8 *)(usize)(frame + direct + (va & (PAGE_SIZE - 1)));
    if (write)
      *p = buf[i];
    else
      buf[i] = *p;
  }
  if (!write)
    memcpy(value, buf, 8);
  return 0;
}

/* ── register exchange ── */
static void frame_to_uregs(const struct interrupt_frame *f,
                           struct user_regs_struct *u) {
  memset(u, 0, sizeof(*u));
  u->r15 = f->r15; u->r14 = f->r14; u->r13 = f->r13; u->r12 = f->r12;
  u->rbp = f->rbp; u->rbx = f->rbx; u->r11 = f->r11; u->r10 = f->r10;
  u->r9 = f->r9;   u->r8 = f->r8;   u->rax = f->rax; u->rcx = f->rcx;
  u->rdx = f->rdx; u->rsi = f->rsi; u->rdi = f->rdi;
  u->orig_rax = f->rax;
  u->rip = f->rip; u->cs = f->cs; u->eflags = f->rflags;
  u->rsp = f->rsp; u->ss = f->ss;
}

static void uregs_to_frame(const struct user_regs_struct *u,
                           struct interrupt_frame *f) {
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
    struct interrupt_frame snap = l->snapshot;
    l->regs_dirty = 0;
    spin_unlock_irqrestore(&g_ptrace_lock, flags);
    frame->rip = snap.rip;
    frame->rsp = snap.rsp;
    frame->rax = snap.rax; frame->rbx = snap.rbx; frame->rcx = snap.rcx;
    frame->rdx = snap.rdx; frame->rsi = snap.rsi; frame->rdi = snap.rdi;
    frame->rbp = snap.rbp;
    frame->r8 = snap.r8;   frame->r9 = snap.r9;   frame->r10 = snap.r10;
    frame->r11 = snap.r11; frame->r12 = snap.r12; frame->r13 = snap.r13;
    frame->r14 = snap.r14; frame->r15 = snap.r15;
    frame->rflags = snap.rflags;
    spin_lock_irqsave(&g_ptrace_lock, &flags);
  }
  if (l->used) {
    if (l->single_step)
      frame->rflags |= X86_RFLAGS_TF;
    else
      frame->rflags &= ~X86_RFLAGS_TF;
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
  if (frame->cs != 0x1B && frame->cs != 0x23)
    return 0;
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

  frame->rflags &= ~X86_RFLAGS_TF;
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

  if (request == PTRACE_ATTACH) {
    /* Any task may attach to another it is allowed to signal; waitpid reports
     * a tracee's stops to its tracer as well as to its parent
     * (scheduler_waitpid). Attaching to yourself, or to an already-traced
     * task, is refused. */
    if (t == current_task)
      return -EPERM;
    const struct cred *c = scheduler_get_current_cred();
    const struct cred *tc = t->cred;
    if (c && tc && c->euid != ROOT_UID && c->euid != tc->uid &&
        !cred_has_cap(c, CAP_SYS_PTRACE))
      return -EPERM;
    spin_lock_irqsave(&g_ptrace_lock, &flags);
    if (link_of(t)) {
      spin_unlock_irqrestore(&g_ptrace_lock, flags);
      return -EPERM;
    }
    struct ptrace_link *l = link_alloc(t, scheduler_get_pid());
    spin_unlock_irqrestore(&g_ptrace_lock, flags);
    if (!l)
      return -ENOMEM;
    /* Leave SIGSTOP pending instead of forcing the state: the tracee stops
     * itself at its next return to ring 3, where its register frame is
     * complete and safe to snapshot. */
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

  switch (request) {
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
    struct user_regs_struct u;
    spin_lock_irqsave(&g_ptrace_lock, &flags);
    struct interrupt_frame snap = l->snapshot;
    spin_unlock_irqrestore(&g_ptrace_lock, flags);
    frame_to_uregs(&snap, &u);
    if (syscall_copyout((void *)(usize)data, &u, sizeof(u)) < 0)
      return -EFAULT;
    return 0;
  }
  case PTRACE_SETREGS: {
    if (!stopped)
      return -ESRCH;
    struct user_regs_struct u;
    if (syscall_copyin(&u, (const void *)(usize)data, sizeof(u)) < 0)
      return -EFAULT;
    spin_lock_irqsave(&g_ptrace_lock, &flags);
    uregs_to_frame(&u, &l->snapshot);
    l->regs_dirty = 1;
    spin_unlock_irqrestore(&g_ptrace_lock, flags);
    return 0;
  }
  case PTRACE_CONT:
  case PTRACE_SINGLESTEP: {
    if (!stopped)
      return -ESRCH;
    spin_lock_irqsave(&g_ptrace_lock, &flags);
    l->single_step = (request == PTRACE_SINGLESTEP);
    l->inject_signal = (int)data;
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
    l->stopped = 0;
    l->used = 0;
    spin_unlock_irqrestore(&g_ptrace_lock, flags);
    scheduler_kill(pid, SIGCONT); /* the scheduler's own stop->ready path */
    return 0;
  }
  case PTRACE_KILL:
    spin_lock_irqsave(&g_ptrace_lock, &flags);
    l->stopped = 0;
    l->used = 0;
    spin_unlock_irqrestore(&g_ptrace_lock, flags);
    scheduler_kill(pid, SIGKILL);
    return 0;
  default:
    return -EIO;
  }
}
