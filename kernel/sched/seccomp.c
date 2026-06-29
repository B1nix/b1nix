/*
 * M63: seccomp-bpf — classic-BPF syscall filtering.
 *
 * A task installs an immutable filter (SECCOMP_SET_MODE_FILTER) or enters strict
 * mode (SECCOMP_SET_MODE_STRICT). On every syscall, seccomp_filter_syscall()
 * runs the task's filter chain over a struct seccomp_data and enforces the
 * verdict: ALLOW proceeds, ERRNO short-circuits the syscall with -errno, KILL
 * terminates the task with SIGSYS, TRAP raises SIGSYS to a handler. Filters are
 * inherited by children (shared, refcounted) and survive execve; they can only
 * be added, never removed, so a descendant is always at least as restricted.
 *
 * Only tasks that installed a filter pay any cost — seccomp_active() is a single
 * side-table load and the syscall hot path gates on it.
 */

#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/mm.h>
#include <b1nix/sched.h>
#include <b1nix/seccomp.h>
#include <b1nix/syscall.h>
#include <string.h>

/* BPF program length cap (Linux uses BPF_MAXINSNS = 4096). */
#define SECCOMP_MAX_INSNS 4096

/* An installed filter: one BPF program plus a link to the previously-installed
 * filter (filters stack — all are run, the most restrictive verdict wins).
 * Reference-counted so fork can share it with the child without copying. */
struct seccomp_filter {
  struct sock_filter *prog;
  u32 len;
  struct seccomp_filter *prev;
  int refcount;
};

/* ── classic-BPF opcodes (subset seccomp uses) ─────────────────────────────── */
#define BPF_LD   0x00
#define BPF_LDX  0x01
#define BPF_ST   0x02
#define BPF_STX  0x03
#define BPF_ALU  0x04
#define BPF_JMP  0x05
#define BPF_RET  0x06
#define BPF_MISC 0x07
#define BPF_CLASS(c) ((c)&0x07)

#define BPF_W 0x00 /* 32-bit */
#define BPF_SIZE(c) ((c)&0x18)

#define BPF_MODE(c) ((c)&0xe0)
#define BPF_IMM 0x00
#define BPF_ABS 0x20
#define BPF_IND 0x40
#define BPF_MEM 0x60
#define BPF_LEN 0x80

#define BPF_OP(c) ((c)&0xf0)
#define BPF_ADD 0x00
#define BPF_SUB 0x10
#define BPF_MUL 0x20
#define BPF_DIV 0x30
#define BPF_OR  0x40
#define BPF_AND 0x50
#define BPF_LSH 0x60
#define BPF_RSH 0x70
#define BPF_NEG 0x80
#define BPF_MOD 0x90
#define BPF_XOR 0xa0

#define BPF_JA   0x00
#define BPF_JEQ  0x10
#define BPF_JGT  0x20
#define BPF_JGE  0x30
#define BPF_JSET 0x40

#define BPF_SRC(c) ((c)&0x08)
#define BPF_K 0x00
#define BPF_X 0x08

#define BPF_RVAL(c) ((c)&0x18)
#define BPF_A 0x10

#define BPF_MISCOP(c) ((c)&0xf8)
#define BPF_TAX 0x00
#define BPF_TXA 0x80

#define BPF_MEMWORDS 16

/* Run one BPF program over `data`. Returns the 32-bit verdict (the value of the
 * executed BPF_RET). A malformed program returns SECCOMP_RET_KILL_PROCESS so a
 * bad filter fails closed. Loads are bounded to the seccomp_data window. */
static u32 bpf_run(const struct sock_filter *prog, u32 len,
                   const struct seccomp_data *data) {
  u32 A = 0, X = 0;
  u32 mem[BPF_MEMWORDS];
  memset(mem, 0, sizeof(mem));
  const u8 *base = (const u8 *)data;
  const u32 dlen = (u32)sizeof(struct seccomp_data);

  for (u32 pc = 0; pc < len; pc++) {
    const struct sock_filter *in = &prog[pc];
    u16 code = in->code;
    switch (BPF_CLASS(code)) {
    case BPF_LD:
      switch (BPF_MODE(code)) {
      case BPF_ABS: {
        u32 off = in->k;
        if (off + 4 > dlen || off + 4 < off)
          return SECCOMP_RET_KILL_PROCESS; /* out-of-bounds load */
        u32 v;
        memcpy(&v, base + off, 4);
        A = v;
        break;
      }
      case BPF_IMM:
        A = in->k;
        break;
      case BPF_MEM:
        if (in->k >= BPF_MEMWORDS) return SECCOMP_RET_KILL_PROCESS;
        A = mem[in->k];
        break;
      case BPF_LEN:
        A = dlen;
        break;
      default:
        return SECCOMP_RET_KILL_PROCESS;
      }
      break;
    case BPF_LDX:
      switch (BPF_MODE(code)) {
      case BPF_IMM:
        X = in->k;
        break;
      case BPF_MEM:
        if (in->k >= BPF_MEMWORDS) return SECCOMP_RET_KILL_PROCESS;
        X = mem[in->k];
        break;
      case BPF_LEN:
        X = dlen;
        break;
      default:
        return SECCOMP_RET_KILL_PROCESS;
      }
      break;
    case BPF_ST:
      if (in->k >= BPF_MEMWORDS) return SECCOMP_RET_KILL_PROCESS;
      mem[in->k] = A;
      break;
    case BPF_STX:
      if (in->k >= BPF_MEMWORDS) return SECCOMP_RET_KILL_PROCESS;
      mem[in->k] = X;
      break;
    case BPF_ALU: {
      u32 src = (BPF_SRC(code) == BPF_X) ? X : in->k;
      switch (BPF_OP(code)) {
      case BPF_ADD: A += src; break;
      case BPF_SUB: A -= src; break;
      case BPF_MUL: A *= src; break;
      case BPF_DIV: if (!src) return SECCOMP_RET_KILL_PROCESS; A /= src; break;
      case BPF_MOD: if (!src) return SECCOMP_RET_KILL_PROCESS; A %= src; break;
      case BPF_OR:  A |= src; break;
      case BPF_AND: A &= src; break;
      case BPF_XOR: A ^= src; break;
      case BPF_LSH: A <<= (src & 31); break;
      case BPF_RSH: A >>= (src & 31); break;
      case BPF_NEG: A = (u32)(-(i32)A); break;
      default: return SECCOMP_RET_KILL_PROCESS;
      }
      break;
    }
    case BPF_JMP: {
      if (BPF_OP(code) == BPF_JA) {
        if (in->k >= len - pc - 1) return SECCOMP_RET_KILL_PROCESS;
        pc += in->k;
        break;
      }
      u32 cmp = (BPF_SRC(code) == BPF_X) ? X : in->k;
      int t;
      switch (BPF_OP(code)) {
      case BPF_JEQ:  t = (A == cmp); break;
      case BPF_JGT:  t = (A > cmp);  break;
      case BPF_JGE:  t = (A >= cmp); break;
      case BPF_JSET: t = (A & cmp) != 0; break;
      default: return SECCOMP_RET_KILL_PROCESS;
      }
      u8 jmp = t ? in->jt : in->jf;
      if (jmp > len - pc - 1) return SECCOMP_RET_KILL_PROCESS;
      pc += jmp;
      break;
    }
    case BPF_RET: {
      if (BPF_RVAL(code) == BPF_A)
        return A;
      return in->k; /* BPF_K */
    }
    case BPF_MISC:
      if (BPF_MISCOP(code) == BPF_TAX) X = A;
      else if (BPF_MISCOP(code) == BPF_TXA) A = X;
      else return SECCOMP_RET_KILL_PROCESS;
      break;
    default:
      return SECCOMP_RET_KILL_PROCESS;
    }
  }
  /* Fell off the end without a RET — Linux rejects this at install time; treat
   * as a kill so it fails closed. */
  return SECCOMP_RET_KILL_PROCESS;
}

int seccomp_active(void) {
  return current_task && task_seccomp_filter(current_task) != 0;
}

static void filter_unref(struct seccomp_filter *f) {
  while (f) {
    if (__atomic_sub_fetch(&f->refcount, 1, __ATOMIC_ACQ_REL) != 0)
      break; /* still shared by another task */
    struct seccomp_filter *prev = f->prev;
    kfree(f->prog);
    kfree(f);
    f = prev;
  }
}

void seccomp_inherit(struct task *parent, struct task *child) {
  struct seccomp_filter *f = parent ? task_seccomp_filter(parent) : 0;
  if (f)
    __atomic_add_fetch(&f->refcount, 1, __ATOMIC_ACQ_REL);
  task_set_seccomp_filter(child, f);
  task_set_no_new_privs(child, parent ? task_no_new_privs(parent) : 0);
}

/* Release a task's filter chain at exit/reap. */
void seccomp_release(struct task *t) {
  struct seccomp_filter *f = t ? task_seccomp_filter(t) : 0;
  if (f) {
    task_set_seccomp_filter(t, 0);
    filter_unref(f);
  }
}

int seccomp_set_no_new_privs(void) {
  task_set_no_new_privs(current_task, 1);
  return 0;
}
int seccomp_get_no_new_privs(void) {
  return task_no_new_privs(current_task);
}

int seccomp_set_mode_filter(u32 flags, const void *user_prog) {
  (void)flags; /* SECCOMP_FILTER_FLAG_* not honored yet (TSYNC/LOG/SPEC_ALLOW) */
  if (!current_task)
    return -ESRCH;

  struct sock_fprog fp;
  if (syscall_copyin(&fp, user_prog, sizeof(fp)) < 0)
    return -EFAULT;
  if (fp.len == 0 || fp.len > SECCOMP_MAX_INSNS)
    return -EINVAL;

  usize bytes = (usize)fp.len * sizeof(struct sock_filter);
  struct sock_filter *prog = kmalloc(bytes);
  if (!prog)
    return -ENOMEM;
  if (syscall_copyin(prog, fp.filter, bytes) < 0) {
    kfree(prog);
    return -EFAULT;
  }

  struct seccomp_filter *f = kzalloc(sizeof(*f));
  if (!f) {
    kfree(prog);
    return -ENOMEM;
  }
  f->prog = prog;
  f->len = fp.len;
  f->refcount = 1;
  f->prev = (struct seccomp_filter *)task_seccomp_filter(current_task);
  task_set_seccomp_filter(current_task, f);
  return 0;
}

int seccomp_set_mode_strict(void) {
  /* Strict mode is modeled as a tiny filter allowing only read/write/exit/
   * sigreturn and killing on anything else — built in-kernel so no user program
   * is needed. We store a sentinel filter with a NULL prog and length 0 that the
   * checker special-cases. */
  if (!current_task)
    return -ESRCH;
  struct seccomp_filter *f = kzalloc(sizeof(*f));
  if (!f)
    return -ENOMEM;
  f->prog = 0; /* sentinel: strict mode */
  f->len = 0;
  f->refcount = 1;
  f->prev = (struct seccomp_filter *)task_seccomp_filter(current_task);
  task_set_seccomp_filter(current_task, f);
  return 0;
}

/* Strict-mode allow-list (b1nix syscall numbers). */
static int strict_allows(u64 nr) {
  return nr == SYS_READ || nr == SYS_WRITE || nr == SYS_EXIT ||
         nr == SYS_SIGRETURN;
}

isize seccomp_filter_syscall(u64 number, u64 a0, u64 a1, u64 a2, u64 a3,
                             u64 a4, u64 a5, struct interrupt_frame *frame) {
  struct seccomp_filter *f =
      (struct seccomp_filter *)task_seccomp_filter(current_task);
  if (!f)
    return 0; /* no filter — allow */

  struct seccomp_data data;
  memset(&data, 0, sizeof(data));
  data.nr = (int)number;
  data.arch = AUDIT_ARCH_X86_64;
  data.instruction_pointer = frame ? frame->rip : 0;
  data.args[0] = a0;
  data.args[1] = a1;
  data.args[2] = a2;
  data.args[3] = a3;
  data.args[4] = a4;
  data.args[5] = a5;

  /* Run every stacked filter; the most-severe ACTION wins (Linux precedence:
   * KILL_PROCESS > KILL_THREAD > TRAP > ERRNO > TRACE > LOG > ALLOW), keeping
   * that verdict's data bits. Note the action precedence is NOT the raw numeric
   * order (KILL_PROCESS = 0x80000000 is the largest value but the most severe),
   * so we rank by an explicit severity rather than a raw min. */
  u32 verdict = SECCOMP_RET_ALLOW;
  int verdict_sev = 100;
  for (struct seccomp_filter *cur = f; cur; cur = cur->prev) {
    u32 r;
    if (cur->prog == 0) {
      /* strict-mode sentinel */
      r = strict_allows(number) ? SECCOMP_RET_ALLOW : SECCOMP_RET_KILL_PROCESS;
    } else {
      r = bpf_run(cur->prog, cur->len, &data);
    }
    int sev;
    switch (r & SECCOMP_RET_ACTION_FULL) {
    case SECCOMP_RET_KILL_PROCESS: sev = 0; break;
    case SECCOMP_RET_KILL_THREAD:  sev = 1; break;
    case SECCOMP_RET_TRAP:         sev = 2; break;
    case SECCOMP_RET_ERRNO:        sev = 3; break;
    case SECCOMP_RET_TRACE:        sev = 4; break;
    case SECCOMP_RET_LOG:          sev = 5; break;
    case SECCOMP_RET_ALLOW:        sev = 6; break;
    default:                       sev = 0; break; /* unknown action: fail closed */
    }
    if (sev < verdict_sev) {
      verdict_sev = sev;
      verdict = r;
    }
  }

  switch (verdict & SECCOMP_RET_ACTION_FULL) {
  case SECCOMP_RET_ALLOW:
  case SECCOMP_RET_LOG:
    return 0;
  case SECCOMP_RET_ERRNO: {
    u32 e = verdict & SECCOMP_RET_DATA;
    if (e > 4095)
      e = 4095; /* Linux clamps to MAX_ERRNO */
    return -(isize)e;
  }
  case SECCOMP_RET_TRAP:
    /* Deliver SIGSYS to the task; if it has no handler the default kills it. */
    scheduler_kill(current_task->id, SIGSYS);
    return -(isize)EPERM; /* the syscall is denied regardless */
  case SECCOMP_RET_TRACE:
    /* No tracer attached (ptrace is M80) — Linux treats "no tracer" as ENOSYS. */
    return -(isize)ENOSYS;
  case SECCOMP_RET_KILL_THREAD:
  case SECCOMP_RET_KILL_PROCESS:
  default:
    console_write("seccomp: pid ");
    console_write_dec(current_task->id);
    console_write(" killed by SIGSYS (syscall ");
    console_write_dec(number);
    console_write(")\n");
    scheduler_exit_current(TASK_EXIT_SIGNALED | SIGSYS);
    return -(isize)EPERM; /* unreachable */
  }
}
