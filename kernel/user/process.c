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

extern void x86_user_jump(u64 entry, u64 stack, u64 argc, u64 argv);
extern struct task *current_task;

#define MAX_PROGRAMS 64
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
  char tmp[1024];

  if (!copy)
    return -1;
  if (src) {
    for (; count < max_count; count++) {
      const char *ptr = 0;
      /* Check if src is in kernel space (high addresses) */
      if ((u64)src >= 0xffff800000000000) {
        ptr = src[count];
      } else {
        if (syscall_copyin(&ptr, src + count, sizeof(ptr)) != 0)
          return -1;
      }

      if (!ptr)
        break;

      /* Check if ptr is in kernel space */
      if ((u64)ptr >= 0xffff800000000000) {
        strncpy(tmp, ptr, sizeof(tmp));
        tmp[sizeof(tmp) - 1] = '\0';
      } else {
        if (syscall_copyinstr(tmp, sizeof(tmp), ptr) != 0)
          return -1;
      }

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

  /* FIX: Ensure the final stack pointer will be 16-byte aligned.
   * We push: argc(1), argv(argc), NULL(1), envp(envc), NULL(1), auxv(5).
   * Total slots = 1 + image->argc + 1 + envc + 1 + 5 = image->argc + envc + 9.
   * If total_slots is odd, we need 8 bytes of padding to keep 16-byte alignment.
   */
  usize total_slots = 9 + (usize)image->argc + (usize)envc;
  if (total_slots % 2 != 0) {
    user_stack_push_u64(stack, &sp, 0);
  }

  user_stack_push_u64(stack, &sp, 0); /* AT_NULL */
  user_stack_push_u64(stack, &sp, 9); /* AT_ENTRY */
  user_stack_push_u64(stack, &sp, image->entry);
  user_stack_push_u64(stack, &sp,
                      3); /* AT_PHDR */
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
  if (!node || IS_ERR(node)) {
    return -1;
  }
  if (node->inode->type != VFS_FILE || node->inode->size == 0) {
    return -1;
  }

  int fd = vfs_open(path);
  if (fd < 0) {
    return -1;
  }

  char *data = kmalloc(node->inode->size);
  if (!data) {
    vfs_close(fd);
    return -1;
  }

  usize total_read = 0;
  while (total_read < node->inode->size) {
    isize got = vfs_read(fd, data + total_read, node->inode->size - total_read);
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

  if (total_read != node->inode->size) {
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
  image->entry = ehdr->e_entry;
  image->address_space = user_address_space_create();

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
    if (phdr->p_offset + phdr->p_filesz > file_size ||
        phdr->p_filesz > phdr->p_memsz) {
      kfree(file_data);
      return -1;
    }

    struct user_image_segment *segment =
        &image->segments[image->segment_count++];
    segment->vaddr = phdr->p_vaddr;
    segment->memsz = phdr->p_memsz;
    segment->filesz = phdr->p_filesz;
    segment->flags = phdr->p_flags;

    /* FIX: Allocate only p_filesz to avoid huge memory allocations for .bss */
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

  kfree(file_data);
  return image->segment_count > 0 ? 0 : -1;
}

void user_image_free(struct user_loaded_image *image) {
  if (!image)
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
}

static struct user_loaded_image *user_load_image(const char *path, int argc,
                                                 const char **argv,
                                                 const char **envp) {
  struct user_loaded_image *image = kzalloc(sizeof(*image));
  if (!image)
    return 0;

  console_write("user_load_image: allocated image=");
  console_write_hex64((u64)(usize)image);
  console_write(" path=");
  console_write(path ? path : "null");
  console_write("\n");

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
    console_write("user_load_image: loaded ELF64 entry=");
    console_write_hex64(image->entry);
    console_write("\n");
    return image;
  }

  const struct user_program *program = user_find_program(path);
  if (program) {
    image->kind = USER_IMAGE_BUILTIN;
    image->path = kernel_strdup(path);
    image->entry = (u64)(usize)program->entry;
    image->address_space = user_address_space_create();
    if (user_build_initial_stack(image) != 0)
      return 0;
    console_write("user_load_image: loaded BUILTIN kind=");
    console_write_hex64(image->kind);
    console_write(" entry=");
    console_write_hex64(image->entry);
    console_write(" path=");
    console_write(image->path);
    console_write("\n");
    return image;
  }

  console_write("user_load_image: failed to load\n");
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
}

static int user_run_elf_image(struct user_loaded_image *image) {
  /* FIX: Clear old VMAs if this is an execve image replacement */
  if (current_task) {
    user_address_space_cleanup(current_task);
  }

  console_write("user_run_elf: entry=");
  console_write_hex64(image->entry);
  console_write("\n");
  int compat_code = 0;
  if (user_try_run_b1nxexec_image(image, &compat_code))
    return compat_code;

  /* Map segments into the address space */
  for (usize i = 0; i < image->segment_count; i++) {
    struct user_image_segment *segment = &image->segments[i];
    u64 vaddr_start = segment->vaddr & ~(PAGE_SIZE - 1);
    u64 vaddr_end =
        (segment->vaddr + segment->memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    u64 direct_base = vmm_direct_map_base();
    for (u64 v = vaddr_start; v < vaddr_end; v += PAGE_SIZE) {
      u64 frame = pmm_alloc_frame();
      if (!frame)
        return -ENOMEM;

      u64 flags = VMM_USER | VMM_WRITABLE; // Simplify for now
      vmm_map_page(v, frame, flags);

      /* Clear page to avoid leaking kernel data and to handle .bss */
      u64 direct_v = direct_base + frame;
      memset((void *)(usize)direct_v, 0, PAGE_SIZE);

      /* Copy data if within filesz */
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
    stack_vma->start = stack_aligned;
    stack_vma->end = image->address_space.stack_top;
    stack_vma->prot = PROT_READ | PROT_WRITE;
    stack_vma->flags = MAP_PRIVATE | MAP_ANONYMOUS;
    stack_vma->next = current_task->vma_list;
    current_task->vma_list = stack_vma;
  }

  /* Loader check: Verify entry point content */
  for (usize i = 0; i < image->segment_count; i++) {
    struct user_image_segment *segment = &image->segments[i];
    if (image->entry >= segment->vaddr &&
        image->entry < segment->vaddr + segment->memsz) {
      u64 offset = image->entry - segment->vaddr;
      if (offset < segment->filesz && segment->data) {
        u8 *ptr = (u8 *)segment->data + offset;
        console_write("Loader check: First 4 bytes at entry (0x");
        console_write_hex64(image->entry);
        console_write("): ");
        for (int j = 0; j < 4; j++) {
          console_write_hex64(ptr[j]);
          console_write(" ");
        }
        console_write("\n");
      }
    }
  }

  x86_user_jump(image->entry, image->address_space.stack_base, (u64)image->argc,
                image->address_space.stack_base + sizeof(u64));

  return 0; // Should not reach here
}

static void user_process_thread(void *arg) {
  struct process_start *start = arg;
  struct user_loaded_image *image = start ? start->image : 0;

  console_write("user_process_thread: start=");
  console_write_hex64((u64)(usize)start);
  console_write(" image=");
  console_write_hex64((u64)(usize)image);
  console_write("\n");

  console_write("user_process_thread: name=");
  console_write((image && image->path) ? image->path : "null");
  console_write(" kind=");
  console_write_hex64(image ? image->kind : 999);
  console_write(" entry=");
  console_write_hex64(image ? image->entry : 999);
  console_write("\n");

  scheduler_set_user_image(image);
  kfree(start);

  int code = 0;
  if (image->kind == USER_IMAGE_ELF64) {
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

  /* FIX: Truncate thread name to 15 chars to prevent kthread_create from
   * failing */
  char safe_name[16];
  usize plen = strlen(path);
  if (plen > 15)
    plen = 15;
  memcpy(safe_name, path, plen);
  safe_name[plen] = '\0';

  int tid = kthread_create(safe_name, user_process_thread, start);
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
  vfs_node_put(node);

  struct user_loaded_image *image = user_load_image(path, 0, argv, envp);
  if (!image)
    return -1;

  vfs_close_on_exec();

  // POSIX: Reset caught signals to default action across execve.
  // Ignored signals (SIG_IGN) remain ignored.
  for (int sig = 1; sig < NSIG; sig++) {
    if (current_task->sigactions[sig].sa_handler != SIG_IGN) {
      current_task->sigactions[sig].sa_handler = SIG_DFL;
    }
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
  if (program_count >= MAX_PROGRAMS) {
    klog_warn("too many builtin user programs, skipping registration");
    return;
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
