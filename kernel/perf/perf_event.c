/* SPDX-License-Identifier: GPL-2.0-only */
/* perf_event_open(2) — software counters, timer-driven sampling and the
 * mmap'd ring buffer `perf` reads (M126).
 *
 * WHAT THIS IS
 *
 * perf_event_open(2) hands back a descriptor that names one counter. read(2)
 * on it gives the count; ioctl(2) starts, stops and resets it; and mmap(2) on
 * it gives a ring buffer into which the kernel writes records — the samples a
 * profiler turns into a profile. The ABI is Linux's, taken verbatim from
 * <b1nix/perf_event_abi.h>, because the distribution's own `perf` binary is
 * compiled against those structures.
 *
 * WHAT IS COUNTED, AND WHAT IS REFUSED
 *
 * Software counters only, and only the ones this kernel really keeps:
 *
 *   PERF_COUNT_SW_TASK_CLOCK        the task's own CPU time (M86 accounting)
 *   PERF_COUNT_SW_CPU_CLOCK         wall time while the counter is enabled
 *   PERF_COUNT_SW_PAGE_FAULTS       faults the task took, from the fault path
 *   PERF_COUNT_SW_PAGE_FAULTS_MIN   ... that needed no I/O
 *   PERF_COUNT_SW_PAGE_FAULTS_MAJ   ... that came from swap or a file
 *   PERF_COUNT_SW_CONTEXT_SWITCHES  the scheduler's own nvcsw + nivcsw
 *   PERF_COUNT_SW_DUMMY             counts nothing, by definition; `perf` opens
 *                                   one purely to get a ring buffer for the
 *                                   PERF_RECORD_MMAP/COMM stream
 *
 * Everything else is refused where a caller can still do something about it:
 * PERF_TYPE_HARDWARE, PERF_TYPE_HW_CACHE and PERF_TYPE_RAW are -EOPNOTSUPP
 * because there is no PMU driver here (no IA32_PERFEVTSELx programming, no
 * counter-overflow NMI), and a counter that read zero for ever would be worse
 * than an honest refusal. PERF_TYPE_TRACEPOINT is -EOPNOTSUPP because there is
 * no tracepoint registry to attach to. A sample_type bit whose field this does
 * not produce is -EOPNOTSUPP at open for the same reason.
 *
 * HOW A SAMPLE IS TAKEN
 *
 * From the LAPIC timer tick, on the CPU that took it, with the register file
 * of whatever it interrupted. There is no counter-overflow interrupt to hang
 * sampling off, so the tick is the sampling clock: attr.freq asks for N
 * samples a second and gets one every HZ/N ticks, and an attr.sample_period on
 * a clock counter is charged the tick's worth of nanoseconds. That is a real
 * profile — the same statistical sampling `perf record` performs — at the
 * resolution the timer gives (SCHED_TICKS_PER_SEC, 1 kHz here), and the period
 * written into each PERF_RECORD_SAMPLE says exactly what it is worth.
 *
 * The call chain is walked without faulting: user frame pointers are resolved
 * through the page tables with paging_user_frame and a page that is not there
 * simply ends the chain. A sample is taken in interrupt context, so nothing
 * here allocates, sleeps or takes a lock another path holds while it sleeps.
 */

#include <b1nix/perf_event.h>

#include <b1nix/arch.h>
#include <b1nix/bpf.h>
#include <b1nix/bootinfo.h>
#include <b1nix/errno.h>
#include <b1nix/klog.h>
#include <b1nix/ktime.h>
#include <b1nix/mm.h>
#include <b1nix/perf_event_abi.h>
#include <b1nix/posix.h>
#include <b1nix/sched.h>
#include <b1nix/syscall.h>
#include <b1nix/uidgid.h>
#include <b1nix/vfs.h>

#include <stdio.h>
#include <string.h>

extern void *vfs_poll_chan;

/* perf_event_open(2): 298 on x86_64, 241 on aarch64. */
#if defined(__aarch64__)
#define PERF_NR_open 241
#else
#define PERF_NR_open 298
#endif

/* The most events one machine may have open at once. Each holds a ring buffer
 * of its own, so this is a bound on kernel memory a profiler can ask for. */
#define PERF_MAX_EVENTS 512
/* The largest ring buffer a single event may map: 1 control page plus this
 * many data pages. `perf record`'s default is 512 data pages. */
#define PERF_MAX_DATA_PAGES 1024u
/* How deep a call chain is walked, user and kernel together. */
#define PERF_MAX_CALLCHAIN 96

struct perf_ev {
  struct perf_ev *next; /* g_events */

  struct perf_event_attr attr;
  u64 id;

  /* Who is counted. `target` is a pid; 0 means every task on the machine,
   * which is what perf_event_open(pid = -1) asks for. */
  usize target;
  int cpu; /* -1 = any CPU */

  /* The group this event belongs to. A leader points at itself. */
  struct perf_ev *leader;

  int enabled;
  /* PERF_EVENT_IOC_REFRESH: how many more samples may be delivered before the
   * event disables itself. -1 when no refresh limit is in force. */
  i64 refresh;

  /* What has been counted while enabled, and the target's raw counter value at
   * the moment counting last started. */
  u64 count;
  u64 base;
  u64 time_enabled, time_running, since;

  /* Sampling. `ticks_per_sample` is what attr.freq turns into; `period_ns` is
   * what an explicit sample_period on a clock counter is charged against. */
  u64 ticks_per_sample, ticks_left;
  u64 period_ns, ns_left;
  u64 nr_samples;

  /* A hardware counter, when this event is one: the slot the PMU driver gave
   * us, or -1 for a software counter. */
  int pmu_slot;

  /* An eBPF program attached with PERF_EVENT_IOC_SET_BPF. It runs on every
   * sample this event takes, and its return value decides whether the sample
   * is kept -- which is how a profiler turns a firehose of records into a
   * histogram it builds in a map. */
  void *bpf_prog;

  /* The ring buffer, kernel-owned physical pages the process maps. */
  u64 rb_phys;
  usize rb_pages; /* control page + data pages */
  struct perf_event_mmap_page *ctrl;
  char *data;
  u64 data_size;
  u64 head;  /* our copy of ctrl->data_head */
  u64 lost;  /* records dropped since the last PERF_RECORD_LOST */

  struct vfs_node *node;
};

static struct perf_ev *g_events;
static u32 g_nr_events;
static spinlock_t g_perf_lock = SPINLOCK_INIT;
static u64 g_next_id = 1;
/* /proc/sys/kernel/perf_event_paranoid. 2 is Linux's default: an unprivileged
 * process may profile its own tasks and nothing else. */
static int g_paranoid = 2;

int perf_event_paranoid_get(void) { return g_paranoid; }
void perf_event_paranoid_set(int v) { g_paranoid = v; }

/* ---- the counters ------------------------------------------------------- */

/* Whether this attr names something really counted here. */
static int perf_attr_supported(const struct perf_event_attr *a) {
  if (a->type == PERF_TYPE_HARDWARE || a->type == PERF_TYPE_HW_CACHE ||
      a->type == PERF_TYPE_RAW) {
    /* The PMU driver decides: it knows which counters this CPU has and which
     * architectural events it reports as available. On a machine with no PMU
     * at all it refuses everything, and hardware events stay -EOPNOTSUPP. */
    u64 evsel;

    return perf_pmu_map(a, &evsel) == 0;
  }
  if (a->type != PERF_TYPE_SOFTWARE)
    return 0;
  switch (a->config) {
  case PERF_COUNT_SW_CPU_CLOCK:
  case PERF_COUNT_SW_TASK_CLOCK:
  case PERF_COUNT_SW_PAGE_FAULTS:
  case PERF_COUNT_SW_PAGE_FAULTS_MIN:
  case PERF_COUNT_SW_PAGE_FAULTS_MAJ:
  case PERF_COUNT_SW_CONTEXT_SWITCHES:
  case PERF_COUNT_SW_DUMMY:
    return 1;
  default:
    return 0;
  }
}

/* A counter in the units the ABI gives it: nanoseconds for the clocks, events
 * for the rest. Read straight from the accounting the kernel already keeps. */
static u64 perf_raw_one(u64 config, struct task *t) {
  switch (config) {
  case PERF_COUNT_SW_CPU_CLOCK:
    return ktime_monotonic_ns();
  case PERF_COUNT_SW_TASK_CLOCK:
    return t ? task_utime_ns(t) + task_stime_ns(t) : 0;
  case PERF_COUNT_SW_PAGE_FAULTS:
    return t ? task_minflt(t) + task_majflt(t) : 0;
  case PERF_COUNT_SW_PAGE_FAULTS_MIN:
    return t ? task_minflt(t) : 0;
  case PERF_COUNT_SW_PAGE_FAULTS_MAJ:
    return t ? task_majflt(t) : 0;
  case PERF_COUNT_SW_CONTEXT_SWITCHES:
    return t ? task_nvcsw(t) + task_nivcsw(t) : 0;
  default:
    return 0; /* PERF_COUNT_SW_DUMMY counts nothing, and says so */
  }
}

/* The same, summed over every task when the event watches the machine. */
static u64 perf_raw(const struct perf_ev *ev) {
  if (ev->pmu_slot >= 0)
    return perf_pmu_slot_count(ev->pmu_slot);
  if (ev->attr.config == PERF_COUNT_SW_CPU_CLOCK)
    return ktime_monotonic_ns();
  if (ev->target)
    return perf_raw_one(ev->attr.config, scheduler_task_by_pid(ev->target));
  {
    u64 sum = 0;

    usize slots = scheduler_task_slots();

    for (usize i = 0; i < slots; i++) {
      struct task *t = scheduler_task_slot(i);

      if (t)
        sum += perf_raw_one(ev->attr.config, t);
    }
    return sum;
  }
}

static u64 perf_count_now(const struct perf_ev *ev) {
  u64 c = ev->count;

  if (ev->enabled)
    c += perf_raw(ev) - ev->base;
  return c;
}

static void perf_enable(struct perf_ev *ev) {
  if (ev->enabled)
    return;
  ev->base = perf_raw(ev);
  ev->since = ktime_monotonic_ns();
  ev->enabled = 1;
}

static void perf_disable(struct perf_ev *ev) {
  if (!ev->enabled)
    return;
  ev->count += perf_raw(ev) - ev->base;
  {
    u64 now = ktime_monotonic_ns();

    ev->time_enabled += now - ev->since;
    ev->time_running += now - ev->since;
  }
  ev->enabled = 0;
}

/* ---- the ring buffer ---------------------------------------------------- */

static void *perf_kva(u64 phys) {
  return (void *)(usize)(phys + vmm_direct_map_base());
}

/* Bytes the program has not consumed yet. */
static u64 perf_rb_used(struct perf_ev *ev) {
  u64 tail = __atomic_load_n(&ev->ctrl->data_tail, __ATOMIC_ACQUIRE);

  return ev->head - tail;
}

/* Copy into the ring at the write head, wrapping. The caller has already made
 * sure the record fits. */
static void perf_rb_put(struct perf_ev *ev, const void *src, u32 len) {
  u64 off = ev->head & (ev->data_size - 1);
  u64 first = ev->data_size - off;

  if (first > len)
    first = len;
  memcpy(ev->data + off, src, first);
  if (len > first)
    memcpy(ev->data, (const char *)src + first, len - first);
  ev->head += len;
}

/* One record, header first. Returns 0 when it went in, -1 when the buffer was
 * full — in which case it is counted and reported as PERF_RECORD_LOST later,
 * which is exactly what the count is for. */
static int perf_rb_record(struct perf_ev *ev, u32 type, u16 misc,
                          const void *body, u32 body_len) {
  struct perf_event_header h;
  u32 total = (u32)sizeof(h) + body_len;

  if (!ev->data || !ev->ctrl)
    return -1;
  if (perf_rb_used(ev) + total > ev->data_size) {
    ev->lost++;
    return -1;
  }
  h.type = type;
  h.misc = misc;
  h.size = (u16)total;
  perf_rb_put(ev, &h, sizeof(h));
  if (body_len)
    perf_rb_put(ev, body, body_len);
  __atomic_store_n(&ev->ctrl->data_head, ev->head, __ATOMIC_RELEASE);
  return 0;
}

/* The sample-id trailer every non-SAMPLE record carries when
 * attr.sample_id_all is set. The field order is the ABI's, and it is the
 * reverse-engineering hazard of this whole interface: `perf` parses the
 * trailer from the END of the record, so a field too many or too few makes
 * every record after it nonsense. */
static u32 perf_id_trailer(struct perf_ev *ev, u64 *out, usize pid, usize tid) {
  u64 st = ev->attr.sample_type;
  u32 n = 0;

  if (!ev->attr.sample_id_all)
    return 0;
  if (st & PERF_SAMPLE_TID)
    out[n++] = ((u64)(u32)tid << 32) | (u32)pid;
  if (st & PERF_SAMPLE_TIME)
    out[n++] = ktime_monotonic_ns();
  if (st & PERF_SAMPLE_ID)
    out[n++] = ev->id;
  if (st & PERF_SAMPLE_STREAM_ID)
    out[n++] = ev->id;
  if (st & PERF_SAMPLE_CPU)
    out[n++] = 0;
  if (st & PERF_SAMPLE_IDENTIFIER)
    out[n++] = ev->id;
  return n * (u32)sizeof(u64);
}

/* ---- sampling ----------------------------------------------------------- */

/* Read one 64-bit word of the current process's memory without faulting: the
 * page tables are walked directly, and a page that is not resident ends the
 * walk. A sample is taken from an interrupt, where a fault would be a fault in
 * interrupt context. */
static int perf_peek_user_u64(u64 pml4, u64 va, u64 *out) {
  u64 frame;

  if (!pml4 || (va & 7u))
    return -1;
  /* A word that straddles two pages would need both; refuse rather than read
   * half of it. */
  if (((va + 7) & ~(u64)(PAGE_SIZE - 1)) != (va & ~(u64)(PAGE_SIZE - 1)))
    return -1;
  frame = paging_user_frame(pml4, va & ~(u64)(PAGE_SIZE - 1));
  if (!frame)
    return -1;
  *out = *(volatile u64 *)(usize)(frame + vmm_direct_map_base() +
                                  (va & (PAGE_SIZE - 1)));
  return 0;
}

/* Walk the user frame-pointer chain from (pc, fp). Returns how many entries
 * were written, PERF_CONTEXT_USER included. */
static u32 perf_user_callchain(u64 *ips, u32 max, u64 pc, u64 fp) {
  struct task *t = current_task;
  u64 pml4 = t ? t->pml4_phys : 0;
  u32 n = 0;

  if (max < 2)
    return 0;
  ips[n++] = PERF_CONTEXT_USER;
  ips[n++] = pc;
  while (n < max && fp) {
    u64 next_fp = 0, ret = 0;

    if (perf_peek_user_u64(pml4, fp, &next_fp) < 0)
      break;
    if (perf_peek_user_u64(pml4, fp + 8, &ret) < 0)
      break;
    if (!ret)
      break;
    ips[n++] = ret;
    /* A frame pointer that does not move forward is a chain that has stopped
     * meaning anything — a leaf compiled without frame pointers, or a corrupt
     * stack. Stop rather than loop. */
    if (next_fp <= fp)
      break;
    fp = next_fp;
  }
  return n;
}

/* Write one PERF_RECORD_SAMPLE. Interrupt context: no allocation, no sleeping.
 * The body is assembled in a stack buffer whose size bounds the sample. */
static void perf_emit_sample(struct perf_ev *ev, u64 pc, u64 fp, int in_user,
                             int cpu, u64 period) {
  u64 body[16 + PERF_MAX_CALLCHAIN];
  u32 n = 0;
  u64 st = ev->attr.sample_type;
  struct task *t = current_task;
  usize pid = t ? task_tgid(t) : 0;
  usize tid = t ? t->id : 0;
  u16 misc = in_user ? PERF_RECORD_MISC_USER : PERF_RECORD_MISC_KERNEL;

  /* The order is the ABI's (Linux's perf_output_sample); every consumer reads
   * the fields positionally. */
  if (st & PERF_SAMPLE_IDENTIFIER)
    body[n++] = ev->id;
  if (st & PERF_SAMPLE_IP)
    body[n++] = pc;
  if (st & PERF_SAMPLE_TID)
    body[n++] = ((u64)(u32)tid << 32) | (u32)pid;
  if (st & PERF_SAMPLE_TIME)
    body[n++] = ktime_monotonic_ns();
  if (st & PERF_SAMPLE_ADDR)
    body[n++] = 0;
  if (st & PERF_SAMPLE_ID)
    body[n++] = ev->id;
  if (st & PERF_SAMPLE_STREAM_ID)
    body[n++] = ev->id;
  if (st & PERF_SAMPLE_CPU)
    body[n++] = (u64)(u32)cpu; /* cpu, then a reserved word of zero */
  if (st & PERF_SAMPLE_PERIOD)
    body[n++] = period;
  if (st & PERF_SAMPLE_CALLCHAIN) {
    u32 slot = n++;
    u32 got = 0;

    if (in_user)
      got = perf_user_callchain(&body[n], PERF_MAX_CALLCHAIN, pc, fp);
    else {
      /* Interrupted in the kernel: report the kernel context marker and the
       * kernel PC. The kernel's own frame chain is not walked here — the
       * profile a user asks for is of their program. */
      body[n] = PERF_CONTEXT_KERNEL;
      body[n + 1] = pc;
      got = 2;
    }
    body[slot] = got;
    n += got;
  }
  if (perf_rb_record(ev, PERF_RECORD_SAMPLE, misc, body,
                     n * (u32)sizeof(u64)) == 0)
    ev->nr_samples++;
}

/* Does this event want the task that was running when the tick landed? */
static int perf_watches(const struct perf_ev *ev, struct task *t, int cpu) {
  /* A sampling event needs somewhere for the sample to go: a ring buffer, or
   * an attached program, which IS the consumer -- a profiler that counts into
   * a map never maps a buffer at all. */
  if (!ev->enabled || (!ev->data && !ev->bpf_prog))
    return 0;
  if (ev->cpu >= 0 && ev->cpu != cpu)
    return 0;
  if (!ev->target)
    return 1; /* every task */
  if (!t)
    return 0;
  return t->id == ev->target || task_tgid(t) == ev->target;
}

void perf_event_tick_sample(u64 pc, u64 fp, int in_user, int cpu) {
  struct task *t = current_task;
  u64 flags;
  int woke = 0;

  if (!g_events)
    return;
  spin_lock_irqsave(&g_perf_lock, &flags);
  for (struct perf_ev *ev = g_events; ev; ev = ev->next) {
    u64 period;

    if (!perf_watches(ev, t, cpu))
      continue;
    if (!ev->ticks_per_sample && !ev->period_ns)
      continue; /* a counting event, not a sampling one */

    if (ev->ticks_per_sample) {
      if (--ev->ticks_left)
        continue;
      ev->ticks_left = ev->ticks_per_sample;
      period = ev->ticks_per_sample * (1000000000ull / SCHED_TICKS_PER_SEC);
    } else {
      u64 tick_ns = 1000000000ull / SCHED_TICKS_PER_SEC;

      if (ev->ns_left > tick_ns) {
        ev->ns_left -= tick_ns;
        continue;
      }
      ev->ns_left = ev->period_ns;
      period = ev->period_ns;
    }

    /* An attached program sees the sample first. Zero means "drop it", which
     * is what a program that is counting into a map returns: it has already
     * recorded what it wanted and no record needs to reach userspace. */
    if (ev->bpf_prog) {
      u64 pid_tgid = 0;

      if (current_task)
        pid_tgid = ((u64)task_tgid(current_task) << 32) | (u32)current_task->id;
      if (bpf_run_perf(ev->bpf_prog, pc, pid_tgid, (u64)cpu) == 0)
        continue;
    }

    if (!ev->data)
      continue; /* the program was the consumer; there is no buffer to fill */
    perf_emit_sample(ev, pc, fp, in_user, cpu, period);
    woke = 1;

    /* PERF_EVENT_IOC_REFRESH: deliver this many and then stop, so a caller
     * that asked for one sample gets one. */
    if (ev->refresh > 0 && --ev->refresh == 0)
      ev->enabled = 0;
  }
  spin_unlock_irqrestore(&g_perf_lock, flags);
  if (woke)
    scheduler_wake_all(vfs_poll_chan);
}

/* ---- records about processes -------------------------------------------- */

/* PERF_RECORD_COMM and one PERF_RECORD_MMAP2 per file-backed mapping, so a
 * `perf report` can put a sampled address in a function of a library. Emitted
 * when the event is enabled, which is where Linux's synthesised records come
 * from too (perf's own --synth does it from /proc). */
static void perf_emit_task_records(struct perf_ev *ev, struct task *t) {
  char body[VFS_MAX_PATH + 128];
  u64 trailer[8];
  u32 tlen;

  if (!t || !ev->data)
    return;

  {
    /* struct { u32 pid, tid; char comm[]; } — the name padded to 8 bytes. */
    u32 *p = (u32 *)(void *)body;
    const char *nm = t->name ? t->name : "?";
    usize len = strlen(nm) + 1;
    usize pad;

    if (len > 64)
      len = 64;
    p[0] = (u32)task_tgid(t);
    p[1] = (u32)t->id;
    memcpy(body + 8, nm, len - 1);
    body[8 + len - 1] = 0;
    pad = (8 + len + 7u) & ~(usize)7u;
    memset(body + 8 + len, 0, pad - (8 + len));
    tlen = perf_id_trailer(ev, trailer, task_tgid(t), t->id);
    memcpy(body + pad, trailer, tlen);
    perf_rb_record(ev, PERF_RECORD_COMM, 0, body, (u32)pad + tlen);
  }

  for (struct vm_area *v = t->vma_list; v; v = v->next) {
    char path[VFS_MAX_PATH];
    usize len, off;

    if (!v->node)
      continue;
    if (vfs_get_node_path(v->node, path, sizeof(path)) != 0 || !path[0])
      continue;
    /* struct perf_record_mmap2: pid, tid, addr, len, pgoff, maj, min, ino,
     * ino_generation, prot, flags, then the path. */
    {
      u32 *p32 = (u32 *)(void *)body;
      u64 *p64;

      p32[0] = (u32)task_tgid(t);
      p32[1] = (u32)t->id;
      p64 = (u64 *)(void *)(body + 8);
      p64[0] = v->start;
      p64[1] = v->end - v->start;
      p64[2] = (u64)v->offset;
      p64[3] = 0; /* maj/min as one 64-bit pair of zeros */
      p64[4] = v->node->inode ? v->node->inode->ino : 0;
      p64[5] = 0; /* ino_generation */
      off = 8 + 6 * 8;
      *(u32 *)(void *)(body + off) = v->prot;
      *(u32 *)(void *)(body + off + 4) = 0x0001; /* MAP_SHARED-or-not: flags */
      off += 8;
      len = strlen(path) + 1;
      if (off + len + 8 > sizeof(body))
        continue;
      memcpy(body + off, path, len);
      off += len;
      while (off & 7u)
        body[off++] = 0;
      tlen = perf_id_trailer(ev, trailer, task_tgid(t), t->id);
      memcpy(body + off, trailer, tlen);
      perf_rb_record(ev, PERF_RECORD_MMAP2,
                     PERF_RECORD_MISC_USER, body, (u32)off + tlen);
    }
  }
}

void perf_event_task_exit(struct task *t) {
  u64 flags;
  u64 trailer[8];

  if (!g_events || !t)
    return;
  spin_lock_irqsave(&g_perf_lock, &flags);
  for (struct perf_ev *ev = g_events; ev; ev = ev->next) {
    u32 tlen;
    char buf[24 + sizeof(trailer)];

    if (!ev->data || !ev->attr.task)
      continue;
    if (ev->target && ev->target != t->id && ev->target != task_tgid(t))
      continue;
    /* struct { u32 pid, ppid, tid, ptid; u64 time; } */
    {
      u32 *p = (u32 *)(void *)buf;

      p[0] = (u32)task_tgid(t);
      p[1] = (u32)t->parent_id;
      p[2] = (u32)t->id;
      p[3] = (u32)t->parent_id;
      *(u64 *)(void *)(buf + 16) = ktime_monotonic_ns();
    }
    tlen = perf_id_trailer(ev, trailer, task_tgid(t), t->id);
    memcpy(buf + 24, trailer, tlen);
    perf_rb_record(ev, PERF_RECORD_EXIT, 0, buf, 24 + tlen);
  }
  spin_unlock_irqrestore(&g_perf_lock, flags);
}

/* ---- the descriptor ----------------------------------------------------- */

static const struct vfs_file_ops perf_file_ops;

static void perf_free(struct perf_ev *ev) {
  if (ev->bpf_prog) {
    bpf_prog_put(ev->bpf_prog);
    ev->bpf_prog = 0;
  }
  if (ev->pmu_slot >= 0) {
    perf_pmu_slot_free(ev->pmu_slot);
    ev->pmu_slot = -1;
  }
  if (ev->rb_phys) {
    for (usize i = 0; i < ev->rb_pages; i++)
      pmm_free_frame(ev->rb_phys + (u64)i * PAGE_SIZE);
  }
  kfree(ev);
}

static void perf_unlink(struct perf_ev *ev) {
  struct perf_ev **pp = &g_events;

  while (*pp) {
    if (*pp == ev) {
      *pp = ev->next;
      if (g_nr_events)
        g_nr_events--;
      return;
    }
    pp = &(*pp)->next;
  }
}

static void perf_handle_release(struct vfs_handle *h) {
  struct perf_ev *ev = (struct perf_ev *)h->private_data;
  u64 flags;

  h->private_data = 0;
  if (ev) {
    spin_lock_irqsave(&g_perf_lock, &flags);
    perf_unlink(ev);
    spin_unlock_irqrestore(&g_perf_lock, flags);
    perf_free(ev);
  }
  if (h->node) {
    vfs_node_put(h->node);
    h->node = 0;
  }
}

/* read(2): the count, in whichever of the PERF_FORMAT_* shapes was asked for. */
static isize perf_handle_read(struct vfs_handle *h, char *buf, usize len) {
  struct perf_ev *ev = (struct perf_ev *)h->private_data;
  u64 vals[8];
  u32 n = 0;
  u64 flags;
  u64 now;

  if (!ev)
    return -EBADF;
  spin_lock_irqsave(&g_perf_lock, &flags);
  now = ktime_monotonic_ns();
  vals[n++] = perf_count_now(ev);
  if (ev->attr.read_format & PERF_FORMAT_TOTAL_TIME_ENABLED)
    vals[n++] = ev->time_enabled + (ev->enabled ? now - ev->since : 0);
  if (ev->attr.read_format & PERF_FORMAT_TOTAL_TIME_RUNNING)
    vals[n++] = ev->time_running + (ev->enabled ? now - ev->since : 0);
  if (ev->attr.read_format & PERF_FORMAT_ID)
    vals[n++] = ev->id;
  spin_unlock_irqrestore(&g_perf_lock, flags);

  if (len < n * sizeof(u64))
    return -ENOSPC;
  memcpy(buf, vals, n * sizeof(u64));
  return (isize)(n * sizeof(u64));
}

static int perf_handle_poll(struct vfs_handle *h, struct b1nix_pollfd *pfd) {
  struct perf_ev *ev = (struct perf_ev *)h->private_data;

  pfd->revents = 0;
  if (!ev)
    return -EBADF;
  if (ev->ctrl && perf_rb_used(ev) > 0)
    pfd->revents |= B1NIX_POLLIN;
  return 0;
}

static int perf_handle_ioctl(struct vfs_handle *h, u64 request, void *arg) {
  struct perf_ev *ev = (struct perf_ev *)h->private_data;
  u64 flags;
  int rc = 0;

  if (!ev)
    return -EBADF;
  spin_lock_irqsave(&g_perf_lock, &flags);
  switch (request) {
  case PERF_EVENT_IOC_ENABLE:
    perf_enable(ev);
    break;
  case PERF_EVENT_IOC_DISABLE:
    perf_disable(ev);
    break;
  case PERF_EVENT_IOC_RESET:
    ev->count = 0;
    ev->base = perf_raw(ev);
    ev->time_enabled = 0;
    ev->time_running = 0;
    ev->since = ktime_monotonic_ns();
    break;
  case PERF_EVENT_IOC_REFRESH: {
    i64 n = (i64)(isize)arg;

    if (n <= 0) {
      rc = -EINVAL;
      break;
    }
    ev->refresh = (ev->refresh > 0) ? ev->refresh + n : n;
    perf_enable(ev);
    break;
  }
  case PERF_EVENT_IOC_PERIOD: {
    u64 p = 0;

    spin_unlock_irqrestore(&g_perf_lock, flags);
    if (syscall_copyin(&p, arg, sizeof(p)) < 0)
      return -EFAULT;
    spin_lock_irqsave(&g_perf_lock, &flags);
    if (!p) {
      rc = -EINVAL;
      break;
    }
    if (ev->attr.freq) {
      ev->ticks_per_sample = SCHED_TICKS_PER_SEC / p ? SCHED_TICKS_PER_SEC / p : 1;
      ev->ticks_left = ev->ticks_per_sample;
    } else {
      ev->period_ns = p;
      ev->ns_left = p;
    }
    break;
  }
  case PERF_EVENT_IOC_SET_BPF: {
    /* The argument is a descriptor for a loaded program. Attaching takes a
     * reference so the program stays alive while samples can still run it,
     * even after the loader closes its own descriptor. */
    void *prog = bpf_prog_get((int)(usize)arg);

    if (!prog) {
      rc = -EBADF;
      break;
    }
    void *old = ev->bpf_prog;

    ev->bpf_prog = prog;
    if (old)
      bpf_prog_put(old);
    break;
  }
  case PERF_EVENT_IOC_ID: {
    u64 id = ev->id;

    spin_unlock_irqrestore(&g_perf_lock, flags);
    return syscall_copyout(arg, &id, sizeof(id)) == 0 ? 0 : -EFAULT;
  }
  default:
    /* PERF_EVENT_IOC_SET_OUTPUT would redirect this event's records into
     * another event's buffer, SET_FILTER is a tracepoint filter and SET_BPF
     * attaches a program: none of the three has anything to act on here. */
    rc = -ENOTTY;
    break;
  }
  spin_unlock_irqrestore(&g_perf_lock, flags);
  return rc;
}

static int perf_rb_alloc(struct perf_ev *ev, usize pages);

/* mmap(2) on the event. The ABI fixes the shape: page 0 is the control page
 * and the rest is the data ring, whose size must be a power of two of pages.
 * The buffer is allocated HERE, on the first mapping, because the length is
 * what says how big the program wants it — perf_event_open(2) never says.
 *
 * The region is physically contiguous, so this is the single-base callback
 * rather than io_uring's per-page one. */
static int perf_mmap_phys(struct vfs_handle *handle, u64 offset, usize length,
                          u64 *out_phys) {
  struct perf_ev *ev;
  int rc;

  if (!handle || !handle->private_data)
    return -EBADF;
  ev = (struct perf_ev *)handle->private_data;
  if (offset != 0 || (length & (PAGE_SIZE - 1)) || length == 0)
    return -EINVAL;
  rc = perf_rb_alloc(ev, length / PAGE_SIZE);
  if (rc < 0)
    return rc;
  *out_phys = ev->rb_phys;
  return 0;
}

static const struct vfs_file_ops perf_file_ops = {
    .read = perf_handle_read,
    .poll = perf_handle_poll,
    .ioctl = perf_handle_ioctl,
    .release = perf_handle_release,
};

/* ---- perf_event_open ---------------------------------------------------- */

/* Every sample_type bit whose field this really writes. A caller asking for
 * anything else is told so at open, where it can still choose differently. */
#define PERF_SAMPLE_SUPPORTED                                                  \
  (PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_ADDR |    \
   PERF_SAMPLE_ID | PERF_SAMPLE_STREAM_ID | PERF_SAMPLE_CPU |                  \
   PERF_SAMPLE_PERIOD | PERF_SAMPLE_CALLCHAIN | PERF_SAMPLE_IDENTIFIER)

#define PERF_READ_FORMAT_SUPPORTED                                             \
  (PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING |           \
   PERF_FORMAT_ID)

static isize perf_open(u64 uattr, i32 pid, i32 cpu, i32 group_fd, u64 flags) {
  struct perf_event_attr attr;
  struct perf_ev *ev;
  u32 size = 0;

  if (flags & ~(u64)(PERF_FLAG_FD_CLOEXEC))
    return -EINVAL;
  /* attr.size names the version of the structure the caller was built
   * against: read that many bytes and leave the rest zero. It is the second
   * u32 of the structure, after attr.type. */
  if (syscall_copyin(&size, (const void *)(usize)(uattr + 4), sizeof(size)) < 0)
    return -EFAULT;
  if (size < PERF_ATTR_SIZE_VER0)
    return -EINVAL;
  memset(&attr, 0, sizeof(attr));
  if (syscall_copyin(&attr, (const void *)(usize)uattr,
                     size > sizeof(attr) ? sizeof(attr) : size) < 0)
    return -EFAULT;

  if (!perf_attr_supported(&attr))
    return -EOPNOTSUPP;
  if (attr.sample_type & ~(u64)PERF_SAMPLE_SUPPORTED)
    return -EOPNOTSUPP;
  if (attr.read_format & ~(u64)PERF_READ_FORMAT_SUPPORTED)
    return -EOPNOTSUPP;
  /* A group read would have to report every member in one go; groups here are
   * only a scheduling relationship, so the format is refused rather than
   * answered with the leader's value alone. */
  if (attr.read_format & PERF_FORMAT_GROUP)
    return -EOPNOTSUPP;
  if (attr.inherit)
    /* Counting a task's children too needs the counter to follow fork; it does
     * not, and reporting the parent's count as if it covered the children
     * would be the wrong number rather than none. */
    return -EOPNOTSUPP;
  if (attr.precise_ip)
    return -EOPNOTSUPP; /* no PEBS-style hardware to make an IP exact */

  if (pid == -1 && cpu == -1)
    return -EINVAL; /* Linux refuses this pair */
  if (pid > 0 && !scheduler_task_by_pid((usize)pid))
    return -ESRCH;
  /* perf_event_paranoid: 2 lets an unprivileged caller profile only its own
   * tasks; anything machine-wide needs privilege. */
  {
    const struct cred *cred = scheduler_get_current_cred();
    int privileged = cred && cred_has_cap_effective(cred, CAP_SYS_ADMIN);

    if (pid == -1 && g_paranoid > 0 && !privileged)
      return -EACCES;
    if (pid > 0 && g_paranoid > 1 && !privileged) {
      struct task *t = scheduler_task_by_pid((usize)pid);

      if (!t || !current_task || task_tgid(t) != task_tgid(current_task))
        return -EACCES;
    }
  }

  if (group_fd >= 0) {
    struct vfs_handle *gh = scheduler_fd_get(group_fd);

    if (!gh || gh->ops != &perf_file_ops)
      return -EINVAL;
  }

  if (g_nr_events >= PERF_MAX_EVENTS)
    return -EMFILE;

  ev = kzalloc(sizeof(*ev));
  if (!ev)
    return -ENOMEM;
  ev->attr = attr;
  ev->cpu = cpu;
  ev->target = (pid == -1) ? 0 : (pid == 0 ? (current_task ? current_task->id : 0)
                                           : (usize)pid);
  ev->leader = ev;
  ev->refresh = -1;
  ev->pmu_slot = -1;
  if (attr.type == PERF_TYPE_HARDWARE || attr.type == PERF_TYPE_HW_CACHE ||
      attr.type == PERF_TYPE_RAW) {
    u64 evsel = 0;
    int rc = perf_pmu_map(&attr, &evsel);

    if (rc == 0) {
      /* exclude_user / exclude_kernel are privilege filters the counter
       * itself applies, so they are the one place where what is asked for is
       * exactly what the hardware is told. */
      rc = perf_pmu_slot_alloc(evsel, ev->target, !attr.exclude_user,
                               !attr.exclude_kernel);
    }
    if (rc < 0) {
      kfree(ev);
      return rc; /* -EOPNOTSUPP with no PMU, -EBUSY with no counter free */
    }
    ev->pmu_slot = rc;
  }
  ev->enabled = attr.disabled ? 0 : 1;
  ev->since = ktime_monotonic_ns();
  ev->base = perf_raw(ev);

  if (attr.freq) {
    u64 f = attr.sample_freq ? attr.sample_freq : 1;

    if (f > SCHED_TICKS_PER_SEC)
      f = SCHED_TICKS_PER_SEC; /* the timer IS the sampling clock: say so */
    ev->ticks_per_sample = SCHED_TICKS_PER_SEC / f;
    if (!ev->ticks_per_sample)
      ev->ticks_per_sample = 1;
    ev->ticks_left = ev->ticks_per_sample;
  } else if (attr.sample_period) {
    /* A period in counter units. For the two clock counters that is
     * nanoseconds, which the tick can charge directly. For an event counter
     * there is no overflow interrupt to hang it off, so it is refused rather
     * than approximated. */
    if (attr.config != PERF_COUNT_SW_CPU_CLOCK &&
        attr.config != PERF_COUNT_SW_TASK_CLOCK) {
      kfree(ev);
      return -EOPNOTSUPP;
    }
    ev->period_ns = attr.sample_period;
    ev->ns_left = attr.sample_period;
  }

  struct vfs_node *node = vfs_create_node(VFS_DEVICE);

  if (!node) {
    kfree(ev);
    return -ENOMEM;
  }
  strncpy(node->name, "perf_event", sizeof(node->name) - 1);
  node->name[sizeof(node->name) - 1] = 0;
  node->deleted = 1;
  node->inode->nlink = 0;
  node->inode->mode = 0600;
  {
    const struct cred *cred = scheduler_get_current_cred();

    node->inode->uid = cred ? cred->euid : ROOT_UID;
    node->inode->gid = cred ? cred->egid : ROOT_GID;
  }
  node->inode->mmap_handle_phys_cb = perf_mmap_phys;
  ev->node = node;

  struct vfs_handle *h = alloc_raw_handle(VFS_HANDLE_NODE);

  if (!h) {
    vfs_node_put(node);
    kfree(ev);
    return -ENFILE;
  }
  h->node = node;
  h->private_data = ev;
  h->ops = &perf_file_ops;
  h->flags = B1NIX_O_RDWR;

  {
    u64 lock_flags;

    spin_lock_irqsave(&g_perf_lock, &lock_flags);
    ev->id = g_next_id++;
    ev->next = g_events;
    g_events = ev;
    g_nr_events++;
    spin_unlock_irqrestore(&g_perf_lock, lock_flags);
  }

  int fd = scheduler_fd_alloc(h);

  if (fd < 0) {
    vfs_handle_release(h);
    return fd == -ENOMEM ? -ENOMEM : -EMFILE;
  }
  if (flags & PERF_FLAG_FD_CLOEXEC)
    scheduler_fd_flags_set(fd, B1NIX_FD_CLOEXEC);
  return fd;
}

/* One control page plus a power of two of data pages, as the ABI defines. */
static int perf_rb_alloc(struct perf_ev *ev, usize pages) {
  usize data_pages;
  u64 phys;

  if (ev->rb_phys)
    return (pages <= ev->rb_pages) ? 0 : -EINVAL;
  if (pages < 2)
    return -EINVAL;
  data_pages = pages - 1;
  if (data_pages & (data_pages - 1))
    return -EINVAL; /* the data area must be a power of two of pages */
  if (data_pages > PERF_MAX_DATA_PAGES)
    return -ENOMEM;

  phys = pmm_alloc_frames(pages);
  if (!phys)
    return -ENOMEM;
  memset(perf_kva(phys), 0, pages * PAGE_SIZE);
  ev->rb_phys = phys;
  ev->rb_pages = pages;
  ev->ctrl = (struct perf_event_mmap_page *)perf_kva(phys);
  ev->data = (char *)perf_kva(phys) + PAGE_SIZE;
  ev->data_size = (u64)data_pages * PAGE_SIZE;
  ev->head = 0;
  ev->ctrl->version = 0;
  ev->ctrl->compat_version = 0;
  ev->ctrl->data_offset = PAGE_SIZE;
  ev->ctrl->data_size = ev->data_size;
  /* No rdpmc and no user-space time conversion here: every capability bit
   * stays clear but the one that says bit 0 is deprecated, so `perf` reads the
   * counter with read(2) rather than an instruction that would return rubbish.
   */
  ev->ctrl->capabilities = 0;
  ev->ctrl->cap_bit0_is_deprecated = 1;

  /* Now that there is somewhere to put them, describe the process this event
   * watches: its name and its file-backed mappings, so an address in a sample
   * can be turned back into a function. */
  {
    u64 flags;
    struct task *t = ev->target ? scheduler_task_by_pid(ev->target)
                                : current_task;

    spin_lock_irqsave(&g_perf_lock, &flags);
    perf_emit_task_records(ev, t);
    spin_unlock_irqrestore(&g_perf_lock, flags);
  }
  return 0;
}

int perf_event_syscall(u64 nr, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4,
                       u64 *ret) {
  (void)a4;
  if (nr != PERF_NR_open)
    return 0;
  *ret = (u64)perf_open(a0, (i32)a1, (i32)a2, (i32)a3, a4);
  return 1;
}
