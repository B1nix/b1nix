/* rseq(2) — restartable sequences.
 *
 * A registered task hands the kernel a `struct rseq` in its own memory. The
 * kernel keeps two promises about it:
 *
 *   1. `cpu_id_start` / `cpu_id` always name the CPU the task is running on,
 *      refreshed before control returns to userspace. A lock-free algorithm
 *      reads them to pick a per-CPU data structure without a syscall.
 *   2. If the task is interrupted (preemption, signal, migration) while its
 *      instruction pointer sits inside a critical section described by the
 *      `rseq_cs` descriptor it published, execution resumes at the section's
 *      abort handler instead of in the middle of the sequence. That is what
 *      makes the sequence *restartable*: the algorithm can assume it either
 *      ran to the commit instruction uninterrupted, or not at all.
 *
 * Registration state lives in a side table keyed by task, not in struct task —
 * growing that struct disturbs the LAPIC page-table layout (see M29).
 */

#include <b1nix/arch.h>
#include <b1nix/errno.h>
#include <b1nix/mm.h>
#include <b1nix/rseq.h>
#include <b1nix/sched.h>
#include <b1nix/syscall.h>
#include <string.h>

/* struct rseq, as the ABI fixes it (kernel/rseq.h in Linux):
 *   u32 cpu_id_start;   offset 0
 *   u32 cpu_id;         offset 4
 *   u64 rseq_cs;        offset 8   (pointer to struct rseq_cs, or 0)
 *   u32 flags;          offset 16
 * The structure is 32 bytes and 32-byte aligned. */
#define RSEQ_CPU_ID_START_OFF 0
#define RSEQ_CPU_ID_OFF 4
#define RSEQ_CS_OFF 8
#define RSEQ_MIN_SIZE 32
#define RSEQ_CPU_ID_UNINITIALIZED ((u32)-1)

/* struct rseq_cs: version, flags, start_ip, post_commit_offset, abort_ip. */
struct rseq_cs_desc {
  u32 version;
  u32 flags;
  u64 start_ip;
  u64 post_commit_offset;
  u64 abort_ip;
};

/* One entry per task the scheduler can hold, so this table can never be the
 * thing that refuses a registration.
 *
 * It was 64. glibc registers rseq for every thread it creates and treats a
 * refusal on a thread as fatal -- "Fatal glibc error: rseq registration
 * failed", which is a killed process and not a degraded one -- so a desktop
 * with more than 64 threads alive at once could not run. (The leak fixed
 * alongside this made it worse, but 64 was too few regardless.) At 32 bytes an
 * entry the whole table is 128 KiB of BSS. */
#define RSEQ_MAX_TASKS 4096

struct rseq_reg {
  struct task *task;
  u64 uptr; /* user address of struct rseq */
  u32 len;
  u32 sig; /* signature that must precede abort_ip */
  int used;
};

static struct rseq_reg g_rseq[RSEQ_MAX_TASKS];
static spinlock_t g_rseq_lock = SPINLOCK_INIT;

static struct rseq_reg *rseq_find(struct task *t) {
  for (usize i = 0; i < RSEQ_MAX_TASKS; i++)
    if (g_rseq[i].used && g_rseq[i].task == t)
      return &g_rseq[i];
  return 0;
}

int rseq_register(struct task *t, u64 uptr, u32 len, u32 sig, int unregister) {
  if (!t)
    return -EINVAL;
  if (len < RSEQ_MIN_SIZE || (uptr & 0x1f))
    return -EINVAL;

  u64 flags;
  spin_lock_irqsave(&g_rseq_lock, &flags);
  struct rseq_reg *r = rseq_find(t);

  if (unregister) {
    if (!r || r->uptr != uptr || r->sig != sig) {
      spin_unlock_irqrestore(&g_rseq_lock, flags);
      return r ? -EINVAL : -EINVAL;
    }
    r->used = 0;
    spin_unlock_irqrestore(&g_rseq_lock, flags);
    /* Leave cpu_id as "unregistered" so a stale reader notices. */
    u32 uninit = RSEQ_CPU_ID_UNINITIALIZED;
    syscall_copyout((void *)(usize)(uptr + RSEQ_CPU_ID_OFF), &uninit,
                 sizeof(uninit));
    return 0;
  }

  if (r) {
    /* Re-registering the same area is the idempotent case libcs rely on when
     * two libraries both call rseq(); anything else is a conflict. */
    int same = (r->uptr == uptr && r->len == len && r->sig == sig);
    spin_unlock_irqrestore(&g_rseq_lock, flags);
    return same ? -EBUSY : -EINVAL;
  }

  for (usize i = 0; i < RSEQ_MAX_TASKS; i++) {
    if (g_rseq[i].used)
      continue;
    g_rseq[i].task = t;
    g_rseq[i].uptr = uptr;
    g_rseq[i].len = len;
    g_rseq[i].sig = sig;
    g_rseq[i].used = 1;
    spin_unlock_irqrestore(&g_rseq_lock, flags);
    /* Publish the current CPU immediately: the ABI says a successful
     * registration leaves cpu_id valid, before any further syscall. */
    rseq_on_return_to_user(0);
    return 0;
  }
  spin_unlock_irqrestore(&g_rseq_lock, flags);
  return -ENOMEM;
}

void rseq_task_cleanup(struct task *t) {
  if (!t)
    return;
  u64 flags;
  spin_lock_irqsave(&g_rseq_lock, &flags);
  for (usize i = 0; i < RSEQ_MAX_TASKS; i++)
    if (g_rseq[i].used && g_rseq[i].task == t)
      g_rseq[i].used = 0;
  spin_unlock_irqrestore(&g_rseq_lock, flags);
}

/* A forked child does not inherit the parent's registration (Linux clears it
 * unless CLONE_VM), and a thread that shares the address space needs its own
 * area anyway — so this is just the cleanup entry under another name. */
void rseq_fork_clear(struct task *child) { rseq_task_cleanup(child); }

void rseq_on_return_to_user(struct interrupt_frame *frame) {
  struct task *t = current_task;
  if (!t)
    return;
  u64 flags;
  spin_lock_irqsave(&g_rseq_lock, &flags);
  struct rseq_reg *r = rseq_find(t);
  u64 uptr = r ? r->uptr : 0;
  u32 sig = r ? r->sig : 0;
  spin_unlock_irqrestore(&g_rseq_lock, flags);
  if (!uptr)
    return;

  struct percpu *pcpu = get_percpu();
  u32 cpu = pcpu ? (u32)pcpu->cpu_id : 0;

  /* Promise 1: the CPU ids userspace reads without a syscall. */
  if (syscall_copyout((void *)(usize)(uptr + RSEQ_CPU_ID_START_OFF), &cpu,
                   sizeof(cpu)) < 0)
    return;
  if (syscall_copyout((void *)(usize)(uptr + RSEQ_CPU_ID_OFF), &cpu,
                   sizeof(cpu)) < 0)
    return;

  /* Promise 2: abort a critical section we are returning into the middle of.
   * Only meaningful when we know where userspace will resume, i.e. when a
   * frame was passed (syscall/interrupt return). */
  if (!frame)
    return;
  u64 csptr = 0;
  if (syscall_copyin(&csptr, (const void *)(usize)(uptr + RSEQ_CS_OFF),
                     sizeof(csptr)) < 0)
    return;
  if (!csptr)
    return;

  struct rseq_cs_desc cs;
  if (syscall_copyin(&cs, (const void *)(usize)csptr, sizeof(cs)) < 0)
    return;
  if (cs.version != 0)
    return;

  u64 rip = frame->rip;
  if (rip < cs.start_ip || rip >= cs.start_ip + cs.post_commit_offset) {
    /* Not inside the sequence: the descriptor is consumed either way, so a
     * later interruption cannot re-abort against a stale section. */
    u64 zero = 0;
    syscall_copyout((void *)(usize)(uptr + RSEQ_CS_OFF), &zero, sizeof(zero));
    return;
  }

  /* The four bytes before abort_ip must be the signature the task registered.
   * This is what stops an attacker who can write rseq_cs from redirecting
   * execution to an arbitrary address. */
  u32 got = 0;
  if (syscall_copyin(&got, (const void *)(usize)(cs.abort_ip - 4),
                     sizeof(got)) < 0)
    return;
  if (got != sig) {
    scheduler_kill(t->id, SIGSEGV);
    return;
  }

  u64 zero = 0;
  if (syscall_copyout((void *)(usize)(uptr + RSEQ_CS_OFF), &zero, sizeof(zero)) < 0)
    return;
  frame->rip = cs.abort_ip;
}

int rseq_is_registered(struct task *t) {
  u64 flags;
  spin_lock_irqsave(&g_rseq_lock, &flags);
  int reg = rseq_find(t) != 0;
  spin_unlock_irqrestore(&g_rseq_lock, flags);
  return reg;
}
