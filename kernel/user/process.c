#include <b1nix/arch.h>
#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/mm.h>
#include <b1nix/panic.h>
#include <b1nix/sched.h>
#include <b1nix/syscall.h>
#include <b1nix/user.h>
#include <b1nix/vfs.h>
#include <string.h>

extern void x86_user_jump(usize entry, usize stack, usize argc, usize argv);
extern void arch_fpu_init_current(void); /* reset FPU/MXCSR to ABI default */

/* Built-in program registry (C2 audit): grows on demand from the kheap
 * starting at PROGRAMS_INITIAL slots and doubling each time. There is no
 * hard ceiling — the only limit is kheap exhaustion, signalled by
 * user_register_program emitting the existing warning. */
#define PROGRAMS_INITIAL 16
#define ELF_MAGIC0 0x7f
#define ELF_MAGIC1 'E'
#define ELF_MAGIC2 'L'
#define ELF_MAGIC3 'F'
#define ELF_CLASS_64 2
#define ELF_DATA_LE 1
#define ELF_TYPE_EXEC 2
#define ELF_TYPE_DYN 3
#define ELF_MACHINE_X86_64 0x3e
#define ELF_MACHINE_AARCH64 0xb7
#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_INTERP  3

/* M30 — ELF64 dynamic-tag identifiers (subset honoured by the in-kernel
 * loader). Standard `Elf64_Dyn` tag values. */
#define DT_NULL    0
#define DT_PLTRELSZ 2
#define DT_RELA    7
#define DT_RELASZ  8
#define DT_RELAENT 9
#define DT_JMPREL  23
#define DT_PLTREL  20

/* M30 — x86-64 ELF64 relocation types (subset). */
#define R_X86_64_NONE     0
#define R_X86_64_64       1
#define R_X86_64_GLOB_DAT 6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE 8

struct elf64_rela {
  u64 r_offset;
  u64 r_info;
  i64 r_addend;
} __attribute__((packed));

struct elf64_dyn {
  i64 d_tag;
  u64 d_val;
} __attribute__((packed));

/* M30 — base address at which PIE/ET_DYN images get loaded. Picked well
 * above the standard 0x400000 ET_EXEC load base and below the user
 * stack top (0x800000000000) so a PIE binary and the existing static
 * binaries don't collide. */
#ifdef __x86_64__
#define PIE_LOAD_BASE 0x0000500000000000ULL
#else
#define PIE_LOAD_BASE 0x40000000ULL
#endif

struct process_start {
  struct user_loaded_image *image;
};

struct elf64_ehdr {
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

struct elf64_phdr {
  u32 p_type;
  u32 p_flags;
  u64 p_offset;
  u64 p_vaddr;
  u64 p_paddr;
  u64 p_filesz;
  u64 p_memsz;
  u64 p_align;
} __attribute__((packed));

static struct user_program *programs = 0;
static usize program_count = 0;
static usize programs_capacity = 0;
static const u64 USER_STACK_MAX_SIZE = 8ULL * 1024ULL * 1024ULL;

static struct user_address_space user_address_space_create(void) {
  struct user_address_space address_space;

  address_space.pml4_frame = 0;
  address_space.stack_base = 0;
  address_space.stack_size = USER_STACK_SIZE;
  address_space.stack_top = USER_STACK_TOP;
  address_space.stack_image_size = USER_STACK_SIZE;
  address_space.stack_image = kzalloc(USER_STACK_SIZE);

  return address_space;
}

static char *kernel_strdup(const char *src) {
  if (!src)
    return 0;
  usize len = strlen(src);
  char *copy = kmalloc(len + 1);
  if (!copy)
    return 0;
  memcpy(copy, src, len + 1);
  return copy;
}

static int copy_string_vector(const char **src, int max_count,
                              const char ***out, int *out_count,
                              int source_is_user) {
  const char **copy = kzalloc(sizeof(char *) * (max_count + 1));
  int count = 0;
  char tmp[1024];

  if (!copy) {
    console_write("copy_string_vector: kzalloc failed\n");
    return -1;
  }
  if (src) {
    for (; count < max_count; count++) {
      const char *ptr = 0;
      if (source_is_user) {
        if (syscall_copyin(&ptr, src + count, sizeof(ptr)) != 0) {
          console_write("copy_string_vector: syscall_copyin src failed at ");
          console_write_dec(count);
          console_write("\n");
          return -1;
        }
      } else {
        ptr = src[count];
      }

      if (!ptr)
        break;

      if (source_is_user) {
        if (syscall_copyinstr(tmp, sizeof(tmp), ptr) != 0) {
          console_write("copy_string_vector: syscall_copyinstr ptr failed at ");
          console_write_dec(count);
          console_write("\n");
          return -1;
        }
      } else {
        strncpy(tmp, ptr, sizeof(tmp));
        tmp[sizeof(tmp) - 1] = '\0';
      }

      copy[count] = kernel_strdup(tmp);
      if (!copy[count]) {
        console_write("copy_string_vector: kernel_strdup failed at ");
        console_write_dec(count);
        console_write("\n");
        return -1;
      }
    }
  }
  copy[count] = 0;
  *out = copy;
  if (out_count)
    *out_count = count;
  return 0;
}

static int user_stack_push_usize(char *stack, usize *sp, usize value) {
  if (*sp < sizeof(usize))
    return -1;
  *sp -= sizeof(usize);
  memcpy(stack + *sp, &value, sizeof(value));
  return 0;
}

static usize user_stack_push_string(char *stack, usize *sp, const char *text) {
  usize len = strlen(text) + 1;
  if (*sp < len)
    return 0; // Return 0 to indicate error (0 is never a valid user pointer for strings)
  *sp -= len;
  memcpy(stack + *sp, text, len);
  return USER_STACK_TOP - USER_STACK_SIZE + *sp;
}

static int user_build_initial_stack(struct user_loaded_image *image) {
  char *stack = image->address_space.stack_image;
  usize argv_ptrs[USER_MAX_ARGS];
  usize envp_ptrs[USER_MAX_ENVS];
  usize sp = USER_STACK_SIZE;

  if (!stack)
    return -1;

  for (int i = image->argc - 1; i >= 0; i--) {
    argv_ptrs[i] = user_stack_push_string(stack, &sp, image->argv[i]);
    if (argv_ptrs[i] == 0) return -1;
  }

  int envc = 0;
  if (image->envp) {
    for (; envc < USER_MAX_ENVS && image->envp[envc]; envc++)
      ;
  }
  for (int i = envc - 1; i >= 0; i--) {
    envp_ptrs[i] = user_stack_push_string(stack, &sp, image->envp[i]);
    if (envp_ptrs[i] == 0) return -1;
  }

  sp &= ~(usize)0xf;

  /* FIX: Ensure the final stack pointer will be 16-byte aligned.
   * We push: argc(1), argv(argc), NULL(1), envp(envc), NULL(1), auxv(5).
   * Total slots = 1 + image->argc + 1 + envc + 1 + 5 = image->argc + envc + 8.
   * If total_slots * sizeof(usize) is not a multiple of 16, we need padding.
   */
  usize total_slots = 8 + (usize)image->argc + (usize)envc;
  if ((total_slots * sizeof(usize)) % 16 != 0) {
    if (user_stack_push_usize(stack, &sp, 0) < 0) return -1;
  }

  if (user_stack_push_usize(stack, &sp, 0) < 0) return -1; /* AT_NULL */
  if (user_stack_push_usize(stack, &sp, 9) < 0) return -1; /* AT_ENTRY */
  if (user_stack_push_usize(stack, &sp, (usize)image->entry) < 0) return -1;
  if (user_stack_push_usize(stack, &sp, 3) < 0) return -1; /* AT_PHDR */
  if (user_stack_push_usize(stack, &sp, 0) < 0) return -1;

  if (user_stack_push_usize(stack, &sp, 0) < 0) return -1;
  for (int i = envc - 1; i >= 0; i--) {
    if (user_stack_push_usize(stack, &sp, envp_ptrs[i]) < 0) return -1;
  }

  if (user_stack_push_usize(stack, &sp, 0) < 0) return -1;
  for (int i = image->argc - 1; i >= 0; i--) {
    if (user_stack_push_usize(stack, &sp, argv_ptrs[i]) < 0) return -1;
  }
  if (user_stack_push_usize(stack, &sp, (usize)image->argc) < 0) return -1;

  image->address_space.stack_base = USER_STACK_TOP - USER_STACK_SIZE + sp;
  image->address_space.stack_size = USER_STACK_TOP - image->address_space.stack_base;
  return 0;
}

static int user_image_read_vfs_file(const char *path, char **out_data,
                                    usize *out_size) {
  struct vfs_node *node = vfs_find_node(path);
  if (!node || IS_ERR(node)) {
    return -1;
  }
  if (node->inode->type != VFS_FILE || node->inode->size == 0) {
    vfs_node_put(node);
    return -1;
  }

  usize file_size = node->inode->size;
  vfs_node_put(node);

  int fd = vfs_open(path);
  if (fd < 0) {
    return -1;
  }

  char *data = kmalloc(file_size);
  if (!data) {
    vfs_close(fd);
    return -1;
  }

  usize total_read = 0;
  while (total_read < file_size) {
    isize got = vfs_read(fd, data + total_read, file_size - total_read);
    if (got < 0) {
      if (got == -EAGAIN || got == -EWOULDBLOCK) {
        scheduler_yield();
        continue;
      }
      vfs_close(fd);
      kfree(data);
      return -1;
    }
    if (got == 0)
      break; /* EOF */
    total_read += (usize)got;
  }
  vfs_close(fd);

  if (total_read != file_size) {
    kfree(data);
    return -1;
  }

  *out_data = data;
  *out_size = total_read;
  return 0;
}

static int user_load_elf64(struct user_loaded_image *image, const char *path) {
  char *file_data = 0;
  usize file_size = 0;
  if (user_image_read_vfs_file(path, &file_data, &file_size) != 0)
    return -1;
  if (file_size < sizeof(struct elf64_ehdr)) {
    kfree(file_data);
    return -1;
  }

  struct elf64_ehdr *ehdr = (struct elf64_ehdr *)file_data;
  if (ehdr->e_ident[0] != ELF_MAGIC0 || ehdr->e_ident[1] != ELF_MAGIC1 ||
      ehdr->e_ident[2] != ELF_MAGIC2 || ehdr->e_ident[3] != ELF_MAGIC3) {
    kfree(file_data);
    return -1;
  }
  if (ehdr->e_ident[4] != ELF_CLASS_64 || ehdr->e_ident[5] != ELF_DATA_LE) {
    kfree(file_data);
    return -1;
  }
  if (ehdr->e_type != ELF_TYPE_EXEC && ehdr->e_type != ELF_TYPE_DYN) {
    kfree(file_data);
    return -1;
  }
  if (ehdr->e_machine != ELF_MACHINE_X86_64 &&
      ehdr->e_machine != ELF_MACHINE_AARCH64) {
    kfree(file_data);
    return -1;
  }
  if (ehdr->e_phoff + ((u64)ehdr->e_phentsize * ehdr->e_phnum) > file_size) {
    kfree(file_data);
    return -1;
  }

  image->kind = USER_IMAGE_ELF64;
  image->path = kernel_strdup(path);

  /* M30: PIE / ET_DYN support. For ET_DYN the segment vaddrs are 0-based
   * and the loader gets to choose where to place the image. We use a
   * fixed base (PIE_LOAD_BASE) — above the standard ET_EXEC load
   * address (0x400000) so a PIE binary can coexist with statically-
   * linked ones in the same userspace map. After segment loading we
   * also walk PT_DYNAMIC and apply R_X86_64_RELATIVE relocations so
   * absolute pointers in the binary (e.g. into .rodata or function
   * tables) point at the relocated base. */
  u64 load_base = (ehdr->e_type == ELF_TYPE_DYN) ? PIE_LOAD_BASE : 0;

  image->entry = ehdr->e_entry + load_base;
  console_write("ELF load: ");
  console_write(path);
  console_write(" entry=0x");
  console_write_hex64(image->entry);
  if (load_base) {
    console_write(" (PIE base=0x");
    console_write_hex64(load_base);
    console_write(")");
  }
  console_write("\n");
  image->address_space = user_address_space_create();

  /* PT_INTERP: log which interpreter the binary asks for. With PIE +
   * RELATIVE relocations the kernel does the relocation work itself; a
   * proper userspace dynamic linker is still future work, so binaries
   * that have undefined external symbols won't run. We accept the
   * PT_INTERP segment as informational rather than rejecting outright. */
  for (u16 j = 0; j < ehdr->e_phnum; j++) {
    struct elf64_phdr *p =
        (struct elf64_phdr *)(file_data + ehdr->e_phoff +
                              ((u64)j * ehdr->e_phentsize));
    if (p->p_type != PT_INTERP) continue;
    if (p->p_offset + p->p_filesz > file_size) continue;
    char interp[64];
    usize ilen = p->p_filesz < sizeof(interp) ? p->p_filesz
                                              : sizeof(interp) - 1;
    memcpy(interp, file_data + p->p_offset, ilen);
    interp[ilen] = '\0';
    console_write("ELF load: PT_INTERP=");
    console_write(interp);
    console_write(" (b1nix applies RELATIVE relocs in-kernel — no separate ld.so handoff)\n");
  }

  /* First pass: PT_LOAD segments. Vaddrs are offset by `load_base` for
   * PIE; this also offsets any later relocation targets we compute. */
  for (u16 i = 0; i < ehdr->e_phnum; i++) {
    struct elf64_phdr *phdr =
        (struct elf64_phdr *)(file_data + ehdr->e_phoff +
                              ((u64)i * ehdr->e_phentsize));
    if (phdr->p_type != PT_LOAD)
      continue;
    if (image->segment_count >= USER_MAX_IMAGE_SEGMENTS) {
      kfree(file_data);
      return -1;
    }
    if (phdr->p_offset + phdr->p_filesz < phdr->p_offset ||
        phdr->p_offset + phdr->p_filesz > file_size ||
        phdr->p_filesz > phdr->p_memsz) {
      kfree(file_data);
      return -1;
    }
    u64 reloc_vaddr = phdr->p_vaddr + load_base;
    if (reloc_vaddr + phdr->p_memsz < reloc_vaddr ||
        reloc_vaddr + phdr->p_memsz > 0x00007FFFFFFFFFFFULL) {
      kfree(file_data);
      return -1;
    }

    struct user_image_segment *segment =
        &image->segments[image->segment_count++];
    segment->vaddr = reloc_vaddr;
    segment->memsz = phdr->p_memsz;
    segment->filesz = phdr->p_filesz;
    segment->flags = phdr->p_flags;

    /* Allocate only p_filesz to avoid huge .bss kernel-heap allocations. */
    if (phdr->p_filesz > 0) {
      segment->data = kzalloc(phdr->p_filesz);
      if (!segment->data) {
        kfree(file_data);
        return -1;
      }
      memcpy(segment->data, file_data + phdr->p_offset, phdr->p_filesz);
    } else {
      segment->data = 0;
    }
  }

  /* Second pass: apply PIE relocations. Skipped for ET_EXEC since
   * static binaries have no PT_DYNAMIC. Only R_X86_64_RELATIVE is
   * handled — that covers PIE binaries with no external symbol
   * references (the only flavour b1nix currently supports). */
  if (load_base != 0) {
    u64 rela_off = 0, rela_sz = 0;
    u64 jmprel_off = 0, jmprel_sz = 0;
    int jmprel_is_rela = 1;
    for (u16 i = 0; i < ehdr->e_phnum; i++) {
      struct elf64_phdr *phdr =
          (struct elf64_phdr *)(file_data + ehdr->e_phoff +
                                ((u64)i * ehdr->e_phentsize));
      if (phdr->p_type != PT_DYNAMIC) continue;
      if (phdr->p_offset + phdr->p_filesz > file_size) continue;
      struct elf64_dyn *dyn =
          (struct elf64_dyn *)(file_data + phdr->p_offset);
      usize ndyn = phdr->p_filesz / sizeof(struct elf64_dyn);
      for (usize d = 0; d < ndyn && dyn[d].d_tag != DT_NULL; d++) {
        switch (dyn[d].d_tag) {
          case DT_RELA:    rela_off = dyn[d].d_val; break;
          case DT_RELASZ:  rela_sz = dyn[d].d_val; break;
          case DT_JMPREL:  jmprel_off = dyn[d].d_val; break;
          case DT_PLTRELSZ: jmprel_sz = dyn[d].d_val; break;
          case DT_PLTREL:  jmprel_is_rela = (dyn[d].d_val == DT_RELA); break;
        }
      }
      break;
    }

    /* Translate a PIE-relative virtual address to a kernel pointer into
     * one of the staging buffers we just kzalloc'd. Returns 0 if the
     * vaddr lies outside any loaded segment. */
    #define VADDR_TO_STAGE(v, n) \
      _vaddr_to_stage(image, (v) + load_base, (n))

    /* Inline helper as a static-scope lambda-replacement. */
    /* (declared inline above; body further down) */

    /* Walk DT_RELA. */
    if (rela_off && rela_sz) {
      usize nrela = rela_sz / sizeof(struct elf64_rela);
      /* The DT_RELA address is a PIE vaddr;
       * convert it to a staging-buffer pointer. We use a small helper. */
      for (usize r = 0; r < nrela; r++) {
        /* Read the rela entry out of the staging buffer (kernel-mapped
         * pre-relocation snapshot of the segments). */
        u8 *rela_stage = 0;
        for (usize s = 0; s < image->segment_count; s++) {
          u64 sv = image->segments[s].vaddr;
          u64 sf = image->segments[s].filesz;
          u64 target = rela_off + load_base + r * sizeof(struct elf64_rela);
          if (target >= sv && target + sizeof(struct elf64_rela) <= sv + sf) {
            rela_stage = (u8 *)image->segments[s].data + (target - sv);
            break;
          }
        }
        if (!rela_stage) continue;
        struct elf64_rela rr;
        memcpy(&rr, rela_stage, sizeof(rr));
        u32 type = (u32)(rr.r_info & 0xffffffff);
        if (type != R_X86_64_RELATIVE) continue;
        u64 target_va = rr.r_offset + load_base;
        u64 value = (u64)((i64)load_base + rr.r_addend);
        /* Find the segment that contains target_va and write `value`
         * into the staging buffer at the appropriate offset. */
        for (usize s = 0; s < image->segment_count; s++) {
          u64 sv = image->segments[s].vaddr;
          u64 sf = image->segments[s].filesz;
          if (target_va >= sv && target_va + 8 <= sv + sf) {
            memcpy((u8 *)image->segments[s].data + (target_va - sv), &value, 8);
            break;
          }
        }
      }
    }

    /* DT_JMPREL (PLT relocations) — same shape, applied separately. */
    if (jmprel_off && jmprel_sz && jmprel_is_rela) {
      usize nrela = jmprel_sz / sizeof(struct elf64_rela);
      for (usize r = 0; r < nrela; r++) {
        u8 *rela_stage = 0;
        for (usize s = 0; s < image->segment_count; s++) {
          u64 sv = image->segments[s].vaddr;
          u64 sf = image->segments[s].filesz;
          u64 target = jmprel_off + load_base + r * sizeof(struct elf64_rela);
          if (target >= sv && target + sizeof(struct elf64_rela) <= sv + sf) {
            rela_stage = (u8 *)image->segments[s].data + (target - sv);
            break;
          }
        }
        if (!rela_stage) continue;
        struct elf64_rela rr;
        memcpy(&rr, rela_stage, sizeof(rr));
        u32 type = (u32)(rr.r_info & 0xffffffff);
        if (type != R_X86_64_RELATIVE) continue;
        u64 target_va = rr.r_offset + load_base;
        u64 value = (u64)((i64)load_base + rr.r_addend);
        for (usize s = 0; s < image->segment_count; s++) {
          u64 sv = image->segments[s].vaddr;
          u64 sf = image->segments[s].filesz;
          if (target_va >= sv && target_va + 8 <= sv + sf) {
            memcpy((u8 *)image->segments[s].data + (target_va - sv), &value, 8);
            break;
          }
        }
      }
    }
    #undef VADDR_TO_STAGE
  }

  kfree(file_data);
  return image->segment_count > 0 ? 0 : -1;
}

/* Helper referenced by VADDR_TO_STAGE — kept out-of-line to keep the
 * loader body readable. (Currently unused after the inlined searches
 * above, but retained for the next iteration when relocations become
 * symbol-aware.) */
__attribute__((unused))
static u8 *_vaddr_to_stage(struct user_loaded_image *image, u64 va, usize n) {
  for (usize s = 0; s < image->segment_count; s++) {
    u64 sv = image->segments[s].vaddr;
    u64 sf = image->segments[s].filesz;
    if (va >= sv && va + n <= sv + sf) {
      return (u8 *)image->segments[s].data + (va - sv);
    }
  }
  return 0;
}

void user_image_free(struct user_loaded_image *image) {
  if (!image)
    return;

  /* Atomic dec-and-test: a fork'd process shares its parent's user_image
   * (the read-only ELF), so under -smp two cores exiting tasks that share one
   * image race here. A non-atomic `if (refcount > 1) refcount--` lets both read
   * the same value and either double-free the image or free it while the other
   * core still references it — the heap/page-table corruption seen in the
   * fork-heavy M33 shell tests. Only the core that drops the count to 0 frees. */
  if (__atomic_sub_fetch(&image->refcount, 1, __ATOMIC_ACQ_REL) > 0)
    return;

  if (image->path)
    kfree((void *)image->path);

  if (image->argv) {
    for (int i = 0; i < image->argc; i++) {
      if (image->argv[i])
        kfree((void *)image->argv[i]);
    }
    kfree((void *)image->argv);
  }

  if (image->envp) {
    for (int i = 0; image->envp[i]; i++) {
      kfree((void *)image->envp[i]);
    }
    kfree((void *)image->envp);
  }

  for (usize i = 0; i < image->segment_count; i++) {
    if (image->segments[i].data) {
      kfree(image->segments[i].data);
    }
  }

  if (image->address_space.stack_image) {
    kfree(image->address_space.stack_image);
  }

  kfree(image);
#define ELF_CLASS_32 1
#define ELF_MACHINE_386 3
#define DT_REL 17
#define DT_RELSZ 18
#define R_386_RELATIVE 8

struct elf32_ehdr {
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

struct elf32_phdr {
  u32 p_type;
  u32 p_offset;
  u32 p_vaddr;
  u32 p_paddr;
  u32 p_filesz;
  u32 p_memsz;
  u32 p_flags;
  u32 p_align;
} __attribute__((packed));

struct elf32_rel {
  u32 r_offset;
  u32 r_info;
} __attribute__((packed));

struct elf32_dyn {
  i32 d_tag;
  u32 d_val;
} __attribute__((packed));

static int user_load_elf32(struct user_loaded_image *image, const char *path) {
  char *file_data = 0;
  usize file_size = 0;
  if (user_image_read_vfs_file(path, &file_data, &file_size) != 0)
    return -1;
  if (file_size < sizeof(struct elf32_ehdr)) {
    kfree(file_data);
    return -1;
  }

  struct elf32_ehdr *ehdr = (struct elf32_ehdr *)file_data;
  if (ehdr->e_ident[0] != ELF_MAGIC0 || ehdr->e_ident[1] != ELF_MAGIC1 ||
      ehdr->e_ident[2] != ELF_MAGIC2 || ehdr->e_ident[3] != ELF_MAGIC3) {
    kfree(file_data);
    return -1;
  }
  if (ehdr->e_ident[4] != ELF_CLASS_32 || ehdr->e_ident[5] != ELF_DATA_LE) {
    kfree(file_data);
    return -1;
  }
  if (ehdr->e_type != ELF_TYPE_EXEC && ehdr->e_type != ELF_TYPE_DYN) {
    kfree(file_data);
    return -1;
  }
  if (ehdr->e_machine != ELF_MACHINE_386) {
    kfree(file_data);
    return -1;
  }
  if (ehdr->e_phoff + ((u64)ehdr->e_phentsize * ehdr->e_phnum) > file_size) {
    kfree(file_data);
    return -1;
  }

  image->kind = USER_IMAGE_ELF32;
  image->path = kernel_strdup(path);

  u64 load_base = (ehdr->e_type == ELF_TYPE_DYN) ? PIE_LOAD_BASE : 0;

  image->entry = ehdr->e_entry + load_base;
  console_write("ELF32 load: ");
  console_write(path);
  console_write(" entry=0x");
  console_write_hex64(image->entry);
  if (load_base) {
    console_write(" (PIE base=0x");
    console_write_hex64(load_base);
    console_write(")");
  }
  console_write("\n");
  image->address_space = user_address_space_create();

  /* PT_INTERP */
  for (u16 j = 0; j < ehdr->e_phnum; j++) {
    struct elf32_phdr *p =
        (struct elf32_phdr *)(file_data + ehdr->e_phoff +
                              ((u64)j * ehdr->e_phentsize));
    if (p->p_type != PT_INTERP) continue;
    if (p->p_offset + p->p_filesz > file_size) continue;
    char interp[64];
    usize ilen = p->p_filesz < sizeof(interp) ? p->p_filesz
                                              : sizeof(interp) - 1;
    memcpy(interp, file_data + p->p_offset, ilen);
    interp[ilen] = '\0';
    console_write("ELF32 load: PT_INTERP=");
    console_write(interp);
    console_write(" (b1nix applies RELATIVE relocs in-kernel — no separate ld.so handoff)\n");
  }

  /* First pass: PT_LOAD segments */
  for (u16 i = 0; i < ehdr->e_phnum; i++) {
    struct elf32_phdr *phdr =
        (struct elf32_phdr *)(file_data + ehdr->e_phoff +
                              ((u64)i * ehdr->e_phentsize));
    if (phdr->p_type != PT_LOAD)
      continue;
    if (image->segment_count >= USER_MAX_IMAGE_SEGMENTS) {
      kfree(file_data);
      return -1;
    }
    if (phdr->p_offset + phdr->p_filesz < phdr->p_offset ||
        phdr->p_offset + phdr->p_filesz > file_size ||
        phdr->p_filesz > phdr->p_memsz) {
      kfree(file_data);
      return -1;
    }
    u64 reloc_vaddr = phdr->p_vaddr + load_base;
    if (reloc_vaddr + phdr->p_memsz < reloc_vaddr ||
        reloc_vaddr + phdr->p_memsz > USER_SPACE_LIMIT) {
      kfree(file_data);
      return -1;
    }

    struct user_image_segment *segment =
        &image->segments[image->segment_count++];
    segment->vaddr = reloc_vaddr;
    segment->memsz = phdr->p_memsz;
    segment->filesz = phdr->p_filesz;
    segment->flags = phdr->p_flags;

    if (phdr->p_filesz > 0) {
      segment->data = kzalloc(phdr->p_filesz);
      if (!segment->data) {
        kfree(file_data);
        return -1;
      }
      memcpy(segment->data, file_data + phdr->p_offset, phdr->p_filesz);
    } else {
      segment->data = 0;
    }
  }

  /* Second pass: apply relocations */
  if (load_base != 0) {
    u64 rel_off = 0, rel_sz = 0;
    for (u16 i = 0; i < ehdr->e_phnum; i++) {
      struct elf32_phdr *phdr =
          (struct elf32_phdr *)(file_data + ehdr->e_phoff +
                                ((u64)i * ehdr->e_phentsize));
      if (phdr->p_type != PT_DYNAMIC) continue;
      if (phdr->p_offset + phdr->p_filesz > file_size) continue;
      struct elf32_dyn *dyn =
          (struct elf32_dyn *)(file_data + phdr->p_offset);
      usize ndyn = phdr->p_filesz / sizeof(struct elf32_dyn);
      for (usize d = 0; d < ndyn && dyn[d].d_tag != DT_NULL; d++) {
        switch (dyn[d].d_tag) {
          case DT_REL:    rel_off = dyn[d].d_val; break;
          case DT_RELSZ:  rel_sz = dyn[d].d_val; break;
        }
      }
      break;
    }

    if (rel_off && rel_sz) {
      usize nrel = rel_sz / sizeof(struct elf32_rel);
      u8 *rel_stage = _vaddr_to_stage(image, rel_off + load_base, rel_sz);
      if (!rel_stage) {
        kfree(file_data);
        return -1;
      }
      struct elf32_rel *rel = (struct elf32_rel *)rel_stage;
      for (usize r = 0; r < nrel; r++) {
        u32 r_type = rel[r].r_info & 0xff;
        if (r_type == R_386_RELATIVE) {
          u8 *target = _vaddr_to_stage(image, rel[r].r_offset + load_base, 4);
          if (!target) {
            kfree(file_data);
            return -1;
          }
          u32 addend = *(u32 *)target;
          *(u32 *)target = addend + (u32)load_base;
        }
      }
    }
  }

  kfree(file_data);
  return 0;
}

static struct user_loaded_image *user_load_image(const char *path, int argc,
                                                 const char **argv,
                                                 const char **envp,
                                                 int argv_is_user,
                                                 int envp_is_user) {
  struct user_loaded_image *image = kzalloc(sizeof(*image));
  if (!image)
    return 0;
  image->refcount = 1;

  if (copy_string_vector(argv, USER_MAX_ARGS, &image->argv, &image->argc,
                         argv_is_user) != 0) {
    console_write("user_load_image: copy_string_vector argv failed\n");
    user_image_free(image);
    return 0;
  }
  if (argc > 0 && image->argc > argc)
    image->argc = argc;
  if (copy_string_vector(envp, USER_MAX_ENVS, &image->envp, 0,
                         envp_is_user) != 0) {
    console_write("user_load_image: copy_string_vector envp failed\n");
    user_image_free(image);
    return 0;
  }

  if (image->argc == 0) {
    const char **default_argv = image->argv;
    default_argv[0] = kernel_strdup(path);
    default_argv[1] = 0;
    image->argc = 1;
  }

  if (user_load_elf64(image, path) == 0) {
    if (user_build_initial_stack(image) != 0) {
      console_write("user_load_image: user_build_initial_stack ELF64 failed\n");
      user_image_free(image);
      return 0;
    }
    return image;
  }

  if (user_load_elf32(image, path) == 0) {
    if (user_build_initial_stack(image) != 0) {
      console_write("user_load_image: user_build_initial_stack ELF32 failed\n");
      user_image_free(image);
      return 0;
    }
    return image;
  }

  const struct user_program *program = user_find_program(path);
  if (program) {
    image->kind = USER_IMAGE_BUILTIN;
    image->path = kernel_strdup(path);
    image->entry = (u64)(usize)program->entry;
    image->address_space = user_address_space_create();
    if (user_build_initial_stack(image) != 0) {
      console_write("user_load_image: user_build_initial_stack BUILTIN failed\n");
      user_image_free(image);
      return 0;
    }
    return image;
  }

  console_write("user_load_image: failed to load\n");
  user_image_free(image);
  return 0;
}

static int user_try_run_b1nxexec_image(struct user_loaded_image *image,
                                       int *code) {
  for (usize i = 0; i < image->segment_count; i++) {
    struct user_image_segment *segment = &image->segments[i];
    const char *payload = segment->data;
    if (segment->filesz < 10 || !payload || memcmp(payload, "B1NXEXEC", 9) != 0)
      continue;

    const char *op = payload + 9;
    if (strcmp(op, "echo") == 0) {
      const char *message = op + strlen(op) + 1;
      syscall_dispatch(SYS_WRITE, 1, (u64)(usize)message, (u64)strlen(message),
                       0, 0, 0);
      *code = 0;
      return 1;
    }
    if (strcmp(op, "init") == 0) {
      const char *child = op + strlen(op) + 1;
      while (*child) {
        const char *child_argv[] = {child, 0};
        int pid = user_spawn(child, 1, child_argv);
        if (pid >= 0) {
          int status = 0;
          scheduler_wait((usize)pid, &status);
        }
        child += strlen(child) + 1;
      }
      while (1) {
        int status = 0;
        scheduler_wait(0, &status);
        scheduler_yield();
      }
    }
  }
  return 0;
}

void user_address_space_cleanup(struct task *t) {
  if (!t) return;

  extern void swap_free_all_slots(u64 pml4_phys);
  extern void eviction_unregister_all_pages(struct task *task);
  swap_free_all_slots(t->pml4_phys);
  eviction_unregister_all_pages(t);

  interrupts_disable();
  struct vm_area *vma = t->vma_list;
  t->vma_list = NULL;
  interrupts_enable();


  while (vma) {
    struct vm_area *next = vma->next;

    /* FIX: Unmap actual physical hardware frames to prevent memory leaks */
    for (u64 v = vma->start; v < vma->end; v += PAGE_SIZE) {
      paging_unmap_page_from_space(t->pml4_phys, v);
    }

    if (vma->node) {
      vfs_node_put(vma->node);
    }
    kfree(vma);
    vma = next;
  }

  /* execve/exit tear down whole user images, often at the same virtual
   * addresses that the next exec will immediately reuse. A process may have
   * run on another CPU before this cleanup, so that CPU can retain stale TLB
   * translations for the old image and later resume the same task after exec.
   * Flush all CPUs after the bulk unmap; per-page remote shootdowns would be
   * much more expensive and still need the same cross-CPU guarantee. */
  extern void tlb_shootdown_all(void);
  tlb_shootdown_all();
}

static int user_run_elf_image(struct user_loaded_image *image) {
  /* FIX: Clear old VMAs if this is an execve image replacement */
  if (current_task) {
    user_address_space_cleanup(current_task);
  }

  int compat_code = 0;
  if (user_try_run_b1nxexec_image(image, &compat_code))
    return compat_code;

  /* Map segments into the address space. PIE/ET_DYN binaries can have
   * multiple PT_LOAD segments share the same 4 KB page (e.g. .data +
   * .dynamic both starting at vaddr 0x1000 + 0x10d0). We track which
   * vaddrs we've already mapped this image and reuse those frames so
   * the later segment's copy doesn't overwrite the earlier one with a
   * fresh zero page. Tracked locally rather than via vmm_virt_to_phys
   * because the user pml4 also inherits the kernel's low-4GB identity
   * map (PML4[0]) — `vmm_virt_to_phys((void *)0x2000000)` would return
   * the identity-mapped phys frame and we'd corrupt physical memory. */
  #define MAX_MAPPED_PAGES 64
  u64 mapped_va[MAX_MAPPED_PAGES];
  u64 mapped_frame[MAX_MAPPED_PAGES];
  usize n_mapped = 0;

  for (usize i = 0; i < image->segment_count; i++) {
    struct user_image_segment *segment = &image->segments[i];
    u64 vaddr_start = segment->vaddr & ~(PAGE_SIZE - 1);
    u64 vaddr_end =
        (segment->vaddr + segment->memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    u64 direct_base = vmm_direct_map_base();
    for (u64 v = vaddr_start; v < vaddr_end; v += PAGE_SIZE) {
      u64 frame = 0;
      for (usize m = 0; m < n_mapped; m++) {
        if (mapped_va[m] == v) { frame = mapped_frame[m]; break; }
      }
      if (!frame) {
        frame = pmm_alloc_frame();
        if (!frame)
          return -ENOMEM;
        u64 flags = VMM_USER | VMM_WRITABLE;
        vmm_map_page(v, frame, flags);
        memset((void *)(usize)(direct_base + frame), 0, PAGE_SIZE);
        if (n_mapped < MAX_MAPPED_PAGES) {
          mapped_va[n_mapped] = v;
          mapped_frame[n_mapped] = frame;
          n_mapped++;
        }
      }

      u64 direct_v = direct_base + frame;

      /* Copy this segment's slice into the page at the appropriate offset. */
      u64 page_offset = 0;
      if (v < segment->vaddr)
        page_offset = segment->vaddr - v;

      if (v + page_offset < segment->vaddr + segment->filesz) {
        u64 chunk_offset = (v + page_offset) - segment->vaddr;
        u64 chunk_size = segment->filesz - chunk_offset;
        if (chunk_size > PAGE_SIZE - page_offset)
          chunk_size = PAGE_SIZE - page_offset;

        if (segment->data) {
          memcpy((void *)(usize)(direct_v + page_offset),
                 (char *)segment->data + chunk_offset, chunk_size);
        }
      }
    }

    // Create a VMA for this segment
    struct vm_area *vma = kzalloc(sizeof(struct vm_area));
    if (vma) {
      vma->start = vaddr_start;
      vma->end = vaddr_end;
      vma->prot = PROT_READ | PROT_WRITE | PROT_EXEC; // Simplify for now
      vma->flags = MAP_PRIVATE;
      vma->next = current_task->vma_list;
      current_task->vma_list = vma;
    }

    /* The segment is now resident in the user address space. Its staging
     * buffer is never read again: fork clones the address space via COW page
     * tables (it does not re-instantiate from segment->data), and execve
     * builds a fresh image. Holding it would pin tens of MB per process for
     * the image's whole lifetime (cc1's text segment alone is ~16MB), which
     * is the dominant kheap leak that OOMs the in-guest self-host build. */
    if (segment->data) {
      kfree(segment->data);
      segment->data = 0;
    }
  }

  /* Initialize heap bounds based on the end of the highest segment */
  u64 max_vaddr = 0;
  for (usize i = 0; i < image->segment_count; i++) {
    u64 end = image->segments[i].vaddr + image->segments[i].memsz;
    if (end > max_vaddr)
      max_vaddr = end;
  }
  if (current_task) {
    current_task->heap_start = (max_vaddr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    current_task->user_brk = current_task->heap_start;

    // Create an initial heap VMA
    struct vm_area *vma = kzalloc(sizeof(struct vm_area));
    if (vma) {
      vma->start = current_task->heap_start;
      vma->end = current_task->heap_start;
      vma->prot = PROT_READ | PROT_WRITE;
      vma->flags = MAP_PRIVATE | MAP_ANONYMOUS;
      vma->next = current_task->vma_list;
      current_task->vma_list = vma;
    }
  }

  /* Map stack */
  u64 stack_start =
      image->address_space.stack_top - image->address_space.stack_image_size;
  u64 stack_aligned = stack_start & ~(PAGE_SIZE - 1);
  u64 direct_base = vmm_direct_map_base();
  for (u64 v = stack_aligned; v < image->address_space.stack_top;
       v += PAGE_SIZE) {
    u64 frame = pmm_alloc_frame();
    if (!frame) {
      return -ENOMEM;
    }
    vmm_map_page(v, frame, VMM_USER | VMM_WRITABLE);

    /* Clear stack page */
    u64 direct_v = direct_base + frame;
    memset((void *)(usize)direct_v, 0, PAGE_SIZE);

    u64 image_base =
        image->address_space.stack_top - image->address_space.stack_image_size;
    u64 offset = v - image_base;
    if (offset < image->address_space.stack_image_size) {
      u64 chunk = image->address_space.stack_image_size - offset;
      if (chunk > PAGE_SIZE)
        chunk = PAGE_SIZE;
      memcpy((void *)(usize)direct_v,
             (char *)image->address_space.stack_image + offset, chunk);
    }
  }

  // Create VMA for stack
  struct vm_area *stack_vma = kzalloc(sizeof(struct vm_area));
  if (stack_vma) {
    /* Reserve a stack growth window; faults map pages lazily on demand. */
    stack_vma->start = image->address_space.stack_top - USER_STACK_MAX_SIZE;
    stack_vma->end = image->address_space.stack_top;
    stack_vma->prot = PROT_READ | PROT_WRITE;
    stack_vma->flags = MAP_PRIVATE | MAP_ANONYMOUS;
    stack_vma->next = current_task->vma_list;
    current_task->vma_list = stack_vma;
  }



  if ((image->address_space.stack_base & 0xFULL) != 0) {
    console_write("user: reject unaligned ring3 stack\n");
    return -1;
  }
  if (image->entry >= USER_SPACE_LIMIT ||
      image->address_space.stack_base >= USER_SPACE_LIMIT) {
    console_write("user: reject non-canonical ring3 frame\n");
    return -1;
  }

  /* A freshly started program expects a clean FPU/MXCSR (SysV ABI). Reset the
   * live FPU here since exec replaces the image without a context switch. */
  arch_fpu_init_current();

  x86_user_jump((usize)image->entry, (usize)image->address_space.stack_base,
                (usize)image->argc,
                (usize)(image->address_space.stack_base + sizeof(usize)));

  return 0; // Should not reach here
}

static void user_process_thread(void *arg) {
  struct process_start *start = arg;
  struct user_loaded_image *image = start ? start->image : 0;

  console_write("[DEBUG] user_process_thread: starting ");
  if (image) {
    console_write(image->path);
    console_write(" kind=");
    if (image->kind == USER_IMAGE_ELF64) console_write("ELF64");
    else if (image->kind == USER_IMAGE_ELF32) console_write("ELF32");
    else if (image->kind == USER_IMAGE_BUILTIN) console_write("BUILTIN");
    else console_write("UNKNOWN");
  } else {
    console_write("NULL");
  }
  console_write("\n");

  scheduler_set_user_image(image);
  kfree(start);

  int code = 0;
  if (image->kind == USER_IMAGE_ELF64) {
    code = user_run_elf_image(image);
  } else {
    user_program_entry entry = (user_program_entry)(usize)image->entry;
    console_write("[DEBUG] user_process_thread: calling entry point at 0x");
    console_write_hex64((u64)(usize)entry);
    console_write("\n");
    code = entry(image->argc, image->argv);
    console_write("[DEBUG] user_process_thread: entry point returned\n");
  }
  syscall_dispatch(SYS_EXIT, (u64)code, 0, 0, 0, 0, 0);

  while (1) {
    scheduler_yield();
  }
}

void userspace_init(void) {
  program_count = 0;
  user_register_builtin_programs();
  console_write("userspace: builtin programs 0x");
  console_write_hex64(program_count);
  console_write("\n");
}

int user_spawn(const char *path, int argc, const char **argv) {
  struct vfs_node *node = vfs_find_node(path);
  if (!node || IS_ERR(node)) {
    return -1;
  }

  const struct cred *cred = scheduler_get_current_cred();
  if (!cred || vfs_get_node_perm(node, cred, 1) != 1) {
    vfs_node_put(node);
    return -EACCES;
  }
  vfs_node_put(node);

  const char *empty_env[] = {"PATH=/bin", 0};
  struct user_loaded_image *image =
      user_load_image(path, argc, argv, empty_env, 0, 0);
  if (!image) {
    return -1;
  }

  struct process_start *start = kzalloc(sizeof(*start));
  if (!start) {
    user_image_free(image);
    return -1;
  }
  start->image = image;

  /* FIX: Truncate thread name to 15 chars to prevent kthread_create from
   * failing */
  char safe_name[16];
  usize plen = strlen(path);
  if (plen > 15)
    plen = 15;
  memcpy(safe_name, path, plen);
  safe_name[plen] = '\0';

  /* Real userspace ELF processes may run on Application Processors: they enter
   * ring 3 and so release the Big Kernel Lock. Builtins run in kernel mode and
   * stay on the BSP (ap_runnable=0). */
  int ap_runnable = (image->kind == USER_IMAGE_ELF64);
  int tid = kthread_create_user(safe_name, user_process_thread, start, ap_runnable);
  if (tid < 0) {
    kfree(start);
    user_image_free(image);
    return -1;
  }
  return tid;
}

int user_execve_current(const char *path, const char **argv,
                        const char **envp) {
  struct vfs_node *node = vfs_find_node(path);
  if (IS_ERR(node))
    return (int)PTR_ERR(node);

  /* POSIX: Check execute permission */
  const struct cred *cred = scheduler_get_current_cred();
  if (vfs_get_node_perm(node, cred, 1) != 1) {
    vfs_node_put(node);
    return -EACCES;
  }

  u16 file_mode = node->inode->mode;
  u16 file_uid = node->inode->uid;
  u16 file_gid = node->inode->gid;

  vfs_node_put(node);

  struct user_loaded_image *image = user_load_image(path, 0, argv, envp, 0, 0);
  if (!image)
    return -1;

  free_kernel_array((char **)argv);
  free_kernel_array((char **)envp);

  vfs_close_on_exec();

  /* POSIX: Apply SUID/SGID bits */
  if (file_mode & 04000) { /* S_ISUID */
    current_task->cred->euid = file_uid;
    current_task->cred->suid = file_uid;
  }
  if (file_mode & 02000) { /* S_ISGID */
    current_task->cred->egid = file_gid;
    current_task->cred->sgid = file_gid;
  }

  // POSIX: Reset caught signals to default action across execve.
  // Ignored signals (SIG_IGN) remain ignored.
  for (int sig = 1; sig < NSIG; sig++) {
    if (current_task->sigactions[sig - 1].sa_handler != SIG_IGN) {
      current_task->sigactions[sig - 1].sa_handler = SIG_DFL;
    }
  }

  if (current_task->name) {
    kfree((void *)current_task->name);
  }
  current_task->name = strdup(path);

  if (current_task->user_image) {
    user_image_free(current_task->user_image);
  }
  current_task->user_image = image;
  task_set_tls_base(current_task, 0);
  {
    extern void arch_set_fs_base(u64 base);
    arch_set_fs_base(0);
  }

  int code = 0;
  if (image->kind == USER_IMAGE_ELF64) {
    code = user_run_elf_image(image);
  } else {
    user_program_entry entry = (user_program_entry)(usize)image->entry;
    code = entry(image->argc, image->argv);
  }
  scheduler_exit_current(code);
}

void user_register_program(const char *path, user_program_entry entry) {
  /* Grow the registry on demand: start at PROGRAMS_INITIAL, double each
   * time we fill up. Existing entries stay in place (memcpy to the new
   * buffer), so any pointer returned by a prior user_find_program() is
   * invalidated — callers never cache such pointers across registration,
   * so this is safe. */
  if (program_count >= programs_capacity) {
    usize new_cap = programs_capacity ? programs_capacity * 2 : PROGRAMS_INITIAL;
    struct user_program *grown = kzalloc(new_cap * sizeof(struct user_program));
    if (!grown) {
      klog_warn("too many builtin user programs, skipping registration");
      return;
    }
    if (programs && program_count) {
      memcpy(grown, programs, program_count * sizeof(struct user_program));
    }
    if (programs) kfree(programs);
    programs = grown;
    programs_capacity = new_cap;
  }

  programs[program_count].path = path;
  programs[program_count].entry = entry;
  program_count++;

  /* Ensure the program exists in the VFS so vfs_find_node/vfs_get_node_perm
   * works */
  if (vfs_create(path, 0755) == 0) {
    /* File created with 0755 mode */
  }
}

const struct user_program *user_find_program(const char *path) {
  for (usize i = 0; i < program_count; i++) {
    if (strcmp(programs[i].path, path) == 0) {
      return &programs[i];
    }
  }

  return 0;
}
