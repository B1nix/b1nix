/* coredump — ELF core dump generation on fatal signals (M35).
 *
 * When a user process dies on a fatal CPU-fault signal (SIGSEGV/SIGABRT/
 * SIGILL/SIGFPE/SIGBUS) with no handler, the exception handler calls
 * coredump_write() before terminating it. We emit a structurally valid
 * ET_CORE ELF to /tmp/core containing:
 *
 *   - a PT_NOTE segment with an NT_PRSTATUS note carrying the faulting
 *     register file (GDB reads $rip/$rsp/... from here);
 *   - one PT_LOAD segment per mapped run of the dying task's address space.
 *
 * Safety: this runs in exception context in the faulting task's address
 * space (its pml4 is live). We never read a user page blindly — every page is
 * first probed with vmm_virt_to_phys(), so a lazily-unmapped page can't fault
 * us a second time. /tmp is ramfs, so the writes never block on a device.
 */

#include <b1nix/arch_x86_64.h>
#include <b1nix/mm.h>
#include <b1nix/posix.h>
#include <b1nix/sched.h>
#include <b1nix/vfs.h>
#include <string.h>

#define ET_CORE 4
#define EM_X86_64 62
#define PT_LOAD 1
#define PT_NOTE 4
#define NT_PRSTATUS 1
#define PF_X 1
#define PF_W 2
#define PF_R 4

struct e64_ehdr {
  u8 e_ident[16];
  u16 e_type;
  u16 e_machine;
  u32 e_version;
  u64 e_entry;
  u64 e_phoff;
  u64 e_shoff;
  u32 e_flags;
  u16 e_ehsize;
  u16 e_phentsize;
  u16 e_phnum;
  u16 e_shentsize;
  u16 e_shnum;
  u16 e_shstrndx;
} __attribute__((packed));

struct e64_phdr {
  u32 p_type;
  u32 p_flags;
  u64 p_offset;
  u64 p_vaddr;
  u64 p_paddr;
  u64 p_filesz;
  u64 p_memsz;
  u64 p_align;
} __attribute__((packed));

struct e64_nhdr {
  u32 n_namesz;
  u32 n_descsz;
  u32 n_type;
} __attribute__((packed));

/* Bounds so a runaway address space can't produce an enormous dump. */
#define CORE_MAX_SEGS 32
#define CORE_MAX_BYTES (1024 * 1024)

struct core_seg {
  u64 vaddr;
  u64 len;
  u32 flags;
};

/* elf_prstatus: only pr_reg matters to a debugger; the surrounding fields are
 * zeroed. pr_reg sits at offset 112 and holds 27 u64 in user_regs_struct order
 * (r15,r14,r13,r12,rbp,rbx,r11,r10,r9,r8,rax,rcx,rdx,rsi,rdi,orig_rax,rip,cs,
 *  eflags,rsp,ss,fs_base,gs_base,ds,es,fs,gs). */
#define PRSTATUS_SIZE 336
#define PRSTATUS_REG_OFF 112

static void fill_prstatus(u8 *desc, struct interrupt_frame *f) {
  memset(desc, 0, PRSTATUS_SIZE);
  u64 *r = (u64 *)(desc + PRSTATUS_REG_OFF);
  r[0] = f->r15;       r[1] = f->r14;  r[2] = f->r13;  r[3] = f->r12;
  r[4] = f->rbp;       r[5] = f->rbx;  r[6] = f->r11;  r[7] = f->r10;
  r[8] = f->r9;        r[9] = f->r8;   r[10] = f->rax; r[11] = f->rcx;
  r[12] = f->rdx;      r[13] = f->rsi; r[14] = f->rdi;
  r[15] = f->rax;      /* orig_rax */
  r[16] = f->rip;      r[17] = f->cs;  r[18] = f->rflags;
  r[19] = f->rsp;      r[20] = f->ss;
  /* fs_base/gs_base/ds/es/fs/gs left zero. */
}

/* Append `len` bytes from `buf` to the open core fd. */
static int core_emit(int fd, const void *buf, usize len) {
  isize w = vfs_write(fd, (const char *)buf, len);
  return (w == (isize)len) ? 0 : -1;
}

/* Collect mapped page-runs from the task's VMAs into segs[]. Returns the run
 * count; *total receives the byte sum. */
static int collect_segs(struct task *t, struct core_seg *segs, u64 *total) {
  int n = 0;
  u64 sum = 0;
  for (struct vm_area *v = t->vma_list; v && n < CORE_MAX_SEGS; v = v->next) {
    u32 flags = ((v->prot & 0x1) ? PF_R : 0) | ((v->prot & 0x2) ? PF_W : 0) |
                ((v->prot & 0x4) ? PF_X : 0);
    u64 a = v->start & ~(u64)(PAGE_SIZE - 1);
    while (a < v->end && n < CORE_MAX_SEGS && sum < CORE_MAX_BYTES) {
      /* Extend a contiguous run of mapped pages. */
      if (vmm_virt_to_phys((void *)(usize)a) == 0) {
        a += PAGE_SIZE;
        continue;
      }
      u64 run_start = a;
      while (a < v->end && vmm_virt_to_phys((void *)(usize)a) != 0 &&
             sum + (a - run_start) < CORE_MAX_BYTES) {
        a += PAGE_SIZE;
      }
      u64 run_len = a - run_start;
      if (run_len == 0)
        break;
      segs[n].vaddr = run_start;
      segs[n].len = run_len;
      segs[n].flags = flags;
      sum += run_len;
      n++;
    }
  }
  *total = sum;
  return n;
}

void coredump_write(struct interrupt_frame *frame, int sig) {
  struct task *t = current_task;
  if (!t)
    return;
  (void)sig;

  struct core_seg segs[CORE_MAX_SEGS];
  u64 total = 0;
  int nseg = collect_segs(t, segs, &total);

  /* Note: NT_PRSTATUS with name "CORE". */
  u8 desc[PRSTATUS_SIZE];
  fill_prstatus(desc, frame);
  const char note_name[8] = "CORE\0\0\0"; /* 5 bytes padded to 8 */
  u32 namesz = 5;
  u32 name_pad = 8;            /* 5 rounded up to 4 → 8 incl. the term layout */
  u32 desc_pad = PRSTATUS_SIZE; /* already 4-aligned */
  usize note_size =
      sizeof(struct e64_nhdr) + name_pad + desc_pad;

  u16 phnum = (u16)(1 + nseg); /* PT_NOTE + one PT_LOAD per run */
  usize hdrs = sizeof(struct e64_ehdr) + (usize)phnum * sizeof(struct e64_phdr);
  usize note_off = hdrs;
  usize data_off = note_off + note_size;

  if (vfs_create("/tmp/core", 0600) < 0) {
    /* may already exist; truncate on open below */
  }
  int fd = vfs_open_flags("/tmp/core", B1NIX_O_WRONLY | B1NIX_O_TRUNC);
  if (fd < 0)
    return;

  /* ELF header */
  struct e64_ehdr eh;
  memset(&eh, 0, sizeof(eh));
  eh.e_ident[0] = 0x7f; eh.e_ident[1] = 'E'; eh.e_ident[2] = 'L';
  eh.e_ident[3] = 'F';
  eh.e_ident[4] = 2; /* ELFCLASS64 */
  eh.e_ident[5] = 1; /* ELFDATA2LSB */
  eh.e_ident[6] = 1; /* EV_CURRENT */
  eh.e_type = ET_CORE;
  eh.e_machine = EM_X86_64;
  eh.e_version = 1;
  eh.e_phoff = sizeof(struct e64_ehdr);
  eh.e_ehsize = sizeof(struct e64_ehdr);
  eh.e_phentsize = sizeof(struct e64_phdr);
  eh.e_phnum = phnum;
  if (core_emit(fd, &eh, sizeof(eh)) < 0)
    goto done;

  /* PT_NOTE program header */
  struct e64_phdr ph;
  memset(&ph, 0, sizeof(ph));
  ph.p_type = PT_NOTE;
  ph.p_offset = note_off;
  ph.p_filesz = note_size;
  ph.p_align = 4;
  if (core_emit(fd, &ph, sizeof(ph)) < 0)
    goto done;

  /* PT_LOAD program headers */
  {
    usize off = data_off;
    for (int i = 0; i < nseg; i++) {
      memset(&ph, 0, sizeof(ph));
      ph.p_type = PT_LOAD;
      ph.p_flags = segs[i].flags;
      ph.p_offset = off;
      ph.p_vaddr = segs[i].vaddr;
      ph.p_paddr = segs[i].vaddr;
      ph.p_filesz = segs[i].len;
      ph.p_memsz = segs[i].len;
      ph.p_align = PAGE_SIZE;
      if (core_emit(fd, &ph, sizeof(ph)) < 0)
        goto done;
      off += segs[i].len;
    }
  }

  /* PT_NOTE contents: nhdr + name + desc */
  {
    struct e64_nhdr nh;
    nh.n_namesz = namesz;
    nh.n_descsz = PRSTATUS_SIZE;
    nh.n_type = NT_PRSTATUS;
    if (core_emit(fd, &nh, sizeof(nh)) < 0)
      goto done;
    if (core_emit(fd, note_name, name_pad) < 0)
      goto done;
    if (core_emit(fd, desc, PRSTATUS_SIZE) < 0)
      goto done;
  }

  /* PT_LOAD contents: copy each mapped run straight from the live user pages
   * (page-by-page so a run that straddles a TLB boundary stays safe). */
  for (int i = 0; i < nseg; i++) {
    u64 a = segs[i].vaddr;
    u64 endv = a + segs[i].len;
    while (a < endv) {
      if (vmm_virt_to_phys((void *)(usize)a) == 0)
        break; /* should not happen — collected as mapped */
      if (core_emit(fd, (const void *)(usize)a, PAGE_SIZE) < 0)
        goto done;
      a += PAGE_SIZE;
    }
  }

done:
  vfs_close(fd);
}
