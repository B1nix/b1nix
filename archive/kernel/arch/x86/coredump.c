#include <b1nix/arch_x86.h>
#include <b1nix/mm.h>
#include <b1nix/posix.h>
#include <b1nix/sched.h>
#include <b1nix/vfs.h>
#include <string.h>

#define ET_CORE 4
#define EM_386 3
#define PT_LOAD 1
#define PT_NOTE 4
#define NT_PRSTATUS 1
#define PF_X 1
#define PF_W 2
#define PF_R 4

struct e32_ehdr {
  u8 e_ident[16];
  u16 e_type;
  u16 e_machine;
  u32 e_version;
  u32 e_entry;
  u32 e_phoff;
  u32 e_shoff;
  u32 e_flags;
  u16 e_ehsize;
  u16 e_phentsize;
  u16 e_phnum;
  u16 e_shentsize;
  u16 e_shnum;
  u16 e_shstrndx;
} __attribute__((packed));

struct e32_phdr {
  u32 p_type;
  u32 p_offset;
  u32 p_vaddr;
  u32 p_paddr;
  u32 p_filesz;
  u32 p_memsz;
  u32 p_flags;
  u32 p_align;
} __attribute__((packed));

struct e32_nhdr {
  u32 n_namesz;
  u32 n_descsz;
  u32 n_type;
} __attribute__((packed));

#define CORE_MAX_SEGS 32
#define CORE_MAX_BYTES (1024 * 1024)

struct core_seg {
  u32 vaddr;
  u32 len;
  u32 flags;
};

#define PRSTATUS_SIZE 144
#define PRSTATUS_REG_OFF 72

static void fill_prstatus(u8 *desc, struct interrupt_frame *f) {
  memset(desc, 0, PRSTATUS_SIZE);
  u32 *r = (u32 *)(desc + PRSTATUS_REG_OFF);
  r[0] = f->ebx;       r[1] = f->ecx;  r[2] = f->edx;  r[3] = f->esi;
  r[4] = f->edi;       r[5] = f->ebp;  r[6] = f->eax;
  r[7] = 0x23;         r[8] = 0x23;    r[9] = 0x23;    r[10] = 0x33; /* ds, es, fs, gs */
  r[11] = f->eax;      /* orig_eax */
  r[12] = f->eip;      r[13] = f->cs;  r[14] = f->eflags;
  r[15] = f->esp;      r[16] = f->ss;
}

static int core_emit(int fd, const void *buf, usize len) {
  isize w = vfs_write(fd, (const char *)buf, len);
  return (w == (isize)len) ? 0 : -1;
}

static int collect_segs(struct task *t, struct core_seg *segs, u32 *total) {
  int n = 0;
  u32 sum = 0;
  for (struct vm_area *v = t->vma_list; v && n < CORE_MAX_SEGS; v = v->next) {
    u32 flags = ((v->prot & 0x1) ? PF_R : 0) | ((v->prot & 0x2) ? PF_W : 0) |
                ((v->prot & 0x4) ? PF_X : 0);
    u32 a = (u32)(v->start & ~(PAGE_SIZE - 1));
    while (a < v->end && n < CORE_MAX_SEGS && sum < CORE_MAX_BYTES) {
      if (vmm_virt_to_phys((void *)(usize)a) == 0) {
        a += PAGE_SIZE;
        continue;
      }
      u32 run_start = a;
      while (a < v->end && vmm_virt_to_phys((void *)(usize)a) != 0 &&
             sum + (a - run_start) < CORE_MAX_BYTES) {
        a += PAGE_SIZE;
      }
      u32 run_len = a - run_start;
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
  u32 total = 0;
  int nseg = collect_segs(t, segs, &total);

  u8 desc[PRSTATUS_SIZE];
  fill_prstatus(desc, frame);
  const char note_name[8] = "CORE\0\0\0";
  u32 namesz = 5;
  u32 name_pad = 8;
  u32 desc_pad = PRSTATUS_SIZE;
  usize note_size = sizeof(struct e32_nhdr) + name_pad + desc_pad;

  u16 phnum = (u16)(1 + nseg);
  usize hdrs = sizeof(struct e32_ehdr) + (usize)phnum * sizeof(struct e32_phdr);
  usize note_off = hdrs;
  usize data_off = note_off + note_size;

  if (vfs_create("/tmp/core", 0600) < 0) {}
  int fd = vfs_open_flags("/tmp/core", B1NIX_O_WRONLY | B1NIX_O_TRUNC);
  if (fd < 0)
    return;

  struct e32_ehdr eh;
  memset(&eh, 0, sizeof(eh));
  eh.e_ident[0] = 0x7f; eh.e_ident[1] = 'E'; eh.e_ident[2] = 'L';
  eh.e_ident[3] = 'F';
  eh.e_ident[4] = 1; /* ELFCLASS32 */
  eh.e_ident[5] = 1; /* ELFDATA2LSB */
  eh.e_ident[6] = 1; /* EV_CURRENT */
  eh.e_type = ET_CORE;
  eh.e_machine = EM_386;
  eh.e_version = 1;
  eh.e_phoff = sizeof(struct e32_ehdr);
  eh.e_ehsize = sizeof(struct e32_ehdr);
  eh.e_phentsize = sizeof(struct e32_phdr);
  eh.e_phnum = phnum;
  if (core_emit(fd, &eh, sizeof(eh)) < 0)
    goto done;

  struct e32_phdr ph;
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
    struct e32_nhdr nh;
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
    u32 a = segs[i].vaddr;
    u32 endv = a + segs[i].len;
    while (a < endv) {
      if (vmm_virt_to_phys((void *)(usize)a) == 0)
        break;
      if (core_emit(fd, (const void *)(usize)a, PAGE_SIZE) < 0)
        goto done;
      a += PAGE_SIZE;
    }
  }

done:
  vfs_close(fd);
}
