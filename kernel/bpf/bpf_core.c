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
#include <b1nix/klog.h>
#include <b1nix/bpf.h>

#if defined(__aarch64__)
#include <b1nix/arch_aarch64.h>
#else
#include <b1nix/arch_x86_64.h>
#endif

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

#include <b1nix/bpf_insn.h>

/* ── maps ────────────────────────────────────────────────────────────────── */

/*
 * A BPF_MAP_TYPE_RINGBUF: Linux's layout, because libbpf maps it and reads it
 * without asking the kernel anything. Page 0 holds the consumer position, the
 * one word userspace writes; page 1 the producer position; then the data, which
 * userspace maps twice in a row so a record read across the end is contiguous.
 * Every record is an 8-byte header -- its length, BUSY while the program still
 * owns it, DISCARD when it is to be skipped -- then the data, 8-byte aligned.
 *
 * The kernel has no second mapping of the data, so a record never crosses the
 * end: when one would, the rest of the ring becomes a discarded record and the
 * new one starts at the beginning. The producer position the kernel works from
 * is its own; the page userspace can see only publishes it.
 */
struct bpf_ringbuf {
  u64 cons_phys, prod_phys, data_phys;
  usize data_pages;
  u64 size; /* data bytes, a power of two */
  u64 prod;
  spinlock_t lock;
};

struct bpf_map {
  int used;
  u32 type;
  u32 key_size, value_size, max_entries;
  u8 *keys;    /* hash maps only */
  u8 *values;
  u8 *present;
  struct bpf_ringbuf *rb; /* ring buffers only */
  struct vfs_handle **evs; /* perf-event arrays: the events, held */
  u32 ncpu, stride;        /* per-CPU maps: copies, and bytes per copy */
  int refs;
  spinlock_t lock;
  char name[16];
  u32 id; /* BPF_MAP_GET_NEXT_ID's name for it */
};

static struct bpf_map g_maps[BPF_MAX_MAPS];
/* Ids are handed out in order and never reused while the kernel runs, as on
 * Linux: a tool that walked to id N and comes back finds N or nothing, never
 * a different object under the same number. */
static u32 g_map_next_id = 1, g_prog_next_id = 1;

/* Take a reference to an object whose count may be falling to zero on another
 * CPU: only a live one (refs > 0) can be taken. */
static int ref_get_live(int *refs) {
  int r = __atomic_load_n(refs, __ATOMIC_ACQUIRE);

  while (r > 0)
    if (__atomic_compare_exchange_n(refs, &r, r + 1, 0, __ATOMIC_ACQ_REL,
                                    __ATOMIC_ACQUIRE))
      return 1;
  return 0;
}
static spinlock_t g_bpf_lock = SPINLOCK_INIT;

static void ringbuf_free(struct bpf_ringbuf *rb) {
  if (!rb)
    return;
  if (rb->cons_phys)
    pmm_free_frame(rb->cons_phys);
  if (rb->prod_phys)
    pmm_free_frame(rb->prod_phys);
  for (usize i = 0; rb->data_phys && i < rb->data_pages; i++)
    pmm_free_frame(rb->data_phys + i * PAGE_SIZE);
  kfree(rb);
}

/* max_entries is the data size: a power of two, a whole number of pages, as
 * Linux requires. */
static struct bpf_ringbuf *ringbuf_alloc(u32 size) {
  struct bpf_ringbuf *rb;

  if (size < PAGE_SIZE || (size & (size - 1)) || size > (16u << 20))
    return 0;
  rb = kzalloc(sizeof(*rb));
  if (!rb)
    return 0;
  rb->size = size;
  rb->data_pages = size / PAGE_SIZE;
  rb->lock = SPINLOCK_INIT;
  rb->cons_phys = pmm_alloc_frame();
  rb->prod_phys = pmm_alloc_frame();
  rb->data_phys = pmm_alloc_frames(rb->data_pages);
  if (!rb->cons_phys || !rb->prod_phys || !rb->data_phys) {
    if (!rb->data_phys)
      rb->data_pages = 0;
    ringbuf_free(rb);
    return 0;
  }
  return rb;
}

extern int g_max_cpus;

/* Array-shaped maps answer by index and every index exists; hash-shaped ones
 * keep the keys they were given. */
static int map_is_array(const struct bpf_map *m) {
  return m->type == BPF_MAP_TYPE_ARRAY || m->type == BPF_MAP_TYPE_PERCPU_ARRAY;
}

static int map_is_hash(const struct bpf_map *m) {
  return m->type == BPF_MAP_TYPE_HASH || m->type == BPF_MAP_TYPE_PERCPU_HASH;
}

static int map_is_percpu(const struct bpf_map *m) {
  return m->type == BPF_MAP_TYPE_PERCPU_ARRAY ||
         m->type == BPF_MAP_TYPE_PERCPU_HASH;
}

/* The bytes one element takes: a per-CPU map keeps one value per possible
 * CPU, each rounded up to 8 bytes -- the layout bpf(2) reads and writes them
 * in, which is what libbpf sizes its buffers for from cpu/possible. */
static usize map_elem_size(const struct bpf_map *m) {
  return map_is_percpu(m) ? (usize)m->ncpu * m->stride : m->value_size;
}

static u8 *map_elem(struct bpf_map *m, int slot) {
  return m->values + (usize)slot * map_elem_size(m);
}

static u32 bpf_this_cpu(void) {
  struct percpu *p = get_percpu();

  return p ? (u32)p->cpu_id : 0;
}

static struct bpf_map *map_alloc(u32 type, u32 key_size, u32 value_size,
                                 u32 max_entries, const char *name) {
  struct bpf_ringbuf *rb = 0;
  u32 ncpu = g_max_cpus > 0 ? (u32)g_max_cpus : 1;

  if (type == BPF_MAP_TYPE_RINGBUF) {
    if (key_size || value_size)
      return 0;
    rb = ringbuf_alloc(max_entries);
    if (!rb)
      return 0;
  } else if (type == BPF_MAP_TYPE_STACK_TRACE) {
    /* A stack of return addresses per entry, keyed by the id get_stackid
     * hands out. */
    if (key_size != 4 || !value_size || (value_size % 8) ||
        value_size > 127 * 8 || !max_entries || max_entries > 65536)
      return 0;
  } else if (type == BPF_MAP_TYPE_PERF_EVENT_ARRAY) {
    /* Indexed by CPU, holding perf events by descriptor. */
    if (key_size != 4 || value_size != 4 || !max_entries || max_entries > 4096)
      return 0;
  } else {
    if (!map_is_array(&(struct bpf_map){.type = type}) &&
        !map_is_hash(&(struct bpf_map){.type = type}))
      return 0;
    if (!value_size || !max_entries || value_size > 4096 || max_entries > 65536)
      return 0;
    if (map_is_array(&(struct bpf_map){.type = type}) && key_size != 4)
      return 0;
    if (map_is_hash(&(struct bpf_map){.type = type}) &&
        (!key_size || key_size > 64))
      return 0;
    if (map_is_percpu(&(struct bpf_map){.type = type}) &&
        (usize)ncpu * ((value_size + 7) & ~7u) > 65536)
      return 0;
  }

  u64 flags;
  struct bpf_map *m = 0;

  spin_lock_irqsave(&g_bpf_lock, &flags);
  for (int i = 0; i < BPF_MAX_MAPS; i++) {
    if (!g_maps[i].used) {
      m = &g_maps[i];
      m->used = 1;
      m->id = g_map_next_id++;
      break;
    }
  }
  spin_unlock_irqrestore(&g_bpf_lock, flags);
  if (!m) {
    ringbuf_free(rb);
    return 0;
  }

  m->type = type;
  m->key_size = key_size;
  m->value_size = value_size;
  m->max_entries = max_entries;
  m->ncpu = ncpu;
  m->stride = (value_size + 7) & ~7u;
  m->refs = 1;
  m->lock = SPINLOCK_INIT;
  m->rb = rb;
  m->values = m->present = m->keys = 0;
  m->evs = 0;
  memset(m->name, 0, sizeof(m->name));
  if (name)
    memcpy(m->name, name, sizeof(m->name) - 1);
  if (rb)
    return m;
  if (type == BPF_MAP_TYPE_PERF_EVENT_ARRAY) {
    m->evs = kzalloc(sizeof(*m->evs) * max_entries);
    if (!m->evs) {
      m->used = 0;
      return 0;
    }
    return m;
  }
  m->values = kzalloc(map_elem_size(m) * max_entries);
  m->present = kzalloc(max_entries);
  m->keys = map_is_hash(m) ? kzalloc((usize)key_size * max_entries) : 0;
  if (!m->values || !m->present || (map_is_hash(m) && !m->keys)) {
    kfree(m->values);
    kfree(m->present);
    kfree(m->keys);
    m->values = m->present = m->keys = 0;
    m->used = 0;
    return 0;
  }
  if (map_is_array(m))
    memset(m->present, 1, max_entries); /* every entry exists, reading zero */
  return m;
}

static void map_put(struct bpf_map *m) {
  if (!m)
    return;
  int left = __atomic_sub_fetch(&m->refs, 1, __ATOMIC_ACQ_REL);
  KASSERT(left >= 0, "bpf map %p put at refcount %d", (void *)m, left + 1);
  if (left > 0)
    return;
  kfree(m->values);
  kfree(m->present);
  kfree(m->keys);
  ringbuf_free(m->rb);
  for (u32 i = 0; m->evs && i < m->max_entries; i++)
    if (m->evs[i])
      vfs_handle_release(m->evs[i]);
  kfree(m->evs);
  m->evs = 0;
  m->rb = 0;
  m->values = m->present = m->keys = 0;
  m->used = 0;
}

/* Caller holds the map lock. */
static int map_find(struct bpf_map *m, const void *key) {
  if (map_is_array(m) || m->type == BPF_MAP_TYPE_STACK_TRACE) {
    u32 idx;

    memcpy(&idx, key, 4);
    if (idx >= m->max_entries)
      return -1;
    if (m->type == BPF_MAP_TYPE_STACK_TRACE && !m->present[idx])
      return -1;
    return (int)idx;
  }
  for (u32 i = 0; i < m->max_entries; i++) {
    if (m->present[i] &&
        memcmp(m->keys + (usize)i * m->key_size, key, m->key_size) == 0)
      return (int)i;
  }
  return -1;
}

/* A program's view: this CPU's value, for a per-CPU map. */
static void *map_lookup(struct bpf_map *m, const void *key) {
  int slot = map_find(m, key);
  u32 cpu = bpf_this_cpu();

  if (slot < 0)
    return 0;
  if (map_is_percpu(m))
    return map_elem(m, slot) + (usize)(cpu < m->ncpu ? cpu : 0) * m->stride;
  return map_elem(m, slot);
}

/* `all_cpus`: the value is bpf(2)'s, one per CPU for a per-CPU map; a
 * program's update writes its own CPU's value only. */
static int map_update(struct bpf_map *m, const void *key, const void *value,
                      u64 flags, int all_cpus) {
  u64 lf;
  int rc = 0;

  if (flags > BPF_EXIST)
    return -EINVAL;
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
    if (!map_is_hash(m)) {
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
    memset(map_elem(m, slot), 0, map_elem_size(m));
    m->present[slot] = 1;
  }
  if (!map_is_percpu(m))
    memcpy(map_elem(m, slot), value, m->value_size);
  else if (all_cpus)
    memcpy(map_elem(m, slot), value, map_elem_size(m));
  else
    memcpy(map_elem(m, slot) +
               (usize)(bpf_this_cpu() < m->ncpu ? bpf_this_cpu() : 0) *
                   m->stride,
           value, m->value_size);
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
    if (map_is_array(m)) {
      rc = -EINVAL; /* an array entry cannot go away: Linux says EINVAL */
    } else {
      m->present[slot] = 0;
      memset(map_elem(m, slot), 0, map_elem_size(m));
      rc = 0;
    }
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
  /* The program's own BTF (prog_btf_fd), held while it is loaded. */
  struct bpf_btf *btf;
  u32 id;
  u32 uid;       /* who loaded it: bpf_prog_info.created_by_uid */
  u64 load_time; /* ns since boot, as bpf_prog_info.load_time */
  /* The compiled program, when the JIT could translate it: called instead of
   * the interpreter, with the same context and the same return value. */
  u64 (*jit_fn)(void *ctx);
};

static struct bpf_prog g_progs[BPF_MAX_PROGS];
/* How many loaded programs the JIT could translate, for diagnostics. */
static u64 g_bpf_jitted;

static void prog_put(struct bpf_prog *p) {
  if (!p)
    return;
  int left = __atomic_sub_fetch(&p->refs, 1, __ATOMIC_ACQ_REL);
  KASSERT(left >= 0, "bpf prog %p put at refcount %d", (void *)p, left + 1);
  if (left > 0)
    return;
  for (int i = 0; i < p->nmaps; i++)
    map_put(p->maps[i]);
  if (p->jit_fn) {
    bpf_jit_free((void *)(usize)p->jit_fn);
    p->jit_fn = 0;
  }
  kfree(p->insns);
  p->insns = 0;
  p->nmaps = 0;
  btf_put(p->btf);
  p->btf = 0;
  p->used = 0;
}

void bpf_prog_put(void *prog) { prog_put((struct bpf_prog *)prog); }

static struct bpf_map *map_from_fd(int fd);

/* ── the verifier ────────────────────────────────────────────────────────── */

enum reg_kind {
  REG_NOT_INIT = 0,
  REG_SCALAR,
  REG_PTR_CTX,
  REG_PTR_STACK,
  REG_PTR_MAP_VALUE,    /* a map value that may still be NULL */
  REG_PTR_MAP_VALUE_OK, /* the same, proved not NULL */
  REG_PTR_MAP,
  REG_PTR_MEM,          /* a ring-buffer record that may still be NULL */
  REG_PTR_MEM_OK,       /* the same, proved not NULL */
};

struct reg_state {
  u8 kind;
  u8 known;     /* SCALAR: `val` is its exact value */
  u16 ref;      /* PTR_MEM[_OK]: the reservation it is (see vstate::refs) */
  u32 mem_size; /* PTR_MEM[_OK]: the bytes the record holds */
  i64 off;      /* pointers: the offset into the object */
  u64 val;
  struct bpf_map *map;
};

/* The stack, one 8-byte slot at a time. A slot the program wrote a whole
 * register into holds that register's state, so a pointer spilled to the
 * stack and read back is still a pointer -- the compiler spills whenever it
 * runs short of registers, and without this every such program was refused.
 * Anything else written there is plain data. */
#define VSLOTS (BPF_STACK_SIZE / 8)
enum { SLOT_INVALID = 0, SLOT_MISC, SLOT_SPILL };

/* Reservations a path holds: ring-buffer records it reserved and has not yet
 * submitted or discarded. Each is named by the instruction that made it -- the
 * program is a DAG, so no instruction runs twice on one path, and the same
 * name means the same reservation on every path. */
#define VREFS 8

struct vstate {
  struct reg_state regs[BPF_REG_CNT];
  u8 slot[VSLOTS];
  struct reg_state spill[VSLOTS];
  u16 refs[VREFS];
  u8 nrefs;
};

/* One path still to walk: where it starts and what it knows. */
struct vpending {
  u32 pc;
  struct vstate st;
};

/* A program is walked along every path it has, but a path that reaches an
 * instruction knowing no more than an earlier path did there is not walked
 * again: whatever was safe for the earlier one is safe for it. That is the
 * pruning; this is the budget on the walk that remains, Linux's complexity
 * limit in smaller form. */
#define VERIFY_BUDGET 262144
#define VERIFY_PENDING 512

struct verifier {
  const struct bpf_insn *insns;
  u32 cnt;
  struct bpf_prog *prog;
  u8 *half;              /* the second half of a 16-byte load */
  struct vstate **cache; /* per instruction: a state proved from there */
  struct vpending *pend;
  u32 npend;
  u64 steps;
  char err[96];
};

static int verr(struct verifier *v, u32 pc, const char *msg) {
  snprintf(v->err, sizeof(v->err), "insn %u: %s", pc, msg);
  return -EINVAL;
}

/* What a helper takes and gives. */
enum arg_type {
  ARG_NONE = 0,
  ARG_ANY,             /* a number, any number (or a pointer given as one) */
  ARG_CONST_MAP,       /* a map, loaded with the 16-byte map load */
  ARG_MAP_KEY,         /* memory holding the map's key */
  ARG_MAP_VALUE,       /* memory holding a map value */
  ARG_MEM,             /* memory read by the helper; the next argument sizes it */
  ARG_UNINIT_MEM,      /* memory written by the helper; the next sizes it */
  ARG_CONST_SIZE,      /* the size of the memory before it: known, > 0 */
  ARG_CONST_SIZE_OR_0, /* the same, and may be 0 */
  ARG_CTX,             /* the context the program was called with */
  ARG_RINGBUF_MEM,     /* a reserved record, released by the call */
};

enum ret_type {
  RET_SCALAR = 0,
  RET_MAP_VALUE_OR_NULL,
  RET_MEM_OR_NULL, /* a reservation: the call acquires it */
};

struct helper_proto {
  u32 id;
  u8 nargs;
  u8 arg[5];
  u8 ret;
  u32 map_type; /* for ARG_CONST_MAP: the type it must be; 0 = hash or array */
};

static const struct helper_proto g_helpers[] = {
    {BPF_FUNC_map_lookup_elem, 2, {ARG_CONST_MAP, ARG_MAP_KEY},
     RET_MAP_VALUE_OR_NULL, 0},
    {BPF_FUNC_map_update_elem, 4,
     {ARG_CONST_MAP, ARG_MAP_KEY, ARG_MAP_VALUE, ARG_ANY}, RET_SCALAR, 0},
    {BPF_FUNC_map_delete_elem, 2, {ARG_CONST_MAP, ARG_MAP_KEY}, RET_SCALAR, 0},
    {BPF_FUNC_probe_read, 3, {ARG_UNINIT_MEM, ARG_CONST_SIZE_OR_0, ARG_ANY},
     RET_SCALAR, 0},
    {BPF_FUNC_ktime_get_ns, 0, {0}, RET_SCALAR, 0},
    {BPF_FUNC_trace_printk, 2, {ARG_MEM, ARG_CONST_SIZE}, RET_SCALAR, 0},
    {BPF_FUNC_get_smp_processor_id, 0, {0}, RET_SCALAR, 0},
    {BPF_FUNC_get_current_pid_tgid, 0, {0}, RET_SCALAR, 0},
    {BPF_FUNC_get_current_uid_gid, 0, {0}, RET_SCALAR, 0},
    {BPF_FUNC_get_current_comm, 2, {ARG_UNINIT_MEM, ARG_CONST_SIZE},
     RET_SCALAR, 0},
    {BPF_FUNC_perf_event_output, 5,
     {ARG_CTX, ARG_CONST_MAP, ARG_ANY, ARG_MEM, ARG_CONST_SIZE_OR_0},
     RET_SCALAR, BPF_MAP_TYPE_PERF_EVENT_ARRAY},
    {BPF_FUNC_get_stackid, 3, {ARG_CTX, ARG_CONST_MAP, ARG_ANY}, RET_SCALAR,
     BPF_MAP_TYPE_STACK_TRACE},
    {BPF_FUNC_get_current_task, 0, {0}, RET_SCALAR, 0},
    {BPF_FUNC_probe_read_str, 3, {ARG_UNINIT_MEM, ARG_CONST_SIZE, ARG_ANY},
     RET_SCALAR, 0},
    {BPF_FUNC_get_stack, 4,
     {ARG_CTX, ARG_UNINIT_MEM, ARG_CONST_SIZE_OR_0, ARG_ANY}, RET_SCALAR, 0},
    {BPF_FUNC_probe_read_user, 3,
     {ARG_UNINIT_MEM, ARG_CONST_SIZE_OR_0, ARG_ANY}, RET_SCALAR, 0},
    {BPF_FUNC_probe_read_kernel, 3,
     {ARG_UNINIT_MEM, ARG_CONST_SIZE_OR_0, ARG_ANY}, RET_SCALAR, 0},
    {BPF_FUNC_probe_read_user_str, 3,
     {ARG_UNINIT_MEM, ARG_CONST_SIZE, ARG_ANY}, RET_SCALAR, 0},
    {BPF_FUNC_probe_read_kernel_str, 3,
     {ARG_UNINIT_MEM, ARG_CONST_SIZE, ARG_ANY}, RET_SCALAR, 0},
    {BPF_FUNC_ktime_get_boot_ns, 0, {0}, RET_SCALAR, 0},
    {BPF_FUNC_ringbuf_output, 4,
     {ARG_CONST_MAP, ARG_MEM, ARG_CONST_SIZE_OR_0, ARG_ANY}, RET_SCALAR,
     BPF_MAP_TYPE_RINGBUF},
    {BPF_FUNC_ringbuf_reserve, 3, {ARG_CONST_MAP, ARG_ANY, ARG_ANY},
     RET_MEM_OR_NULL, BPF_MAP_TYPE_RINGBUF},
    {BPF_FUNC_ringbuf_submit, 2, {ARG_RINGBUF_MEM, ARG_ANY}, RET_SCALAR, 0},
    {BPF_FUNC_ringbuf_discard, 2, {ARG_RINGBUF_MEM, ARG_ANY}, RET_SCALAR, 0},
    {BPF_FUNC_ringbuf_query, 2, {ARG_CONST_MAP, ARG_ANY}, RET_SCALAR,
     BPF_MAP_TYPE_RINGBUF},
};

static const struct helper_proto *helper_of(u32 id) {
  for (usize i = 0; i < sizeof(g_helpers) / sizeof(g_helpers[0]); i++)
    if (g_helpers[i].id == id)
      return &g_helpers[i];
  return 0;
}

static int reg_is_ptr(u8 k) {
  return k != REG_NOT_INIT && k != REG_SCALAR;
}

/* The context a program is given, by type, in Linux's layouts: a kprobe's
 * struct pt_regs, a sampling event's bpf_perf_event_data (the regs, then the
 * sample period and address), and a tracepoint's record -- the common header,
 * then the site's three words, exactly as tracefs's `format` says. */
#if defined(__aarch64__)
#define BPF_REGS_SIZE (34 * 8) /* user_pt_regs: x0-x30, sp, pc, pstate */
#else
#define BPF_REGS_SIZE (21 * 8) /* pt_regs, r15 first and ss last */
#endif
#define BPF_TP_RECORD_SIZE 32
#define BPF_CTX_MAX (BPF_REGS_SIZE + 16)

static i64 bpf_ctx_size(u32 prog_type) {
  switch (prog_type) {
  case BPF_PROG_TYPE_KPROBE:
    return BPF_REGS_SIZE;
  case BPF_PROG_TYPE_PERF_EVENT:
    return BPF_REGS_SIZE + 16;
  default:
    return BPF_TP_RECORD_SIZE;
  }
}

static void reg_scalar(struct reg_state *r, int known, u64 val) {
  memset(r, 0, sizeof(*r));
  r->kind = REG_SCALAR;
  r->known = (u8)known;
  r->val = val;
}

/* Is `o`, a state some path was already proved safe with at an instruction,
 * at least as general as `n`? Then everything `n` can do from there, `o`
 * could, and it was safe. A register `o` never initialised was never read; a
 * number `o` knew nothing about was safe as any number. */
static int reg_covers(const struct reg_state *o, const struct reg_state *n) {
  if (o->kind == REG_NOT_INIT)
    return 1;
  if (o->kind != n->kind)
    return 0;
  switch (o->kind) {
  case REG_SCALAR:
    return !o->known || (n->known && n->val == o->val);
  case REG_PTR_CTX:
  case REG_PTR_STACK:
    return o->off == n->off;
  case REG_PTR_MAP:
    return o->map == n->map;
  case REG_PTR_MAP_VALUE:
  case REG_PTR_MAP_VALUE_OK:
    return o->map == n->map && o->off == n->off;
  case REG_PTR_MEM:
  case REG_PTR_MEM_OK:
    return o->ref == n->ref && o->off == n->off && o->mem_size == n->mem_size;
  default:
    return 0;
  }
}

static int state_covers(const struct vstate *o, const struct vstate *n) {
  for (int r = 0; r < BPF_REG_CNT; r++)
    if (!reg_covers(&o->regs[r], &n->regs[r]))
      return 0;
  for (int s = 0; s < VSLOTS; s++) {
    if (o->slot[s] == SLOT_SPILL) {
      if (n->slot[s] != SLOT_SPILL || !reg_covers(&o->spill[s], &n->spill[s]))
        return 0;
    } else if (n->slot[s] == SLOT_SPILL && reg_is_ptr(n->spill[s].kind)) {
      return 0; /* `o` read plain data here; `n` would read a pointer */
    }
  }
  if (o->nrefs != n->nrefs)
    return 0;
  for (int i = 0; i < o->nrefs; i++)
    if (o->refs[i] != n->refs[i])
      return 0;
  return 1;
}

static int vpush(struct verifier *v, u32 pc, const struct vstate *st) {
  if (v->npend >= VERIFY_PENDING)
    return verr(v, pc, "too many branches waiting to be checked");
  v->pend[v->npend].pc = pc;
  v->pend[v->npend].st = *st;
  v->npend++;
  return 0;
}

static int insn_bytes(u8 code) {
  int sz = BPF_SIZE(code);

  return sz == BPF_B ? 1 : sz == BPF_H ? 2 : sz == BPF_W ? 4 : 8;
}

/* Memory `size` bytes long at `r`, as the program may reach it. */
static int check_mem(struct verifier *v, u32 pc, const struct vstate *st,
                     const struct reg_state *r, i64 at, u64 size) {
  switch (r->kind) {
  case REG_PTR_STACK:
    if (at < -BPF_STACK_SIZE || at + (i64)size > 0)
      return verr(v, pc, "outside the stack frame");
    return 0;
  case REG_PTR_MAP_VALUE_OK:
    if (at < 0 || (u64)at + size > r->map->value_size)
      return verr(v, pc, "outside the map value");
    return 0;
  case REG_PTR_MEM_OK:
    if (at < 0 || (u64)at + size > r->mem_size)
      return verr(v, pc, "outside the ring-buffer record");
    return 0;
  case REG_PTR_CTX:
    if (at < 0 || at + (i64)size > bpf_ctx_size(v->prog->type))
      return verr(v, pc, "outside the context");
    return 0;
  case REG_PTR_MAP_VALUE:
  case REG_PTR_MEM:
    return verr(v, pc, "pointer used before it was tested for NULL");
  default:
    (void)st;
    return verr(v, pc, "memory access through something that is not a pointer");
  }
}

/* A stack write of `bytes` at `at`: a whole aligned register becomes a spill,
 * anything else makes the slots it touches plain data. */
static void stack_write(struct vstate *st, i64 at, int bytes,
                        const struct reg_state *spilled) {
  i64 lo = at + BPF_STACK_SIZE;

  if (spilled && bytes == 8 && (lo % 8) == 0) {
    st->slot[lo / 8] = SLOT_SPILL;
    st->spill[lo / 8] = *spilled;
    return;
  }
  for (i64 b = lo; b < lo + bytes; b++) {
    st->slot[b / 8] = SLOT_MISC;
    memset(&st->spill[b / 8], 0, sizeof(st->spill[0]));
  }
}

/* Constant folding, for the scalars the model knows exactly. Division by zero
 * is defined, as BPF defines it: the quotient is 0, the remainder the dividend. */
static int alu_fold(u8 op, int is64, u64 a, u64 b, u64 *out) {
  if (!is64) {
    a = (u32)a;
    b = (u32)b;
  }
  switch (op) {
  case BPF_ADD: a += b; break;
  case BPF_SUB: a -= b; break;
  case BPF_MUL: a *= b; break;
  case BPF_DIV: a = b ? a / b : 0; break;
  case BPF_MOD: a = b ? a % b : a; break;
  case BPF_OR: a |= b; break;
  case BPF_AND: a &= b; break;
  case BPF_XOR: a ^= b; break;
  case BPF_LSH: a <<= (b & (is64 ? 63 : 31)); break;
  case BPF_RSH: a >>= (b & (is64 ? 63 : 31)); break;
  case BPF_ARSH:
    a = is64 ? (u64)((i64)a >> (b & 63)) : (u64)(u32)((i32)(u32)a >> (b & 31));
    break;
  default:
    return 0;
  }
  *out = is64 ? a : (u32)a;
  return 1;
}

/* Whether a comparison between two known values is taken. */
static int jmp_fold(u8 op, int is64, u64 a, u64 b, int *taken) {
  if (!is64) {
    a = (u32)a;
    b = (u32)b;
  }
  i64 sa = is64 ? (i64)a : (i64)(i32)(u32)a;
  i64 sb = is64 ? (i64)b : (i64)(i32)(u32)b;

  switch (op) {
  case BPF_JEQ: *taken = a == b; return 1;
  case BPF_JNE: *taken = a != b; return 1;
  case BPF_JGT: *taken = a > b; return 1;
  case BPF_JGE: *taken = a >= b; return 1;
  case BPF_JLT: *taken = a < b; return 1;
  case BPF_JLE: *taken = a <= b; return 1;
  case BPF_JSET: *taken = (a & b) != 0; return 1;
  case BPF_JSGT: *taken = sa > sb; return 1;
  case BPF_JSGE: *taken = sa >= sb; return 1;
  case BPF_JSLT: *taken = sa < sb; return 1;
  case BPF_JSLE: *taken = sa <= sb; return 1;
  default: return 0;
  }
}

/* The atomic operations the interpreter performs (BPF_STX | BPF_ATOMIC). */
static int atomic_op_ok(i32 imm) {
  switch (imm) {
  case BPF_ADD:
  case BPF_OR:
  case BPF_AND:
  case BPF_XOR:
  case BPF_ADD | BPF_FETCH:
  case BPF_OR | BPF_FETCH:
  case BPF_AND | BPF_FETCH:
  case BPF_XOR | BPF_FETCH:
  case BPF_XCHG:
  case BPF_CMPXCHG:
    return 1;
  default:
    return 0;
  }
}

static int verify_call(struct verifier *v, u32 pc, struct vstate *st,
                       const struct bpf_insn *i) {
  const struct helper_proto *proto;
  struct bpf_map *arg_map = 0;
  u32 res_size = 0;
  int release = -1;

  if (INSN_SRC(i) != 0 || i->off != 0)
    return verr(v, pc, "only helper calls are supported, not subprograms");
  proto = helper_of((u32)i->imm);
  if (!proto)
    return verr(v, pc, "call to a helper this kernel does not have");

  for (int a = 0; a < proto->nargs; a++) {
    struct reg_state *r = &st->regs[1 + a];
    u8 t = proto->arg[a];

    if (r->kind == REG_NOT_INIT)
      return verr(v, pc, "helper argument was never written");
    switch (t) {
    case ARG_ANY:
      if (r->kind != REG_SCALAR && r->kind != REG_PTR_STACK &&
          r->kind != REG_PTR_MAP_VALUE_OK && r->kind != REG_PTR_CTX)
        return verr(v, pc, "helper wanted a number here");
      break;
    case ARG_CONST_MAP:
      if (r->kind != REG_PTR_MAP)
        return verr(v, pc, "helper wanted a map here");
      if (proto->map_type ? r->map->type != proto->map_type
                          : (!map_is_hash(r->map) && !map_is_array(r->map)))
        return verr(v, pc, "helper does not take a map of this type");
      arg_map = r->map;
      break;
    case ARG_MAP_KEY:
    case ARG_MAP_VALUE: {
      u64 need = t == ARG_MAP_KEY ? arg_map->key_size : arg_map->value_size;
      int rc = check_mem(v, pc, st, r, r->off, need);

      if (rc < 0)
        return rc;
      break;
    }
    case ARG_MEM:
    case ARG_UNINIT_MEM: {
      struct reg_state *sz = &st->regs[2 + a];
      u8 szt = proto->arg[a + 1];

      if (sz->kind != REG_SCALAR || !sz->known)
        return verr(v, pc, "the size of a helper's memory must be a constant");
      if (sz->val > BPF_STACK_SIZE * 8ull ||
          (sz->val == 0 && szt != ARG_CONST_SIZE_OR_0))
        return verr(v, pc, "helper memory size out of range");
      if (sz->val) {
        int rc = check_mem(v, pc, st, r, r->off, sz->val);

        if (rc < 0)
          return rc;
      }
      if (t == ARG_UNINIT_MEM && r->kind == REG_PTR_STACK && sz->val)
        stack_write(st, r->off, (int)sz->val, 0); /* the helper fills it */
      break;
    }
    case ARG_CONST_SIZE:
    case ARG_CONST_SIZE_OR_0:
      break; /* checked with the memory it sizes */
    case ARG_CTX:
      if (r->kind != REG_PTR_CTX || r->off != 0)
        return verr(v, pc, "helper wanted the context here");
      break;
    case ARG_RINGBUF_MEM: {
      int k;

      if (r->kind != REG_PTR_MEM_OK || r->off != 0)
        return verr(v, pc, "helper wanted a reserved record, unmodified");
      for (k = 0; k < st->nrefs; k++)
        if (st->refs[k] == r->ref)
          break;
      if (k == st->nrefs)
        return verr(v, pc, "the record was already submitted or discarded");
      release = k;
      break;
    }
    default:
      return verr(v, pc, "helper argument of an unknown kind");
    }
  }

  if (proto->ret == RET_MEM_OR_NULL) {
    struct reg_state *sz = &st->regs[2];

    if (sz->kind != REG_SCALAR || !sz->known || !sz->val ||
        sz->val > arg_map->max_entries / 4)
      return verr(v, pc, "a reservation's size must be a small constant");
    if (st->nrefs >= VREFS)
      return verr(v, pc, "too many records reserved at once");
    res_size = (u32)sz->val;
  }

  if (release >= 0) {
    u16 gone = st->refs[release];

    st->refs[release] = st->refs[--st->nrefs];
    /* Every copy of the released pointer is dead now. */
    for (int r = 0; r < BPF_REG_CNT; r++)
      if ((st->regs[r].kind == REG_PTR_MEM_OK ||
           st->regs[r].kind == REG_PTR_MEM) && st->regs[r].ref == gone)
        reg_scalar(&st->regs[r], 0, 0);
    for (int s = 0; s < VSLOTS; s++)
      if (st->slot[s] == SLOT_SPILL &&
          (st->spill[s].kind == REG_PTR_MEM_OK ||
           st->spill[s].kind == REG_PTR_MEM) && st->spill[s].ref == gone)
        st->slot[s] = SLOT_MISC;
  }

  for (int a = 1; a <= 5; a++)
    memset(&st->regs[a], 0, sizeof(st->regs[a])); /* clobbered: NOT_INIT */
  memset(&st->regs[0], 0, sizeof(st->regs[0]));
  switch (proto->ret) {
  case RET_MAP_VALUE_OR_NULL:
    st->regs[0].kind = REG_PTR_MAP_VALUE;
    st->regs[0].map = arg_map;
    break;
  case RET_MEM_OR_NULL:
    st->regs[0].kind = REG_PTR_MEM;
    st->regs[0].ref = (u16)(pc + 1);
    st->regs[0].mem_size = res_size;
    st->refs[st->nrefs++] = (u16)(pc + 1);
    break;
  default:
    st->regs[0].kind = REG_SCALAR;
    break;
  }
  return 0;
}

/* One instruction. Returns 0 to go on at *pc, 1 when the path has ended. */
static int verify_insn(struct verifier *v, u32 *pcp, struct vstate *st) {
  u32 pc = *pcp;
  const struct bpf_insn *i = &v->insns[pc];
  u8 cls = BPF_CLASS(i->code);
  int dst = INSN_DST(i), src = INSN_SRC(i);

  if (dst >= BPF_REG_CNT || src >= BPF_REG_CNT)
    return verr(v, pc, "register number out of range");
  if (dst == 10 && cls != BPF_ST && cls != BPF_STX && cls != BPF_JMP &&
      cls != BPF_JMP32)
    return verr(v, pc, "the frame pointer is read-only");

  switch (cls) {
  case BPF_ALU:
  case BPF_ALU64: {
    u8 op = BPF_OP(i->code);
    int is64 = cls == BPF_ALU64;
    struct reg_state *d = &st->regs[dst];
    struct reg_state sv;

    if (i->off != 0)
      return verr(v, pc, "signed division and sign-extending moves are not supported");
    if (op == BPF_END) {
      if (is64 || (i->imm != 16 && i->imm != 32 && i->imm != 64))
        return verr(v, pc, "unsupported byte swap");
      if (d->kind != REG_SCALAR)
        return verr(v, pc, "byte swap of something that is not a number");
      reg_scalar(d, 0, 0);
      break;
    }
    if (op == BPF_NEG) {
      if (BPF_SRC(i->code) != BPF_K || d->kind != REG_SCALAR)
        return verr(v, pc, "negation of something that is not a number");
      if (d->known)
        d->val = is64 ? (u64)(-(i64)d->val) : (u32)(-(i32)(u32)d->val);
      break;
    }
    if (op > BPF_END)
      return verr(v, pc, "unknown arithmetic operation");
    if (BPF_SRC(i->code) == BPF_X) {
      if (st->regs[src].kind == REG_NOT_INIT)
        return verr(v, pc, "reading a register that was never written");
      sv = st->regs[src];
    } else {
      if (i->dst_src >> 4)
        return verr(v, pc, "reserved source register field is not zero");
      reg_scalar(&sv, 1, is64 ? (u64)(i64)i->imm : (u64)(u32)i->imm);
    }
    if (op == BPF_MOV) {
      if (!is64 && reg_is_ptr(sv.kind))
        reg_scalar(d, 0, 0); /* a 32-bit move keeps no pointer */
      else if (!is64 && sv.known)
        reg_scalar(d, 1, (u32)sv.val);
      else if (!is64)
        reg_scalar(d, 0, 0);
      else
        *d = sv;
      break;
    }
    if (d->kind == REG_NOT_INIT)
      return verr(v, pc, "arithmetic on a register that was never written");
    if (reg_is_ptr(d->kind)) {
      if ((d->kind != REG_PTR_STACK && d->kind != REG_PTR_MAP_VALUE_OK &&
           d->kind != REG_PTR_MEM_OK) ||
          !is64 || (op != BPF_ADD && op != BPF_SUB) || sv.kind != REG_SCALAR ||
          !sv.known)
        return verr(v, pc, "pointer arithmetic this cannot follow");
      i64 delta = (i64)sv.val;

      if (delta > (1 << 20) || delta < -(1 << 20))
        return verr(v, pc, "pointer moved too far");
      d->off += op == BPF_ADD ? delta : -delta;
      break;
    }
    if (reg_is_ptr(sv.kind))
      return verr(v, pc, "arithmetic with a pointer as the operand");
    if ((op == BPF_DIV || op == BPF_MOD) && BPF_SRC(i->code) == BPF_K &&
        i->imm == 0)
      return verr(v, pc, "division by a literal zero");
    {
      u64 res;

      if (d->known && sv.known && alu_fold(op, is64, d->val, sv.val, &res))
        reg_scalar(d, 1, res);
      else
        reg_scalar(d, 0, 0);
    }
    break;
  }

  case BPF_LDX: {
    int bytes = insn_bytes(i->code);
    struct reg_state *p = &st->regs[src];
    i64 at = p->off + i->off;
    int rc;

    if ((i->code & 0xe0) != BPF_MEM)
      return verr(v, pc, "only plain loads are supported");
    rc = check_mem(v, pc, st, p, at, (u64)bytes);
    if (rc < 0)
      return rc;
    if (p->kind == REG_PTR_STACK && bytes == 8 &&
        ((at + BPF_STACK_SIZE) % 8) == 0 &&
        st->slot[(at + BPF_STACK_SIZE) / 8] == SLOT_SPILL) {
      st->regs[dst] = st->spill[(at + BPF_STACK_SIZE) / 8];
      break;
    }
    reg_scalar(&st->regs[dst], 0, 0);
    break;
  }

  case BPF_ST:
  case BPF_STX: {
    int bytes = insn_bytes(i->code);
    struct reg_state *p = &st->regs[dst];
    i64 at = p->off + i->off;
    u8 mode = i->code & 0xe0;
    int rc;

    if (cls == BPF_STX && st->regs[src].kind == REG_NOT_INIT)
      return verr(v, pc, "storing a register that was never written");
    if (mode == 0xc0 /* BPF_ATOMIC */) {
      if (cls != BPF_STX || (bytes != 4 && bytes != 8) || !atomic_op_ok(i->imm))
        return verr(v, pc, "unsupported atomic operation");
      if (st->regs[src].kind != REG_SCALAR)
        return verr(v, pc, "an atomic operand must be a number");
      if (p->kind != REG_PTR_STACK && p->kind != REG_PTR_MAP_VALUE_OK)
        return verr(v, pc, "atomic operations are for the stack and map values");
      if (i->imm == BPF_CMPXCHG && st->regs[0].kind != REG_SCALAR)
        return verr(v, pc, "compare-and-exchange compares r0, which is not a number");
    } else if (mode != BPF_MEM) {
      return verr(v, pc, "unsupported store mode");
    }
    if (cls == BPF_ST && (i->dst_src >> 4))
      return verr(v, pc, "reserved source register field is not zero");
    if (p->kind == REG_PTR_CTX)
      return verr(v, pc, "the context is read-only");
    rc = check_mem(v, pc, st, p, at, (u64)bytes);
    if (rc < 0)
      return rc;
    if (p->kind == REG_PTR_STACK) {
      if (mode == BPF_MEM && cls == BPF_STX)
        stack_write(st, at, bytes, &st->regs[src]);
      else if (mode == BPF_MEM && bytes == 8) {
        struct reg_state k;

        reg_scalar(&k, 1, (u64)(i64)i->imm);
        stack_write(st, at, bytes, &k);
      } else {
        stack_write(st, at, bytes, 0);
      }
    }
    if (mode == 0xc0 && (i->imm & BPF_FETCH)) {
      if (i->imm == BPF_CMPXCHG)
        reg_scalar(&st->regs[0], 0, 0);
      else
        reg_scalar(&st->regs[src], 0, 0);
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
    if (v->insns[pc + 1].code || v->insns[pc + 1].dst_src || v->insns[pc + 1].off)
      return verr(v, pc, "malformed second half of a 16-byte load");
    if (src == 1) { /* BPF_PSEUDO_MAP_FD, rewritten below on the first pass */
      struct bpf_map *m = 0;
      int known = -1;

      if (i->off >= 0 && i->off < v->prog->nmaps && i->imm == (i32)0x7fffffff)
        known = i->off; /* already resolved by an earlier path */
      if (known < 0) {
        m = map_from_fd(i->imm);
        if (!m || !m->used)
          return verr(v, pc, "the map descriptor is not a map");
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
         * working after the loader closes the map's fd. The immediate is
         * marked so a second path through here knows it is resolved. */
        ((struct bpf_insn *)i)->off = (i16)known;
        ((struct bpf_insn *)i)->imm = (i32)0x7fffffff;
      }
      memset(&st->regs[dst], 0, sizeof(st->regs[dst]));
      st->regs[dst].kind = REG_PTR_MAP;
      st->regs[dst].map = v->prog->maps[known];
    } else if (src == 0) {
      reg_scalar(&st->regs[dst], 1,
                 (u64)(u32)i->imm | ((u64)(u32)v->insns[pc + 1].imm << 32));
    } else {
      return verr(v, pc, "unsupported pseudo load");
    }
    *pcp = pc + 2;
    return 0;
  }

  case BPF_JMP:
  case BPF_JMP32: {
    u8 op = BPF_OP(i->code);
    int is64 = cls == BPF_JMP;

    if (op == BPF_EXIT) {
      if (cls != BPF_JMP)
        return verr(v, pc, "unknown jump");
      if (st->regs[0].kind != REG_SCALAR)
        return verr(v, pc, "exit without a number in r0");
      if (st->nrefs)
        return verr(v, pc, "exit with a reserved record not submitted or discarded");
      return 1;
    }
    if (op == BPF_CALL) {
      if (cls != BPF_JMP)
        return verr(v, pc, "unknown jump");
      int rc = verify_call(v, pc, st, i);

      if (rc < 0)
        return rc;
      break;
    }
    if (op == BPF_JA) {
      if (cls != BPF_JMP || i->imm || (i->dst_src))
        return verr(v, pc, "unsupported jump");
      if (i->off < 0)
        return verr(v, pc, "backward jump: this verifier proves no loops");
      *pcp = pc + 1 + (u32)i->off;
      return 0;
    }
    if (op > BPF_JSLE)
      return verr(v, pc, "unknown jump");

    i64 target = (i64)pc + 1 + i->off;
    struct reg_state *d = &st->regs[dst];
    struct reg_state sv;

    if (target < 0 || target >= (i64)v->cnt)
      return verr(v, pc, "jump outside the program");
    if (i->off < 0)
      return verr(v, pc, "backward jump: this verifier proves no loops");
    if (BPF_SRC(i->code) == BPF_X) {
      if (st->regs[src].kind == REG_NOT_INIT)
        return verr(v, pc, "comparing a register that was never written");
      sv = st->regs[src];
    } else {
      reg_scalar(&sv, 1, (u64)(i64)i->imm);
    }
    if (d->kind == REG_NOT_INIT)
      return verr(v, pc, "comparing a register that was never written");

    /* Testing a nullable pointer against zero is what proves it; which arm
     * learns that depends on the comparison. */
    if ((d->kind == REG_PTR_MAP_VALUE || d->kind == REG_PTR_MEM) && is64 &&
        sv.kind == REG_SCALAR && sv.known && sv.val == 0 &&
        (op == BPF_JEQ || op == BPF_JNE)) {
      struct vstate other = *st;
      u8 ok = d->kind == REG_PTR_MAP_VALUE ? REG_PTR_MAP_VALUE_OK
                                           : REG_PTR_MEM_OK;
      u16 ref = d->ref;
      u8 was = d->kind;
      struct bpf_map *wmap = d->map;
      /* The NULL arm: the pointer is 0, and a reservation that failed is not
       * one the program holds. */
      struct vstate *null_arm = op == BPF_JEQ ? &other : st;
      struct vstate *ok_arm = op == BPF_JEQ ? st : &other;

      for (int r = 0; r < BPF_REG_CNT; r++) {
        struct reg_state *a = &ok_arm->regs[r];
        struct reg_state *b = &null_arm->regs[r];

        if (a->kind == was && (was != REG_PTR_MEM || a->ref == ref) &&
            (was != REG_PTR_MAP_VALUE || (a->map == wmap && r == dst)))
          a->kind = ok;
        if (b->kind == was && (was != REG_PTR_MEM || b->ref == ref) &&
            (was != REG_PTR_MAP_VALUE || r == dst))
          reg_scalar(b, 1, 0);
      }
      if (was == REG_PTR_MEM)
        for (int k = 0; k < null_arm->nrefs; k++)
          if (null_arm->refs[k] == ref) {
            null_arm->refs[k] = null_arm->refs[--null_arm->nrefs];
            break;
          }
      int rc = vpush(v, (u32)target, &other);

      if (rc < 0)
        return rc;
      break;
    }
    if (reg_is_ptr(d->kind) || reg_is_ptr(sv.kind)) {
      /* Comparing pointers says nothing the model uses; both arms go on. */
      int rc = vpush(v, (u32)target, st);

      if (rc < 0)
        return rc;
      break;
    }
    if (d->known && sv.known) {
      int taken;

      if (jmp_fold(op, is64, d->val, sv.val, &taken)) {
        if (taken)
          *pcp = (u32)target;
        else
          *pcp = pc + 1;
        return 0;
      }
    }
    {
      int rc = vpush(v, (u32)target, st);

      if (rc < 0)
        return rc;
    }
    break;
  }

  default:
    return verr(v, pc, "unknown instruction class");
  }
  *pcp = pc + 1;
  return 0;
}

static int verify_path(struct verifier *v, u32 pc, struct vstate *st) {
  for (;;) {
    if (++v->steps > VERIFY_BUDGET)
      return verr(v, pc, "program too complex to verify");
    if (pc >= v->cnt)
      return verr(v, v->cnt, "the program runs off its end without exiting");
    if (v->half[pc])
      return verr(v, pc, "jump into the middle of a 16-byte load");
    if (v->cache[pc] && state_covers(v->cache[pc], st))
      return 0; /* proved already, from a state at least this general */
    if (!v->cache[pc]) {
      v->cache[pc] = kmalloc(sizeof(struct vstate));
      if (!v->cache[pc])
        return -ENOMEM;
    }
    *v->cache[pc] = *st;

    int rc = verify_insn(v, &pc, st);

    if (rc < 0)
      return rc;
    if (rc == 1)
      return 0;
  }
}

static int bpf_verify(struct bpf_prog *p, u64 log_buf, u32 log_size) {
  struct verifier v;
  struct vstate *start;
  int rc = 0;

  memset(&v, 0, sizeof(v));
  v.insns = p->insns;
  v.cnt = p->insn_cnt;
  v.prog = p;
  v.half = kzalloc(p->insn_cnt);
  v.cache = kzalloc((usize)p->insn_cnt * sizeof(*v.cache));
  v.pend = kmalloc(sizeof(struct vpending) * VERIFY_PENDING);
  start = kzalloc(sizeof(*start));
  if (!v.half || !v.cache || !v.pend || !start) {
    rc = -ENOMEM;
    goto out;
  }
  for (u32 pc = 0; pc + 1 < p->insn_cnt; pc++)
    if (p->insns[pc].code == (BPF_LD | BPF_DW | BPF_IMM))
      v.half[++pc] = 1;

  start->regs[1].kind = REG_PTR_CTX; /* the context, on entry */
  start->regs[10].kind = REG_PTR_STACK;
  start->regs[10].off = 0; /* the frame pointer: the TOP of the frame */

  rc = verify_path(&v, 0, start);
  while (rc == 0 && v.npend) {
    struct vpending *pp = &v.pend[--v.npend];
    u32 pc = pp->pc;

    *start = pp->st;
    rc = verify_path(&v, pc, start);
  }

out:
  if (v.cache)
    for (u32 pc = 0; pc < p->insn_cnt; pc++)
      kfree(v.cache[pc]);
  kfree(v.cache);
  kfree(v.half);
  kfree(v.pend);
  kfree(start);
  if (rc == -ENOMEM)
    snprintf(v.err, sizeof(v.err), "out of memory while verifying");
  if (rc < 0 && log_buf && log_size) {
    usize n = strlen(v.err) + 1;

    (void)syscall_copyout((void *)(usize)log_buf, v.err,
                          n < log_size ? n : log_size);
  }
  return rc;
}

/* ── the interpreter ─────────────────────────────────────────────────────── */

struct bpf_run_ctx {
  u64 ctx[BPF_CTX_MAX / 8]; /* what the program sees, first */
  /* Past the context, so the program cannot read them; get_stackid and
   * get_stack start their walk here. */
  u64 ip;
  u64 fp;
  u64 in_user;
};

/* pt_regs from an interrupt frame, in the arch's Linux layout. */
static void bpf_fill_regs(u64 *r, const void *frame_v, int in_user) {
#if defined(__aarch64__)
  const struct interrupt_frame *f = frame_v;

  memcpy(r, frame_v, 31 * 8); /* x0-x30 open the frame, in order */
  r[31] = in_user ? f->sp_el0 : (u64)(usize)(f + 1);
  r[32] = f->elr;
  r[33] = f->spsr;
#else
  const struct interrupt_frame *f = frame_v;

  (void)in_user;
  r[0] = f->r15;
  r[1] = f->r14;
  r[2] = f->r13;
  r[3] = f->r12;
  r[4] = f->rbp;
  r[5] = f->rbx;
  r[6] = f->r11;
  r[7] = f->r10;
  r[8] = f->r9;
  r[9] = f->r8;
  r[10] = f->rax;
  r[11] = f->rcx;
  r[12] = f->rdx;
  r[13] = f->rsi;
  r[14] = f->rdi;
  r[15] = (u64)-1; /* orig_ax: not a system call */
  r[16] = f->rip;
  r[17] = f->cs;
  r[18] = f->rflags;
  r[19] = f->rsp;
  r[20] = f->ss;
#endif
}

/* Where get_stackid starts: the interrupted instruction and frame. */
static void bpf_frame_origin(const void *frame_v, u64 *ip, u64 *fp) {
  const struct interrupt_frame *f = frame_v;
#if defined(__aarch64__)
  *ip = f->elr;
  *fp = f->x29;
#else
  *ip = f->rip;
  *fp = f->rbp;
#endif
}

/* ── reading memory a program points at ─────────────────────────────────────
 *
 * probe_read_* is given an address and nothing else, and may run where no page
 * fault can be taken (a sample fires in the timer interrupt). So an address is
 * translated through the page tables and read through the direct map, a page at
 * a time: a page that is not there, or is not RAM, fails the read, as Linux's
 * nofault copies fail it, and the destination is zeroed. */
#define BPF_USER_TOP 0x0000800000000000ull

static int bpf_read_nofault(void *dst, u64 src, u64 len, int user) {
  extern int paging_is_kernel_va(u64 va);
  u8 *d = dst;

  /* The kernel test is the page's, not the number's: on aarch64 the kernel's
   * windows share the numeric range user space uses. */
  if (user ? (src >= BPF_USER_TOP || src + len > BPF_USER_TOP ||
              paging_is_kernel_va(src))
           : !paging_is_kernel_va(src))
    return -EFAULT;
  if (src + len < src)
    return -EFAULT;
  while (len) {
    u64 chunk = PAGE_SIZE - (src & (PAGE_SIZE - 1));
    u64 phys = vmm_virt_to_phys((void *)(usize)src);

    if (chunk > len)
      chunk = len;
    if (!phys || !pmm_frame_readable(phys & ~(u64)(PAGE_SIZE - 1)))
      return -EFAULT;
    memcpy(d, (const void *)(usize)(phys + vmm_direct_map_base()), chunk);
    d += chunk;
    src += chunk;
    len -= chunk;
  }
  return 0;
}

static i64 bpf_probe_read(void *dst, u64 size, u64 src, int user) {
  int rc = bpf_read_nofault(dst, src, size, user);

  if (rc < 0)
    memset(dst, 0, size);
  return rc;
}

/* The string forms: copy up to the NUL, which is copied too, and return the
 * length including it; a fault returns -EFAULT with the buffer zeroed. */
static i64 bpf_probe_read_str(char *dst, u64 size, u64 src, int user) {
  u64 n = 0;

  if (!size)
    return -EINVAL;
  while (n + 1 < size) {
    char c;

    if (bpf_read_nofault(&c, src + n, 1, user) < 0) {
      memset(dst, 0, size);
      return -EFAULT;
    }
    dst[n++] = c;
    if (!c)
      return (i64)n;
  }
  dst[n] = 0;
  return (i64)n + 1;
}

/* ── stacks ──────────────────────────────────────────────────────────────── */

extern char __kernel_text_start[], __kernel_text_end[];

static int bpf_kernel_text(u64 a) {
  return a >= (u64)(usize)__kernel_text_start &&
         a < (u64)(usize)__kernel_text_end;
}

/* Return addresses, innermost first, by the frame-pointer chain both arches
 * keep: [fp] is the caller's fp, [fp + 8] the return address. The kernel stack
 * of a sample taken in the kernel; the user stack of one taken in userspace.
 * A sample in the kernel has no user frames to give -- the registers the task
 * entered the kernel with are not at hand -- and one in userspace has no
 * kernel frames, as on Linux. */
static u32 bpf_collect_stack(const struct bpf_run_ctx *c, int user, u32 skip,
                             u64 *ips, u32 max) {
  u32 n = 0, seen = 0;
  u64 fp;

  if (!c || (int)(c->in_user != 0) != user || !c->ip)
    return 0;
  if (seen++ >= skip && n < max)
    ips[n++] = c->ip;
  fp = c->fp;
  for (u32 depth = 0; depth < 128 && n < max && fp && !(fp & 7); depth++) {
    u64 frame[2];

    if (bpf_read_nofault(frame, fp, sizeof(frame), user) < 0)
      break;
    if (!frame[1] || (!user && !bpf_kernel_text(frame[1])))
      break;
    if (seen++ >= skip)
      ips[n++] = frame[1];
    if (frame[0] <= fp)
      break; /* frames go up the stack; anything else is not a chain */
    fp = frame[0];
  }
  return n;
}

static i64 bpf_get_stackid(struct bpf_run_ctx *c, struct bpf_map *m,
                           u64 flags) {
  u64 ips[127];
  u32 max, n, id;
  u64 hash = 1469598103934665603ull;
  u64 lf;
  i64 rc;

  if (!m || m->type != BPF_MAP_TYPE_STACK_TRACE ||
      (flags & ~(BPF_F_SKIP_FIELD_MASK | BPF_F_USER_STACK |
                 BPF_F_FAST_STACK_CMP | BPF_F_REUSE_STACKID)))
    return -EINVAL;
  max = m->value_size / 8;
  n = bpf_collect_stack(c, (flags & BPF_F_USER_STACK) != 0,
                        (u32)(flags & BPF_F_SKIP_FIELD_MASK), ips, max);
  if (!n)
    return -EFAULT;
  for (u32 i = n; i < max; i++)
    ips[i] = 0;
  for (u32 i = 0; i < n; i++)
    for (int b = 0; b < 8; b++)
      hash = (hash ^ ((ips[i] >> (b * 8)) & 0xff)) * 1099511628211ull;
  id = (u32)(hash % m->max_entries);

  spin_lock_irqsave(&m->lock, &lf);
  u8 *slot = m->values + (usize)id * m->value_size;

  if (!m->present[id] || (flags & BPF_F_REUSE_STACKID)) {
    memcpy(slot, ips, m->value_size);
    m->present[id] = 1;
    rc = id;
  } else {
    /* The bucket holds a stack already: this one, or a collision. */
    rc = memcmp(slot, ips, m->value_size) == 0 ? (i64)id : -EEXIST;
  }
  spin_unlock_irqrestore(&m->lock, lf);
  return rc;
}

static i64 bpf_get_stack(struct bpf_run_ctx *c, u64 *buf, u64 size,
                         u64 flags) {
  u32 n;

  if ((flags & ~(BPF_F_SKIP_FIELD_MASK | BPF_F_USER_STACK)) || (size % 8)) {
    memset(buf, 0, size);
    return -EINVAL;
  }
  n = bpf_collect_stack(c, (flags & BPF_F_USER_STACK) != 0,
                        (u32)(flags & BPF_F_SKIP_FIELD_MASK), buf,
                        (u32)(size / 8));
  memset(buf + n, 0, size - (u64)n * 8);
  return n ? (i64)n * 8 : -EFAULT;
}

/* ── ring buffers ────────────────────────────────────────────────────────── */

/* A program's ring-buffer records ask for a wake of the readers; the perf path
 * that ran it does the waking once it is out of its own lock. */
static int g_bpf_wake;

int bpf_take_wakeup(void) {
  return __atomic_exchange_n(&g_bpf_wake, 0, __ATOMIC_ACQ_REL);
}

static u64 *rb_word(u64 phys) {
  return (u64 *)(usize)(phys + vmm_direct_map_base());
}

static u64 rb_cons(struct bpf_ringbuf *rb) {
  return __atomic_load_n(rb_word(rb->cons_phys), __ATOMIC_ACQUIRE);
}

/* Reserve `size` bytes of record. Returns the data's address, or 0 when the
 * consumer has not left room. */
static void *ringbuf_reserve(struct bpf_ringbuf *rb, u64 size) {
  u64 len = (size + BPF_RINGBUF_HDR_SZ + 7) & ~7ull;
  u64 lf, cons, prod, off;
  u8 *data = (u8 *)(usize)(rb->data_phys + vmm_direct_map_base());
  void *rec = 0;

  if (!size || size > (u64)(BPF_RINGBUF_BUSY_BIT - 1) || len > rb->size)
    return 0;
  spin_lock_irqsave(&rb->lock, &lf);
  cons = rb_cons(rb);
  prod = rb->prod;
  off = prod & (rb->size - 1);
  if (off + len > rb->size) {
    /* It would cross the end: the rest becomes a record the reader skips. */
    u64 pad = rb->size - off;

    if (prod + pad + len - cons > rb->size)
      goto out;
    __atomic_store_n((u32 *)(data + off),
                     (u32)(pad - BPF_RINGBUF_HDR_SZ) | BPF_RINGBUF_DISCARD_BIT,
                     __ATOMIC_RELEASE);
    prod += pad;
    off = 0;
  } else if (prod + len - cons > rb->size) {
    goto out;
  }
  ((u32 *)(data + off))[1] = (u32)(off / PAGE_SIZE);
  __atomic_store_n((u32 *)(data + off), (u32)size | BPF_RINGBUF_BUSY_BIT,
                   __ATOMIC_RELEASE);
  rb->prod = prod + len;
  __atomic_store_n(rb_word(rb->prod_phys), rb->prod, __ATOMIC_RELEASE);
  rec = data + off + BPF_RINGBUF_HDR_SZ;
out:
  spin_unlock_irqrestore(&rb->lock, lf);
  return rec;
}

static void ringbuf_commit(void *rec, int discard, u64 flags) {
  u32 *hdr = (u32 *)((u8 *)rec - BPF_RINGBUF_HDR_SZ);
  u32 len = __atomic_load_n(hdr, __ATOMIC_ACQUIRE) & ~BPF_RINGBUF_BUSY_BIT;

  if (discard)
    len |= BPF_RINGBUF_DISCARD_BIT;
  __atomic_store_n(hdr, len, __ATOMIC_RELEASE);
  if (!(flags & BPF_RB_NO_WAKEUP))
    __atomic_store_n(&g_bpf_wake, 1, __ATOMIC_RELEASE);
}

/* What bpf_ringbuf_query asks: the ring's state, as the kernel sees it. */
static u64 ringbuf_query(struct bpf_ringbuf *rb, u64 what) {
  u64 cons = rb_cons(rb);

  switch (what) {
  case BPF_RB_AVAIL_DATA:
    return rb->prod >= cons ? rb->prod - cons : 0;
  case BPF_RB_RING_SIZE:
    return rb->size;
  case BPF_RB_CONS_POS:
    return cons;
  case BPF_RB_PROD_POS:
    return rb->prod;
  default:
    return 0;
  }
}

static u64 bpf_helper(u32 id, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
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
                                (const void *)(usize)a3, a4, 0);
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
  case BPF_FUNC_probe_read:
    /* The old form reads from wherever the address points. */
    return (u64)bpf_probe_read((void *)(usize)a1, a2, a3,
                               a3 < BPF_USER_TOP);
  case BPF_FUNC_probe_read_user:
    return (u64)bpf_probe_read((void *)(usize)a1, a2, a3, 1);
  case BPF_FUNC_probe_read_kernel:
    return (u64)bpf_probe_read((void *)(usize)a1, a2, a3, 0);
  case BPF_FUNC_probe_read_str:
    return (u64)bpf_probe_read_str((char *)(usize)a1, a2, a3,
                                   a3 < BPF_USER_TOP);
  case BPF_FUNC_probe_read_user_str:
    return (u64)bpf_probe_read_str((char *)(usize)a1, a2, a3, 1);
  case BPF_FUNC_probe_read_kernel_str:
    return (u64)bpf_probe_read_str((char *)(usize)a1, a2, a3, 0);
  case BPF_FUNC_get_current_task:
    return (u64)(usize)current_task;
  case BPF_FUNC_perf_event_output: {
    /* bpf_perf_event_output(ctx, map, flags, data, size): into the event the
     * map holds at the index flags name -- this CPU's, usually. */
    struct bpf_map *m = (struct bpf_map *)(usize)a2;
    u64 idx = a3 & BPF_F_INDEX_MASK;
    u32 cpu = bpf_this_cpu();
    int rc;

    if (!m || !m->evs || (a3 & ~BPF_F_INDEX_MASK))
      return (u64)(i64)-EINVAL;
    if (idx == BPF_F_CURRENT_CPU)
      idx = cpu;
    if (idx >= m->max_entries)
      return (u64)(i64)-E2BIG;
    {
      /* Held across the write: bpf(2) may replace the element meanwhile. */
      u64 lf;
      struct vfs_handle *h;

      spin_lock_irqsave(&m->lock, &lf);
      h = m->evs[idx];
      if (h)
        vfs_handle_retain(h);
      spin_unlock_irqrestore(&m->lock, lf);
      rc = perf_event_bpf_output(h, (const void *)(usize)a4, (u32)a5,
                                 (int)cpu);
      if (h)
        vfs_handle_release(h);
    }
    if (rc == 0)
      __atomic_store_n(&g_bpf_wake, 1, __ATOMIC_RELEASE);
    return (u64)(i64)rc;
  }
  case BPF_FUNC_get_stackid:
    return (u64)bpf_get_stackid((struct bpf_run_ctx *)(usize)a1,
                                (struct bpf_map *)(usize)a2, a3);
  case BPF_FUNC_get_stack:
    return (u64)bpf_get_stack((struct bpf_run_ctx *)(usize)a1,
                              (u64 *)(usize)a2, a3, a4);
  case BPF_FUNC_ringbuf_output: {
    struct bpf_map *m = (struct bpf_map *)(usize)a1;
    void *rec;

    if (!m || !m->rb || (a4 & ~(BPF_RB_NO_WAKEUP | BPF_RB_FORCE_WAKEUP)))
      return (u64)(i64)-EINVAL;
    rec = ringbuf_reserve(m->rb, a3);
    if (!rec)
      return (u64)(i64)-EAGAIN;
    memcpy(rec, (const void *)(usize)a2, a3);
    ringbuf_commit(rec, 0, a4);
    return 0;
  }
  case BPF_FUNC_ringbuf_reserve: {
    struct bpf_map *m = (struct bpf_map *)(usize)a1;

    if (!m || !m->rb || a3)
      return 0;
    return (u64)(usize)ringbuf_reserve(m->rb, a2);
  }
  case BPF_FUNC_ringbuf_submit:
  case BPF_FUNC_ringbuf_discard:
    ringbuf_commit((void *)(usize)a1, id == BPF_FUNC_ringbuf_discard, a2);
    return 0;
  case BPF_FUNC_ringbuf_query: {
    struct bpf_map *m = (struct bpf_map *)(usize)a1;

    return (m && m->rb) ? ringbuf_query(m->rb, a2) : 0;
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

static u64 bpf_helper(u32 id, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5);

/* The JIT is tried once, when the program has passed the verifier. Its failure
 * is not the caller's business: the program runs either way. */
static void bpf_try_jit(struct bpf_prog *p) {
  struct bpf_jit_req req;

  memset(&req, 0, sizeof(req));
  req.insns = p->insns;
  req.insn_cnt = p->insn_cnt;
  req.maps = (void *const *)p->maps;
  req.nmaps = p->nmaps;
  req.helper_fn = (u64)(usize)&bpf_helper;
  p->jit_fn = (u64 (*)(void *))bpf_jit_compile(&req, 0);
  if (p->jit_fn)
    g_bpf_jitted++;
}

static u64 bpf_run(struct bpf_prog *p, struct bpf_run_ctx *ctx) {
  u64 regs[BPF_REG_CNT];
  u8 stack[BPF_STACK_SIZE];
  u32 pc = 0;
  u32 steps = 0;

  if (p->jit_fn)
    return p->jit_fn(ctx);

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

    if (is32)
      sv = (u32)sv; /* a 32-bit operation's operand is 32 bits wide */

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
      case BPF_END: {
        /* To little-endian is a truncation on this host; to big-endian a
         * byte swap of that width. */
        u64 v0 = regs[dst];

        if (i->imm == 16)
          v0 = (u16)v0;
        else if (i->imm == 32)
          v0 = (u32)v0;
        if (BPF_SRC(i->code) == BPF_X)
          v0 = i->imm == 16   ? __builtin_bswap16((u16)v0)
               : i->imm == 32 ? __builtin_bswap32((u32)v0)
                              : __builtin_bswap64(v0);
        regs[dst] = v0;
        pc++;
        continue;
      }
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

      if ((i->code & 0xe0) == 0xc0) { /* BPF_ATOMIC, as the verifier allowed */
        int dw = BPF_SIZE(i->code) == BPF_DW;
        u64 old = 0;

        switch (i->imm & ~BPF_FETCH) {
        case BPF_ADD:
          old = dw ? __atomic_fetch_add((u64 *)addr, val, __ATOMIC_SEQ_CST)
                   : __atomic_fetch_add((u32 *)addr, (u32)val, __ATOMIC_SEQ_CST);
          break;
        case BPF_OR:
          old = dw ? __atomic_fetch_or((u64 *)addr, val, __ATOMIC_SEQ_CST)
                   : __atomic_fetch_or((u32 *)addr, (u32)val, __ATOMIC_SEQ_CST);
          break;
        case BPF_AND:
          old = dw ? __atomic_fetch_and((u64 *)addr, val, __ATOMIC_SEQ_CST)
                   : __atomic_fetch_and((u32 *)addr, (u32)val, __ATOMIC_SEQ_CST);
          break;
        case BPF_XOR:
          old = dw ? __atomic_fetch_xor((u64 *)addr, val, __ATOMIC_SEQ_CST)
                   : __atomic_fetch_xor((u32 *)addr, (u32)val, __ATOMIC_SEQ_CST);
          break;
        case BPF_XCHG & ~BPF_FETCH:
          old = dw ? __atomic_exchange_n((u64 *)addr, val, __ATOMIC_SEQ_CST)
                   : __atomic_exchange_n((u32 *)addr, (u32)val, __ATOMIC_SEQ_CST);
          break;
        case BPF_CMPXCHG & ~BPF_FETCH:
          if (dw) {
            u64 expect = regs[0];

            __atomic_compare_exchange_n((u64 *)addr, &expect, val, 0,
                                        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
            old = expect;
          } else {
            u32 expect = (u32)regs[0];

            __atomic_compare_exchange_n((u32 *)addr, &expect, (u32)val, 0,
                                        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
            old = expect;
          }
          regs[0] = old;
          break;
        }
        if ((i->imm & BPF_FETCH) && i->imm != BPF_CMPXCHG)
          regs[src] = dw ? old : (u32)old;
        break;
      }

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
        regs[0] = bpf_helper((u32)i->imm, regs[1], regs[2], regs[3], regs[4],
                             regs[5]);
        break;
      }
      u64 a = regs[dst], b = sv;

      i64 sa = (i64)a, sb = (i64)b;

      if (cls == BPF_JMP32) {
        a = (u32)a;
        b = (u32)b;
        sa = (i32)(u32)a; /* signed comparisons are 32-bit signed too */
        sb = (i32)(u32)b;
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
      case BPF_JSGT: take = (sa > sb); break;
      case BPF_JSGE: take = (sa >= sb); break;
      case BPF_JSLT: take = (sa < sb); break;
      case BPF_JSLE: take = (sa <= sb); break;
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

u32 bpf_prog_type(void *prog) {
  return prog ? ((struct bpf_prog *)prog)->type : 0;
}

u64 bpf_run_perf(void *prog, const void *frame, int in_user, u64 period) {
  struct bpf_prog *p = prog;
  struct bpf_run_ctx ctx;

  if (!p || !p->used || !frame)
    return 0;
  memset(&ctx, 0, sizeof(ctx));
  bpf_fill_regs(ctx.ctx, frame, in_user);
  ctx.ctx[BPF_REGS_SIZE / 8] = period; /* bpf_perf_event_data.sample_period */
  ctx.ctx[BPF_REGS_SIZE / 8 + 1] = 0;  /* .addr: no data address sampled */
  bpf_frame_origin(frame, &ctx.ip, &ctx.fp);
  ctx.in_user = in_user ? 1 : 0;
  return bpf_run(p, &ctx);
}

u64 bpf_run_trace(void *prog, u16 id, u64 a, u64 b, u64 c, const void *frame) {
  struct bpf_prog *p = prog;
  struct bpf_run_ctx ctx;

  if (!p || !p->used)
    return 0;
  memset(&ctx, 0, sizeof(ctx));
  if (p->type == BPF_PROG_TYPE_KPROBE) {
    if (!frame)
      return 0;
    bpf_fill_regs(ctx.ctx, frame, 0);
    bpf_frame_origin(frame, &ctx.ip, &ctx.fp);
  } else {
    /* The record: common_type, flags, preempt_count, common_pid, then the
     * three words. */
    u8 *rec = (u8 *)ctx.ctx;
    u32 pid = current_task ? (u32)current_task->id : 0;

    memcpy(rec, &id, 2);
    memcpy(rec + 4, &pid, 4);
    ctx.ctx[1] = a;
    ctx.ctx[2] = b;
    ctx.ctx[3] = c;
    /* The stack of a static site is the tracer's own caller chain. */
    ctx.ip = (u64)(usize)__builtin_return_address(0);
    ctx.fp = (u64)(usize)__builtin_frame_address(0);
  }
  return bpf_run(p, &ctx);
}

/* ── the descriptors ─────────────────────────────────────────────────────── */

static void bpf_map_release(struct vfs_handle *h);
static void bpf_prog_release(struct vfs_handle *h);

static void bpf_btf_release(struct vfs_handle *h);
static void bpf_link_release(struct vfs_handle *h);

static int bpf_map_poll(struct vfs_handle *h, struct b1nix_pollfd *pfd);

static const struct vfs_file_ops bpf_map_ops = {.release = bpf_map_release,
                                                .poll = bpf_map_poll};

/* A ring buffer is readable while the producer is ahead of the consumer:
 * that is what libbpf's ring_buffer__poll waits for with epoll. */
static int bpf_map_poll(struct vfs_handle *h, struct b1nix_pollfd *pfd) {
  struct bpf_map *m = h ? (struct bpf_map *)h->private_data : 0;

  pfd->revents = 0;
  if (m && m->rb && rb_cons(m->rb) != __atomic_load_n(&m->rb->prod,
                                                      __ATOMIC_ACQUIRE))
    pfd->revents |= B1NIX_POLLIN;
  return 0;
}

/* mmap of a ring buffer, in Linux's layout: the consumer page, the producer
 * page, then the data -- twice, so a reader sees a record that runs past the
 * end in one piece. */
static int bpf_ringbuf_mmap_page(struct vfs_handle *h, u64 offset,
                                 u64 *out_phys) {
  struct bpf_map *m = h ? (struct bpf_map *)h->private_data : 0;
  struct bpf_ringbuf *rb = m ? m->rb : 0;
  u64 page = offset / PAGE_SIZE;

  if (!rb || (offset & (PAGE_SIZE - 1)))
    return -EINVAL;
  if (page == 0)
    *out_phys = rb->cons_phys;
  else if (page == 1)
    *out_phys = rb->prod_phys;
  else if (page - 2 < 2 * rb->data_pages)
    *out_phys = rb->data_phys + ((page - 2) % rb->data_pages) * PAGE_SIZE;
  else
    return -EINVAL;
  return 0;
}
static const struct vfs_file_ops bpf_prog_ops = {.release = bpf_prog_release};
static const struct vfs_file_ops bpf_btf_ops = {.release = bpf_btf_release};
static const struct vfs_file_ops bpf_link_ops = {.release = bpf_link_release};

/*
 * bpf_link: an attachment as an object of its own. The program stays attached
 * while the link does, whoever holds it; closing the last descriptor detaches,
 * and BPF_LINK_DETACH detaches while the descriptor lives on. The only target
 * here is a perf event (BPF_PERF_EVENT), the one place programs run.
 */
struct bpf_link {
  int used;
  int refs;
  u32 id;
  u32 type;
  struct bpf_prog *prog;     /* for the link's own answers: its id */
  struct vfs_handle *target; /* the perf event, held */
  int detached;
};

#define BPF_MAX_LINKS 64
static struct bpf_link g_links[BPF_MAX_LINKS];
static u32 g_link_next_id = 1;

static void link_detach(struct bpf_link *l) {
  if (l->detached)
    return;
  l->detached = 1;
  perf_event_bpf_detach(l->target, l->prog);
}

static void link_put(struct bpf_link *l) {
  if (!l)
    return;
  int left = __atomic_sub_fetch(&l->refs, 1, __ATOMIC_ACQ_REL);
  KASSERT(left >= 0, "bpf link %p put at refcount %d", (void *)l, left + 1);
  if (left > 0)
    return;
  link_detach(l);
  if (l->target)
    vfs_handle_release(l->target);
  prog_put(l->prog);
  l->target = 0;
  l->prog = 0;
  l->used = 0;
}

static void bpf_link_release(struct vfs_handle *h) {
  struct bpf_link *l = h ? (struct bpf_link *)h->private_data : 0;

  h->private_data = 0;
  if (h->node) {
    vfs_node_put(h->node);
    h->node = 0;
  }
  link_put(l);
}

static struct bpf_link *link_from_fd(int fd) {
  struct vfs_handle *h = scheduler_fd_get(fd);

  if (!h || h->ops != &bpf_link_ops)
    return 0;
  return (struct bpf_link *)h->private_data;
}

/* Linux's struct bpf_link_info, as far as a perf-event link fills it. */
struct bpf_link_info_abi {
  u32 type, id, prog_id, pad;
  u64 rest[6];
} __attribute__((aligned(8)));

static int link_fill_info(void *obj, void *out) {
  struct bpf_link *l = obj;
  struct bpf_link_info_abi *in = out;

  memset(in, 0, sizeof(*in));
  in->type = l->type;
  in->id = l->id;
  in->prog_id = (l->prog && !l->detached) ? l->prog->id : 0;
  return 0;
}

static void bpf_btf_release(struct vfs_handle *h) {
  struct bpf_btf *b = h ? (struct bpf_btf *)h->private_data : 0;

  h->private_data = 0;
  if (h->node) {
    vfs_node_put(h->node);
    h->node = 0;
  }
  btf_put(b);
}

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
  if (ops == &bpf_map_ops && ((struct bpf_map *)obj)->rb)
    node->inode->mmap_handle_page_phys_cb = bpf_ringbuf_mmap_page;

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

static struct bpf_btf *btf_from_fd(int fd) {
  struct vfs_handle *h = scheduler_fd_get(fd);

  if (!h || h->ops != &bpf_btf_ops)
    return 0;
  return (struct bpf_btf *)h->private_data;
}

/* The log a loader asked for, with the reason a load was refused. */
static void bpf_log_out(u64 ubuf, u32 ubuf_len, const char *msg) {
  usize n = strlen(msg) + 1;

  if (!ubuf || !ubuf_len)
    return;
  if (n > ubuf_len)
    n = ubuf_len;
  (void)syscall_copyout((void *)(usize)ubuf, msg, n);
}

/*
 * A program's BTF and the records that point into it.
 *
 * func_info names, for each function in the program, the instruction it starts
 * at and the BTF FUNC that describes it; line_info maps instructions to source
 * lines. Both are checked against the program and the BTF, as Linux checks
 * them: records sorted, starting at instruction 0, inside the program, and
 * naming a FUNC and strings that exist. A record size larger than the kernel's
 * is accepted when the extra bytes are zero -- the ABI's rule for growth.
 */
static int prog_check_btf_info(struct bpf_prog *p, union bpf_attr *attr) {
  struct bpf_btf *b = p->btf;

  if (attr->load.func_info_cnt) {
    u32 rs = attr->load.func_info_rec_size;

    if (!b || rs < 8 || rs > 256 || (rs & 3))
      return -EINVAL;
    u32 prev = 0;

    for (u32 i = 0; i < attr->load.func_info_cnt; i++) {
      u32 rec[64];

      if (syscall_copyin(rec, (const void *)(usize)(attr->load.func_info +
                                                    (u64)i * rs), rs) < 0)
        return -EFAULT;
      for (u32 w = 2; w < rs / 4; w++)
        if (rec[w])
          return -E2BIG;
      if ((i == 0 && rec[0] != 0) || (i && rec[0] <= prev) ||
          rec[0] >= p->insn_cnt)
        return -EINVAL;
      if (btf_type_kind(b->data, rec[1]) != 12 /* BTF_KIND_FUNC */)
        return -EINVAL;
      prev = rec[0];
    }
  }
  if (attr->load.line_info_cnt) {
    u32 rs = attr->load.line_info_rec_size;
    u32 slen = b ? btf_str_len(b->data) : 0;

    if (!b || rs < 16 || rs > 256 || (rs & 3))
      return -EINVAL;
    u32 prev = 0;

    for (u32 i = 0; i < attr->load.line_info_cnt; i++) {
      u32 rec[64];

      if (syscall_copyin(rec, (const void *)(usize)(attr->load.line_info +
                                                    (u64)i * rs), rs) < 0)
        return -EFAULT;
      for (u32 w = 4; w < rs / 4; w++)
        if (rec[w])
          return -E2BIG;
      if ((i == 0 && rec[0] != 0) || (i && rec[0] < prev) ||
          rec[0] >= p->insn_cnt || rec[1] >= slen || rec[2] >= slen)
        return -EINVAL;
      prev = rec[0];
    }
  }
  return 0;
}

static struct bpf_map *map_from_fd(int fd) {
  struct vfs_handle *h = scheduler_fd_get(fd);

  if (!h || h->ops != &bpf_map_ops)
    return 0;
  return (struct bpf_map *)h->private_data;
}

/* bpf_prog_info and bpf_map_info, in Linux's layout. A caller passes the
 * size it knows; the kernel fills that much and says how much it filled. */
struct bpf_prog_info_abi {
  u32 type, id;
  u8 tag[8];
  u32 jited_prog_len, xlated_prog_len;
  u64 jited_prog_insns, xlated_prog_insns, load_time;
  u32 created_by_uid, nr_map_ids;
  u64 map_ids;
  char name[16];
  u32 ifindex, gpl_compatible;
  u64 netns_dev, netns_ino;
  u32 nr_jited_ksyms, nr_jited_func_lens;
  u64 jited_ksyms, jited_func_lens;
  u32 btf_id, func_info_rec_size;
  u64 func_info;
  u32 nr_func_info, nr_line_info;
  u64 line_info, jited_line_info;
  u32 nr_jited_line_info, line_info_rec_size, jited_line_info_rec_size,
      nr_prog_tags;
  u64 prog_tags, run_time_ns, run_cnt, recursion_misses;
  u32 verified_insns, attach_btf_obj_id, attach_btf_id;
} __attribute__((aligned(8)));
_Static_assert(sizeof(struct bpf_prog_info_abi) == 232, "Linux's bpf_prog_info");

struct bpf_map_info_abi {
  u32 type, id, key_size, value_size, max_entries, map_flags;
  char name[16];
  u32 ifindex, btf_vmlinux_value_type_id;
  u64 netns_dev, netns_ino;
  u32 btf_id, btf_key_type_id, btf_value_type_id, btf_vmlinux_id;
  u64 map_extra;
} __attribute__((aligned(8)));
_Static_assert(sizeof(struct bpf_map_info_abi) == 88, "Linux's bpf_map_info");

/* Copy the caller's structure in (it carries the buffers it wants filled),
 * then the answer out, both limited to the size it gave. */
static isize info_reply(union bpf_attr *attr, u64 uattr, u64 size, void *in,
                        u32 in_size, int (*fill)(void *, void *), void *obj) {
  u32 ulen = attr->info.info_len < in_size ? attr->info.info_len : in_size;

  memset(in, 0, in_size);
  if (ulen && syscall_copyin(in, (const void *)(usize)attr->info.info, ulen) < 0)
    return -EFAULT;
  int rc = fill(obj, in);

  if (rc < 0)
    return rc;
  if (ulen && syscall_copyout((void *)(usize)attr->info.info, in, ulen) < 0)
    return -EFAULT;
  attr->info.info_len = ulen;
  return syscall_copyout((void *)(usize)uattr, attr, (usize)size) == 0 ? 0
                                                                        : -EFAULT;
}

static int prog_fill_info(void *obj, void *out) {
  struct bpf_prog *p = obj;
  struct bpf_prog_info_abi *in = out;
  u64 want_maps = in->map_ids;
  u32 room = in->nr_map_ids;
  u64 want_insns = in->xlated_prog_insns;
  u32 insn_room = in->xlated_prog_len;
  u32 insn_bytes = p->insn_cnt * (u32)sizeof(struct bpf_insn);

  /* The program as loaded, into the caller's buffer, as much as fits; the
   * length says how much there is, so a caller can size its next ask. */
  if (want_insns && insn_room &&
      syscall_copyout((void *)(usize)want_insns, p->insns,
                      insn_room < insn_bytes ? insn_room : insn_bytes) < 0)
    return -EFAULT;
  in->type = p->type;
  in->id = p->id;
  in->xlated_prog_len = insn_bytes;
  in->load_time = p->load_time;
  in->created_by_uid = p->uid;
  memcpy(in->name, p->name, sizeof(in->name));
  in->btf_id = p->btf ? p->btf->id : 0;
  in->nr_map_ids = (u32)p->nmaps;
  for (u32 i = 0; want_maps && i < room && i < (u32)p->nmaps; i++) {
    u32 mid = p->maps[i] ? p->maps[i]->id : 0;

    if (syscall_copyout((void *)(usize)(want_maps + (u64)i * 4), &mid, 4) < 0)
      return -EFAULT;
  }
  /* No JIT image, function or line info is handed out: their counts say
   * none, so a caller reads nothing through the buffers it passed. */
  in->jited_prog_len = 0;
  in->nr_jited_ksyms = in->nr_jited_func_lens = 0;
  in->nr_func_info = in->nr_line_info = in->nr_jited_line_info = 0;
  in->nr_prog_tags = 0;
  return 0;
}

static int map_fill_info(void *obj, void *out) {
  struct bpf_map *m = obj;
  struct bpf_map_info_abi *in = out;

  memset(in, 0, sizeof(*in));
  in->type = m->type;
  in->id = m->id;
  in->key_size = m->key_size;
  in->value_size = m->value_size;
  in->max_entries = m->max_entries;
  memcpy(in->name, m->name, sizeof(in->name));
  return 0;
}

static isize prog_get_info(struct bpf_prog *p, union bpf_attr *attr, u64 uattr,
                           u64 size) {
  struct bpf_prog_info_abi in;

  return info_reply(attr, uattr, size, &in, sizeof(in), prog_fill_info, p);
}

static isize map_get_info(struct bpf_map *m, union bpf_attr *attr, u64 uattr,
                          u64 size) {
  struct bpf_map_info_abi in;

  return info_reply(attr, uattr, size, &in, sizeof(in), map_fill_info, m);
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
    /* A ring buffer has no elements, and a stack map's entries are written
     * by get_stackid only -- Linux refuses the same. */
    if (m->rb ||
        (m->type == BPF_MAP_TYPE_STACK_TRACE && cmd == BPF_MAP_UPDATE_ELEM))
      return -EINVAL;

    u8 key[64];
    u64 lf;
    usize vlen = m->evs ? 4 : map_elem_size(m);
    isize rc = 0;

    if (m->key_size > sizeof(key))
      return -EINVAL;
    memset(key, 0, sizeof(key));
    if (attr.elem.key &&
        syscall_copyin(key, (const void *)(usize)attr.elem.key, m->key_size) < 0)
      return -EFAULT;

    if (m->evs) {
      /* A perf-event array: the value is a perf event's descriptor, and the
       * map holds the event itself. */
      u32 idx;

      memcpy(&idx, key, 4);
      if (cmd == BPF_MAP_GET_NEXT_KEY) {
        u32 next = attr.elem.key ? idx + 1 : 0;

        if (attr.elem.key && idx >= m->max_entries)
          next = 0;
        if (next >= m->max_entries)
          return -ENOENT;
        return syscall_copyout((void *)(usize)attr.elem.next_key, &next, 4) == 0
                   ? 0
                   : -EFAULT;
      }
      if (idx >= m->max_entries)
        return cmd == BPF_MAP_LOOKUP_ELEM ? -ENOENT : -E2BIG;
      if (cmd == BPF_MAP_LOOKUP_ELEM)
        return -EOPNOTSUPP; /* a descriptor is not a value to hand back */
      struct vfs_handle *nh = 0, *old;

      if (cmd == BPF_MAP_UPDATE_ELEM) {
        u32 efd;

        if (syscall_copyin(&efd, (const void *)(usize)attr.elem.value, 4) < 0)
          return -EFAULT;
        nh = scheduler_fd_get((int)efd);
        if (!nh)
          return -EBADF;
        if (!perf_event_is_handle(nh))
          return -EINVAL;
        vfs_handle_retain(nh);
      }
      spin_lock_irqsave(&m->lock, &lf);
      old = m->evs[idx];
      m->evs[idx] = nh;
      spin_unlock_irqrestore(&m->lock, lf);
      if (old)
        vfs_handle_release(old);
      return (cmd == BPF_MAP_DELETE_ELEM && !old) ? -ENOENT : 0;
    }

    switch (cmd) {
    case BPF_MAP_LOOKUP_ELEM: {
      u8 *buf = kmalloc(vlen);

      if (!buf)
        return -ENOMEM;
      spin_lock_irqsave(&m->lock, &lf);
      int slot = map_find(m, key);

      if (slot >= 0)
        memcpy(buf, map_elem(m, slot), vlen);
      spin_unlock_irqrestore(&m->lock, lf);
      if (slot < 0)
        rc = -ENOENT;
      else if (syscall_copyout((void *)(usize)attr.elem.value, buf, vlen) < 0)
        rc = -EFAULT;
      kfree(buf);
      return rc;
    }
    case BPF_MAP_UPDATE_ELEM: {
      u8 *buf = kmalloc(vlen);

      if (!buf)
        return -ENOMEM;
      if (syscall_copyin(buf, (const void *)(usize)attr.elem.value, vlen) < 0)
        rc = -EFAULT;
      else
        rc = map_update(m, key, buf, attr.elem.flags, 1);
      kfree(buf);
      return rc;
    }
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
        if (!map_is_hash(m)) {
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
        p->id = g_prog_next_id++;
        break;
      }
    }
    spin_unlock_irqrestore(&g_bpf_lock, flags);
    if (!p)
      return -ENOSPC;

    p->type = attr.load.prog_type;
    p->uid = cred->euid;
    p->load_time = ktime_monotonic_ns();
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

    /* The program's own BTF, when the loader sent it. */
    if (attr.load.prog_btf_fd) {
      struct bpf_btf *b = btf_from_fd((int)attr.load.prog_btf_fd);

      if (!b) {
        prog_put(p);
        return -EBADF;
      }
      btf_get(b);
      p->btf = b;
    }
    {
      int brc = prog_check_btf_info(p, &attr);

      if (brc < 0) {
        bpf_log_out(attr.load.log_buf, attr.load.log_size,
                    "func_info or line_info does not match the program and its BTF\n");
        prog_put(p);
        return brc;
      }
    }

    int rc = bpf_verify(p, attr.load.log_buf, attr.load.log_size);

    if (rc < 0) {
      prog_put(p);
      return rc;
    }
    bpf_try_jit(p);

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
    /* The context is the caller's ctx_in, as Linux's test run takes it: the
     * type's layout, zero past what was given. */
    if (attr.test.ctx_in && attr.test.ctx_size_in) {
      u32 n = attr.test.ctx_size_in;

      if (n > (u32)bpf_ctx_size(p->type))
        return -EINVAL;
      if (syscall_copyin(ctx.ctx, (const void *)(usize)attr.test.ctx_in, n) < 0)
        return -EFAULT;
    }
    if (p->type != BPF_PROG_TYPE_TRACEPOINT) {
#if defined(__aarch64__)
      ctx.ip = ctx.ctx[32];
      ctx.fp = ctx.ctx[29];
#else
      ctx.ip = ctx.ctx[16];
      ctx.fp = ctx.ctx[4];
#endif
    }

    u64 t0 = ktime_monotonic_ns();
    u64 ret = bpf_run(p, &ctx);

    if (bpf_take_wakeup())
      scheduler_wake_all(vfs_poll_chan); /* a ring-buffer reader may wait */

    attr.test.retval = (u32)ret;
    attr.test.duration = (u32)(ktime_monotonic_ns() - t0);
    attr.test.data_size_out = 0;
    return syscall_copyout((void *)(usize)uattr, &attr, (usize)size) == 0
               ? 0
               : -EFAULT;
  }

  case BPF_BTF_LOAD: {
    const struct cred *cred = scheduler_get_current_cred();

    if (!cred || !cred_has_cap_effective(cred, CAP_SYS_ADMIN))
      return -EPERM;
    if (attr.btf_load.btf_flags || attr.btf_load.btf_token_fd)
      return -EINVAL; /* no BPF tokens here */
    u32 bsize = attr.btf_load.btf_size;

    if (!bsize || bsize > (16u << 20))
      return -E2BIG;
    u8 *data = kmalloc(bsize);

    if (!data)
      return -ENOMEM;
    if (syscall_copyin(data, (const void *)(usize)attr.btf_load.btf, bsize) < 0) {
      kfree(data);
      return -EFAULT;
    }
    char log[160];
    u32 ntypes = 0;

    log[0] = 0;
    int rc = btf_parse(data, bsize, &ntypes, log, sizeof(log));

    if (rc < 0) {
      bpf_log_out(attr.btf_load.btf_log_buf, attr.btf_load.btf_log_size, log);
      kfree(data);
      return rc;
    }
    struct bpf_btf *b = btf_new(data, bsize);

    if (!b) {
      kfree(data);
      return -ENOSPC;
    }
    b->ntypes = ntypes;
    int fd = bpf_anon_fd(b, &bpf_btf_ops, "btf");

    if (fd < 0)
      btf_put(b);
    return fd;
  }

  case BPF_BTF_GET_FD_BY_ID: {
    const struct cred *cred = scheduler_get_current_cred();

    if (!cred || !cred_has_cap_effective(cred, CAP_SYS_ADMIN))
      return -EPERM;
    struct bpf_btf *b = btf_by_id(attr.getid.start_id);

    if (!b)
      return -ENOENT;
    int fd = bpf_anon_fd(b, &bpf_btf_ops, "btf");

    if (fd < 0)
      btf_put(b);
    return fd;
  }

  case BPF_BTF_GET_NEXT_ID: {
    const struct cred *cred = scheduler_get_current_cred();

    if (!cred || !cred_has_cap_effective(cred, CAP_SYS_ADMIN))
      return -EPERM;
    u32 next = btf_next_id(attr.getid.start_id);

    if (!next)
      return -ENOENT;
    attr.getid.next_id = next;
    return syscall_copyout((void *)(usize)uattr, &attr, (usize)size) == 0
               ? 0
               : -EFAULT;
  }

  case BPF_PROG_GET_NEXT_ID:
  case BPF_MAP_GET_NEXT_ID: {
    const struct cred *cred = scheduler_get_current_cred();
    u32 best = 0;
    u64 lf;

    if (!cred || !cred_has_cap_effective(cred, CAP_SYS_ADMIN))
      return -EPERM;
    spin_lock_irqsave(&g_bpf_lock, &lf);
    if (cmd == BPF_PROG_GET_NEXT_ID) {
      for (int i = 0; i < BPF_MAX_PROGS; i++)
        if (g_progs[i].used && g_progs[i].refs > 0 &&
            g_progs[i].id > attr.getid.start_id &&
            (!best || g_progs[i].id < best))
          best = g_progs[i].id;
    } else {
      for (int i = 0; i < BPF_MAX_MAPS; i++)
        if (g_maps[i].used && g_maps[i].refs > 0 &&
            g_maps[i].id > attr.getid.start_id &&
            (!best || g_maps[i].id < best))
          best = g_maps[i].id;
    }
    spin_unlock_irqrestore(&g_bpf_lock, lf);
    if (!best)
      return -ENOENT;
    attr.getid.next_id = best;
    return syscall_copyout((void *)(usize)uattr, &attr, (usize)size) == 0
               ? 0
               : -EFAULT;
  }

  case BPF_LINK_CREATE: {
    const struct cred *cred = scheduler_get_current_cred();
    struct bpf_prog *p;
    struct vfs_handle *target;
    struct bpf_link *l = 0;
    u64 lf;
    int rc, fd;

    if (!cred || !cred_has_cap_effective(cred, CAP_SYS_ADMIN))
      return -EPERM;
    if (attr.link_create.attach_type != BPF_PERF_EVENT)
      return -EINVAL; /* the only attachment point there is here */
    if (attr.link_create.flags)
      return -EINVAL;
    target = scheduler_fd_get((int)attr.link_create.target_fd);
    if (!target)
      return -EBADF;
    p = bpf_prog_get((int)attr.link_create.prog_fd);
    if (!p)
      return -EBADF;
    spin_lock_irqsave(&g_bpf_lock, &lf);
    for (int i = 0; i < BPF_MAX_LINKS && !l; i++)
      if (!g_links[i].used) {
        l = &g_links[i];
        memset(l, 0, sizeof(*l));
        l->used = 1;
        l->refs = 1;
        l->id = g_link_next_id++;
        l->type = BPF_LINK_TYPE_PERF_EVENT;
      }
    spin_unlock_irqrestore(&g_bpf_lock, lf);
    if (!l) {
      prog_put(p);
      return -ENOSPC;
    }
    /* One reference goes to the event, one stays with the link. */
    __atomic_add_fetch(&p->refs, 1, __ATOMIC_ACQ_REL);
    rc = perf_event_bpf_attach(target, p);
    if (rc < 0) {
      prog_put(p);
      prog_put(p);
      l->used = 0;
      return rc;
    }
    vfs_handle_retain(target);
    l->target = target;
    l->prog = p;
    fd = bpf_anon_fd(l, &bpf_link_ops, "bpf-link");
    if (fd < 0)
      link_put(l);
    return fd;
  }

  case BPF_LINK_DETACH: {
    struct bpf_link *l = link_from_fd((int)attr.link_detach.link_fd);

    if (!l)
      return -EBADF;
    link_detach(l);
    return 0;
  }

  case BPF_LINK_UPDATE:
    /* A perf-event link cannot swap its program on Linux either. */
    return link_from_fd((int)attr.link_update.link_fd) ? -EOPNOTSUPP : -EBADF;

  case BPF_LINK_GET_NEXT_ID: {
    const struct cred *cred = scheduler_get_current_cred();
    u32 best = 0;
    u64 lf;

    if (!cred || !cred_has_cap_effective(cred, CAP_SYS_ADMIN))
      return -EPERM;
    spin_lock_irqsave(&g_bpf_lock, &lf);
    for (int i = 0; i < BPF_MAX_LINKS; i++)
      if (g_links[i].used && g_links[i].refs > 0 &&
          g_links[i].id > attr.getid.start_id &&
          (!best || g_links[i].id < best))
        best = g_links[i].id;
    spin_unlock_irqrestore(&g_bpf_lock, lf);
    if (!best)
      return -ENOENT;
    attr.getid.next_id = best;
    return syscall_copyout((void *)(usize)uattr, &attr, (usize)size) == 0
               ? 0
               : -EFAULT;
  }

  case BPF_LINK_GET_FD_BY_ID: {
    const struct cred *cred = scheduler_get_current_cred();
    struct bpf_link *l = 0;
    u64 lf;
    int fd;

    if (!cred || !cred_has_cap_effective(cred, CAP_SYS_ADMIN))
      return -EPERM;
    spin_lock_irqsave(&g_bpf_lock, &lf);
    for (int i = 0; i < BPF_MAX_LINKS && !l; i++)
      if (g_links[i].used && g_links[i].id == attr.getid.start_id &&
          ref_get_live(&g_links[i].refs))
        l = &g_links[i];
    spin_unlock_irqrestore(&g_bpf_lock, lf);
    if (!l)
      return -ENOENT;
    fd = bpf_anon_fd(l, &bpf_link_ops, "bpf-link");
    if (fd < 0)
      link_put(l);
    return fd;
  }

  case BPF_PROG_GET_FD_BY_ID:
  case BPF_MAP_GET_FD_BY_ID: {
    const struct cred *cred = scheduler_get_current_cred();
    void *obj = 0;
    u64 lf;

    if (!cred || !cred_has_cap_effective(cred, CAP_SYS_ADMIN))
      return -EPERM;
    spin_lock_irqsave(&g_bpf_lock, &lf);
    if (cmd == BPF_PROG_GET_FD_BY_ID) {
      for (int i = 0; i < BPF_MAX_PROGS && !obj; i++)
        if (g_progs[i].used && g_progs[i].id == attr.getid.start_id &&
            ref_get_live(&g_progs[i].refs))
          obj = &g_progs[i];
    } else {
      for (int i = 0; i < BPF_MAX_MAPS && !obj; i++)
        if (g_maps[i].used && g_maps[i].id == attr.getid.start_id &&
            ref_get_live(&g_maps[i].refs))
          obj = &g_maps[i];
    }
    spin_unlock_irqrestore(&g_bpf_lock, lf);
    if (!obj)
      return -ENOENT;
    int fd = cmd == BPF_PROG_GET_FD_BY_ID
                 ? bpf_anon_fd(obj, &bpf_prog_ops, "bpf-prog")
                 : bpf_anon_fd(obj, &bpf_map_ops, "bpf-map");

    if (fd < 0) {
      if (cmd == BPF_PROG_GET_FD_BY_ID)
        prog_put(obj);
      else
        map_put(obj);
    }
    return fd;
  }

  case BPF_OBJ_GET_INFO_BY_FD: {
    struct bpf_btf *b = btf_from_fd((int)attr.info.bpf_fd);
    struct vfs_handle *ih = scheduler_fd_get((int)attr.info.bpf_fd);

    if (!b && ih && ih->ops == &bpf_prog_ops && ih->private_data)
      return prog_get_info(ih->private_data, &attr, uattr, size);
    if (!b && ih && ih->ops == &bpf_map_ops && ih->private_data)
      return map_get_info(ih->private_data, &attr, uattr, size);
    if (!b && ih && ih->ops == &bpf_link_ops && ih->private_data) {
      struct bpf_link_info_abi in;

      return info_reply(&attr, uattr, size, &in, sizeof(in), link_fill_info,
                        ih->private_data);
    }
    if (!b)
      return ih ? -EINVAL : -EBADF; /* not a BPF object */
    struct bpf_btf_info in;
    u32 ulen = attr.info.info_len;

    memset(&in, 0, sizeof(in));
    if (ulen > sizeof(in))
      ulen = sizeof(in);
    if (ulen && syscall_copyin(&in, (const void *)(usize)attr.info.info, ulen) < 0)
      return -EFAULT;
    /* The blob, into the caller's buffer, as much as it has room for; the
     * full size is reported either way so it can ask again. */
    if (in.btf && in.btf_size) {
      u32 n = in.btf_size < b->size ? in.btf_size : b->size;

      if (syscall_copyout((void *)(usize)in.btf, b->data, n) < 0)
        return -EFAULT;
    }
    /* The name's length is reported whether or not a buffer came with the
     * question: a caller asks once to learn it, then again to read it. A
     * buffer too small gets what fits, terminated, and ENOSPC -- Linux's
     * answer. */
    int name_rc = 0;
    {
      const char *nm = b->kernel ? "vmlinux" : "";
      u32 len = (u32)strlen(nm);

      if (in.name && in.name_len) {
        char tmp[16];
        u32 n = len + 1 <= in.name_len ? len : in.name_len - 1;

        memcpy(tmp, nm, n);
        tmp[n] = 0;
        if (syscall_copyout((void *)(usize)in.name, tmp, n + 1) < 0)
          return -EFAULT;
        if (n < len)
          name_rc = -ENOSPC;
      }
      in.name_len = len;
    }
    in.btf_size = b->size;
    in.id = b->id;
    in.kernel_btf = (u32)b->kernel;
    if (ulen && syscall_copyout((void *)(usize)attr.info.info, &in, ulen) < 0)
      return -EFAULT;
    attr.info.info_len = ulen;
    if (syscall_copyout((void *)(usize)uattr, &attr, (usize)size) < 0)
      return -EFAULT;
    return name_rc;
  }

  default:
    /* Pinning, cgroup attachment, program and map ids: none of them exists
     * here, and each is refused where the loader can see it rather than
     * ignored. */
    return -EOPNOTSUPP;
  }
}

int bpf_syscall(u64 nr, u64 a0, u64 a1, u64 a2, u64 *ret) {
  if (nr != BPF_NR_bpf)
    return 0;
  *ret = (u64)bpf_cmd(a0, a1, a2);
  return 1;
}
