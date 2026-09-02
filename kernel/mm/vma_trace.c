/*
 * The address-space edit ring.
 *
 * Shared, not per-arch: it records mmap/munmap/mremap ranges and the thread
 * that made them, and none of that is architecture-specific. It lived in
 * kernel/arch/x86_64/paging.c only because that is where the first caller was,
 * which left kernel/arch/aarch64/arch.c carrying an empty vma_trace_record()
 * stub -- so on that arch the shared callers in kernel/syscall/syscall.c
 * recorded nothing and the dumps below could never say anything. One copy,
 * both arches.
 */
#include <b1nix/arch.h>
#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/mm.h>
#include <b1nix/sched.h>
#include <b1nix/types.h>

/* A ring of the last address-space edits, for attributing leftovers.
 *
 * When a freshly allocated destination turns out to hold pages, the only
 * question that matters is which earlier mapping left them — and by then every
 * trace of that mapping is gone. Recording each mmap/munmap/move with its range
 * and the thread that made it costs four stores on paths that already walk page
 * tables, and turns "somebody left pages here" into a named sequence of calls.
 *
 * Deliberately lock-free: the index is bumped atomically and a torn entry would
 * at worst misreport one line of a diagnostic. A lock here would sit on the
 * munmap path of every process for the sake of a message almost never printed.
 */
#define VMA_TRACE_N 8192
struct vma_trace_ent {
  u64 start, end;
  u64 seq;
  u64 space;  /* the PML4 frame: which address space these addresses are in */
  u32 pid;
  u32 task;   /* which thread of it: the task slot, not the shared pid */
  const char *what;
};
static struct vma_trace_ent g_vma_trace[VMA_TRACE_N];
static u64 g_vma_trace_seq;

/* Page installs are recorded too, when asked for: they are the only edits that
 * happen with no mapping call behind them, so a leftover with no munmap to
 * blame is either theirs or nobody's. Off by default — a fault is frequent
 * enough that four stores per fault is a real cost, and the flag is read once. */
static int vma_trace_faults = -1;

int vma_trace_faults_enabled(void) {
  /* Do not cache a "no" answer taken before the command line was parsed: the
   * first user page is mapped early enough that an eager read of the flag
   * settles on 0 for the whole boot, and the tracing then never happens no
   * matter what the run asked for. Cache only the positive. */
  if (vma_trace_faults < 0) {
    const struct boot_info *bi = bootinfo_get();

    if (!bi || !bi->command_line[0])
      return 0;
    vma_trace_faults = bootinfo_has_flag("b1nix.vma-trace") ? 1 : 0;
    /* Say so once. A probe that silently never armed looks exactly like a
     * probe that armed and found nothing, and the two have opposite meanings. */
    console_write(vma_trace_faults ? "vma-trace: armed\n"
                                   : "vma-trace: not requested\n");
  }
  return vma_trace_faults;
}

void vma_trace_record(const char *what, u64 start, u64 end) {
  u64 seq = __atomic_add_fetch(&g_vma_trace_seq, 1, __ATOMIC_RELAXED);
  struct vma_trace_ent *e = &g_vma_trace[seq % VMA_TRACE_N];

  e->start = start;
  e->end = end;
  e->space = current_task ? current_task->pml4_phys : 0;
  e->pid = current_task ? (u32)current_task->id : 0;
  e->task = (u32)(((u64)(usize)current_task >> 6) & 0xffff);
  e->what = what;
  __atomic_store_n(&e->seq, seq, __ATOMIC_RELEASE);
}

/* The newest recorded edit covering one address, however far back it is.
 *
 * The range listing is capped, and the entry that explains a leftover page can
 * be older than the cap — this answers "what was the last mapping call that
 * covered THIS page" with no window at all. */
void vma_trace_dump_addr(u64 va) {
  u64 space = current_task ? current_task->pml4_phys : 0;
  u64 now = __atomic_load_n(&g_vma_trace_seq, __ATOMIC_ACQUIRE);
  u64 first = now > VMA_TRACE_N ? now - VMA_TRACE_N : 1;

  for (u64 i = now; i >= first; i--) {
    struct vma_trace_ent *e = &g_vma_trace[i % VMA_TRACE_N];

    if (__atomic_load_n(&e->seq, __ATOMIC_ACQUIRE) != i || e->space != space)
      continue;
    if (va < e->start || va >= e->end)
      continue;
    console_write("  last edit covering 0x");
    console_write_hex64(va);
    console_write(": #");
    console_write_dec(e->seq);
    console_write(" tid ");
    console_write_dec(e->pid);
    console_write(" ");
    console_write(e->what);
    console_write(" 0x");
    console_write_hex64(e->start);
    console_write("-0x");
    console_write_hex64(e->end);
    console_write("\n");
    return;
  }
  console_write("  no recorded edit ever covered 0x");
  console_write_hex64(va);
  console_write("\n");
}

/* Every recorded edit that touched this range, oldest first. */
void vma_trace_dump(u64 start, u64 end) {
  /* Only this address space's edits. The ring is global, and an address in one
   * process says nothing about the same address in another — mixing them in
   * makes an unrelated process's mapping look like an explanation. */
  u64 space = current_task ? current_task->pml4_phys : 0;
  u64 now = __atomic_load_n(&g_vma_trace_seq, __ATOMIC_ACQUIRE);
  u64 first = now > VMA_TRACE_N ? now - VMA_TRACE_N : 1;
  unsigned shown = 0;

  /* Newest first: the edits that explain a leftover are the ones just before
   * it, and an oldest-first listing spends its whole budget on history. */
  console_write("  edits touching this range (newest first):\n");
  for (u64 i = now; i >= first && shown < 16; i--) {
    struct vma_trace_ent *e = &g_vma_trace[i % VMA_TRACE_N];

    if (__atomic_load_n(&e->seq, __ATOMIC_ACQUIRE) != i)
      continue;
    if (e->start >= end || e->end <= start)
      continue;
    if (e->space != space)
      continue;
    shown++;
    console_write("    #");
    console_write_dec(e->seq);
    console_write(" pid ");
    console_write_dec(e->pid);
    console_write(" thread ");
    console_write_dec(e->task);
    console_write(" ");
    console_write(e->what);
    console_write(" 0x");
    console_write_hex64(e->start);
    console_write("-0x");
    console_write_hex64(e->end);
    console_write("\n");
  }
  if (!shown)
    console_write("    (none recorded)\n");

  /* And the last few edits of any range, so an empty list above can be read as
   * "nothing touched this range" rather than "nothing is being recorded". */
  console_write("  most recent edits of this space, any range:\n");
  shown = 0;
  for (u64 i = now; i >= first && shown < 6; i--) {
    struct vma_trace_ent *e = &g_vma_trace[i % VMA_TRACE_N];

    if (__atomic_load_n(&e->seq, __ATOMIC_ACQUIRE) != i || e->space != space)
      continue;
    shown++;
    console_write("    #");
    console_write_dec(e->seq);
    console_write(" tid ");
    console_write_dec(e->pid);
    console_write(" ");
    console_write(e->what);
    console_write(" 0x");
    console_write_hex64(e->start);
    console_write("\n");
  }
}