#include <string.h>
#include <b1nix/console.h>
#include <b1nix/mm.h>
#include <b1nix/panic.h>
#include <b1nix/sched.h>
#include <b1nix/syscall.h>
#include <b1nix/user.h>
#include <b1nix/vfs.h>
#include <b1nix/errno.h>

extern void x86_user_jump(u64 entry, u64 stack, u64 argc, u64 argv);

#define MAX_PROGRAMS 64
#define ELF_MAGIC0 0x7f
#define ELF_MAGIC1 'E'
#define ELF_MAGIC2 'L'
#define ELF_MAGIC3 'F'
#define ELF_CLASS_64 2
#define ELF_DATA_LE 1
#define ELF_TYPE_EXEC 2
#define ELF_MACHINE_X86_64 0x3e
#define ELF_MACHINE_AARCH64 0xb7
#define PT_LOAD 1

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

static struct user_program programs[MAX_PROGRAMS];
static usize program_count;

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
                              const char ***out, int *out_count) {
  const char **copy = kzalloc(sizeof(char *) * (max_count + 1));
  int count = 0;
  char tmp[256];

  if (!copy)
    return -1;
  if (src) {
    for (; count < max_count; count++) {
      const char *user_string = 0;
      if (syscall_copyin(&user_string, src + count, sizeof(user_string)) != 0)
        return -1;
      if (!user_string)
        break;
      if (syscall_copyinstr(tmp, sizeof(tmp), user_string) != 0)
        return -1;
      copy[count] = kernel_strdup(tmp);
      if (!copy[count])
        return -1;
    }
  }
  copy[count] = 0;
  *out = copy;
  if (out_count)
    *out_count = count;
  return 0;
}

static void user_stack_push_u64(char *stack, usize *sp, u64 value) {
  *sp -= sizeof(u64);
  memcpy(stack + *sp, &value, sizeof(value));
}

static u64 user_stack_push_string(char *stack, usize *sp, const char *text) {
  usize len = strlen(text) + 1;
  *sp -= len;
  memcpy(stack + *sp, text, len);
  return USER_STACK_TOP - USER_STACK_SIZE + *sp;
}

static int user_build_initial_stack(struct user_loaded_image *image) {
  char *stack = image->address_space.stack_image;
  u64 argv_ptrs[USER_MAX_ARGS];
  u64 envp_ptrs[USER_MAX_ENVS];
  usize sp = USER_STACK_SIZE;

  if (!stack)
    return -1;

  for (int i = image->argc - 1; i >= 0; i--) {
    argv_ptrs[i] = user_stack_push_string(stack, &sp, image->argv[i]);
  }

  int envc = 0;
  if (image->envp) {
    for (; envc < USER_MAX_ENVS && image->envp[envc]; envc++)
      ;
  }
  for (int i = envc - 1; i >= 0; i--) {
    envp_ptrs[i] = user_stack_push_string(stack, &sp, image->envp[i]);
  }

  sp &= ~(usize)0xf;
  user_stack_push_u64(stack, &sp, 0); /* AT_NULL */
  user_stack_push_u64(stack, &sp, 9); /* AT_ENTRY */
  user_stack_push_u64(stack, &sp, image->entry);
  user_stack_push_u64(stack, &sp,
                      3); /* AT_PHDR, populated virtually for debuggers */
  user_stack_push_u64(stack, &sp, 0);

  user_stack_push_u64(stack, &sp, 0);
  for (int i = envc - 1; i >= 0; i--) {
    user_stack_push_u64(stack, &sp, envp_ptrs[i]);
  }

  user_stack_push_u64(stack, &sp, 0);
  for (int i = image->argc - 1; i >= 0; i--) {
    user_stack_push_u64(stack, &sp, argv_ptrs[i]);
  }
  user_stack_push_u64(stack, &sp, (u64)image->argc);

  image->address_space.stack_base = USER_STACK_TOP - USER_STACK_SIZE + sp;
  image->address_space.stack_size =
      USER_STACK_TOP - image->address_space.stack_base;
  return 0;
}

static int user_image_read_vfs_file(const char *path, char **out_data,
                                    usize *out_size) {
  struct vfs_node *node = vfs_find_node(path);
  if (!node || node->type != VFS_FILE || node->size == 0)
    return -1;

  /* Enforce executable permission (at least one 'x' bit) */
  if ((node->mode & 0111) == 0) {
    return -1;
  }

  int fd = vfs_open(path);
  if (fd < 0)
    return -1;

  char *data = kmalloc(node->size);
  if (!data) {
    vfs_close(fd);
    return -1;
  }

  isize got = vfs_read(fd, data, node->size);
  vfs_close(fd);
  if (got < 0 || (usize)got != node->size)
    return -1;

  *out_data = data;
  *out_size = node->size;
  return 0;
}

static int user_load_elf64(struct user_loaded_image *image, const char *path) {
  char *file_data = 0;
  usize file_size = 0;
  if (user_image_read_vfs_file(path, &file_data, &file_size) != 0)
    return -1;
  if (file_size < sizeof(struct elf64_ehdr))
    return -1;

  struct elf64_ehdr *ehdr = (struct elf64_ehdr *)file_data;
  if (ehdr->e_ident[0] != ELF_MAGIC0 || ehdr->e_ident[1] != ELF_MAGIC1 ||
      ehdr->e_ident[2] != ELF_MAGIC2 || ehdr->e_ident[3] != ELF_MAGIC3) {
    return -1;
  }
  if (ehdr->e_ident[4] != ELF_CLASS_64 || ehdr->e_ident[5] != ELF_DATA_LE)
    return -1;
  if (ehdr->e_type != ELF_TYPE_EXEC)
    return -1;
  if (ehdr->e_machine != ELF_MACHINE_X86_64 &&
      ehdr->e_machine != ELF_MACHINE_AARCH64)
    return -1;
  if (ehdr->e_phoff + ((u64)ehdr->e_phentsize * ehdr->e_phnum) > file_size)
    return -1;

  image->kind = USER_IMAGE_ELF64;
  image->path = kernel_strdup(path);
  image->entry = ehdr->e_entry;
  image->address_space = user_address_space_create();

  for (u16 i = 0; i < ehdr->e_phnum; i++) {
    struct elf64_phdr *phdr =
        (struct elf64_phdr *)(file_data + ehdr->e_phoff +
                              ((u64)i * ehdr->e_phentsize));
    if (phdr->p_type != PT_LOAD)
      continue;
    if (image->segment_count >= USER_MAX_IMAGE_SEGMENTS)
      return -1;
    if (phdr->p_offset + phdr->p_filesz > file_size ||
        phdr->p_filesz > phdr->p_memsz)
      return -1;

    struct user_image_segment *segment =
        &image->segments[image->segment_count++];
    segment->vaddr = phdr->p_vaddr;
    segment->memsz = phdr->p_memsz;
    segment->filesz = phdr->p_filesz;
    segment->flags = phdr->p_flags;
    segment->data = kzalloc(phdr->p_memsz);
    if (!segment->data)
      return -1;
    memcpy(segment->data, file_data + phdr->p_offset, phdr->p_filesz);
  }

  return image->segment_count > 0 ? 0 : -1;
}

void user_image_free(struct user_loaded_image *image) {
  if (!image) return;

  if (image->path) kfree((void *)image->path);

  if (image->argv) {
    for (int i = 0; i < image->argc; i++) {
      if (image->argv[i]) kfree((void *)image->argv[i]);
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
}

static struct user_loaded_image *user_load_image(const char *path, int argc,
                                                 const char **argv,
                                                 const char **envp) {
  struct user_loaded_image *image = kzalloc(sizeof(*image));
  if (!image)
    return 0;

  if (copy_string_vector(argv, USER_MAX_ARGS, &image->argv, &image->argc) != 0)
    return 0;
  if (argc > 0 && image->argc > argc)
    image->argc = argc;
  if (copy_string_vector(envp, USER_MAX_ENVS, &image->envp, 0) != 0)
    return 0;

  if (image->argc == 0) {
    const char **default_argv = image->argv;
    default_argv[0] = kernel_strdup(path);
    default_argv[1] = 0;
    image->argc = 1;
  }

  if (user_load_elf64(image, path) == 0) {
    if (user_build_initial_stack(image) != 0)
      return 0;
    return image;
  }

  const struct user_program *program = user_find_program(path);
  if (!program)
    return 0;
  image->kind = USER_IMAGE_BUILTIN;
  image->path = kernel_strdup(path);
  image->entry = (u64)(usize)program->entry;
  image->address_space = user_address_space_create();
  user_build_initial_stack(image);
  return image;
}

static int user_try_run_b1nxexec_image(struct user_loaded_image *image, int *code)
{
  for (usize i = 0; i < image->segment_count; i++) {
    struct user_image_segment *segment = &image->segments[i];
    const char *payload = segment->data;
    if (segment->filesz < 10 || memcmp(payload, "B1NXEXEC", 9) != 0) continue;

    const char *op = payload + 9;
    if (strcmp(op, "echo") == 0) {
      const char *message = op + strlen(op) + 1;
      syscall_dispatch(SYS_WRITE, (u64)(usize)message, strlen(message), 0, 0);
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

static int user_run_elf_image(struct user_loaded_image *image) {
  int compat_code = 0;
  if (user_try_run_b1nxexec_image(image, &compat_code)) return compat_code;

  /* Map segments into the address space */
  for (usize i = 0; i < image->segment_count; i++) {
    struct user_image_segment *segment = &image->segments[i];
    u64 vaddr_start = segment->vaddr & ~(PAGE_SIZE - 1);
    u64 vaddr_end = (segment->vaddr + segment->memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    
    for (u64 v = vaddr_start; v < vaddr_end; v += PAGE_SIZE) {
      u64 frame = pmm_alloc_frame();
      if (!frame) return -ENOMEM;
      
      u64 flags = VMM_USER | VMM_WRITABLE; // Simplify for now
      vmm_map_page(v, frame, flags);
      
      /* Copy data if within filesz */
      u64 page_offset = 0;
      if (v < segment->vaddr) page_offset = segment->vaddr - v;
      
      if (v + page_offset < segment->vaddr + segment->filesz) {
        u64 chunk_offset = (v + page_offset) - segment->vaddr;
        u64 chunk_size = segment->filesz - chunk_offset;
        if (chunk_size > PAGE_SIZE - page_offset) chunk_size = PAGE_SIZE - page_offset;
        
        memcpy((void *)(usize)(v + page_offset), (char *)segment->data + chunk_offset, chunk_size);
      }
    }
  }

  /* Map stack */
  u64 stack_start = image->address_space.stack_top - image->address_space.stack_size;
  u64 stack_aligned = stack_start & ~(PAGE_SIZE - 1);
  for (u64 v = stack_aligned; v < image->address_space.stack_top; v += PAGE_SIZE) {
    u64 frame = pmm_alloc_frame();
    vmm_map_page(v, frame, VMM_USER | VMM_WRITABLE);
    u64 image_base = image->address_space.stack_top - image->address_space.stack_image_size;
    u64 offset = v - image_base;
    if (offset < image->address_space.stack_image_size) {
      u64 chunk = image->address_space.stack_image_size - offset;
      if (chunk > PAGE_SIZE) chunk = PAGE_SIZE;
      memcpy((void *)(usize)v, (char *)image->address_space.stack_image + offset, chunk);
    }
  }

  x86_user_jump(image->entry, image->address_space.stack_base, (u64)image->argc, (u64)image->argv);
  
  return 0; // Should not reach here
}

static void user_process_thread(void *arg) {
  struct process_start *start = arg;
  struct user_loaded_image *image = start->image;
  
  scheduler_set_user_image(image);
  kfree(start);

  int code = 0;
  if (image->kind == USER_IMAGE_ELF64) {
    code = user_run_elf_image(image);
  } else {
    user_program_entry entry = (user_program_entry)(usize)image->entry;
    code = entry(image->argc, image->argv);
  }
  syscall_dispatch(SYS_EXIT, (u64)code, 0, 0, 0);

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
  const char *empty_env[] = {"PATH=/bin", 0};
  struct user_loaded_image *image =
      user_load_image(path, argc, argv, empty_env);
  if (!image) {
    return -1;
  }

  struct process_start *start = kzalloc(sizeof(*start));
  if (!start) {
    user_image_free(image);
    return -1;
  }
  start->image = image;

  int tid = kthread_create(path, user_process_thread, start);
  if (tid < 0) {
    kfree(start);
    user_image_free(image);
    return -1;
  }
  return tid;
}

int user_execve_current(const char *path, const char **argv,
                        const char **envp) {
  struct user_loaded_image *image = user_load_image(path, 0, argv, envp);
  if (!image)
    return -1;

  vfs_close_on_exec();

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
  if (program_count >= MAX_PROGRAMS) {
    klog_warn("too many builtin user programs, skipping registration");
    return;
  }

  programs[program_count].path = path;
  programs[program_count].entry = entry;
  program_count++;
}

const struct user_program *user_find_program(const char *path) {
  for (usize i = 0; i < program_count; i++) {
    if (strcmp(programs[i].path, path) == 0) {
      return &programs[i];
    }
  }

  return 0;
}
