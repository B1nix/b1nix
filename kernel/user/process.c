#include <b1nix/arch.h>
#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/linux_abi.h>
#include <b1nix/mm.h>
#include <b1nix/panic.h>
#include <b1nix/ptrace.h>
#include <b1nix/sched.h>
#include <b1nix/syscall.h>
#include <b1nix/user.h>
#include <b1nix/vfs.h>
#include <stdio.h>
#include <string.h>

extern void x86_user_jump(usize entry, usize stack, usize argc, usize argv);
extern void arch_fpu_init_current(void); /* reset FPU/MXCSR to ABI default */

/* All user programs are Ring 3 ELFs loaded from VFS. The legacy C-level
 * built-in program registry (user_register_program / user_program_entry)
 * has been retired. */
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

/* ELF program-header p_flags bits. The demand-paged loader keys on PF_W: a
 * read-only segment can be shared from the page cache, a writable one cannot. */
#define PF_X 0x1
#define PF_W 0x2
#define PF_R 0x4

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
#define DT_INIT_ARRAY 25
#define DT_INIT_ARRAYSZ 27

/* b1nix-private auxv type carrying the shared-library constructor descriptor
 * table (see user_build_initial_stack / crt0). Far above the standard auxv
 * range (0..51) so it can never collide. Must match userspace/include/sys/auxv.h. */
#define AT_B1NIX_DSO_INIT 0x1000

/* Userspace ld.so support: fixed load base for a real ELF interpreter (musl's
 * ld-musl-x86_64.so.1). Placed above the eager-linker's DT_NEEDED range
 * (0x600000000000 .. 0x600000000000 + DYN_MAX_OBJECTS*0x10000000000) so the
 * two loading paths can never collide, though they are mutually exclusive
 * per-image in practice. */
#define USER_LDSO_LOAD_BASE 0x0000700000000000ULL

/* M92: ELF auxiliary vector types (from <elf.h>). Defined here so the kernel
 * loader can populate the initial stack without depending on userspace headers. */
#define AT_NULL     0
#define AT_PHDR     3
#define AT_PHENT    4
#define AT_PHNUM    5
#define AT_PAGESZ   6
#define AT_BASE     7
#define AT_ENTRY    9
#define AT_CLKTCK  17
#define AT_UID     11
#define AT_EUID    12
#define AT_GID     13
#define AT_EGID    14
#define AT_HWCAP   16
#define AT_SECURE  23
#define AT_RANDOM  25
#define AT_EXECFN  31
#define DT_JMPREL  23
#define DT_PLTREL  20

/* M30 — x86-64 ELF64 relocation types (subset). */
#define R_X86_64_NONE     0
#define R_X86_64_64       1
#define R_X86_64_COPY     5
#define R_X86_64_GLOB_DAT 6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE 8
#define R_X86_64_DTPMOD64 16
#define R_X86_64_DTPOFF64 17
#define R_X86_64_TPOFF64  18

/* ELF64 dynamic-section entry and RELA relocation record. Used by the in-kernel
 * RELATIVE-relocation pass for no-interp PIE binaries (see user_load_elf64). */
struct elf64_dyn { i64 d_tag; u64 d_val; };
struct elf64_rela { u64 r_offset; u64 r_info; i64 r_addend; };

static u8 *_vaddr_to_stage(struct user_loaded_image *image, u64 va, usize n) {
  if (!image) return 0;
  for (usize i = 0; i < image->segment_count; i++) {
    struct user_image_segment *seg = &image->segments[i];
    if (!seg->data) continue;
    if (va >= seg->vaddr && (va + n) <= (seg->vaddr + seg->memsz) && va + n >= va) {
      return (u8 *)seg->data + (va - seg->vaddr);
    }
  }
  return 0;
}

/* M30 — base address at which PIE/ET_DYN images get loaded. Picked well
 * above the standard 0x400000 ET_EXEC load base and below the user
 * stack top (0x800000000000) so a PIE binary and the existing static
 * binaries don't collide. */
#ifdef __x86_64__
#define PIE_LOAD_BASE 0x0000500000000000ULL
#else
#define PIE_LOAD_BASE 0x40000000ULL
#endif

/* M71 ASLR: per-exec load base for PIE/ET_DYN images. Opt-in via the
 * `b1nix.aslr` kernel cmdline flag (default off — conservative, the loader is a
 * load-bearing green path). When on, the base is shifted by a 2 MiB-granular
 * random offset (alignment-preserving; PIE_LOAD_BASE is 2 MiB-aligned) within a
 * bounded window above PIE_LOAD_BASE. 15 bits of entropy × 2 MiB = up to ~64 GiB
 * of jitter — isolated between the upward-growing mmap arena (fills from 4 GiB,
 * V8's ~1.4 TiB reservations stay far below) and the shared-library region at
 * 0x600000000000 (1 TiB above the base). ET_EXEC images (load_base 0 — the
 * fixed-base toolchain/V8/rustc links) are never randomized. */
static u64 aslr_pie_base(void) {
#ifdef __x86_64__
  if (bootinfo_has_flag("b1nix.aslr")) {
    u64 slots = kernel_random_u64() & 0x7fff; /* 15 bits */
    return PIE_LOAD_BASE + slots * 0x200000ULL;
  }
#endif
  return PIE_LOAD_BASE;
}

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

struct elf64_shdr {
  u32 sh_name;
  u32 sh_type;
  u64 sh_flags;
  u64 sh_addr;
  u64 sh_offset;
  u64 sh_size;
  u32 sh_link;
  u32 sh_info;
  u64 sh_addralign;
  u64 sh_entsize;
} __attribute__((packed));

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

/* Userspace ld.so support (musl-port.md, part 1).
 * Loads a real ELF interpreter's (musl's ld-musl-x86_64.so.1) own PT_LOAD
 * segments verbatim at `base`, WITHOUT eager symbol resolution/relocation:
 * the interpreter is a standard ET_DYN that self-relocates (applies its own
 * R_X86_64_RELATIVE entries) as the very first thing it does at its entry
 * point, exactly as on Linux. Returns the interpreter's absolute entry VA in
 * *out_entry. Unlike elf64_load_shared_object, this does NOT call
 * elf64_parse_dynamic — the in-kernel eager linker plays no further part once
 * control transfers to a real interpreter. */
static int elf64_load_interpreter(struct user_loaded_image *image,
                                  const char *path, u64 base, u64 *out_entry)
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
  for (u16 i = 0; i < ehdr->e_phnum; i++) {
    const struct elf64_phdr *ph =
        (const struct elf64_phdr *)(data + ehdr->e_phoff +
                                    (u64)i * ehdr->e_phentsize);
    if (ph->p_type != PT_LOAD)
      continue;
    if (image->segment_count >= USER_MAX_IMAGE_SEGMENTS ||
        ph->p_filesz > ph->p_memsz || ph->p_offset + ph->p_filesz > size) {
      kfree(data);
      return -1;
    }
    struct user_image_segment *seg =
        &image->segments[image->segment_count++];
    seg->vaddr = base + ph->p_vaddr;
    seg->memsz = ph->p_memsz;
    seg->filesz = ph->p_filesz;
    seg->flags = ph->p_flags;
    seg->file_offset = ph->p_offset;
    seg->demand_ok = 0;
    seg->data = kzalloc(ph->p_memsz ? ph->p_memsz : 1);
    if (!seg->data) {
      kfree(data);
      return -1;
    }
    if (ph->p_filesz)
      memcpy(seg->data, data + ph->p_offset, ph->p_filesz);
  }
  *out_entry = base + ehdr->e_entry;
  kfree(data);
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

  /* M92: Push the AT_EXECFN string data BEFORE the 16-byte alignment so its
   * variable-length payload doesn't break the alignment for the auxv pairs.
   * Record its user VA; the auxv entry pushes only the pointer and type. */
  usize execfn_va = 0;
  if (image->path) {
    execfn_va = user_stack_push_string(stack, &sp, image->path);
    if (execfn_va == 0) return -1;
  }

  sp &= ~(usize)0xf;

  /* Ensure the final stack pointer (which becomes ESP/RSP at _start) is
   * 16-byte aligned, as the SysV ABI requires. We are about to push exactly
   * total_slots pointer-sized words: argc(1), argv(argc), NULL(1), envp(envc),
   * NULL(1), auxv(6+). sp is 16-aligned right now, so after the pushes it stays
   * 16-aligned iff total_slots*sizeof(usize) is a multiple of 16. Pad with as
   * many zero words as needed to reach the next 16-byte boundary. */
  usize total_slots = 35 + (usize)image->argc + (usize)envc;
  usize words_per_16 = 16 / sizeof(usize);
  usize rem = total_slots % words_per_16;
  usize pad = rem ? (words_per_16 - rem) : 0;
  if (sp < pad * sizeof(usize)) return -1;
  sp -= pad * sizeof(usize);

  usize rand_va = 0;
  {
    u64 rand_bytes[2];
    rand_bytes[0] = kernel_random_u64();
    rand_bytes[1] = kernel_random_u64();
    if (sp < sizeof(rand_bytes)) return -1;
    sp -= sizeof(rand_bytes);
    memcpy(stack + sp, rand_bytes, sizeof(rand_bytes));
    rand_va = USER_STACK_TOP - USER_STACK_SIZE + sp;
  }

  /* Auxiliary vector. Pushed high-address-first as {a_val, a_type} pairs so that
   * — read forwards from the low end, where getauxval() starts after the envp
   * NULL — each entry appears in the ABI's {a_type, a_val} order and AT_NULL
   * terminates the array at the high end. */
  usize auxv_end_sp = sp; /* high end of the auxv block, for /proc/<pid>/auxv */
  if (user_stack_push_usize(stack, &sp, 0) < 0) return -1; /* AT_NULL  a_val */
  if (user_stack_push_usize(stack, &sp, 0) < 0) return -1; /* AT_NULL  a_type */
  if (user_stack_push_usize(stack, &sp, (usize)image->phdr_vaddr) < 0) return -1;
  if (user_stack_push_usize(stack, &sp, AT_PHDR) < 0) return -1;
  if (user_stack_push_usize(stack, &sp, 56) < 0) return -1;                    /* AT_PHENT = sizeof(Elf64_Phdr) */
  if (user_stack_push_usize(stack, &sp, AT_PHENT) < 0) return -1;
  if (user_stack_push_usize(stack, &sp, (usize)image->phnum) < 0) return -1;
  if (user_stack_push_usize(stack, &sp, AT_PHNUM) < 0) return -1;
  /* AT_ENTRY is always the EXECUTABLE's own entry point (what a real ld.so
   * jumps to once it's done linking), not the interpreter's — image->entry
   * only becomes the interpreter's entry (see PT_INTERP handling) as the
   * process's initial IP, which is separate from this auxv value. */
  if (user_stack_push_usize(
          stack, &sp,
          (usize)(image->interp_base ? image->app_entry : image->entry)) < 0)
    return -1;
  if (user_stack_push_usize(stack, &sp, AT_ENTRY) < 0) return -1;
  if (user_stack_push_usize(stack, &sp, PAGE_SIZE) < 0) return -1;
  if (user_stack_push_usize(stack, &sp, AT_PAGESZ) < 0) return -1;
  /* AT_BASE: the interpreter's own load bias, so its self-relocation and
   * dl_iterate_phdr math agree with where the kernel actually placed it.
   * 0 for images with no real userspace interpreter (unchanged behaviour). */
  if (user_stack_push_usize(stack, &sp, (usize)image->interp_base) < 0) return -1;
  if (user_stack_push_usize(stack, &sp, AT_BASE) < 0) return -1;
  if (user_stack_push_usize(stack, &sp, 100) < 0) return -1;                   /* AT_CLKTCK = 100 Hz tick */
  if (user_stack_push_usize(stack, &sp, AT_CLKTCK) < 0) return -1;
  if (user_stack_push_usize(stack, &sp, (usize)current_task->cred->uid) < 0) return -1;
  if (user_stack_push_usize(stack, &sp, AT_UID) < 0) return -1;
  if (user_stack_push_usize(stack, &sp, (usize)current_task->cred->euid) < 0) return -1;
  if (user_stack_push_usize(stack, &sp, AT_EUID) < 0) return -1;
  if (user_stack_push_usize(stack, &sp, (usize)current_task->cred->gid) < 0) return -1;
  if (user_stack_push_usize(stack, &sp, AT_GID) < 0) return -1;
  if (user_stack_push_usize(stack, &sp, (usize)current_task->cred->egid) < 0) return -1;
  if (user_stack_push_usize(stack, &sp, AT_EGID) < 0) return -1;
  /* AT_RANDOM points to the payload reserved above the auxv. */
  if (user_stack_push_usize(stack, &sp, rand_va) < 0) return -1;
  if (user_stack_push_usize(stack, &sp, AT_RANDOM) < 0) return -1;
  if (user_stack_push_usize(stack, &sp, 0) < 0) return -1;                     /* AT_HWCAP = 0 */
  if (user_stack_push_usize(stack, &sp, AT_HWCAP) < 0) return -1;
  if (user_stack_push_usize(stack, &sp, 0) < 0) return -1;                     /* AT_SECURE = 0 */
  if (user_stack_push_usize(stack, &sp, AT_SECURE) < 0) return -1;
  /* AT_EXECFN: program filename for /proc/self/exe. String data was pushed
   * earlier (before alignment); just push the pointer and type here. */
  if (user_stack_push_usize(stack, &sp, execfn_va) < 0) return -1;
  if (user_stack_push_usize(stack, &sp, AT_EXECFN) < 0) return -1;

  /* The auxv block now spans [sp, auxv_end_sp) in this staging buffer; record
   * the equivalent user VA + length so /proc/<pid>/auxv can read it back. */
  image->auxv_vaddr = USER_STACK_TOP - USER_STACK_SIZE + sp;
  image->auxv_size = (u32)(auxv_end_sp - sp);

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

/* Read exactly `n` bytes at absolute file offset `off` from an open fd into
 * `buf`. The streaming ELF loader (user_load_elf64) uses this to copy each
 * PT_LOAD segment straight from the file into its staging buffer WITHOUT ever
 * holding a whole-file copy in the kernel heap. That halves the per-exec load
 * transient — a 94 MB clang previously needed file_data (94 MB) + the staged
 * segments (~94 MB) resident simultaneously (~188 MB), which is exactly what
 * made the low-RAM self-host (e.g. 512 MB) thrash the page-cache evictor. Now
 * only the ~94 MB of staging is live. Handles short reads and the non-blocking
 * EAGAIN the VFS can return. Returns 0 on success, -1 otherwise. */
static int user_read_at(int fd, u64 off, void *buf, usize n) {
  if (n == 0)
    return 0;
  if (vfs_lseek(fd, (isize)off, 0 /* SEEK_SET */) < 0)
    return -1;
  usize done = 0;
  char *p = (char *)buf;
  while (done < n) {
    isize got = vfs_read(fd, p + done, n - done);
    if (got < 0) {
      if (got == -EAGAIN || got == -EWOULDBLOCK) {
        scheduler_yield();
        continue;
      }
      return -1;
    }
    if (got == 0)
      return -1; /* unexpected EOF inside a declared segment */
    done += (usize)got;
  }
  return 0;
}

/* M40 — decide whether an ELF64 image is a Linux binary. Two independent
 * signals, either of which is conclusive:
 *   1. EI_OSABI (e_ident[7]) == ELFOSABI_LINUX.
 *   2. A PT_NOTE program header containing a GNU NT_GNU_ABI_TAG note whose first
 *      descriptor word (the OS) is GNU_ABI_OS_LINUX. This is what glibc-linked
 *      static Linux binaries carry; b1nix freestanding binaries never emit it.
 * Returns 1 for a Linux binary, 0 otherwise. The streaming loader passes the fd
 * and the in-memory phdr table; each PT_NOTE segment is read from the file into
 * a small bounded buffer, so a crafted note table cannot drive an OOB read. */
static int elf64_is_linux_binary(int fd, const struct elf64_ehdr *ehdr,
                                 const struct elf64_phdr *phdrs) {
  if (ehdr->e_ident[EI_OSABI] == ELFOSABI_LINUX)
    return 1;

  /* b1nix's userspace is musl, and this musl port uses the Linux x86_64 syscall
   * numbers (arch_prctl=158, clone=56, exit=60, ...). Any binary that requests
   * the musl program interpreter therefore speaks the Linux ABI and must run
   * with PERSONALITY_LINUX so those numbers are translated — regardless of its
   * EI_OSABI byte, which b1nix's own clang/lld toolchain leaves at SYSV(0) with
   * no GNU ABI-tag note. Without this, ld.so's __init_tp() gets -ENOSYS from an
   * untranslated arch_prctl(ARCH_SET_FS) and deliberately executes `hlt`, which
   * #GPs in ring 3. */
  for (u16 i = 0; i < ehdr->e_phnum; i++) {
    const struct elf64_phdr *ph = &phdrs[i];
    if (ph->p_type != PT_INTERP)
      continue;
    char interp[64];
    u64 ilen = ph->p_filesz < sizeof(interp) ? ph->p_filesz : sizeof(interp);
    if (ilen == 0 || user_read_at(fd, ph->p_offset, interp, (usize)ilen) != 0)
      continue;
    interp[ilen - 1] = '\0'; /* the on-disk string is NUL-terminated */
    if (strcmp(interp, "/lib/ld-musl-x86_64.so.1") == 0)
      return 1;
  }

  for (u16 i = 0; i < ehdr->e_phnum; i++) {
    const struct elf64_phdr *ph = &phdrs[i];
    if (ph->p_type != PT_NOTE)
      continue;
    /* GNU ABI-tag notes are tiny; read a bounded prefix of the note segment
     * into a stack buffer and walk it. A note past this prefix cannot be the
     * leading ABI tag, and the OSABI check above is the primary signal. */
    unsigned char nb[512];
    u64 end = ph->p_filesz < sizeof(nb) ? ph->p_filesz : sizeof(nb);
    if (end < 12)
      continue;
    if (user_read_at(fd, ph->p_offset, nb, (usize)end) != 0)
      continue;
    /* Walk the note records: [namesz(4)][descsz(4)][type(4)][name, 4-aligned]
     * [desc, 4-aligned]. Offsets are relative to the buffer start. */
    u64 off = 0;
    while (off + 12 <= end) {
      u32 namesz = *(const u32 *)(nb + off);
      u32 descsz = *(const u32 *)(nb + off + 4);
      u32 type = *(const u32 *)(nb + off + 8);
      u64 name_off = off + 12;
      u64 name_pad = ((u64)namesz + 3) & ~3ULL;
      u64 desc_off = name_off + name_pad;
      u64 desc_pad = ((u64)descsz + 3) & ~3ULL;
      u64 next = desc_off + desc_pad;
      if (next < off || next > end) /* malformed / wrap / past buffer */
        break;
      if (type == NT_GNU_ABI_TAG && namesz == 4 && descsz >= 4 &&
          name_off + 4 <= end &&
          nb[name_off] == 'G' && nb[name_off + 1] == 'N' &&
          nb[name_off + 2] == 'U' && nb[name_off + 3] == '\0' &&
          desc_off + 4 <= end) {
        u32 os = *(const u32 *)(nb + desc_off);
        if (os == GNU_ABI_OS_LINUX)
          return 1;
      }
      off = next;
    }
  }
  return 0;
}

static int user_load_elf64(struct user_loaded_image *image, const char *path) {
  int rc = -1;
  struct elf64_phdr *phdrs = 0;
  int fd = vfs_open(path);
  if (fd < 0)
    return -1;

  /* Stream the image instead of slurping the whole file into the heap: read the
   * ELF header, then the program-header table, then copy each PT_LOAD straight
   * into its staging buffer via user_read_at(). A 94 MB clang therefore never
   * holds file_data (94 MB) + staging (~94 MB) resident at once — only the
   * staging — which is what kept the 512 MB self-host from OOMing on a single
   * ~57 MB .text segment allocation. All error paths jump to `cleanup`. */
  struct elf64_ehdr ehdr_storage;
  struct elf64_ehdr *ehdr = &ehdr_storage;
  /* These rejections used to be silent, so "failed to load" covered a short
   * read and a genuinely foreign file alike — indistinguishable in a log. */
  if (user_read_at(fd, 0, ehdr, sizeof(*ehdr)) != 0) {
    console_write("ELF load: cannot read ELF header: ");
    console_write(path);
    console_write("\n");
    goto cleanup;
  }

  if (ehdr->e_ident[0] != ELF_MAGIC0 || ehdr->e_ident[1] != ELF_MAGIC1 ||
      ehdr->e_ident[2] != ELF_MAGIC2 || ehdr->e_ident[3] != ELF_MAGIC3) {
    char line[160];
    snprintf(line, sizeof(line),
             "ELF load: bad magic %02x %02x %02x %02x in %s\n",
             ehdr->e_ident[0], ehdr->e_ident[1], ehdr->e_ident[2],
             ehdr->e_ident[3], path);
    console_write(line);
    goto cleanup;
  }
  if (ehdr->e_ident[4] != ELF_CLASS_64 || ehdr->e_ident[5] != ELF_DATA_LE)
    goto cleanup;
  if (ehdr->e_type != ELF_TYPE_EXEC && ehdr->e_type != ELF_TYPE_DYN)
    goto cleanup;
  if (ehdr->e_machine != ELF_MACHINE_X86_64 &&
      ehdr->e_machine != ELF_MACHINE_AARCH64)
    goto cleanup;
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
    goto cleanup;
  }
#else
  console_write("ELF load: ARCH MISMATCH — 64-bit binary on 32-bit kernel, "
                "refusing: ");
  console_write(path);
  console_write("\n");
  goto cleanup;
#endif
  /* phentsize must match so phdr-table reads and `&phdrs[i]` indexing are
   * well-formed; e_phnum must be non-zero for an executable. */
  if (ehdr->e_phentsize != sizeof(struct elf64_phdr) || ehdr->e_phnum == 0) {
    char line[160];
    snprintf(line, sizeof(line),
             "ELF load: bad phdr table (phentsize %u, phnum %u) in %s\n",
             (unsigned)ehdr->e_phentsize, (unsigned)ehdr->e_phnum, path);
    console_write(line);
    goto cleanup;
  }

  /* Read the program-header table. A file truncated before the declared table
   * makes user_read_at fail here — this replaces the old file_size bound. */
  phdrs = kmalloc((usize)ehdr->e_phnum * sizeof(struct elf64_phdr));
  if (!phdrs) {
    console_write("ELF load: out of memory for the phdr table: ");
    console_write(path);
    console_write("\n");
    goto cleanup;
  }
  if (user_read_at(fd, ehdr->e_phoff, phdrs,
                   (usize)ehdr->e_phnum * sizeof(struct elf64_phdr)) != 0) {
    char line[160];
    snprintf(line, sizeof(line),
             "ELF load: cannot read %u phdrs at offset %lu in %s\n",
             (unsigned)ehdr->e_phnum, (unsigned long)ehdr->e_phoff, path);
    console_write(line);
    goto cleanup;
  }

  image->kind = USER_IMAGE_ELF64;
  image->path = kernel_strdup(path);

  /* M40: tag the binary personality. A Linux binary gets its syscall numbers
   * translated at dispatch time. */
  if (elf64_is_linux_binary(fd, ehdr, phdrs)) {
    image->personality = PERSONALITY_LINUX;
    /* Compose the whole line before writing: console_write() locks/unlocks
     * per call, so multiple calls here would let another CPU's log line
     * interleave mid-sentence (observed corrupting BusyBox test markers
     * under SMP exec churn). */
    char line[VFS_MAX_PATH + 64];
    snprintf(line, sizeof(line), "ELF load: Linux personality detected: %s\n", path);
    console_write(line);
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
  u64 first_load_vaddr = 0;
  for (u16 j = 0; j < ehdr->e_phnum; j++) {
    if (phdrs[j].p_type == PT_LOAD) {
      first_load_vaddr = phdrs[j].p_vaddr;
      break;
    }
  }
  u64 load_base = (ehdr->e_type == ELF_TYPE_DYN && first_load_vaddr == 0) ? aslr_pie_base() : 0;

  image->entry = ehdr->e_entry + load_base;
  /* M92: record program header location for AT_PHDR / AT_PHNUM auxv.
   * For ET_DYN (PIE), segments are 0-based so load_base + e_phoff is correct.
   * For ET_EXEC, e_phoff is a file offset — find the LOAD segment that maps it
   * and compute the actual VA: segment_p_vaddr + (e_phoff - segment_p_offset). */
  /* Default assumes a 0-based PIE (first LOAD maps file offset 0 at vaddr 0),
   * but that is NOT universally true: b1nix's own PIE binaries are linked at a
   * fixed non-zero base (0x2000000, see userspace/linker.ld), so e_phoff (a file
   * offset) does not equal the phdrs' virtual-address offset. Resolve the real
   * VA by locating the PT_LOAD that contains e_phoff and mapping through it —
   * for BOTH ET_EXEC and ET_DYN. For a genuine 0-based PIE the first LOAD has
   * p_offset==0 && p_vaddr==0, so this reduces to load_base + e_phoff. If AT_PHDR
   * is wrong, a real ld.so (musl) never finds PT_DYNAMIC in the app's phdrs, so
   * app.dynv stays NULL and decode_dyn() faults reading *(NULL). */
  image->phdr_vaddr = load_base + ehdr->e_phoff;
  for (u16 j = 0; j < ehdr->e_phnum; j++) {
    struct elf64_phdr *seg = &phdrs[j];
    if (seg->p_type != PT_LOAD) continue;
    if (ehdr->e_phoff >= seg->p_offset &&
        ehdr->e_phoff < seg->p_offset + seg->p_filesz) {
      image->phdr_vaddr = load_base + seg->p_vaddr + (ehdr->e_phoff - seg->p_offset);
      break;
    }
  }
  image->phnum = ehdr->e_phnum;
  {
    char line[VFS_MAX_PATH + 64];
    if (load_base)
      snprintf(line, sizeof(line), "ELF load: %s entry=0x%lx (PIE base=0x%lx)\n",
               path, (unsigned long)image->entry, (unsigned long)load_base);
    else
      snprintf(line, sizeof(line), "ELF load: %s entry=0x%lx\n", path,
               (unsigned long)image->entry);
    console_write(line);
  }
  image->address_space = user_address_space_create();

  /* PT_INTERP: load a real userspace dynamic linker for binaries that name
   * /lib/ld-musl-x86_64.so.1. This file IS the ld.so — musl's libc.so
   * doubles as the dynamic linker (entry _dlstart). The kernel loads its
   * segments unrelocated, jumps to _dlstart, which self-relocates and then
   * loads/links the executable via syscalls.
   *
   * The old in-kernel eager linker (used for the legacy /lib/ld-b1nix.so
   * PT_INTERP) is gone — every rootfs binary is musl-linked now. Any other
   * PT_INTERP value just fails to load below ("failed to load interpreter"). */
  for (u16 j = 0; j < ehdr->e_phnum; j++) {
    struct elf64_phdr *p = &phdrs[j];
    if (p->p_type != PT_INTERP) continue;
    char interp[64];
    usize ilen = p->p_filesz < sizeof(interp) ? p->p_filesz
                                               : sizeof(interp) - 1;
    if (user_read_at(fd, p->p_offset, interp, ilen) != 0)
      continue;
    interp[ilen] = '\0';
    /* Compose each outcome into one buffer and issue a single console_write:
     * building "ELF load: PT_INTERP=..." across several calls left a window
     * where another CPU's log line could interleave mid-sentence (this exact
     * line was observed corrupting the concurrent "M53-HTTPD: ready" marker
     * under SMP exec churn). */
    char line[128 + sizeof(interp)];
    u64 interp_entry = 0;
    usize seg_count_before = image->segment_count;
    /* Try the absolute path first (real / mount), then /mnt/root (test-mode
     * ram0 mount — matches how initramfs-era boot stages find rootfs libs). */
    int interp_rc = elf64_load_interpreter(image, interp, USER_LDSO_LOAD_BASE,
                                           &interp_entry);
    if (interp_rc != 0) {
      image->segment_count = seg_count_before; /* undo partial segments */
      char alt[80];
      usize ilen2 = strlen(interp);
      if (ilen2 + 10 <= sizeof(alt)) {
        memcpy(alt, "/mnt/root", 9);
        memcpy(alt + 9, interp, ilen2 + 1);
        interp_rc = elf64_load_interpreter(image, alt, USER_LDSO_LOAD_BASE,
                                           &interp_entry);
      }
    }
    if (interp_rc != 0) {
      snprintf(line, sizeof(line), "ELF load: PT_INTERP=%s (failed to load interpreter)\n", interp);
      console_write(line);
      goto cleanup;
    }
    image->app_entry = image->entry;
    image->interp_base = USER_LDSO_LOAD_BASE;
    {
      usize il = strlen(interp);
      if (il >= sizeof(image->interp_path))
        il = sizeof(image->interp_path) - 1;
      memcpy(image->interp_path, interp, il);
      image->interp_path[il] = '\0';
    }
    image->entry = interp_entry;
    snprintf(line, sizeof(line), "ELF load: PT_INTERP=%s (userspace ld.so, base=0x%lx)\n",
             interp, (unsigned long)USER_LDSO_LOAD_BASE);
    console_write(line);
    break; /* at most one PT_INTERP */
  }

  /* Demand-paging eligibility (self-host RAM floor). An ET_EXEC (load_base == 0)
   * backed by a real filesystem (its inode has a read_cb, so pages can be read
   * back on a fault) has its read-only PT_LOAD segments faulted in lazily from
   * the page cache instead of pinned in private frames — so the 94 MB self-host
   * clang's resident set is its touched working set, not the whole binary. The
   * dynamic linker still stages every segment (to read symtab/strtab + patch the
   * GOT/PLT); user_run_elf_image then maps the read-only ones file-backed and
   * frees their staging. PIE/ET_DYN (load_base != 0) is excluded — its read-only
   * segments get relocated. initramfs (no read_cb) stays eager. Guards: every
   * PT_LOAD page-congruent; no RO/RW page sharing; the reloc pass clears
   * demand_ok for any segment it writes into. */
  int demand_page = (load_base == 0);
  for (u16 i = 0; demand_page && i < ehdr->e_phnum; i++) {
    struct elf64_phdr *a = &phdrs[i];
    if (a->p_type != PT_LOAD)
      continue;
    if ((a->p_vaddr & (PAGE_SIZE - 1)) != (a->p_offset & (PAGE_SIZE - 1))) {
      demand_page = 0;
      break;
    }
    u64 a_lo = a->p_vaddr & ~(PAGE_SIZE - 1);
    u64 a_hi = (a->p_vaddr + a->p_memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    for (u16 j = 0; j < ehdr->e_phnum; j++) {
      struct elf64_phdr *b = &phdrs[j];
      if (j == i || b->p_type != PT_LOAD)
        continue;
      if (((a->p_flags ^ b->p_flags) & PF_W) == 0)
        continue;
      u64 b_lo = b->p_vaddr & ~(PAGE_SIZE - 1);
      u64 b_hi = (b->p_vaddr + b->p_memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
      if (a_lo < b_hi && b_lo < a_hi) {
        demand_page = 0;
        break;
      }
    }
  }
  if (demand_page) {
    struct vfs_node *en = vfs_find_node(path);
    if (!en || IS_ERR(en) || !en->inode || !en->inode->read_cb) {
      if (en && !IS_ERR(en))
        vfs_node_put(en);
      demand_page = 0; /* initramfs / no backing -> eager */
    } else {
      image->exe_node = en;
    }
  }
  image->demand_paged = demand_page;

  /* First pass: PT_LOAD segments. Vaddrs are offset by `load_base` for
   * PIE; this also offsets any later relocation targets we compute. */
  for (u16 i = 0; i < ehdr->e_phnum; i++) {
    struct elf64_phdr *phdr = &phdrs[i];
    if (phdr->p_type != PT_LOAD)
      continue;
    if (image->segment_count >= USER_MAX_IMAGE_SEGMENTS)
      goto cleanup;
    if (phdr->p_filesz > phdr->p_memsz)
      goto cleanup;
    u64 reloc_vaddr = phdr->p_vaddr + load_base;
    if (reloc_vaddr + phdr->p_memsz < reloc_vaddr ||
        reloc_vaddr + phdr->p_memsz > 0x00007FFFFFFFFFFFULL)
      goto cleanup;
    /* Cap p_memsz before kzalloc: the project rule is OOM = panic, so a crafted
     * multi-GiB p_memsz (well under the vaddr ceiling above) would turn a merely
     * unloadable file into a kernel panic (R4-10). */
    if (phdr->p_memsz > USER_IMAGE_MAX_SEGMENT)
      goto cleanup;

    struct user_image_segment *segment =
        &image->segments[image->segment_count++];
    segment->vaddr = reloc_vaddr;
    segment->memsz = phdr->p_memsz;
    segment->filesz = phdr->p_memsz;
    segment->flags = phdr->p_flags;
    segment->file_offset = phdr->p_offset;
    segment->demand_ok = (u8)(demand_page && !(phdr->p_flags & PF_W) &&
                              phdr->p_filesz == phdr->p_memsz &&
                              (phdr->p_vaddr & (PAGE_SIZE - 1)) == 0);

    /* Relocations may target .bss or the zero-filled tail of a PT_LOAD, so the
     * staging image must cover p_memsz, not only the bytes present on disk. */
    if (phdr->p_memsz > 0) {
      segment->data = kzalloc(phdr->p_memsz);
      if (!segment->data)
        goto cleanup;
      /* Copy the on-disk bytes straight from the file into staging; the tail
       * (p_memsz - p_filesz) stays zero for .bss. A segment that claims more
       * file bytes than exist makes user_read_at fail (short read / EOF). */
      if (phdr->p_filesz &&
          user_read_at(fd, phdr->p_offset, segment->data, phdr->p_filesz) != 0)
        goto cleanup;
    } else {
      segment->data = 0;
    }
  }

  /* Capture the PT_TLS template so user_run_elf_image can set up the main
   * thread's TLS block + FS base. Without this, a binary that uses __thread /
   * std::call_once (e.g. Mesa via util_call_once_data_slow's `mov %fs:0,%rax`)
   * derefs a zero FS base and SIGSEGVs before main does any real work. */
  for (u16 i = 0; i < ehdr->e_phnum; i++) {
    struct elf64_phdr *phdr = &phdrs[i];
    if (phdr->p_type != PT_TLS)
      continue;
    if (phdr->p_filesz > phdr->p_memsz ||
        phdr->p_memsz > USER_IMAGE_MAX_SEGMENT)
      break; /* malformed — leave tls_memsz 0 (no TLS) */
    image->tls_memsz = phdr->p_memsz;
    image->tls_filesz = phdr->p_filesz;
    image->tls_align = phdr->p_align ? phdr->p_align : 8;
    if (phdr->p_filesz) {
      image->tls_data = kmalloc(phdr->p_filesz);
      if (image->tls_data &&
          user_read_at(fd, phdr->p_offset, image->tls_data, phdr->p_filesz) !=
              0) {
        kfree(image->tls_data);
        image->tls_data = 0;
      }
    }
    break; /* at most one PT_TLS */
  }

  /* Dynamic relocations are handled in userspace (ld-musl) for every binary
   * that names a PT_INTERP. A no-interp PIE (ET_DYN with no interpreter — the
   * M30 pie smoke, and any freestanding -pie binary) has nobody else to do it:
   * its .data pointer tables still hold link-time (0-based) addresses, so the
   * first dereference faults. Apply the one relocation type such a binary can
   * emit, R_X86_64_RELATIVE (*target = load_base + addend), into the staged
   * segments before they are mapped. */
  if (load_base != 0 && image->interp_base == 0) {
    for (u16 i = 0; i < ehdr->e_phnum; i++) {
      struct elf64_phdr *phdr = &phdrs[i];
      if (phdr->p_type != PT_DYNAMIC)
        continue;
      usize dyn_count = phdr->p_filesz / sizeof(struct elf64_dyn);
      struct elf64_dyn *dyn = (struct elf64_dyn *)_vaddr_to_stage(
          image, phdr->p_vaddr + load_base, phdr->p_filesz);
      if (!dyn)
        break;
      u64 rela_vaddr = 0, rela_size = 0, rela_ent = sizeof(struct elf64_rela);
      for (usize d = 0; d < dyn_count && dyn[d].d_tag != DT_NULL; d++) {
        if (dyn[d].d_tag == DT_RELA)
          rela_vaddr = dyn[d].d_val;
        else if (dyn[d].d_tag == DT_RELASZ)
          rela_size = dyn[d].d_val;
        else if (dyn[d].d_tag == DT_RELAENT && dyn[d].d_val)
          rela_ent = dyn[d].d_val;
      }
      if (!rela_vaddr || rela_size < rela_ent ||
          rela_ent < sizeof(struct elf64_rela))
        break;
      u8 *rela = _vaddr_to_stage(image, rela_vaddr + load_base, rela_size);
      if (!rela)
        break;
      for (u64 off = 0; off + sizeof(struct elf64_rela) <= rela_size;
           off += rela_ent) {
        struct elf64_rela *r = (struct elf64_rela *)(rela + off);
        if ((r->r_info & 0xFFFFFFFFULL) != R_X86_64_RELATIVE)
          continue;
        u64 *target =
            (u64 *)_vaddr_to_stage(image, r->r_offset + load_base, sizeof(u64));
        if (!target)
          continue;
        *target = load_base + (u64)r->r_addend;
      }
      break; /* at most one PT_DYNAMIC */
    }
  }

  rc = image->segment_count > 0 ? 0 : -1;

cleanup:
  kfree(phdrs);
  if (fd >= 0)
    vfs_close(fd);
  return rc;
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

  /* Drop the demand-paging backing-file reference held for the image lifetime.
   * Each demand-paged segment VMA took its own vfs_node_get (released in
   * user_address_space_cleanup); this is the loader's own reference. */
  if (image->exe_node)
    vfs_node_put(image->exe_node);

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
  /* M92: record program header location for AT_PHDR / AT_PHNUM auxv. */
  image->phdr_vaddr = load_base + ehdr->e_phoff;
  image->phnum = ehdr->e_phnum;
  {
    char line[VFS_MAX_PATH + 64];
    if (load_base)
      snprintf(line, sizeof(line), "ELF32 load: %s entry=0x%lx (PIE base=0x%lx)\n",
               path, (unsigned long)image->entry, (unsigned long)load_base);
    else
      snprintf(line, sizeof(line), "ELF32 load: %s entry=0x%lx\n", path,
               (unsigned long)image->entry);
    console_write(line);
  }
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
    char line[64 + sizeof(interp)];
    snprintf(line, sizeof(line),
             "ELF32 load: PT_INTERP=%s (b1nix applies RELATIVE relocs in-kernel — no separate ld.so handoff)\n",
             interp);
    console_write(line);
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

  /* In-kernel dynamic relocation pass removed: dynamic relocations are handled in userspace (ld-musl). */

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

  /* Report how much memory was left: an image that loads in one boot and not in
   * another is far more often a resource failure than a malformed ELF, and the
   * bare message gave no way to tell the two apart. */
  {
    char line[128];
    snprintf(line, sizeof(line),
             "user_load_image: failed to load %s (free frames %lu)\n",
             path, (unsigned long)pmm_free_frame_count());
    console_write(line);
  }
  user_image_free(image);
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
  /* execve image replacement: release the old image's VMA/shm/swap references,
   * then install a FRESH address space and free the old one — rather than
   * re-mapping the new image in place. A task that reached here via
   * fork()+execve() is running on a COW clone of the parent's pml4; re-mapping
   * a large dynamically-linked image (clang: the executable + each .so at the
   * high 0xC0/0xC1 region + split huge-pages + TLS) over that surviving COW
   * page-table tree wedges silently. A fresh pml4 — mirroring the spawn path's
   * scheduler_set_user_image — removes the COW-residue interaction.
   * paging_free_address_space is refcount-aware, so frames still COW-shared with
   * the (blocked) parent survive. A fresh spawn (no VMAs yet;
   * scheduler_set_user_image already built a clean pml4) skips the swap to avoid
   * double-allocating. */
  if (current_task) {
    int replacing = (current_task->vma_list != NULL);
    user_address_space_cleanup(current_task);
    if (replacing) {
      u64 old_pml4 = current_task->pml4_phys;
      current_task->pml4_phys = paging_create_address_space();
      paging_switch_address_space(current_task->pml4_phys);
      if (old_pml4)
        paging_free_address_space(old_pml4);
    }
    /* A vfork parent is suspended until this point — release it now that the
     * child is running its own image. */
    scheduler_vfork_release();
  }

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

    /* Demand-paged read-only segment (survived the relocation pass): map it
     * file-backed lazy instead of pinning private frames, then free the staging.
     * The page-fault handler (Case 1) reads each touched page from the
     * executable's page cache via inode->read_cb — shared read-only and
     * refcounted across every mapper — so only the working set is resident and
     * untouched pages stay reclaimable. vmm_set_lazy splits the low-4GB identity
     * huge page covering the load base so the lazy PTE is installed correctly. */
    if (image->demand_paged && segment->demand_ok && image->exe_node &&
        segment->data) {
      u64 seg_file_base = segment->file_offset & ~(PAGE_SIZE - 1);
      for (u64 v = vaddr_start; v < vaddr_end; v += PAGE_SIZE) {
        vmm_set_lazy(v);
        /* RO + user: no VMM_WRITABLE, so the shared cache frame can't be written
         * through this mapping; the fault handler honours the saved bits. */
        paging_mprotect_page(v, VMM_USER);
      }
      struct vm_area *fvma = kzalloc(sizeof(struct vm_area));
      if (fvma) {
        fvma->start = vaddr_start;
        fvma->end = vaddr_end;
        fvma->prot = PROT_READ | ((segment->flags & PF_X) ? PROT_EXEC : 0);
        fvma->flags = MAP_PRIVATE;
        fvma->node = vfs_node_get(image->exe_node);
        fvma->offset = (isize)seg_file_base;
        fvma->next = current_task->vma_list;
        current_task->vma_list = fvma;
      }
      kfree(segment->data);
      segment->data = 0;
      continue;
    }

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
    /* The thread pointer sits at region + tls_memsz, NOT region +
     * round_up(memsz, align). The b1nix cross linker emits local-exec offsets
     * as `@tpoff = symbol_offset - p_memsz` (the UN-rounded segment size), so a
     * thread-local at template offset X is read via %fs:(X - p_memsz). Placing
     * TP at the rounded size shifts every read by the alignment padding
     * (p_align - p_memsz%p_align) whenever p_memsz is not an align multiple —
     * e.g. V8's wasm-enabled d8 has memsz=0x108, align=0x10, so a rounded TP
     * reads 8 bytes past each thread-local (garbage -> the heap-allocation
     * assert mis-reads -> snapshot deserialize aborts). For align-aligned memsz
     * (the common case) region+memsz == region+round_up, so this is a no-op
     * there. tls_size (rounded) is still used for block/region sizing. */
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
  } else {
    /* Minimal TLS/TCB for binaries without PT_TLS.  PIE/ET_DYN programs linked
     * against musl's libc.so have no PT_TLS but musl's __init_ssp reads %fs:0
     * (the thread-pointer self-pointer) for stack canary setup.  Allocate a
     * 64-byte TCB so fs:0x0 is valid.  The TP sits at the TCB and TCB[0] = TP. */
    u64 tcb_size = 64;
    u64 region = (image->address_space.stack_top - USER_STACK_MAX_SIZE -
                  tcb_size - PAGE_SIZE) &
                 ~(PAGE_SIZE - 1);
    low_reserved = region;
    u64 tp = region; /* TCB starts at region, TP == region (no tdata) */
    u64 db = vmm_direct_map_base();

    u64 frame = pmm_alloc_frame();
    if (frame) {
      vmm_map_page(region, frame, VMM_USER | VMM_WRITABLE);
      u64 direct_v = db + frame;
      memset((void *)(usize)direct_v, 0, PAGE_SIZE);
      /* Self pointer: TCB[0] = TP */
      *(u64 *)(usize)(direct_v) = tp;
    }

    struct vm_area *tls_vma = kzalloc(sizeof(struct vm_area));
    if (tls_vma) {
      tls_vma->start = region;
      tls_vma->end = region + tcb_size;
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
      /* A Linux-personality task's syscalls are number-translated, so the
       * trampoline must invoke Linux rt_sigreturn (15) — which maps back to
       * SYS_SIGRETURN — not b1nix's SYS_SIGRETURN (99), which Linux would
       * re-translate to sysinfo. */
      u32 sigret_nr = (image->personality == PERSONALITY_LINUX)
                          ? LINUX_NR_RT_SIGRETURN
                          : (u32)SYS_SIGRETURN;
      code[0] = 0xB8; /* mov $imm32, %eax */
      code[1] = (u8)(sigret_nr & 0xff);
      code[2] = (u8)((sigret_nr >> 8) & 0xff);
      code[3] = (u8)((sigret_nr >> 16) & 0xff);
      code[4] = (u8)((sigret_nr >> 24) & 0xff);
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
      /* M80: this task is about to run userspace code, which may use AVX —
       * give it an XSAVE area so the upper YMM halves survive a context
       * switch. Seeded from the clean FXSAVE image just captured. */
      task_fpu_alloc(current_task);
    }
  }

  /* User mappings may replace inherited low-4GB huge pages. Invalidate those
   * translations on every CPU before a freshly loaded image can migrate. */
  extern void tlb_shootdown_all(void);
  tlb_shootdown_all();

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
  /* All user programs are Ring 3 ELFs — no built-in fallback path remains. */
  code = user_run_elf_image(image);
  syscall_dispatch(SYS_EXIT, (u64)code, 0, 0, 0, 0, 0);

  while (1) {
    scheduler_yield();
  }
}

void userspace_init(void) {
  console_write("userspace: builtin programs retired (all binaries run as Ring 3 ELFs)\n");
}

int user_spawn(const char *path, int argc, const char **argv) {
  const char *default_env[] = {"PATH=/bin:/sbin:/usr/bin:/usr/sbin", 0};
  return user_spawn_env(path, argc, argv, default_env);
}

int user_spawn_env(const char *path, int argc, const char **argv,
                   const char **envp) {
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

  struct user_loaded_image *image =
      user_load_image(path, argc, argv, envp, 0, 0);
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

/* Full executable path of a task, for /proc/<pid>/exe. user_spawn stores only
 * the comm basename in task->name (so ps/pidof/pkill match correctly), but the
 * loaded image keeps the full path — which tools like clang's getMainExecutable
 * (and thus its cc1as re-exec for .S files) need to locate themselves. */
const char *user_task_exe_path(struct task *t) {
  if (!t)
    return 0;
  struct user_loaded_image *img = t->user_image;
  if (img && img->path && img->path[0])
    return img->path;
  return t->name; /* fallback: execve already stores the full path in name */
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
    /* -1 here reached userspace as -errno, i.e. EPERM ("operation not
     * permitted") for what is really "this image could not be loaded" — the
     * error every failed exec reported, no matter the cause. */
    return -ENOEXEC;

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
  /* M80: PTRACE_O_TRACEEXEC — report the exec to the tracer. The stop happens
   * when the new image first returns to ring 3, i.e. before its entry point
   * runs, which is where a debugger expects to regain control. */
  ptrace_event_exec(current_task);
  task_set_tls_base(current_task, 0);
  {
    extern void arch_set_fs_base(u64 base);
    arch_set_fs_base(0);
  }

  int code = user_run_elf_image(image);
  scheduler_exit_current(code);
}
