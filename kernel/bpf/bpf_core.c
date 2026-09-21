/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * eBPF: the maps, the verifier and the interpreter. <b1nix/bpf.h> says what
 * this implements and what it refuses.
 *
 * THE VERIFIER IS THE POINT
 *
 * Everything else here is ordinary code. The verifier is what makes it safe to
 * run a program the kernel was handed by a process, and it works the way
 * Linux's does in outline: walk the instructions along every path that can be
 * taken, carry a model of what each register holds, and refuse the moment an
 * instruction could do something the model cannot prove is in bounds.
 *
 * The model is deliberately small, and small is what makes it trustworthy:
 *
 *   NOT_INIT     nothing has been written to this register; reading it would
 *                leak whatever the last program left there
 *   SCALAR       a number: read, written, returned, never used as an address
 *   PTR_CTX      the context the program was called with
 *   PTR_STACK    a pointer into this program's own 512-byte frame, at a known
 *                constant offset
 *   PTR_MAP_VALUE      a map value, which may be NULL until it is tested
 *   PTR_MAP_VALUE_OK   the same, after the program compared it against zero
 *   PTR_MAP      a map itself, which only a helper may be given
 *
 * A load or store must use a pointer whose kind AND offset the model knows,
 * and must land inside the object that pointer names. A helper call must match
 * a prototype. Backward jumps are refused, which makes the program a DAG and
 * the walk terminate -- Linux required exactly that until bounded loops
 * arrived, and proving a loop ends is a great deal of machinery for something
 * no tracing program here needs.
 */
#include <b1nix/bpf.h>

#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/ktime.h>
#include <b1nix/lapic.h>
#include <b1nix/mm.h>
#include <b1nix/posix.h>
#include <b1nix/sched.h>
#include <b1nix/spinlock.h>
#include <b1nix/syscall.h>
#include <b1nix/uidgid.h>
#include <b1nix/vfs.h>

#include <stdio.h>
#include <string.h>

/* bpf(2): 321 on x86_64, 280 on aarch64. */
#if defined(__aarch64__)
#define BPF_NR_bpf 280
#else
#define BPF_NR_bpf 321
#endif

/* ── the instruction encoding ────────────────────────────────────────────── */

#define BPF_CLASS(code) ((code) & 0x07)
#define BPF_LD 0x00
#define BPF_LDX 0x01
#define BPF_ST 0x02
#define BPF_STX 0x03
#define BPF_ALU 0x04
#define BPF_JMP 0x05
#define BPF_JMP32 0x06
#define BPF_ALU64 0x07

#define BPF_OP(code) ((code) & 0xf0)
#define BPF_ADD 0x00
#define BPF_SUB 0x10
#define BPF_MUL 0x20
#define BPF_DIV 0x30
#define BPF_OR 0x40
#define BPF_AND 0x50
#define BPF_LSH 0x60
#define BPF_RSH 0x70
#define BPF_NEG 0x80
#define BPF_MOD 0x90
#define BPF_XOR 0xa0
#define BPF_MOV 0xb0
#define BPF_ARSH 0xc0
#define BPF_END 0xd0

#define BPF_JA 0x00
#define BPF_JEQ 0x10
#define BPF_JGT 0x20
#define BPF_JGE 0x30
#define BPF_JSET 0x40
#define BPF_JNE 0x50
#define BPF_JSGT 0x60
#define BPF_JSGE 0x70
#define BPF_CALL 0x80
#define BPF_EXIT 0x90
#define BPF_JLT 0xa0
#define BPF_JLE 0xb0
#define BPF_JSLT 0xc0
#define BPF_JSLE 0xd0

#define BPF_SRC(code) ((code) & 0x08)
#define BPF_K 0x00
#define BPF_X 0x08

#define BPF_SIZE(code) ((code) & 0x18)
#define BPF_W 0x00
#define BPF_H 0x08
#define BPF_B 0x10
#define BPF_DW 0x18

#define BPF_IMM 0x00
#define BPF_MEM 0x60

#define INSN_DST(i) ((i)->dst_src & 0x0f)
#define INSN_SRC(i) (((i)->dst_src >> 4) & 0x0f)

#define BPF_REG_CNT 11
#define BPF_STACK_SIZE 512
#define BPF_MAX_INSNS 4096
#define BPF_MAX_MAPS 64
#define BPF_MAX_PROGS 64
#define BPF_PROG_MAX_MAPS 8
/* A program runs on every sample, in interrupt context, so it must finish.
 * The verifier forbids loops; this only bounds a pathological DAG. */
#define BPF_MAX_STEPS 16384

/* ── maps ────────────────────────────────────────────────────────────────── */

struct bpf_map {
  int used;
  u32 type;
  u32 key_size, value_size, max_entries;
  u8 *keys;    /* hash maps only */
  u8 *values;
  u8 *present;
  int refs;
  spinlock_t lock;
  char name[16];
};

static struct bpf_map g_maps[BPF_MAX_MAPS];
static spinlock_t g_bpf_lock = SPINLOCK_INIT;

static struct bpf_map *map_alloc(u32 type, u32 key_size, u32 value_size,
                                 u32 max_entries, const char *name) {
  if (type != BPF_MAP_TYPE_HASH && type != BPF_MAP_TYPE_ARRAY)
    return 0;
  if (!value_size || !max_entries || value_size > 4096 || max_entries > 65536)
    return 0;
  if (type == BPF_MAP_TYPE_ARRAY && key_size != 4)
    return 0;
  if (type == BPF_MAP_TYPE_HASH && (!key_size || key_size > 64))
    return 0;

  u64 flags;
  struct bpf_map *m = 0;

  spin_lock_irqsave(&g_bpf_lock, &flags);
  for (int i = 0; i < BPF_MAX_MAPS; i++) {
    if (!g_maps[i].used) {
      m = &g_maps[i];
      m->used = 1;
      break;
    }
  }
  spin_unlock_irqrestore(&g_bpf_lock, flags);
  if (!m)
    return 0;

  m->type = type;
  m->key_size = key_size;
  m->value_size = value_size;
  m->max_entries = max_entries;
  m->refs = 1;
  m->lock = SPINLOCK_INIT;
  memset(m->name, 0, sizeof(m->name));
  if (name)
    memcpy(m->name, name, sizeof(m->name) - 1);
  m->values = kzalloc((usize)value_size * max_entries);
  m->present = kzalloc(max_entries);
  m->keys = (type == BPF_MAP_TYPE_HASH) ? kzalloc((usize)key_size * max_entries)
                                        : 0;
  if (!m->values || !m->present || (type == BPF_MAP_TYPE_HASH && !m->keys)) {
    kfree(m->values);
    kfree(m->present);
    kfree(m->keys);
    m->values = m->present = m->keys = 0;
    m->used = 0;
    return 0;
  }
  if (type == BPF_MAP_TYPE_ARRAY)
    memset(m->present, 1, max_entries); /* every entry exists, reading zero */
  return m;
}

static void map_put(struct bpf_map *m) {
  if (!m)
    return;
  if (__atomic_sub_fetch(&m->refs, 1, __ATOMIC_ACQ_REL) > 0)
    return;
  kfree(m->values);
  kfree(m->present);
  kfree(m->keys);
  m->values = m->present = m->keys = 0;
  m->used = 0;
}

/* Caller holds the map lock. */
static int map_find(struct bpf_map *m, const void *key) {
  if (m->type == BPF_MAP_TYPE_ARRAY) {
    u32 idx;

    memcpy(&idx, key, 4);
    return idx < m->max_entries ? (int)idx : -1;
  }
  for (u32 i = 0; i < m->max_entries; i++) {
    if (m->present[i] &&
        memcmp(m->keys + (usize)i * m->key_size, key, m->key_size) == 0)
      return (int)i;
  }
  return -1;
}

static void *map_lookup(struct bpf_map *m, const void *key) {
  int slot = map_find(m, key);

  if (slot < 0)
    return 0;
  return m->values + (usize)slot * m->value_size;
}

static int map_update(struct bpf_map *m, const void *key, const void *value,
                      u64 flags) {
  u64 lf;
  int rc = 0;

  spin_lock_irqsave(&m->lock, &lf);
  int slot = map_find(m, key);

  if (slot >= 0) {
    if (flags == BPF_NOEXIST) {
      rc = -EEXIST;
      goto out;
    }
  } else {
    if (flags == BPF_EXIST) {
      rc = -ENOENT;
      goto out;
    }
    if (m->type == BPF_MAP_TYPE_ARRAY) {
      rc = -E2BIG;
      goto out;
    }
    for (u32 i = 0; i < m->max_entries; i++) {
      if (!m->present[i]) {
        slot = (int)i;
        break;
      }
    }
    if (slot < 0) {
      rc = -E2BIG;
      goto out;
    }
    memcpy(m->keys + (usize)slot * m->key_size, key, m->key_size);
    m->present[slot] = 1;
  }
  memcpy(m->values + (usize)slot * m->value_size, value, m->value_size);
out:
  spin_unlock_irqrestore(&m->lock, lf);
  return rc;
}

static int map_delete(struct bpf_map *m, const void *key) {
  u64 lf;
  int rc = -ENOENT;

  spin_lock_irqsave(&m->lock, &lf);
  int slot = map_find(m, key);

  if (slot >= 0) {
    if (m->type == BPF_MAP_TYPE_HASH)
      m->present[slot] = 0; /* an array entry cannot go away; Linux zeroes it */
    memset(m->values + (usize)slot * m->value_size, 0, m->value_size);
    rc = 0;
  }
  spin_unlock_irqrestore(&m->lock, lf);
  return rc;
}

/* ── programs ────────────────────────────────────────────────────────────── */

struct bpf_prog {
  int used;
  u32 type;
  u32 insn_cnt;
  struct bpf_insn *insns;
  struct bpf_map *maps[BPF_PROG_MAX_MAPS]; /* held while the program is loaded */
  int nmaps;
  int refs;
  char name[16];
};

static struct bpf_prog g_progs[BPF_MAX_PROGS];

static void prog_put(struct bpf_prog *p) {
  if (!p)
    return;
  if (__atomic_sub_fetch(&p->refs, 1, __ATOMIC_ACQ_REL) > 0)
    return;
  for (int i = 0; i < p->nmaps; i++)
    map_put(p->maps[i]);
  kfree(p->insns);
  p->insns = 0;
  p->nmaps = 0;
  p->used = 0;
}

void bpf_prog_put(void *prog) { prog_put((struct bpf_prog *)prog); }

/* ── the verifier ────────────────────────────────────────────────────────── */

enum reg_kind {
  REG_NOT_INIT = 0,
  REG_SCALAR,
  REG_PTR_CTX,
  REG_PTR_STACK,
  REG_PTR_MAP_VALUE,
  REG_PTR_MAP_VALUE_OK,
  REG_PTR_MAP,
};

struct reg_state {
  enum reg_kind kind;
  i64 off;
  struct bpf_map *map;
};

struct verifier {
  struct reg_state regs[BPF_REG_CNT];
  u8 *seen;
  const struct bpf_insn *insns;
  u32 cnt;
  struct bpf_prog *prog;
  char err[96];
};

static int verr(struct verifier *v, u32 pc, const char *msg) {
  snprintf(v->err, sizeof(v->err), "insn %u: %s", pc, msg);
  return -EINVAL;
}

struct helper_proto {
  u32 id;
  int nargs;
  enum reg_kind arg[5];
  enum reg_kind ret;
};

static const struct helper_proto g_helpers[] = {
    {BPF_FUNC_map_lookup_elem, 2, {REG_PTR_MAP, REG_PTR_STACK}, REG_PTR_MAP_VALUE},
    {BPF_FUNC_map_update_elem,
     4,
     {REG_PTR_MAP, REG_PTR_STACK, REG_PTR_STACK, REG_SCALAR},
     REG_SCALAR},
    {BPF_FUNC_map_delete_elem, 2, {REG_PTR_MAP, REG_PTR_STACK}, REG_SCALAR},
    {BPF_FUNC_ktime_get_ns, 0, {0}, REG_SCALAR},
    {BPF_FUNC_ktime_get_boot_ns, 0, {0}, REG_SCALAR},
    {BPF_FUNC_get_smp_processor_id, 0, {0}, REG_SCALAR},
    {BPF_FUNC_get_current_pid_tgid, 0, {0}, REG_SCALAR},
    {BPF_FUNC_get_current_uid_gid, 0, {0}, REG_SCALAR},
    {BPF_FUNC_get_current_comm, 2, {REG_PTR_STACK, REG_SCALAR}, REG_SCALAR},
    {BPF_FUNC_trace_printk, 2, {REG_PTR_STACK, REG_SCALAR}, REG_SCALAR},
};

static const struct helper_proto *helper_of(u32 id) {
  for (usize i = 0; i < sizeof(g_helpers) / sizeof(g_helpers[0]); i++)
    if (g_helpers[i].id == id)
      return &g_helpers[i];
  return 0;
}

static int reg_is_ptr(enum reg_kind k) {
  return k == REG_PTR_CTX || k == REG_PTR_STACK || k == REG_PTR_MAP_VALUE_OK ||
         k == REG_PTR_MAP;
}

/* The context a tracing program is given: three u64s it may read. */
#define BPF_CTX_SIZE 32

static int verify_from(struct verifier *v, u32 pc, int depth) {
  if (depth > 64)
    return verr(v, pc, "branch nesting too deep");

  while (pc < v->cnt) {
    const struct bpf_insn *i = &v->insns[pc];
    u8 cls = BPF_CLASS(i->code);
    int dst = INSN_DST(i), src = INSN_SRC(i);

    if (dst >= BPF_REG_CNT || src >= BPF_REG_CNT)
      return verr(v, pc, "register number out of range");
    if (v->seen[pc])
      return 0; /* this path is proved already */
    v->seen[pc] = 1;

    switch (cls) {
    case BPF_ALU:
    case BPF_ALU64: {
      u8 op = BPF_OP(i->code);

      if (op == BPF_END || op == BPF_NEG) {
        if (v->regs[dst].kind != REG_SCALAR)
          return verr(v, pc, "arithmetic on something that is not a number");
        break;
      }
      if (BPF_SRC(i->code) == BPF_X && v->regs[src].kind == REG_NOT_INIT)
        return verr(v, pc, "reading a register that was never written");
      if (op == BPF_MOV) {
        if (BPF_SRC(i->code) == BPF_X) {
          v->regs[dst] = v->regs[src];
          if (cls == BPF_ALU && reg_is_ptr(v->regs[dst].kind))
            v->regs[dst].kind = REG_SCALAR; /* a 32-bit move keeps no pointer */
        } else {
          v->regs[dst].kind = REG_SCALAR;
          v->regs[dst].off = i->imm;
          v->regs[dst].map = 0;
        }
        break;
      }
      if (reg_is_ptr(v->regs[dst].kind)) {
        if (v->regs[dst].kind != REG_PTR_STACK || BPF_SRC(i->code) != BPF_K ||
            (op != BPF_ADD && op != BPF_SUB))
          return verr(v, pc, "pointer arithmetic this cannot follow");
        v->regs[dst].off += (op == BPF_ADD) ? i->imm : -i->imm;
        break;
      }
      if (v->regs[dst].kind == REG_NOT_INIT)
        return verr(v, pc, "arithmetic on a register that was never written");
      if ((op == BPF_DIV || op == BPF_MOD) && BPF_SRC(i->code) == BPF_K &&
          i->imm == 0)
        return verr(v, pc, "division by a literal zero");
      v->regs[dst].kind = REG_SCALAR;
      break;
    }

    case BPF_LDX: {
      int sz = BPF_SIZE(i->code);
      int bytes = sz == BPF_B ? 1 : sz == BPF_H ? 2 : sz == BPF_W ? 4 : 8;
      struct reg_state *p = &v->regs[src];

      if (p->kind == REG_PTR_STACK) {
        i64 at = p->off + i->off;

        if (at < -BPF_STACK_SIZE || at + bytes > 0)
          return verr(v, pc, "stack read outside the frame");
      } else if (p->kind == REG_PTR_MAP_VALUE_OK) {
        if (i->off < 0 || (u32)(i->off + bytes) > p->map->value_size)
          return verr(v, pc, "read outside the map value");
      } else if (p->kind == REG_PTR_CTX) {
        if (i->off < 0 || i->off + bytes > BPF_CTX_SIZE)
          return verr(v, pc, "read outside the context");
      } else if (p->kind == REG_PTR_MAP_VALUE) {
        return verr(v, pc, "map value used before it was tested for NULL");
      } else {
        return verr(v, pc, "reading through something that is not a pointer");
      }
      v->regs[dst].kind = REG_SCALAR;
      v->regs[dst].map = 0;
      break;
    }

    case BPF_ST:
    case BPF_STX: {
      int sz = BPF_SIZE(i->code);
      int bytes = sz == BPF_B ? 1 : sz == BPF_H ? 2 : sz == BPF_W ? 4 : 8;
      struct reg_state *p = &v->regs[dst];

      if (cls == BPF_STX && v->regs[src].kind == REG_NOT_INIT)
        return verr(v, pc, "storing a register that was never written");
      if (p->kind == REG_PTR_STACK) {
        i64 at = p->off + i->off;

        if (at < -BPF_STACK_SIZE || at + bytes > 0)
          return verr(v, pc, "stack write outside the frame");
      } else if (p->kind == REG_PTR_MAP_VALUE_OK) {
        if (i->off < 0 || (u32)(i->off + bytes) > p->map->value_size)
          return verr(v, pc, "write outside the map value");
      } else if (p->kind == REG_PTR_MAP_VALUE) {
        return verr(v, pc, "map value used before it was tested for NULL");
      } else {
        return verr(v, pc, "writing through something that is not a pointer");
      }
      break;
    }

    case BPF_LD: {
      /* The 16-byte load: either a 64-bit literal, or a map named by its
       * descriptor, which is how every loader passes a map to a program. */
      if (i->code != (BPF_LD | BPF_DW | BPF_IMM))
        return verr(v, pc, "unsupported load");
      if (pc + 1 >= v->cnt)
        return verr(v, pc, "truncated 16-byte load");
      if (src == 1) { /* BPF_PSEUDO_MAP_FD */
        struct vfs_handle *h = scheduler_fd_get(i->imm);
        struct bpf_map *m = (h && h->private_data) ? h->private_data : 0;

        if (!m || !m->used)
          return verr(v, pc, "the map descriptor is not a map");

        int known = -1;

        for (int k = 0; k < v->prog->nmaps; k++)
          if (v->prog->maps[k] == m)
            known = k;
        if (known < 0) {
          if (v->prog->nmaps >= BPF_PROG_MAX_MAPS)
            return verr(v, pc, "too many maps in one program");
          __atomic_add_fetch(&m->refs, 1, __ATOMIC_ACQ_REL);
          known = v->prog->nmaps++;
          v->prog->maps[known] = m;
        }
        /* Rewrite the instruction so the interpreter needs no descriptor
         * table: the map's index in this program is all it has to know. This
         * is what Linux's verifier does too, and it is why a program keeps
         * working after the loader closes the map's fd. */
        ((struct bpf_insn *)i)->off = (i16)known;
        v->regs[dst].kind = REG_PTR_MAP;
        v->regs[dst].map = m;
      } else {
        v->regs[dst].kind = REG_SCALAR;
        v->regs[dst].map = 0;
      }
      v->seen[pc + 1] = 1;
      pc += 2;
      continue;
    }

    case BPF_JMP:
    case BPF_JMP32: {
      u8 op = BPF_OP(i->code);

      if (op == BPF_EXIT) {
        if (v->regs[0].kind == REG_NOT_INIT)
          return verr(v, pc, "exit without setting r0");
        return 0;
      }
      if (op == BPF_CALL) {
        const struct helper_proto *proto = helper_of((u32)i->imm);

        if (!proto)
          return verr(v, pc, "call to a helper this kernel does not have");
        for (int a = 0; a < proto->nargs; a++) {
          struct reg_state *r = &v->regs[1 + a];

          if (r->kind == REG_NOT_INIT)
            return verr(v, pc, "helper argument was never written");
          if (proto->arg[a] == REG_PTR_MAP && r->kind != REG_PTR_MAP)
            return verr(v, pc, "helper wanted a map here");
          if (proto->arg[a] == REG_PTR_STACK && r->kind != REG_PTR_STACK &&
              r->kind != REG_PTR_MAP_VALUE_OK)
            return verr(v, pc, "helper wanted a pointer to memory here");
          if (proto->arg[a] == REG_SCALAR && r->kind != REG_SCALAR)
            return verr(v, pc, "helper wanted a number here");
        }
        struct bpf_map *arg_map = v->regs[1].map;

        v->regs[0].kind = proto->ret;
        v->regs[0].map = (proto->ret == REG_PTR_MAP_VALUE) ? arg_map : 0;
        for (int a = 1; a <= 5; a++) {
          v->regs[a].kind = REG_NOT_INIT;
          v->regs[a].map = 0;
        }
        break;
      }

      i64 target = (i64)pc + 1 + i->off;

      if (target < 0 || target >= (i64)v->cnt)
        return verr(v, pc, "jump outside the program");
      if (i->off < 0)
        return verr(v, pc, "backward jump: this verifier proves no loops");
      if (op == BPF_JA) {
        pc = (u32)target;
        continue;
      }
      if (BPF_SRC(i->code) == BPF_X && v->regs[src].kind == REG_NOT_INIT)
        return verr(v, pc, "comparing a register that was never written");
      if (v->regs[dst].kind == REG_NOT_INIT)
        return verr(v, pc, "comparing a register that was never written");

      /* Both arms, each with its own state. Testing a map value against zero
       * is what proves it non-NULL, and which arm learns that depends on the
       * comparison. */
      struct verifier branch = *v;

      if (v->regs[dst].kind == REG_PTR_MAP_VALUE && BPF_SRC(i->code) == BPF_K &&
          i->imm == 0) {
        if (op == BPF_JEQ)
          v->regs[dst].kind = REG_PTR_MAP_VALUE_OK;   /* fallthrough: not null */
        else if (op == BPF_JNE)
          branch.regs[dst].kind = REG_PTR_MAP_VALUE_OK; /* taken: not null */
      }

      int rc = verify_from(&branch, (u32)target, depth + 1);

      if (rc < 0) {
        memcpy(v->err, branch.err, sizeof(v->err));
        return rc;
      }
      break;
    }

    default:
      return verr(v, pc, "unknown instruction class");
    }
    pc++;
  }
  return verr(v, v->cnt, "the program runs off its end without exiting");
}

static int bpf_verify(struct bpf_prog *p, u64 log_buf, u32 log_size) {
  struct verifier v;

  memset(&v, 0, sizeof(v));
  v.insns = p->insns;
  v.cnt = p->insn_cnt;
  v.prog = p;
  v.seen = kzalloc(p->insn_cnt);
  if (!v.seen)
    return -ENOMEM;

  v.regs[1].kind = REG_PTR_CTX;  /* the context, on entry */
  v.regs[10].kind = REG_PTR_STACK;
  v.regs[10].off = 0;            /* the frame pointer: the TOP of the frame */

  int rc = verify_from(&v, 0, 0);

  kfree(v.seen);
  if (rc < 0 && log_buf && log_size) {
    usize n = strlen(v.err) + 1;

    (void)syscall_copyout((void *)(usize)log_buf, v.err,
                          n < log_size ? n : log_size);
  }
  return rc;
}

/* ── the interpreter ─────────────────────────────────────────────────────── */

struct bpf_run_ctx {
  u64 ip;
  u64 pid_tgid;
  u64 cpu;
  u64 arg;
};

static u64 bpf_helper(u32 id, u64 a1, u64 a2, u64 a3, u64 a4) {
  switch (id) {
  case BPF_FUNC_map_lookup_elem: {
    struct bpf_map *m = (struct bpf_map *)(usize)a1;
    u64 lf;
    void *v;

    if (!m || !m->used)
      return 0;
    spin_lock_irqsave(&m->lock, &lf);
    v = map_lookup(m, (const void *)(usize)a2);
    spin_unlock_irqrestore(&m->lock, lf);
    return (u64)(usize)v;
  }
  case BPF_FUNC_map_update_elem: {
    struct bpf_map *m = (struct bpf_map *)(usize)a1;

    if (!m || !m->used)
      return (u64)(i64)-EINVAL;
    return (u64)(i64)map_update(m, (const void *)(usize)a2,
                                (const void *)(usize)a3, a4);
  }
  case BPF_FUNC_map_delete_elem: {
    struct bpf_map *m = (struct bpf_map *)(usize)a1;

    if (!m || !m->used)
      return (u64)(i64)-EINVAL;
    return (u64)(i64)map_delete(m, (const void *)(usize)a2);
  }
  case BPF_FUNC_ktime_get_ns:
  case BPF_FUNC_ktime_get_boot_ns:
    return ktime_monotonic_ns();
  case BPF_FUNC_get_smp_processor_id: {
    struct percpu *p = get_percpu();

    return p ? (u64)p->cpu_id : 0;
  }
  case BPF_FUNC_get_current_pid_tgid: {
    struct task *t = current_task;

    return t ? (((u64)task_tgid(t) << 32) | (u32)t->id) : 0;
  }
  case BPF_FUNC_get_current_uid_gid: {
    const struct cred *c = scheduler_get_current_cred();

    return c ? (((u64)c->egid << 32) | c->euid) : 0;
  }
  case BPF_FUNC_get_current_comm: {
    struct task *t = current_task;
    char *dst = (char *)(usize)a1;
    usize n = (usize)a2;

    if (!dst || !n)
      return (u64)(i64)-EINVAL;
    memset(dst, 0, n);
    if (t && t->name) {
      usize l = strlen(t->name);

      memcpy(dst, t->name, l < n - 1 ? l : n - 1);
    }
    return 0;
  }
  case BPF_FUNC_trace_printk: {
    /* Printed literally: the string lives in the program's own frame, which is
     * kernel memory and safe to read, but letting a program drive a format
     * string is not something this kernel offers it. */
    char buf[128];
    const char *fmt = (const char *)(usize)a1;
    usize n = (usize)a2;

    if (!fmt || !n)
      return (u64)(i64)-EINVAL;
    if (n > sizeof(buf) - 1)
      n = sizeof(buf) - 1;
    memcpy(buf, fmt, n);
    buf[n] = 0;
    console_write("bpf: ");
    console_write(buf);
    console_write("\n");
    return (u64)n;
  }
  default:
    return (u64)(i64)-EINVAL;
  }
}

static u64 bpf_run(struct bpf_prog *p, struct bpf_run_ctx *ctx) {
  u64 regs[BPF_REG_CNT];
  u8 stack[BPF_STACK_SIZE];
  u32 pc = 0;
  u32 steps = 0;

  memset(regs, 0, sizeof(regs));
  memset(stack, 0, sizeof(stack));
  regs[1] = (u64)(usize)ctx;
  regs[10] = (u64)(usize)(stack + BPF_STACK_SIZE);

  while (pc < p->insn_cnt && steps++ < BPF_MAX_STEPS) {
    const struct bpf_insn *i = &p->insns[pc];
    u8 cls = BPF_CLASS(i->code);
    u8 op = BPF_OP(i->code);
    int dst = INSN_DST(i), src = INSN_SRC(i);
    u64 sv = (BPF_SRC(i->code) == BPF_X) ? regs[src] : (u64)(i64)i->imm;
    int is32 = (cls == BPF_ALU);

    switch (cls) {
    case BPF_ALU:
    case BPF_ALU64: {
      u64 a = regs[dst];

      if (is32)
        a = (u32)a;
      switch (op) {
      case BPF_ADD: a += sv; break;
      case BPF_SUB: a -= sv; break;
      case BPF_MUL: a *= sv; break;
      case BPF_DIV: a = sv ? a / sv : 0; break;
      case BPF_MOD: a = sv ? a % sv : a; break;
      case BPF_OR: a |= sv; break;
      case BPF_AND: a &= sv; break;
      case BPF_XOR: a ^= sv; break;
      case BPF_LSH: a <<= (sv & (is32 ? 31 : 63)); break;
      case BPF_RSH: a >>= (sv & (is32 ? 31 : 63)); break;
      case BPF_ARSH:
        a = is32 ? (u64)(u32)((i32)(u32)a >> (sv & 31))
                 : (u64)((i64)a >> (sv & 63));
        break;
      case BPF_NEG: a = (u64)(-(i64)a); break;
      case BPF_MOV: a = sv; break;
      case BPF_END: break; /* byte swaps: the host is little-endian */
      default: return 0;
      }
      regs[dst] = is32 ? (u32)a : a;
      break;
    }

    case BPF_LDX: {
      const u8 *addr = (const u8 *)(usize)(regs[src] + i->off);

      switch (BPF_SIZE(i->code)) {
      case BPF_B: regs[dst] = *addr; break;
      case BPF_H: { u16 t; memcpy(&t, addr, 2); regs[dst] = t; break; }
      case BPF_W: { u32 t; memcpy(&t, addr, 4); regs[dst] = t; break; }
      default: { u64 t; memcpy(&t, addr, 8); regs[dst] = t; break; }
      }
      break;
    }

    case BPF_ST:
    case BPF_STX: {
      u8 *addr = (u8 *)(usize)(regs[dst] + i->off);
      u64 val = (cls == BPF_ST) ? (u64)(i64)i->imm : regs[src];

      switch (BPF_SIZE(i->code)) {
      case BPF_B: *addr = (u8)val; break;
      case BPF_H: { u16 t = (u16)val; memcpy(addr, &t, 2); break; }
      case BPF_W: { u32 t = (u32)val; memcpy(addr, &t, 4); break; }
      default: memcpy(addr, &val, 8); break;
      }
      break;
    }

    case BPF_LD:
      if (i->code == (BPF_LD | BPF_DW | BPF_IMM)) {
        if (src == 1) {
          /* The verifier put the map's index in this program into off. */
          int idx = i->off;

          regs[dst] = (idx >= 0 && idx < p->nmaps)
                          ? (u64)(usize)p->maps[idx]
                          : 0;
        } else {
          u64 lo = (u32)i->imm;
          u64 hi = (u32)p->insns[pc + 1].imm;

          regs[dst] = lo | (hi << 32);
        }
        pc += 2;
        continue;
      }
      return 0;

    case BPF_JMP:
    case BPF_JMP32: {
      if (op == BPF_EXIT)
        return regs[0];
      if (op == BPF_CALL) {
        regs[0] = bpf_helper((u32)i->imm, regs[1], regs[2], regs[3], regs[4]);
        break;
      }
      u64 a = regs[dst], b = sv;

      if (cls == BPF_JMP32) {
        a = (u32)a;
        b = (u32)b;
      }
      int take = 0;

      switch (op) {
      case BPF_JA: take = 1; break;
      case BPF_JEQ: take = (a == b); break;
      case BPF_JNE: take = (a != b); break;
      case BPF_JGT: take = (a > b); break;
      case BPF_JGE: take = (a >= b); break;
      case BPF_JLT: take = (a < b); break;
      case BPF_JLE: take = (a <= b); break;
      case BPF_JSET: take = ((a & b) != 0); break;
      case BPF_JSGT: take = ((i64)a > (i64)b); break;
      case BPF_JSGE: take = ((i64)a >= (i64)b); break;
      case BPF_JSLT: take = ((i64)a < (i64)b); break;
      case BPF_JSLE: take = ((i64)a <= (i64)b); break;
      default: return 0;
      }
      if (take) {
        pc = (u32)((i64)pc + 1 + i->off);
        continue;
      }
      break;
    }

    default:
      return 0;
    }
    pc++;
  }
  return regs[0];
}

u64 bpf_run_perf(void *prog, u64 ip, u64 pid_tgid, u64 cpu) {
  struct bpf_prog *p = prog;
  struct bpf_run_ctx ctx;

  if (!p || !p->used)
    return 0;
  memset(&ctx, 0, sizeof(ctx));
  ctx.ip = ip;
  ctx.pid_tgid = pid_tgid;
  ctx.cpu = cpu;
  return bpf_run(p, &ctx);
}

/* ── the descriptors ─────────────────────────────────────────────────────── */

static void bpf_map_release(struct vfs_handle *h);
static void bpf_prog_release(struct vfs_handle *h);

static const struct vfs_file_ops bpf_map_ops = {.release = bpf_map_release};
static const struct vfs_file_ops bpf_prog_ops = {.release = bpf_prog_release};

static void bpf_map_release(struct vfs_handle *h) {
  struct bpf_map *m = h ? (struct bpf_map *)h->private_data : 0;

  h->private_data = 0;
  if (h->node) {
    vfs_node_put(h->node);
    h->node = 0;
  }
  map_put(m);
}

static void bpf_prog_release(struct vfs_handle *h) {
  struct bpf_prog *p = h ? (struct bpf_prog *)h->private_data : 0;

  h->private_data = 0;
  if (h->node) {
    vfs_node_put(h->node);
    h->node = 0;
  }
  prog_put(p);
}

static int bpf_anon_fd(void *obj, const struct vfs_file_ops *ops,
                       const char *name) {
  struct vfs_node *node = vfs_create_node(VFS_DEVICE);

  if (!node)
    return -ENOMEM;
  strncpy(node->name, name, sizeof(node->name) - 1);
  node->name[sizeof(node->name) - 1] = 0;
  node->deleted = 1;
  node->inode->nlink = 0;
  node->inode->mode = 0600;

  struct vfs_handle *h = alloc_raw_handle(VFS_HANDLE_NODE);

  if (!h) {
    vfs_node_put(node);
    return -ENFILE;
  }
  h->node = node;
  h->private_data = obj;
  h->ops = ops;
  h->flags = B1NIX_O_RDWR;
  h->no_notify = 1;

  int fd = scheduler_fd_alloc(h);

  if (fd < 0) {
    h->private_data = 0;
    vfs_handle_release(h);
    vfs_node_put(node);
  }
  return fd;
}

void *bpf_prog_get(int prog_fd) {
  struct vfs_handle *h = scheduler_fd_get(prog_fd);

  if (!h || h->ops != &bpf_prog_ops || !h->private_data)
    return 0;

  struct bpf_prog *p = h->private_data;

  __atomic_add_fetch(&p->refs, 1, __ATOMIC_ACQ_REL);
  return p;
}

static struct bpf_map *map_from_fd(int fd) {
  struct vfs_handle *h = scheduler_fd_get(fd);

  if (!h || h->ops != &bpf_map_ops)
    return 0;
  return (struct bpf_map *)h->private_data;
}

/* ── bpf(2) ──────────────────────────────────────────────────────────────── */

static isize bpf_cmd(u64 cmd, u64 uattr, u64 size) {
  union bpf_attr attr;

  if (size > sizeof(attr))
    size = sizeof(attr);
  memset(&attr, 0, sizeof(attr));
  if (size && syscall_copyin(&attr, (const void *)(usize)uattr, (usize)size) < 0)
    return -EFAULT;

  switch (cmd) {
  case BPF_MAP_CREATE: {
    struct bpf_map *m =
        map_alloc(attr.create.map_type, attr.create.key_size,
                  attr.create.value_size, attr.create.max_entries,
                  attr.create.map_name);

    if (!m)
      return -EINVAL;

    int fd = bpf_anon_fd(m, &bpf_map_ops, "bpf-map");

    if (fd < 0)
      map_put(m);
    return fd;
  }

  case BPF_MAP_LOOKUP_ELEM:
  case BPF_MAP_UPDATE_ELEM:
  case BPF_MAP_DELETE_ELEM:
  case BPF_MAP_GET_NEXT_KEY: {
    struct bpf_map *m = map_from_fd((int)attr.elem.map_fd);

    if (!m || !m->used)
      return -EBADF;

    static u8 value[4096]; /* bounded by map_alloc's value_size limit */
    u8 key[64];
    u64 lf;

    if (m->key_size > sizeof(key) || m->value_size > sizeof(value))
      return -EINVAL;
    memset(key, 0, sizeof(key));
    if (attr.elem.key &&
        syscall_copyin(key, (const void *)(usize)attr.elem.key, m->key_size) < 0)
      return -EFAULT;

    switch (cmd) {
    case BPF_MAP_LOOKUP_ELEM: {
      spin_lock_irqsave(&m->lock, &lf);
      void *v = map_lookup(m, key);

      if (v)
        memcpy(value, v, m->value_size);
      spin_unlock_irqrestore(&m->lock, lf);
      if (!v)
        return -ENOENT;
      return syscall_copyout((void *)(usize)attr.elem.value, value,
                             m->value_size) == 0
                 ? 0
                 : -EFAULT;
    }
    case BPF_MAP_UPDATE_ELEM:
      if (syscall_copyin(value, (const void *)(usize)attr.elem.value,
                         m->value_size) < 0)
        return -EFAULT;
      return map_update(m, key, value, attr.elem.flags);
    case BPF_MAP_DELETE_ELEM:
      return map_delete(m, key);
    default: {
      spin_lock_irqsave(&m->lock, &lf);
      int start = attr.elem.key ? map_find(m, key) + 1 : 0;
      int found = -1;

      for (u32 i = (u32)(start < 0 ? 0 : start); i < m->max_entries; i++) {
        if (m->present[i]) {
          found = (int)i;
          break;
        }
      }
      if (found >= 0) {
        if (m->type == BPF_MAP_TYPE_ARRAY) {
          u32 idx = (u32)found;

          memcpy(key, &idx, 4);
        } else {
          memcpy(key, m->keys + (usize)found * m->key_size, m->key_size);
        }
      }
      spin_unlock_irqrestore(&m->lock, lf);
      if (found < 0)
        return -ENOENT;
      return syscall_copyout((void *)(usize)attr.elem.next_key, key,
                             m->key_size) == 0
                 ? 0
                 : -EFAULT;
    }
    }
  }

  case BPF_PROG_LOAD: {
    const struct cred *cred = scheduler_get_current_cred();

    /* Loading a program the kernel will run needs privilege: Linux made
     * unprivileged BPF opt-in and then off by default, and nothing here needs
     * it to be open to everyone. */
    if (!cred || !cred_has_cap_effective(cred, CAP_SYS_ADMIN))
      return -EPERM;
    if (attr.load.prog_type != BPF_PROG_TYPE_PERF_EVENT &&
        attr.load.prog_type != BPF_PROG_TYPE_KPROBE &&
        attr.load.prog_type != BPF_PROG_TYPE_TRACEPOINT)
      return -EOPNOTSUPP; /* only the tracing types have anywhere to run */
    if (!attr.load.insn_cnt || attr.load.insn_cnt > BPF_MAX_INSNS)
      return -E2BIG;

    struct bpf_prog *p = 0;
    u64 flags;

    spin_lock_irqsave(&g_bpf_lock, &flags);
    for (int i = 0; i < BPF_MAX_PROGS; i++) {
      if (!g_progs[i].used) {
        p = &g_progs[i];
        memset(p, 0, sizeof(*p));
        p->used = 1;
        break;
      }
    }
    spin_unlock_irqrestore(&g_bpf_lock, flags);
    if (!p)
      return -ENOSPC;

    p->type = attr.load.prog_type;
    p->insn_cnt = attr.load.insn_cnt;
    p->refs = 1;
    memcpy(p->name, attr.load.prog_name, sizeof(p->name) - 1);
    p->insns = kzalloc((usize)p->insn_cnt * sizeof(struct bpf_insn));
    if (!p->insns) {
      p->used = 0;
      return -ENOMEM;
    }
    if (syscall_copyin(p->insns, (const void *)(usize)attr.load.insns,
                       (usize)p->insn_cnt * sizeof(struct bpf_insn)) < 0) {
      kfree(p->insns);
      p->insns = 0;
      p->used = 0;
      return -EFAULT;
    }

    int rc = bpf_verify(p, attr.load.log_buf, attr.load.log_size);

    if (rc < 0) {
      prog_put(p);
      return rc;
    }

    int fd = bpf_anon_fd(p, &bpf_prog_ops, "bpf-prog");

    if (fd < 0)
      prog_put(p);
    return fd;
  }

  case BPF_PROG_TEST_RUN: {
    struct vfs_handle *h = scheduler_fd_get((int)attr.test.prog_fd);

    if (!h || h->ops != &bpf_prog_ops || !h->private_data)
      return -EBADF;

    struct bpf_prog *p = h->private_data;
    struct bpf_run_ctx ctx;

    memset(&ctx, 0, sizeof(ctx));
    if (attr.test.data_in && attr.test.data_size_in >= sizeof(u64)) {
      u64 first = 0;

      if (syscall_copyin(&first, (const void *)(usize)attr.test.data_in,
                         sizeof(first)) < 0)
        return -EFAULT;
      ctx.ip = first;
    }
    ctx.pid_tgid = current_task ? (((u64)task_tgid(current_task) << 32) |
                                   (u32)current_task->id)
                                : 0;

    u64 t0 = ktime_monotonic_ns();
    u64 ret = bpf_run(p, &ctx);

    attr.test.retval = (u32)ret;
    attr.test.duration = (u32)(ktime_monotonic_ns() - t0);
    attr.test.data_size_out = 0;
    return syscall_copyout((void *)(usize)uattr, &attr, (usize)size) == 0
               ? 0
               : -EFAULT;
  }

  default:
    /* Pinning, cgroup attachment, ids, BTF: none of them exists here, and each
     * is refused where the loader can see it rather than ignored. */
    return -EOPNOTSUPP;
  }
}

int bpf_syscall(u64 nr, u64 a0, u64 a1, u64 a2, u64 *ret) {
  if (nr != BPF_NR_bpf)
    return 0;
  *ret = (u64)bpf_cmd(a0, a1, a2);
  return 1;
}
