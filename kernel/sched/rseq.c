/* SPDX-License-Identifier: GPL-2.0-only */
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
#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/mm.h>
#include <b1nix/rseq.h>
#include <b1nix/sched.h>
#include <b1nix/syscall.h>
#include <stdio.h>
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

/* One row per task-table slot, indexed by task_slot_index(): finding a task's
 * registration is one load, and the row is cleared when the slot changes
 * hands (scheduler.c, where a recycled slot is handed to a new task).
 *
 * This used to be a list of 4096 entries searched from the top under a global
 * spinlock, interrupts off, on EVERY timer tick that returned to userspace --
 * on every CPU, whether anything was registered or not. Under b1nix.sysprof
 * that one unlock was where 53% of the kernel's weighted tick samples landed
 * while Plasma started: four CPUs' ticks fire together and queued on the
 * lock, each holding interrupts off for the whole scan. At 32 bytes a row the
 * table is 128 KiB of BSS, as before. */
#define RSEQ_MAX_TASKS SCHED_MAX_TASKS
struct rseq_reg {
  struct task *task;
  u64 uptr; /* user address of struct rseq */
  u32 len;
  u32 sig; /* signature that must precede abort_ip */
  int used;
};
static struct rseq_reg g_rseq[RSEQ_MAX_TASKS];
/* Taken by registration and cleanup only. The return-to-user path reads the
 * row without it: the row is written by the task itself (its own rseq(2)
 * call) and cleared once the task is gone or its slot is handed to a newcomer
 * that has not run yet -- and a task returning to userspace is neither. */
static spinlock_t g_rseq_lock = SPINLOCK_INIT;
/* Registrations alive. musl registers nothing, so on most images this stays
 * 0 and the tick path returns before it has to find the task's row. */
static u64 g_rseq_live;

static struct rseq_reg *rseq_row(struct task *t) {
  usize i = task_slot_index(t);
  return i < RSEQ_MAX_TASKS ? &g_rseq[i] : 0;
}

static struct rseq_reg *rseq_find(struct task *t) {
  struct rseq_reg *r = rseq_row(t);
  if (!r || !__atomic_load_n(&r->used, __ATOMIC_ACQUIRE) || r->task != t)
    return 0;
  return r;
}

/* Name the refusal.
 *
 * Every ground below returns a bare errno, and glibc treats a failed rseq
 * registration as fatal ("Fatal glibc error: rseq registration failed"), so a
 * process dies at start-up and the kernel says nothing about which of four
 * different reasons it was. Two rounds of work on this bug have been spent
 * guessing between them. Rate-limited, because a genuine conflict can repeat
 * per thread and the point is the first few. */
static void rseq_refused(const char *why, u64 uptr, u32 len, u32 sig) {
  static volatile u64 n;

  if (__atomic_add_fetch(&n, 1, __ATOMIC_RELAXED) > 12)
    return;
  char b[160];
  snprintf(b, sizeof(b),
           "rseq: refused (%s) uptr=0x%lx len=%u sig=0x%x task=%s\n", why,
           (unsigned long)uptr, (unsigned)len, (unsigned)sig,
           current_task && current_task->name ? current_task->name : "?");
  console_write(b);
}

int rseq_register(struct task *t, u64 uptr, u32 len, u32 sig, int unregister) {
  if (!t)
    return -EINVAL;
  if (len < RSEQ_MIN_SIZE) {
    rseq_refused("len below the ABI minimum", uptr, len, sig);
    return -EINVAL;
  }
  if (uptr & 0x1f) {
    /* The area must be 32-byte aligned. glibc places struct rseq inside the
     * thread descriptor, so a thread whose TLS block is aligned differently
     * from the main thread's arrives here and only here. */
    rseq_refused("area not 32-byte aligned", uptr, len, sig);
    return -EINVAL;
  }

  u64 flags;
  spin_lock_irqsave(&g_rseq_lock, &flags);
  struct rseq_reg *r = rseq_find(t);

  if (unregister) {
    if (!r || r->uptr != uptr || r->sig != sig) {
      spin_unlock_irqrestore(&g_rseq_lock, flags);
      return r ? -EINVAL : -EINVAL;
    }
    __atomic_store_n(&r->used, 0, __ATOMIC_RELEASE);
    __atomic_fetch_sub(&g_rseq_live, 1, __ATOMIC_RELEASE);
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
    u64 had = r->uptr;
    spin_unlock_irqrestore(&g_rseq_lock, flags);
    if (!same)
      rseq_refused(had == uptr ? "re-registration with different len/sig"
                               : "task already registered at another area",
                   uptr, len, sig);
    return same ? -EBUSY : -EINVAL;
  }

  r = rseq_row(t);
  if (!r) {
    spin_unlock_irqrestore(&g_rseq_lock, flags);
    rseq_refused("task has no table row", uptr, len, sig);
    return -ENOMEM;
  }
  r->task = t;
  r->uptr = uptr;
  r->len = len;
  r->sig = sig;
  __atomic_store_n(&r->used, 1, __ATOMIC_RELEASE);
  __atomic_fetch_add(&g_rseq_live, 1, __ATOMIC_RELEASE);
  spin_unlock_irqrestore(&g_rseq_lock, flags);
  /* Publish the current CPU immediately: the ABI says a successful
   * registration leaves cpu_id valid, before any further syscall. */
  rseq_on_return_to_user(0);
  return 0;
}

void rseq_task_cleanup(struct task *t) {
  if (!t)
    return;
  struct rseq_reg *r = rseq_row(t);
  if (!r)
    return;
  u64 flags;
  spin_lock_irqsave(&g_rseq_lock, &flags);
  if (__atomic_load_n(&r->used, __ATOMIC_ACQUIRE) && r->task == t) {
    __atomic_store_n(&r->used, 0, __ATOMIC_RELEASE);
    __atomic_fetch_sub(&g_rseq_live, 1, __ATOMIC_RELEASE);
  }
  r->task = 0;
  r->uptr = 0;
  spin_unlock_irqrestore(&g_rseq_lock, flags);
}

/* A forked child does not inherit the parent's registration (Linux clears it
 * unless CLONE_VM), and a thread that shares the address space needs its own
 * area anyway — so this is just the cleanup entry under another name. */
void rseq_fork_clear(struct task *child) { rseq_task_cleanup(child); }

void rseq_on_return_to_user(struct interrupt_frame *frame) {
  struct task *t = current_task;
  if (!t || !__atomic_load_n(&g_rseq_live, __ATOMIC_ACQUIRE))
    return;
  struct rseq_reg *r = rseq_find(t);
  if (!r || !r->uptr)
    return;
  u64 uptr = r->uptr;
  u32 sig = r->sig;

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

#if defined(__aarch64__)
  u64 rip = frame->elr;
#else
  u64 rip = frame->rip;
#endif
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
#if defined(__aarch64__)
  frame->elr = cs.abort_ip;
#else
  frame->rip = cs.abort_ip;
#endif
}

int rseq_is_registered(struct task *t) { return rseq_find(t) != 0; }
