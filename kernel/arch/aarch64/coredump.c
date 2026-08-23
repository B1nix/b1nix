/* coredump — ELF core dump generation on fatal signals (M35), aarch64.
 *
 * Mirrors kernel/arch/x86_64/coredump.c: the same ET_CORE layout (one PT_NOTE
 * carrying NT_PRSTATUS, one PT_LOAD per mapped run of the dying task's address
 * space) and the same safety rules — every page is probed with
 * vmm_virt_to_phys() before it is read, and /tmp is ramfs so the writes never
 * block on a device. Only the register note and e_machine are arch-specific:
 * aarch64's pr_reg is `struct user_pt_regs` (x0..x30, sp, pc, pstate).
 */

#include <b1nix/arch_aarch64.h>
#include <b1nix/mm.h>
#include <b1nix/posix.h>
#include <b1nix/resource_caps.h>
#include <b1nix/sched.h>
#include <b1nix/vfs.h>
#include <string.h>

#define ET_CORE 4
#define EM_AARCH64 183
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

#define CORE_MAX_SEGS 32

struct core_seg {
  u64 vaddr;
  u64 len;
  u32 flags;
};

/* elf_prstatus is the same LP64 shape both ports use — pr_reg starts at 112;
 * only its size differs. aarch64's is `struct user_pt_regs`: 31 general
 * registers, then sp, pc and pstate (34 * 8 = 272 bytes), followed by
 * pr_fpvalid and its padding. */
#define PRSTATUS_REG_OFF 112
#define PRSTATUS_SIZE (PRSTATUS_REG_OFF + 34 * 8 + 8)

static void fill_prstatus(u8 *desc, struct interrupt_frame *f) {
  memset(desc, 0, PRSTATUS_SIZE);
  u64 *r = (u64 *)(desc + PRSTATUS_REG_OFF);
  /* x0..x30 are consecutive in the frame; copy them without taking the address
   * of a packed member (which the compiler rightly warns may be unaligned). */
  memcpy(r, f, 31 * sizeof(u64));
  r[31] = f->sp_el0;
  r[32] = f->elr;
  r[33] = f->spsr;
}

static int core_emit(int fd, const void *buf, usize len) {
  isize w = vfs_write(fd, (const char *)buf, len);
  return (w == (isize)len) ? 0 : -1;
}

/* Collect mapped page-runs from the task's VMAs into segs[]. Returns the run
 * count; *total receives the byte sum, capped at max_bytes. */
static int collect_segs(struct task *t, struct core_seg *segs, u64 *total,
                        u64 max_bytes) {
  int n = 0;
  u64 sum = 0;
  for (struct vm_area *v = t->vma_list; v && n < CORE_MAX_SEGS; v = v->next) {
    u32 flags = ((v->prot & 0x1) ? PF_R : 0) | ((v->prot & 0x2) ? PF_W : 0) |
                ((v->prot & 0x4) ? PF_X : 0);
    u64 a = v->start & ~(u64)(PAGE_SIZE - 1);
    while (a < v->end && n < CORE_MAX_SEGS && sum < max_bytes) {
      if (paging_user_frame(t->pml4_phys, a) == 0) {
        a += PAGE_SIZE;
        continue;
      }
      u64 run_start = a;
      while (a < v->end && paging_user_frame(t->pml4_phys, a) != 0 &&
             sum + (a - run_start) < max_bytes) {
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

  u64 core_max = g_resource_caps.coredump_max_bytes;
  struct rlimit rl = {0, 0};
  if (scheduler_getrlimit(RLIMIT_CORE, &rl) == 0 && rl.rlim_cur < core_max)
    core_max = rl.rlim_cur;
  if (core_max == 0)
    return;

  struct core_seg segs[CORE_MAX_SEGS];
  u64 total = 0;
  int nseg = collect_segs(t, segs, &total, core_max);

  u8 desc[PRSTATUS_SIZE];
  fill_prstatus(desc, frame);
  const char note_name[8] = "CORE\0\0\0";
  u32 namesz = 5;
  u32 name_pad = 8;
  usize note_size = sizeof(struct e64_nhdr) + name_pad + PRSTATUS_SIZE;

  u16 phnum = (u16)(1 + nseg);
  usize hdrs = sizeof(struct e64_ehdr) + (usize)phnum * sizeof(struct e64_phdr);
  usize note_off = hdrs;
  usize data_off = note_off + note_size;

  if (vfs_create("/tmp/core", 0600) < 0) {
    /* may already exist; the open below truncates */
  }
  int fd = vfs_open_flags("/tmp/core", B1NIX_O_WRONLY | B1NIX_O_TRUNC);
  if (fd < 0)
    return;

  struct e64_ehdr eh;
  memset(&eh, 0, sizeof(eh));
  eh.e_ident[0] = 0x7f; eh.e_ident[1] = 'E'; eh.e_ident[2] = 'L';
  eh.e_ident[3] = 'F';
  eh.e_ident[4] = 2; /* ELFCLASS64 */
  eh.e_ident[5] = 1; /* ELFDATA2LSB */
  eh.e_ident[6] = 1; /* EV_CURRENT */
  eh.e_type = ET_CORE;
  eh.e_machine = EM_AARCH64;
  eh.e_version = 1;
  eh.e_phoff = sizeof(struct e64_ehdr);
  eh.e_ehsize = sizeof(struct e64_ehdr);
  eh.e_phentsize = sizeof(struct e64_phdr);
  eh.e_phnum = phnum;
  if (core_emit(fd, &eh, sizeof(eh)) < 0)
    goto done;

  struct e64_phdr ph;
  memset(&ph, 0, sizeof(ph));
  ph.p_type = PT_NOTE;
  ph.p_offset = note_off;
  ph.p_filesz = note_size;
  ph.p_align = 4;
  if (core_emit(fd, &ph, sizeof(ph)) < 0)
    goto done;

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

  for (int i = 0; i < nseg; i++) {
    u64 a = segs[i].vaddr;
    u64 endv = a + segs[i].len;
    while (a < endv) {
      if (paging_user_frame(t->pml4_phys, a) == 0)
        break; /* should not happen — collected as mapped */
      if (core_emit(fd, (const void *)(usize)a, PAGE_SIZE) < 0)
        goto done;
      a += PAGE_SIZE;
    }
  }

done:
  vfs_close(fd);
}
