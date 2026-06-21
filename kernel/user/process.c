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
#define PT_NOTE    4
#define PT_TLS     7

/* M40 — Linux personality detection. EI_OSABI is e_ident byte 7; Linux/GNU
 * binaries set it to ELFOSABI_LINUX or carry a GNU NT_GNU_ABI_TAG note whose
 * first descriptor word (the OS) is GNU_ABI_OS_LINUX. b1nix's own freestanding
 * userspace sets neither. */
#define EI_OSABI          7
#define ELFOSABI_LINUX    3
#define NT_GNU_ABI_TAG    1
#define GNU_ABI_OS_LINUX  0

/* M30 — ELF64 dynamic-tag identifiers (subset honoured by the in-kernel
 * loader). Standard `Elf64_Dyn` tag values. */
#define DT_NULL    0
#define DT_NEEDED  1
#define DT_PLTRELSZ 2
#define DT_HASH    4
#define DT_STRTAB  5
#define DT_SYMTAB  6
#define DT_RELA    7
#define DT_RELASZ  8
#define DT_RELAENT 9
#define DT_STRSZ   10
#define DT_SYMENT  11
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

struct elf64_sym {
  u32 st_name;
  u8 st_info;
  u8 st_other;
  u16 st_shndx;
  u64 st_value;
  u64 st_size;
} __attribute__((packed));

#define ELF64_ST_BIND(info) ((info) >> 4)
#define STB_WEAK 2
#define SHN_UNDEF 0
#define DYN_MAX_OBJECTS 8
#define DYN_MAX_NEEDED 8

struct elf64_dyn_object {
  u64 base;
  usize first_segment;
  usize segment_count;
  u64 rela;
  u64 rela_size;
  u64 jmprel;
  u64 jmprel_size;
  u64 symtab;
  u64 strtab;
  u64 strsz;
  u64 hash;
  u64 needed[DYN_MAX_NEEDED];
  usize needed_count;
};

/* M30 — base address at which PIE/ET_DYN images get loaded. Picked well
 * above the standard 0x400000 ET_EXEC load base and below the user
 * stack top (0x800000000000) so a PIE binary and the existing static
 * binaries don't collide. */
#ifdef __x86_64__
#define PIE_LOAD_BASE 0x0000500000000000ULL
#else
#define PIE_LOAD_BASE 0x40000000ULL
#endif

/* Sanity ceiling on a single ELF segment's p_memsz. Any real b1nix binary is a
 * few MiB; a crafted multi-GiB p_memsz must be rejected before kzalloc (OOM =
 * panic in this kernel). 512 MiB is well above any legitimate image segment. */
#define USER_IMAGE_MAX_SEGMENT (512ULL * 1024 * 1024)

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
static int user_image_read_vfs_file(const char *path, char **out_data,
                                    usize *out_size);

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

static u8 *elf64_stage_ptr(struct user_loaded_image *image, u64 va, usize n)
{
  if (va + n < va)
    return 0;
  for (usize i = 0; i < image->segment_count; i++) {
    struct user_image_segment *s = &image->segments[i];
    if (va >= s->vaddr && va + n <= s->vaddr + s->memsz)
      return (u8 *)s->data + (va - s->vaddr);
  }
  return 0;
}

/* Copy the NUL-terminated string at `va` into `out` (always NUL-terminated),
 * bounded by both `out_size` and the containing segment's end (R4-8). Returns 1
 * on success, 0 if `va` is not inside any segment. */
static int elf64_copy_str(struct user_loaded_image *image, u64 va, char *out,
                          usize out_size)
{
  if (out_size == 0)
    return 0;
  for (usize i = 0; i < image->segment_count; i++) {
    struct user_image_segment *s = &image->segments[i];
    if (va < s->vaddr || va >= s->vaddr + s->memsz)
      continue;
    const char *str = (const char *)s->data + (va - s->vaddr);
    usize avail = (usize)(s->vaddr + s->memsz - va);
    usize limit = avail < out_size - 1 ? avail : out_size - 1;
    usize k = 0;
    for (; k < limit && str[k]; k++)
      out[k] = str[k];
    out[k] = '\0';
    return 1;
  }
  return 0;
}

/* Compare a NUL-terminated string that lives at `va` inside the staged image
 * against `name`, but never scan past the containing segment's end — a crafted
 * strtab string with no NUL before the segment boundary would otherwise drive
 * an OOB read out of the kmalloc'd segment (R4-8). Returns 1 on equal. */
static int elf64_str_at_equals(struct user_loaded_image *image, u64 va,
                               const char *name)
{
  for (usize i = 0; i < image->segment_count; i++) {
    struct user_image_segment *s = &image->segments[i];
    if (va < s->vaddr || va >= s->vaddr + s->memsz)
      continue;
    const char *str = (const char *)s->data + (va - s->vaddr);
    usize avail = (usize)(s->vaddr + s->memsz - va);
    usize k = 0;
    for (; k < avail; k++) {
      if (str[k] != name[k])
        return 0;
      if (name[k] == '\0') /* both reached NUL at the same position */
        return 1;
    }
    /* Ran off the segment with no NUL — not a valid/equal string. */
    return 0;
  }
  return 0;
}

static int elf64_parse_dynamic(struct user_loaded_image *image,
                               struct elf64_dyn_object *obj,
                               const struct elf64_ehdr *ehdr,
                               const char *file_data, usize file_size)
{
  for (u16 i = 0; i < ehdr->e_phnum; i++) {
    const struct elf64_phdr *ph =
        (const struct elf64_phdr *)(file_data + ehdr->e_phoff +
                                    (u64)i * ehdr->e_phentsize);
    if (ph->p_type != PT_DYNAMIC)
      continue;
    if (ph->p_offset + ph->p_filesz < ph->p_offset ||
        ph->p_offset + ph->p_filesz > file_size)
      return -1;
    const struct elf64_dyn *dyn =
        (const struct elf64_dyn *)(file_data + ph->p_offset);
    usize count = ph->p_filesz / sizeof(*dyn);
    for (usize d = 0; d < count && dyn[d].d_tag != DT_NULL; d++) {
      switch (dyn[d].d_tag) {
      case DT_NEEDED:
        if (obj->needed_count < DYN_MAX_NEEDED)
          obj->needed[obj->needed_count++] = dyn[d].d_val;
        break;
      case DT_HASH: obj->hash = dyn[d].d_val + obj->base; break;
      case DT_STRTAB: obj->strtab = dyn[d].d_val + obj->base; break;
      case DT_SYMTAB: obj->symtab = dyn[d].d_val + obj->base; break;
      case DT_STRSZ: obj->strsz = dyn[d].d_val; break;
      case DT_RELA: obj->rela = dyn[d].d_val + obj->base; break;
      case DT_RELASZ: obj->rela_size = dyn[d].d_val; break;
      case DT_JMPREL: obj->jmprel = dyn[d].d_val + obj->base; break;
      case DT_PLTRELSZ: obj->jmprel_size = dyn[d].d_val; break;
      default: break;
      }
    }
    return 0;
  }
  (void)image;
  return 0;
}

static int elf64_load_shared_object(struct user_loaded_image *image,
                                    const char *path, u64 base,
                                    struct elf64_dyn_object *obj)
{
  char *data = 0;
  usize size = 0;
  if (user_image_read_vfs_file(path, &data, &size) != 0)
    return -1;
  if (size < sizeof(struct elf64_ehdr)) {
    kfree(data);
    return -1;
  }
  const struct elf64_ehdr *ehdr = (const struct elf64_ehdr *)data;
  if (ehdr->e_ident[0] != ELF_MAGIC0 || ehdr->e_ident[1] != ELF_MAGIC1 ||
      ehdr->e_ident[2] != ELF_MAGIC2 || ehdr->e_ident[3] != ELF_MAGIC3 ||
      ehdr->e_ident[4] != ELF_CLASS_64 || ehdr->e_type != ELF_TYPE_DYN ||
      ehdr->e_machine != ELF_MACHINE_X86_64 ||
      ehdr->e_phentsize != sizeof(struct elf64_phdr) ||
      ehdr->e_phoff + (u64)ehdr->e_phnum * ehdr->e_phentsize > size) {
    kfree(data);
    return -1;
  }

  memset(obj, 0, sizeof(*obj));
  obj->base = base;
  obj->first_segment = image->segment_count;
  for (u16 i = 0; i < ehdr->e_phnum; i++) {
    const struct elf64_phdr *ph =
        (const struct elf64_phdr *)(data + ehdr->e_phoff +
                                    (u64)i * ehdr->e_phentsize);
    if (ph->p_type != PT_LOAD)
      continue;
    if (image->segment_count >= USER_MAX_IMAGE_SEGMENTS ||
        ph->p_filesz > ph->p_memsz ||
        ph->p_offset + ph->p_filesz > size) {
      kfree(data);
      return -1;
    }
    struct user_image_segment *seg =
        &image->segments[image->segment_count++];
    seg->vaddr = base + ph->p_vaddr;
    seg->memsz = ph->p_memsz;
    seg->filesz = ph->p_memsz;
    seg->flags = ph->p_flags;
    seg->data = kzalloc(ph->p_memsz ? ph->p_memsz : 1);
    if (!seg->data) {
      kfree(data);
      return -1;
    }
    if (ph->p_filesz)
      memcpy(seg->data, data + ph->p_offset, ph->p_filesz);
    obj->segment_count++;
  }
  int rc = elf64_parse_dynamic(image, obj, ehdr, data, size);
  kfree(data);
  return rc;
}

static int elf64_symbol_count(struct user_loaded_image *image,
                              const struct elf64_dyn_object *obj,
                              u32 *out)
{
  u32 *hash = (u32 *)elf64_stage_ptr(image, obj->hash, 8);
  if (!hash)
    return -1;
  *out = hash[1]; /* SysV hash nchain equals the dynamic symbol count. */
  return 0;
}

static int elf64_resolve_symbol(struct user_loaded_image *image,
                                struct elf64_dyn_object *objects,
                                usize object_count, const char *name,
                                u64 *value)
{
  for (usize o = 0; o < object_count; o++) {
    u32 count = 0;
    if (!objects[o].symtab || !objects[o].strtab ||
        elf64_symbol_count(image, &objects[o], &count) != 0)
      continue;
    for (u32 i = 1; i < count; i++) {
      struct elf64_sym *sym = (struct elf64_sym *)elf64_stage_ptr(
          image, objects[o].symtab + (u64)i * sizeof(*sym), sizeof(*sym));
      if (!sym || sym->st_shndx == SHN_UNDEF || sym->st_name >= objects[o].strsz)
        continue;
      /* Bounded compare — the strtab string is not guaranteed NUL-terminated
       * before its segment ends (R4-8). */
      if (elf64_str_at_equals(image, objects[o].strtab + sym->st_name, name)) {
        *value = objects[o].base + sym->st_value;
        return 0;
      }
    }
  }
  return -1;
}

static int elf64_apply_rela_table(struct user_loaded_image *image,
                                  struct elf64_dyn_object *objects,
                                  usize object_count, usize owner,
                                  u64 table, u64 table_size)
{
  if (!table || !table_size)
    return 0;
  usize count = table_size / sizeof(struct elf64_rela);
  for (usize i = 0; i < count; i++) {
    struct elf64_rela *rela = (struct elf64_rela *)elf64_stage_ptr(
        image, table + i * sizeof(*rela), sizeof(*rela));
    if (!rela)
      return -1;
    u32 type = (u32)rela->r_info;
    u32 sym_index = (u32)(rela->r_info >> 32);
    u64 *target = (u64 *)elf64_stage_ptr(
        image, objects[owner].base + rela->r_offset, sizeof(u64));
    if (!target)
      return -1;
    if (type == R_X86_64_NONE)
      continue;
    if (type == R_X86_64_RELATIVE) {
      *target = objects[owner].base + (u64)rela->r_addend;
      continue;
    }
    if (type == R_X86_64_64 || type == R_X86_64_GLOB_DAT ||
        type == R_X86_64_JUMP_SLOT) {
      struct elf64_sym *sym = (struct elf64_sym *)elf64_stage_ptr(
          image, objects[owner].symtab + (u64)sym_index * sizeof(*sym),
          sizeof(*sym));
      if (!sym || sym->st_name >= objects[owner].strsz)
        return -1;
      /* Copy the undefined symbol's name into a bounded buffer before using it
       * as a lookup key — the raw strtab pointer may not be NUL-terminated
       * within its segment (R4-8). */
      char name[256];
      u64 resolved = 0;
      if (!elf64_copy_str(image, objects[owner].strtab + sym->st_name, name,
                          sizeof(name)) ||
          elf64_resolve_symbol(image, objects, object_count, name, &resolved) != 0) {
        if (ELF64_ST_BIND(sym->st_info) == STB_WEAK)
          resolved = 0;
        else
          return -1;
      }
      *target = resolved + (u64)rela->r_addend;
      continue;
    }
    return -1;
  }
  return 0;
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

  /* Ensure the final stack pointer (which becomes ESP/RSP at _start) is
   * 16-byte aligned, as the SysV ABI requires. We are about to push exactly
   * total_slots pointer-sized words: argc(1), argv(argc), NULL(1), envp(envc),
   * NULL(1), auxv(5). sp is 16-aligned right now, so after the pushes it stays
   * 16-aligned iff total_slots*sizeof(usize) is a multiple of 16. Pad with as
   * many zero words as needed to reach the next 16-byte boundary.
   *
   * On x86_64 sizeof(usize)==8, so a single pad word always sufficed (16/8==2
   * words per 16 bytes). On the 32-bit port sizeof(usize)==4, so up to THREE
   * pad words may be required — the old "push one if misaligned" left ESP 4- or
   * 8-byte aligned for many argc/envc counts, and user_run_elf_image rejected
   * the unaligned ring3 stack, so every ELF32 spawn failed. */
  usize total_slots = 8 + (usize)image->argc + (usize)envc;
  usize words_per_16 = 16 / sizeof(usize);
  usize rem = total_slots % words_per_16;
  usize pad = rem ? (words_per_16 - rem) : 0;
  for (usize p = 0; p < pad; p++) {
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

/* M40 — decide whether an ELF64 image is a Linux binary. Two independent
 * signals, either of which is conclusive:
 *   1. EI_OSABI (e_ident[7]) == ELFOSABI_LINUX.
 *   2. A PT_NOTE program header containing a GNU NT_GNU_ABI_TAG note whose first
 *      descriptor word (the OS) is GNU_ABI_OS_LINUX. This is what glibc-linked
 *      static Linux binaries carry; b1nix freestanding binaries never emit it.
 * Returns 1 for a Linux binary, 0 otherwise. All file accesses are bounds-checked
 * against file_size so a crafted note table cannot drive an OOB read. */
static int elf64_is_linux_binary(const struct elf64_ehdr *ehdr,
                                 const char *file_data, usize file_size) {
  if (ehdr->e_ident[EI_OSABI] == ELFOSABI_LINUX)
    return 1;

  for (u16 i = 0; i < ehdr->e_phnum; i++) {
    const struct elf64_phdr *ph =
        (const struct elf64_phdr *)(file_data + ehdr->e_phoff +
                                    (u64)i * ehdr->e_phentsize);
    if (ph->p_type != PT_NOTE)
      continue;
    if (ph->p_offset + ph->p_filesz < ph->p_offset ||
        ph->p_offset + ph->p_filesz > file_size)
      continue;
    /* Walk the note records: [namesz(4)][descsz(4)][type(4)][name, 4-aligned]
     * [desc, 4-aligned]. */
    u64 off = ph->p_offset;
    u64 end = ph->p_offset + ph->p_filesz;
    while (off + 12 <= end) {
      u32 namesz = *(const u32 *)(file_data + off);
      u32 descsz = *(const u32 *)(file_data + off + 4);
      u32 type = *(const u32 *)(file_data + off + 8);
      u64 name_off = off + 12;
      u64 name_pad = ((u64)namesz + 3) & ~3ULL;
      u64 desc_off = name_off + name_pad;
      u64 desc_pad = ((u64)descsz + 3) & ~3ULL;
      u64 next = desc_off + desc_pad;
      if (next < off || next > end) /* malformed / wrap */
        break;
      if (type == NT_GNU_ABI_TAG && namesz == 4 && descsz >= 4 &&
          name_off + 4 <= end &&
          file_data[name_off] == 'G' && file_data[name_off + 1] == 'N' &&
          file_data[name_off + 2] == 'U' && file_data[name_off + 3] == '\0' &&
          desc_off + 4 <= end) {
        u32 os = *(const u32 *)(file_data + desc_off);
        if (os == GNU_ABI_OS_LINUX)
          return 1;
      }
      off = next;
    }
  }
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
  /* Architecture match against the running kernel. A binary built for a
   * different CPU cannot execute; report the mismatch explicitly instead of
   * silently failing or — worse — attempting to run it. This is exactly the
   * failure mode behind the native-smoke "stale x86_64 .inc embedded in the
   * 32-bit image" bug, which otherwise only showed up as a cryptic fault. */
#ifdef __x86_64__
  if (ehdr->e_machine != ELF_MACHINE_X86_64) {
    console_write("ELF load: ARCH MISMATCH — non-x86_64 binary (e_machine=0x");
    console_write_hex64(ehdr->e_machine);
    console_write(") on x86_64 kernel, refusing: ");
    console_write(path);
    console_write("\n");
    kfree(file_data);
    return -1;
  }
#else
  console_write("ELF load: ARCH MISMATCH — 64-bit binary on 32-bit kernel, "
                "refusing: ");
  console_write(path);
  console_write("\n");
  kfree(file_data);
  return -1;
#endif
  /* The primary entry point must validate e_phentsize like the shared-object
   * loader already does: with a small/zero phentsize the phdr-walk casts would
   * read sizeof(struct elf64_phdr) bytes per slot past the kmalloc(file_size)
   * buffer. Use subtraction-form bounds so a huge e_phoff cannot wrap the sum
   * below file_size and slip a wild pointer through the check (R4-4). */
  if (ehdr->e_phentsize != sizeof(struct elf64_phdr)) {
    kfree(file_data);
    return -1;
  }
  if (ehdr->e_phoff > file_size ||
      (u64)ehdr->e_phentsize * ehdr->e_phnum > file_size - ehdr->e_phoff) {
    kfree(file_data);
    return -1;
  }

  image->kind = USER_IMAGE_ELF64;
  image->path = kernel_strdup(path);

  /* M40: tag the binary personality (e_phoff/e_phentsize already validated
   * above, so the note walk is in-bounds). A Linux binary gets its syscall
   * numbers translated at dispatch time. */
  if (elf64_is_linux_binary(ehdr, file_data, file_size)) {
    image->personality = PERSONALITY_LINUX;
    console_write("ELF load: Linux personality detected: ");
    console_write(path);
    console_write("\n");
  } else {
    image->personality = PERSONALITY_B1NIX;
  }

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

  /* PT_INTERP is informational: the ELF64 loader performs startup dependency
   * loading, symbol resolution, and relocation eagerly before entering user
   * mode rather than handing control to a separate ld.so process. */
  for (u16 j = 0; j < ehdr->e_phnum; j++) {
    struct elf64_phdr *p =
        (struct elf64_phdr *)(file_data + ehdr->e_phoff +
                              ((u64)j * ehdr->e_phentsize));
    if (p->p_type != PT_INTERP) continue;
    if (p->p_offset + p->p_filesz < p->p_offset ||
        p->p_offset + p->p_filesz > file_size) continue;
    char interp[64];
    usize ilen = p->p_filesz < sizeof(interp) ? p->p_filesz
                                              : sizeof(interp) - 1;
    memcpy(interp, file_data + p->p_offset, ilen);
    interp[ilen] = '\0';
    console_write("ELF load: PT_INTERP=");
    console_write(interp);
    console_write(" (eager in-kernel dynamic linking)\n");
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
    /* Cap p_memsz before kzalloc: the project rule is OOM = panic, so a crafted
     * multi-GiB p_memsz (well under the vaddr ceiling above) would turn a merely
     * unloadable file into a kernel panic (R4-10). */
    if (phdr->p_memsz > USER_IMAGE_MAX_SEGMENT) {
      kfree(file_data);
      return -1;
    }

    struct user_image_segment *segment =
        &image->segments[image->segment_count++];
    segment->vaddr = reloc_vaddr;
    segment->memsz = phdr->p_memsz;
    segment->filesz = phdr->p_memsz;
    segment->flags = phdr->p_flags;

    /* Relocations may target .bss or the zero-filled tail of a PT_LOAD, so the
     * staging image must cover p_memsz, not only the bytes present on disk. */
    if (phdr->p_memsz > 0) {
      segment->data = kzalloc(phdr->p_memsz);
      if (!segment->data) {
        kfree(file_data);
        return -1;
      }
      if (phdr->p_filesz)
        memcpy(segment->data, file_data + phdr->p_offset, phdr->p_filesz);
    } else {
      segment->data = 0;
    }
  }

  /* Capture the PT_TLS template so user_run_elf_image can set up the main
   * thread's TLS block + FS base. Without this, a binary that uses __thread /
   * std::call_once (e.g. Mesa via util_call_once_data_slow's `mov %fs:0,%rax`)
   * derefs a zero FS base and SIGSEGVs before main does any real work. */
  for (u16 i = 0; i < ehdr->e_phnum; i++) {
    struct elf64_phdr *phdr =
        (struct elf64_phdr *)(file_data + ehdr->e_phoff +
                              ((u64)i * ehdr->e_phentsize));
    if (phdr->p_type != PT_TLS)
      continue;
    if (phdr->p_filesz > phdr->p_memsz ||
        phdr->p_offset + phdr->p_filesz < phdr->p_offset ||
        phdr->p_offset + phdr->p_filesz > file_size ||
        phdr->p_memsz > USER_IMAGE_MAX_SEGMENT)
      break; /* malformed — leave tls_memsz 0 (no TLS) */
    image->tls_memsz = phdr->p_memsz;
    image->tls_filesz = phdr->p_filesz;
    image->tls_align = phdr->p_align ? phdr->p_align : 8;
    if (phdr->p_filesz) {
      image->tls_data = kmalloc(phdr->p_filesz);
      if (image->tls_data)
        memcpy(image->tls_data, file_data + phdr->p_offset, phdr->p_filesz);
    }
    break; /* at most one PT_TLS */
  }

  /* Eager dynamic linking. The kernel loads each DT_NEEDED ET_DYN object into
   * the process image, resolves symbols globally (main first, then libraries),
   * and patches GOT/PLT before userspace starts. */
  if (load_base != 0) {
    struct elf64_dyn_object objects[DYN_MAX_OBJECTS];
    memset(objects, 0, sizeof(objects));
    usize object_count = 1;
    objects[0].base = load_base;
    objects[0].first_segment = 0;
    objects[0].segment_count = image->segment_count;
    if (elf64_parse_dynamic(image, &objects[0], ehdr, file_data, file_size) != 0) {
      kfree(file_data);
      return -1;
    }

    for (usize n = 0; n < objects[0].needed_count; n++) {
      if (object_count >= DYN_MAX_OBJECTS || !objects[0].strtab) {
        kfree(file_data);
        return -1;
      }
      /* Bound the DT_NEEDED name into a local buffer before strchr/strlen — the
       * raw strtab pointer may run past its segment without a NUL (R4-8). */
      char name[96];
      if (!elf64_copy_str(image, objects[0].strtab + objects[0].needed[n], name,
                          sizeof(name)) ||
          strchr(name, '/')) {
        kfree(file_data);
        return -1;
      }
      char libpath[96] = "/lib/";
      usize name_len = strlen(name);
      if (name_len + 6 > sizeof(libpath)) {
        kfree(file_data);
        return -1;
      }
      memcpy(libpath + 5, name, name_len + 1);
      u64 lib_base = 0x0000600000000000ULL +
                     (u64)(object_count - 1) * 0x0000010000000000ULL;
      if (elf64_load_shared_object(image, libpath, lib_base,
                                   &objects[object_count]) != 0) {
        console_write("ELF load: missing/invalid DT_NEEDED ");
        console_write(libpath);
        console_write("\n");
        kfree(file_data);
        return -1;
      }
      object_count++;
    }

    for (usize o = 0; o < object_count; o++) {
      if (elf64_apply_rela_table(image, objects, object_count, o,
                                 objects[o].rela, objects[o].rela_size) != 0 ||
          elf64_apply_rela_table(image, objects, object_count, o,
                                 objects[o].jmprel,
                                 objects[o].jmprel_size) != 0) {
        console_write("ELF load: unresolved/unsupported dynamic relocation\n");
        kfree(file_data);
        return -1;
      }
    }
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

  if (image->tls_data) {
    kfree(image->tls_data);
  }

  kfree(image);
}

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
  /* See user_load_elf64: report an architecture mismatch clearly. A 32-bit i386
   * binary cannot run on the 64-bit kernel (no compat mode). */
#ifdef __x86_64__
  console_write("ELF32 load: ARCH MISMATCH — i386 binary on x86_64 kernel, "
                "refusing: ");
  console_write(path);
  console_write("\n");
  kfree(file_data);
  return -1;
#endif
  /* Validate e_phentsize and use subtraction-form bounds — see user_load_elf64
   * (R4-4). */
  if (ehdr->e_phentsize != sizeof(struct elf32_phdr)) {
    kfree(file_data);
    return -1;
  }
  if (ehdr->e_phoff > file_size ||
      (u64)ehdr->e_phentsize * ehdr->e_phnum > file_size - ehdr->e_phoff) {
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
    if (p->p_offset + p->p_filesz < p->p_offset ||
        p->p_offset + p->p_filesz > file_size) continue;
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
    /* Cap p_memsz before allocating — see user_load_elf64 (R4-10). */
    if (phdr->p_memsz > USER_IMAGE_MAX_SEGMENT) {
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

  /* Capture PT_TLS so user_run_elf_image sets up the main thread's TLS block +
   * GS base (i686 variant II). Without it, a binary using __thread / call_once
   * (Mesa) derefs a zero GS base and SIGSEGVs. Mirrors the elf64 path. */
  for (u16 i = 0; i < ehdr->e_phnum; i++) {
    struct elf32_phdr *phdr =
        (struct elf32_phdr *)(file_data + ehdr->e_phoff +
                              ((u64)i * ehdr->e_phentsize));
    if (phdr->p_type != PT_TLS)
      continue;
    if (phdr->p_filesz > phdr->p_memsz ||
        phdr->p_offset + phdr->p_filesz < phdr->p_offset ||
        phdr->p_offset + phdr->p_filesz > file_size ||
        phdr->p_memsz > USER_IMAGE_MAX_SEGMENT)
      break;
    image->tls_memsz = phdr->p_memsz;
    image->tls_filesz = phdr->p_filesz;
    image->tls_align = phdr->p_align ? phdr->p_align : 8;
    if (phdr->p_filesz) {
      image->tls_data = kmalloc(phdr->p_filesz);
      if (image->tls_data)
        memcpy(image->tls_data, file_data + phdr->p_offset, phdr->p_filesz);
    }
    break;
  }

  /* Second pass: apply relocations */
  if (load_base != 0) {
    u64 rel_off = 0, rel_sz = 0;
    for (u16 i = 0; i < ehdr->e_phnum; i++) {
      struct elf32_phdr *phdr =
          (struct elf32_phdr *)(file_data + ehdr->e_phoff +
                                ((u64)i * ehdr->e_phentsize));
      if (phdr->p_type != PT_DYNAMIC) continue;
      if (phdr->p_offset + phdr->p_filesz < phdr->p_offset ||
          phdr->p_offset + phdr->p_filesz > file_size) continue;
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
        /* -1 = reap ANY child. pid 0 now means "my process group" (POSIX),
         * which would skip orphans reparented here from other groups. */
        scheduler_wait((usize)-1, &status);
        scheduler_yield();
      }
    }
  }
  return 0;
}

void user_address_space_cleanup(struct task *t) {
  if (!t) return;

  /* Release this task's shm bookkeeping (shm_nattch + per-process attach slot)
   * before we unmap its VMAs. This is the single teardown chokepoint for every
   * path that drops a user address space — voluntary exit (via the reaper),
   * signal kill (OOM-killer, also via the reaper) and execve image replacement
   * — so a SIGKILL'd process no longer leaks its attach forever. The unmap
   * loop below frees the (refcounted) shared frames. */
  {
    extern void shm_account_exit(usize pid);
    shm_account_exit(t->id);
  }

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
      if (vma->node->inode && vma->node->inode->mmap_close_cb)
        vma->node->inode->mmap_close_cb(vma->node);
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
  /* The reuse tracker must cover EVERY page of the image: a shared page can sit
   * anywhere (e.g. Mesa's .data/.init_array share a page ~0x2ac5000, thousands
   * of pages in). A fixed cap silently stopped recording, so the second
   * segment re-zeroed the shared page and wiped the first segment's data
   * (.init_array -> NULL ctor -> crash at rip=0). Size it to the total page
   * count instead. */
  usize total_pages = 0;
  for (usize i = 0; i < image->segment_count; i++) {
    struct user_image_segment *s = &image->segments[i];
    u64 vs = s->vaddr & ~(PAGE_SIZE - 1);
    u64 ve = (s->vaddr + s->memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    total_pages += (ve - vs) / PAGE_SIZE;
  }
  if (!total_pages)
    total_pages = 1;
  u64 *mapped_va = kzalloc(sizeof(u64) * total_pages);
  u64 *mapped_frame = kzalloc(sizeof(u64) * total_pages);
  if (!mapped_va || !mapped_frame) {
    kfree(mapped_va);
    kfree(mapped_frame);
    return -ENOMEM;
  }
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
        if (!frame) {
          kfree(mapped_va);
          kfree(mapped_frame);
          return -ENOMEM;
        }
        u64 flags = VMM_USER | VMM_WRITABLE;
        vmm_map_page(v, frame, flags);
        memset((void *)(usize)(direct_base + frame), 0, PAGE_SIZE);
        mapped_va[n_mapped] = v;
        mapped_frame[n_mapped] = frame;
        n_mapped++;
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

  kfree(mapped_va);
  kfree(mapped_frame);

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

  /* Lowest high-VA region reserved so far (bottom of the stack growth window);
   * lowered to the TLS region when present. The signal trampoline is mapped one
   * page below this, where the upward-growing brk heap cannot reach it. */
  u64 low_reserved = image->address_space.stack_top - USER_STACK_MAX_SIZE;

#if defined(__x86_64__) || defined(__i386__)
  /* Main-thread TLS (x86 variant II). Layout: [ tdata | tbss ][ TCB ], with the
   * thread pointer (TP) at the TCB and TCB[0] = TP (the self pointer that
   * `mov %fs:0` (x86_64) / `mov %gs:0` (i686) reads). Thread-local variables
   * live at negative offsets from TP. arch_set_fs_base() abstracts the register
   * (FS MSR on x86_64, a GS GDT entry on i686). Only binaries with a PT_TLS
   * segment need this; others keep the base 0 (set at exec). */
  if (image->tls_memsz > 0) {
    u64 align = image->tls_align < 8 ? 8 : image->tls_align;
    u64 tls_size = (image->tls_memsz + align - 1) & ~(align - 1);
    u64 tcb_size = 64; /* self pointer (+ spare slots), zero-initialised */
    u64 block_size = tls_size + tcb_size;
    /* Place just below the stack growth window, clear of heap and stack. */
    u64 region = (image->address_space.stack_top - USER_STACK_MAX_SIZE -
                  block_size - PAGE_SIZE) &
                 ~(PAGE_SIZE - 1);
    low_reserved = region;
    u64 tp = region + tls_size;
    u64 db = vmm_direct_map_base();

    for (u64 v = region; v < region + block_size; v += PAGE_SIZE) {
      u64 frame = pmm_alloc_frame();
      if (!frame)
        return -ENOMEM;
      vmm_map_page(v, frame, VMM_USER | VMM_WRITABLE);
      u64 direct_v = db + frame;
      memset((void *)(usize)direct_v, 0, PAGE_SIZE);

      /* Copy any tdata bytes that fall in this page (tbss stays zero). */
      if (image->tls_data && image->tls_filesz) {
        u64 copy_start = region > v ? region : v;
        u64 tdata_end = region + image->tls_filesz;
        u64 copy_end = tdata_end < v + PAGE_SIZE ? tdata_end : v + PAGE_SIZE;
        if (copy_end > copy_start)
          memcpy((void *)(usize)(direct_v + (copy_start - v)),
                 (char *)image->tls_data + (copy_start - region),
                 (usize)(copy_end - copy_start));
      }
      /* Write the self pointer at TP[0] if TP lands in this page. */
      if (tp >= v && tp < v + PAGE_SIZE)
        *(u64 *)(usize)(direct_v + (tp - v)) = tp;
    }

    struct vm_area *tls_vma = kzalloc(sizeof(struct vm_area));
    if (tls_vma) {
      tls_vma->start = region;
      tls_vma->end = region + block_size;
      tls_vma->prot = PROT_READ | PROT_WRITE;
      tls_vma->flags = MAP_PRIVATE | MAP_ANONYMOUS;
      tls_vma->next = current_task->vma_list;
      current_task->vma_list = tls_vma;
    }

    task_set_tls_base(current_task, tp);
    {
      extern void arch_set_fs_base(u64 base);
      arch_set_fs_base(tp);
    }
  }
#endif

  /* M42: kernel-owned signal-return trampoline. Map a read-only, executable
   * page one page below the highest reserved region, holding a tiny
   * `mov $SYS_SIGRETURN, %eax; <syscall>` stub. arch_build_signal_frame points
   * the signal handler's return address here instead of trusting a
   * userspace-supplied sa_restorer — the page is not writable, so userspace
   * cannot tamper with the return path (matters for setuid). */
  {
    u64 tva = (low_reserved - PAGE_SIZE) & ~(PAGE_SIZE - 1);
    u64 tframe = pmm_alloc_frame();
    if (tframe) {
      vmm_map_page(tva, tframe, VMM_USER); /* RO (no WRITABLE) + executable */
      u8 *code = (u8 *)(usize)(vmm_direct_map_base() + tframe);
      memset(code, 0, PAGE_SIZE);
      code[0] = 0xB8; /* mov $imm32, %eax */
      code[1] = (u8)(SYS_SIGRETURN & 0xff);
      code[2] = (u8)((SYS_SIGRETURN >> 8) & 0xff);
      code[3] = (u8)((SYS_SIGRETURN >> 16) & 0xff);
      code[4] = (u8)((SYS_SIGRETURN >> 24) & 0xff);
#ifdef __x86_64__
      code[5] = 0x0F;
      code[6] = 0x05; /* syscall */
#else
      code[5] = 0xCD;
      code[6] = 0x80; /* int $0x80 */
#endif
      struct vm_area *tvma = kzalloc(sizeof(struct vm_area));
      if (tvma) {
        tvma->start = tva;
        tvma->end = tva + PAGE_SIZE;
        tvma->prot = PROT_READ | PROT_EXEC;
        tvma->flags = MAP_PRIVATE | MAP_ANONYMOUS;
        tvma->next = current_task->vma_list;
        current_task->vma_list = tvma;
      }
      image->sigreturn_trampoline = tva;
    }
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
  /* arch_fpu_init_current() resets the LIVE FPU to the masked ABI default but
   * leaves this task's fpu_state save-area untouched — and for a fresh slot
   * that area is still the kzalloc'd zero, i.e. FCW=0x0000 (all x87 exceptions
   * UNMASKED). A later fork()'s memcpy(child, parent, sizeof(struct task)) then
   * copies that zero buffer (plus fpu_initialized=1) into the child, so the
   * child restores an unmasked control word and traps with #MF on the first
   * x87 op that observes a pending flag — e.g. libm nearbyint's fldenv, hit by
   * V8 TurboFan tier-up. Flush the clean state into the save-area now so it
   * always holds the real masked 0x037F. */
  {
    extern void arch_fpu_save(void *area);
    if (current_task) {
      arch_fpu_save(current_task->fpu_state);
      current_task->fpu_initialized = 1;
    }
  }

  x86_user_jump((usize)image->entry, (usize)image->address_space.stack_base,
                (usize)image->argc,
                (usize)(image->address_space.stack_base + sizeof(usize)));

  return 0; // Should not reach here
}

static void user_process_thread(void *arg) {
  struct process_start *start = arg;
  struct user_loaded_image *image = start ? start->image : 0;

  scheduler_set_user_image(image);
  kfree(start);

  int code = 0;
  if (image->kind == USER_IMAGE_ELF64 || image->kind == USER_IMAGE_ELF32) {
    /* Real ELF (both 64- and 32-bit) enters ring 3 via x86_user_jump; its
     * entry is a userspace virtual address, NOT a kernel function pointer. */
    code = user_run_elf_image(image);
  } else {
    user_program_entry entry = (user_program_entry)(usize)image->entry;
    code = entry(image->argc, image->argv);
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

  /* Thread name = the executable's basename, truncated to 15 chars (Linux
   * TASK_COMM_LEN-1). This is the process "comm" that /proc/<pid>/stat and
   * /proc/<pid>/comm expose and that BusyBox procps (ps/pidof/pgrep/pkill)
   * match on. Truncating the full PATH instead (e.g. "/opt/busybox/bin/busybox"
   * -> "/opt/busybox/bi") yields a useless comm "bi" and breaks process lookup
   * by name; take the basename first. */
  char safe_name[16];
  const char *base = strrchr(path, '/');
  base = base ? base + 1 : path;
  if (!*base) /* path ended in '/', fall back to the whole string */
    base = path;
  usize plen = strlen(base);
  if (plen > 15)
    plen = 15;
  memcpy(safe_name, base, plen);
  safe_name[plen] = '\0';

  /* Real userspace ELF processes may run on Application Processors: they enter
   * ring 3 and so release the Big Kernel Lock. Builtins run in kernel mode and
   * stay on the BSP (ap_runnable=0).
   *
   * 32-bit caveat: an AP that picks up a freshly forked ELF32 child wedges the
   * suite (M27's fork()+waitpid) — the child's first ring-3 syscall on the AP
   * can't make progress against the BKL while the parent is blocked in
   * waitpid, so the parent never reaps it. The 32-bit AP/BKL hand-off is not
   * yet safe for ordinary userspace, so ELF32 processes stay on the BSP; APs
   * still run stealable kernel workers (M24b work-stealing). ELF64 (x86_64)
   * keeps running userspace on APs. */
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
  int interp_level = 0;
  struct vfs_node *node;

resolve:
  node = vfs_find_node(path);
  if (!node || IS_ERR(node))
    return node ? (int)PTR_ERR(node) : -ENOENT;

  /* POSIX: Check execute permission */
  const struct cred *cred = scheduler_get_current_cred();
  if (vfs_get_node_perm(node, cred, 1) != 1) {
    vfs_node_put(node);
    return -EACCES;
  }

  u16 file_mode = node->inode->mode;
  u16 file_uid = node->inode->uid;
  u16 file_gid = node->inode->gid;

  /* POSIX `#!` interpreter files: rewrite the exec into
   * "interpreter [optional-arg] script-path argv[1..]". One level only —
   * an interpreter that is itself a script is ENOEXEC. Previously a direct
   * execve() of a script failed at the ELF loader and scripts only ran
   * because the userspace shell retried them via /bin/sh. */
  if (node->inode->type == VFS_FILE) {
    char head[128];
    isize hn = 0;
    if (node->inode->read_cb) {
      hn = node->inode->read_cb(node, 0, head, sizeof(head) - 1, 0);
    } else if (node->inode->data) {
      hn = node->inode->size < sizeof(head) - 1 ? (isize)node->inode->size
                                                : (isize)(sizeof(head) - 1);
      memcpy(head, node->inode->data, (usize)hn);
    }
    if (hn >= 2 && head[0] == '#' && head[1] == '!') {
      vfs_node_put(node);
      if (interp_level++ > 0)
        return -ENOEXEC;
      head[hn] = '\0';
      char *line = head + 2;
      while (*line == ' ' || *line == '\t')
        line++;
      char *nl = line;
      while (*nl && *nl != '\n' && *nl != '\r')
        nl++;
      *nl = '\0';
      if (*line == '\0')
        return -ENOEXEC;
      char *opt = 0;
      char *sp = line;
      while (*sp && *sp != ' ' && *sp != '\t')
        sp++;
      if (*sp) {
        *sp = '\0';
        opt = sp + 1;
        while (*opt == ' ' || *opt == '\t')
          opt++;
        if (*opt == '\0')
          opt = 0;
      }
      usize argc_old = 0;
      if (argv)
        while (argv[argc_old])
          argc_old++;
      usize tail = argc_old > 0 ? argc_old - 1 : 0;
      usize extra = opt ? 3 : 2;
      char **nargv = kmalloc((extra + tail + 1) * sizeof(char *));
      if (!nargv)
        return -ENOMEM;
      usize k = 0;
      nargv[k++] = strdup(line);
      if (opt)
        nargv[k++] = strdup(opt);
      nargv[k++] = strdup(path);
      for (usize a = 1; a < argc_old; a++)
        nargv[k++] = strdup(argv[a]);
      nargv[k] = 0;
      free_kernel_array((char **)argv);
      argv = (const char **)nargv;
      path = nargv[0];
      goto resolve;
    }
  }

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
  /* The new image is committed: from here on, a parent's setpgid() on this
   * (child) task must fail with EACCES (POSIX). */
  scheduler_mark_execed_current();
  task_set_tls_base(current_task, 0);
  {
    extern void arch_set_fs_base(u64 base);
    arch_set_fs_base(0);
  }

  int code = 0;
  if (image->kind == USER_IMAGE_ELF64 || image->kind == USER_IMAGE_ELF32) {
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
