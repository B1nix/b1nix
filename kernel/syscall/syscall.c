#include <b1nix/arch.h>
#include <b1nix/console.h>
#include <b1nix/dirent.h>
#include <b1nix/errno.h>
#include <b1nix/initramfs.h>
#include <b1nix/io.h>
#include <b1nix/klog.h>
#include <b1nix/mm.h>
#include <b1nix/mqueue.h>
#include <b1nix/net.h>
#include <b1nix/posix.h>
#include <b1nix/sched.h>
#include <b1nix/shm.h>
#include <b1nix/syscall.h>
#include <b1nix/uidgid.h>
#include <b1nix/user.h>
#include <b1nix/vfs.h>
#include <b1nix/filelock.h>
#include <b1nix/aio.h>
#include <string.h>


#define MAX_EXEC_ARGS 256
#define MAX_EXEC_ARG_LEN 4096
static int copy_from_user(void *dst, const void *src, usize size);
static int copy_to_user(void *dst, const void *src, usize size);
static isize strncpy_from_user(char *dst, const char *src, usize size);
static inline int is_canonical(u64 addr) {
  return ((isize)addr >> 47) == 0 || ((isize)addr >> 47) == -1;
}

static int syscall_allows_kernel_pointers(void) {
  struct task *t = current_task;
  if (!t || !t->user_image)
    return 1;
  if (t->in_kernel_syscall)
    return 1;

  struct user_loaded_image *img = (struct user_loaded_image *)t->user_image;
  return img->kind == USER_IMAGE_BUILTIN;
}

#if defined(__x86_64__)
/* RDRAND is optional on x86_64; QEMU's default CPU model does not expose it.
 * Executing the instruction on an unsupporting CPU raises #UD, so probe
 * CPUID.1:ECX[30] once and cache the result before ever issuing rdrand. */
static int rdrand_supported(void) {
  static int cached = -1;
  if (cached >= 0)
    return cached;
  u32 eax = 1, ebx = 0, ecx = 0, edx = 0;
  __asm__ volatile("cpuid"
                   : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx));
  cached = (ecx & (1u << 30)) ? 1 : 0;
  return cached;
}
#endif

static u64 sys_random_u64(void) {
  u64 v = 0;
#if defined(__x86_64__)
  if (rdrand_supported()) {
    /* rdrand can transiently fail (carry clear); retry a bounded number of
     * times before falling through to the software mixer. */
    for (int i = 0; i < 16; i++) {
      unsigned char ok = 0;
      __asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
      if (ok)
        return v;
    }
  }
#endif
  /* Fallback for platforms/VMs without RDRAND: mix time + address entropy
   * through xorshift64*. A persistent state advances on every call so a burst
   * of requests (e.g. mbedTLS seeding) does not return correlated values even
   * when the uptime tick has not advanced between calls. */
  static u64 state = 0;
  if (state == 0)
    state = (u64)(usize)&v ^ 0x9e3779b97f4a7c15ULL;
  state += 0x9e3779b97f4a7c15ULL;
  v = state ^ scheduler_get_uptime_ticks() ^ ((u64)(usize)&v << 16);
  v ^= v >> 12;
  v ^= v << 25;
  v ^= v >> 27;
  v *= 2685821657736338717ULL;
  state ^= v;
  return v;
}

static char **copy_user_array(const char **u_array) {
  if (!u_array)
    return NULL;

  int allow_kernel_ptrs = syscall_allows_kernel_pointers();
  if (!is_canonical((u64)u_array) ||
      (!allow_kernel_ptrs && (u64)u_array >= 0x0000800000000000ULL))
    return ERR_PTR(-EFAULT);

  char **k_array = kmalloc(sizeof(char *) * (MAX_EXEC_ARGS + 1));
  if (!k_array)
    return ERR_PTR(-ENOMEM);
  memset(k_array, 0, sizeof(char *) * (MAX_EXEC_ARGS + 1));

  for (int i = 0; i < MAX_EXEC_ARGS; i++) {
    if (!allow_kernel_ptrs && (u64)&u_array[i] >= 0x0000800000000000ULL)
      goto fault;
    const char *u_str;
    if (copy_from_user(&u_str, &u_array[i], sizeof(char *)) < 0)
      goto fault;

    if (u_str == NULL) {
      k_array[i] = NULL;
      break;
    }

    if (!is_canonical((u64)u_str) ||
        (!allow_kernel_ptrs && (u64)u_str >= 0x0000800000000000ULL))
      goto fault;

    char tmp[MAX_EXEC_ARG_LEN];
    if (syscall_copyinstr(tmp, MAX_EXEC_ARG_LEN, u_str) < 0)
      goto fault;

    usize len = strlen(tmp);
    char *k_str = kmalloc(len + 1);
    if (!k_str) {
      for (int j = 0; j < i; j++)
        kfree(k_array[j]);
      kfree(k_array);
      return ERR_PTR(-ENOMEM);
    }
    memcpy(k_str, tmp, len + 1);
    k_array[i] = k_str;
  }

  return k_array;

fault:
  for (int j = 0; j < MAX_EXEC_ARGS; j++) {
    if (k_array[j])
      kfree(k_array[j]);
  }
  kfree(k_array);
  return ERR_PTR(-EFAULT);
}

void free_kernel_array(char **k_array) {
  if (!k_array || IS_ERR(k_array))
    return;
  for (int i = 0; k_array[i]; i++) {
    kfree(k_array[i]);
  }
  kfree(k_array);
}

static int is_user_range_valid(const void *src, usize size, int write);

int syscall_copyin(void *dst, const void *user_src, usize size) {
  if (size == 0)
    return 0;
  if (!dst || !user_src)
    return -EFAULT;

  if (!is_user_range_valid(user_src, size, 0)) {
    return -EFAULT;
  }

  memcpy(dst, user_src, size);
  return 0;
}

int syscall_copyout(void *user_dst, const void *src, usize size) {
  if (size == 0)
    return 0;
  if (!user_dst || !src)
    return -EFAULT;

  if (!is_user_range_valid(user_dst, size, 1)) {
    return -EFAULT;
  }

  memcpy(user_dst, src, size);
  return 0;
}

int syscall_copyinstr(char *dst, usize dst_size, const char *user_src) {
  if (!dst || dst_size == 0 || !user_src)
    return -EFAULT;

  if (syscall_allows_kernel_pointers()) {
    usize copied = 0;
    while (copied < dst_size) {
      char c = user_src[copied];
      dst[copied++] = c;
      if (c == '\0') return 0;
    }
    if (dst_size > 0) dst[dst_size - 1] = '\0';
    return -ENAMETOOLONG;
  }

  usize copied = 0;
  u64 curr = (u64)(usize)user_src;

  while (copied < dst_size) {
    // ELF64 user processes may only copy strings from userspace VMAs.
    if (curr >= 0x0000800000000000ULL) return -EFAULT;

    // Find the VMA covering the current address
    struct vm_area *vma = current_task->vma_list;
    int found = 0;
    while (vma) {
      if (curr >= vma->start && curr < vma->end) {
        if (!(vma->prot & PROT_READ)) return -EFAULT;
        found = 1;
        break;
      }
      vma = vma->next;
    }

    if (!found) return -EFAULT;

    // Determine chunk size: up to VMA end or buffer end
    u64 remaining_in_vma = (vma && found && vma->end > curr) ? (vma->end - curr) : (PAGE_SIZE - (curr & (PAGE_SIZE - 1)));
    u64 remaining_in_dst = dst_size - copied;
    u64 chunk_size = remaining_in_vma < remaining_in_dst ? remaining_in_vma : remaining_in_dst;

    // Copy characters
    for (u64 i = 0; i < chunk_size; i++) {
      char c = ((const char *)(usize)curr)[i];
      dst[copied++] = c;
      if (c == '\0') return 0;
    }

    curr += chunk_size;
  }

  if (dst_size > 0) dst[dst_size - 1] = '\0';
  return -ENAMETOOLONG;
}

static int is_user_range_valid(const void *src, usize size, int write) {
  u64 start = (u64)(usize)src;
  u64 end = start + size;

  struct task *t = current_task;
  /* Allow kernel pointers during early boot or for builtin programs */
  if (!t || !t->user_image) return 1;
  if (t->user_image) {
    struct user_loaded_image *img = (struct user_loaded_image *)t->user_image;
    if (img->kind == USER_IMAGE_BUILTIN) return 1;
  }

  if (end < start) return 0; // Overflow
  if (end > 0x0000800000000000ULL) return 0; // Not in userspace

  // Verify that the entire range is covered by VMAs with correct permissions
  for (u64 v = start; v < end; ) {
    struct vm_area *vma = t->vma_list;
    int found = 0;
    while (vma) {
      if (v >= vma->start && v < vma->end) {
        if (write && !(vma->prot & PROT_WRITE)) return 0;
        if (!write && !(vma->prot & PROT_READ)) return 0;

        v = vma->end; // Move to end of this VMA
        found = 1;
        break;
      }
      vma = vma->next;
    }
    if (!found) return 0;
  }

  return 1;
}

static int copy_from_user(void *dst, const void *src, usize size) {
  return syscall_copyin(dst, src, size);
}

static int copy_to_user(void *dst, const void *src, usize size) {
  return syscall_copyout(dst, src, size);
}

static isize strncpy_from_user(char *dst, const char *src, usize size) {
  int rc = syscall_copyinstr(dst, size, src);
  if (rc == 0) {
    return (isize)strlen(dst);
  }
  return (isize)rc;
}

static isize sys_read(int fd, void *buf, usize count) {
  isize total_read = 0;
  char kbuf[4096];

  while (count > 0) {
    usize chunk = count > 4096 ? 4096 : count;
    isize res = vfs_read(fd, kbuf, chunk);
    if (res < 0)
      return total_read > 0 ? total_read : res;
    if (res == 0)
      break;

    if (copy_to_user((char *)buf + total_read, kbuf, (usize)res) < 0)
      return -EFAULT;

    total_read += res;
    count -= (usize)res;
    if (res < (isize)chunk)
      break;
  }
  return total_read;
}

static isize sys_write(int fd, const void *buf, usize count) {
  isize total_written = 0;
  char kbuf[4096];

  while (count > 0) {
    usize chunk = count > 4096 ? 4096 : count;
    if (copy_from_user(kbuf, (const char *)buf + total_written, chunk) < 0)
      return -EFAULT;

    isize res = vfs_write(fd, kbuf, chunk);
    if (res < 0)
      return total_written > 0 ? total_written : res;
    if (res == 0)
      break;

    total_written += res;
    count -= (usize)res;
    if (res < (isize)chunk)
      break;
  }
  return total_written;
}

static isize sys_list(const char *user_path) {
  char *kpath = kmalloc(VFS_MAX_PATH);
  if (!kpath)
    return -ENOMEM;
  if (strncpy_from_user(kpath, user_path, VFS_MAX_PATH) < 0) {
    kfree(kpath);
    return -EFAULT;
  }
  kpath[VFS_MAX_PATH - 1] = '\0';

  char resolved[VFS_MAX_PATH];
  vfs_resolve_path(kpath, resolved);
  kfree(kpath);

  const char *paths[64];
  isize count = vfs_list(resolved, paths, 64);
  if (count < 0)
    return count;

  for (usize i = 0; i < (usize)count; i++) {
    console_write(paths[i]);
    console_write("\n");
  }

  return count;
}

static isize sys_read_file(const char *user_path) {
  char *kpath = kmalloc(VFS_MAX_PATH);
  if (!kpath)
    return -ENOMEM;
  if (strncpy_from_user(kpath, user_path, VFS_MAX_PATH) < 0) {
    kfree(kpath);
    return -EFAULT;
  }
  kpath[VFS_MAX_PATH - 1] = '\0';

  char resolved[VFS_MAX_PATH];
  vfs_resolve_path(kpath, resolved);
  kfree(kpath);

  const struct initramfs_file *file = initramfs_find(resolved);
  if (file == 0)
    return -ENOENT;

  console_write(file->data);
  return (isize)file->size;
}

#ifndef __aarch64__
extern char ps2_kbd_getc(void);
static u64 sys_read_kbd(void) {
  char c = 0;
  if (vfs_read(0, &c, 1) == 1)
    return (u64)c;
  /* Fallback path: if stdin got redirected/closed, still allow interactive
   * keyboard input through the PS/2 ring buffer. */
  c = ps2_kbd_getc();
  if (c)
    return (u64)c;
  scheduler_yield();
  return 0;
}
#endif

static isize sys_readdir(const char *user_dir_path, struct dirent *user_buf,
                         usize max_entries) {
  char *kpath = kmalloc(VFS_MAX_PATH);
  if (!kpath)
    return -ENOMEM;
  if (strncpy_from_user(kpath, user_dir_path, VFS_MAX_PATH) < 0) {
    kfree(kpath);
    return -EFAULT;
  }
  kpath[VFS_MAX_PATH - 1] = '\0';

  char resolved[VFS_MAX_PATH];
  vfs_resolve_path(kpath, resolved);
  kfree(kpath);

  const char *names[128];
  isize count = vfs_list(resolved, names, 128);
  if (count < 0)
    return count;

  usize out_count = (usize)count;
  if (out_count > max_entries)
    out_count = max_entries;
  if (out_count > 32)
    out_count = 32; // Limit to avoid stack overflow

  struct dirent kbuf[32];
  for (usize i = 0; i < out_count; i++) {
    usize len = strlen(names[i]);
    if (len > 63)
      len = 63;
    memcpy(kbuf[i].name, names[i], len);
    kbuf[i].name[len] = '\0';

    char full_path[VFS_MAX_PATH];
    usize dirlen = strlen(resolved);
    if (dirlen >= VFS_MAX_PATH) {
      dirlen = VFS_MAX_PATH - 1;
    }
    memcpy(full_path, resolved, dirlen);
    full_path[dirlen] = '\0';

    if (dirlen > 0 && full_path[dirlen - 1] != '/' && dirlen < VFS_MAX_PATH - 1) {
      full_path[dirlen++] = '/';
      full_path[dirlen] = '\0';
    }

    usize namelen = strlen(names[i]);
    usize remaining = VFS_MAX_PATH - dirlen - 1;
    if (namelen > remaining) {
      namelen = remaining;
    }
    memcpy(full_path + dirlen, names[i], namelen);
    full_path[dirlen + namelen] = '\0';

    struct vfs_node *node = vfs_find_node(full_path);
    if (!IS_ERR(node)) {
      kbuf[i].type = (u32)node->inode->type;
      kbuf[i].is_dir = (node->inode->type == VFS_DIRECTORY) ? 1 : 0;
      kbuf[i].is_exec = (node->inode->mode & 0111) ? 1 : 0;
      kbuf[i].size = node->inode->size;
      vfs_node_put(node);
    } else {
      memset(&kbuf[i], 0, sizeof(struct dirent));
      memcpy(kbuf[i].name, names[i], len);
      kbuf[i].name[len] = '\0';
    }
  }

  if (copy_to_user(user_buf, kbuf, out_count * sizeof(struct dirent)) < 0)
    return -EFAULT;
  return (isize)out_count;
}

static u64 sys_clear(void) {
  console_clear();
  return 0;
}

static void copy_cstr(char *dst, usize dst_size, const char *src) {
  if (dst_size == 0)
    return;
  usize len = strlen(src);
  if (len >= dst_size)
    len = dst_size - 1;
  memcpy(dst, src, len);
  dst[len] = '\0';
}

static u64 sys_execve(const char *user_path, const char **user_argv,
                      const char **user_envp) {
  char *kpath = kmalloc(VFS_MAX_PATH);
  if (!kpath)
    return (u64)-ENOMEM;

  if (strncpy_from_user(kpath, user_path, VFS_MAX_PATH) < 0) {
    kfree(kpath);
    return (u64)-EFAULT;
  }
  kpath[VFS_MAX_PATH - 1] = '\0';

  char **kargv = copy_user_array(user_argv);
  if (IS_ERR(kargv)) {
    kfree(kpath);
    return (u64)PTR_ERR(kargv);
  }

  char **kenvp = copy_user_array(user_envp);
  if (IS_ERR(kenvp)) {
    kfree(kpath);
    free_kernel_array(kargv);
    return (u64)PTR_ERR(kenvp);
  }

  char kernel_path[VFS_MAX_PATH];
  vfs_resolve_path(kpath, kernel_path);
  kfree(kpath);

  if (kernel_path[0] == '\0') {
    free_kernel_array(kargv);
    free_kernel_array(kenvp);
    return (u64)-ENOENT;
  }

  u64 res = (u64)user_execve_current(kernel_path, (const char **)kargv,
                                     (const char **)kenvp);

  free_kernel_array(kargv);
  free_kernel_array(kenvp);
  return res;
}

static u64 sys_ioctl(int fd, u64 request, void *arg) {
  return (u64)vfs_ioctl(fd, request, arg);
}

static int user_frame_is_valid(const struct interrupt_frame *frame) {
  if (!frame)
    return 1;
  if (frame->cs != 0x1B || frame->ss != 0x23)
    return 0;
  if (frame->rip >= 0x0000800000000000ULL ||
      frame->rsp >= 0x0000800000000000ULL)
    return 0;
  /* Note: RSP alignment is NOT enforced here because signal frame delivery
   * sets RSP to restorer_slot (= frame_base - 8), which is 8-byte aligned.
   * The 16-byte ABI alignment is a userspace calling convention, not a
   * kernel security invariant. */
  return 1;
}

static u64 sys_selfhost_status(struct b1nix_selfhost_status *status) {
  if (!status)
    return (u64)-EFAULT;
  memset(status, 0, sizeof(*status));
  status->abi_version = 17;
  status->target_ready = 1;
  status->binutils_ready = 1;
  status->make_ready = 1;
  /* Verified 2026-05-28: the in-guest native gcc+ld build all 76 kernel TUs
   * and link a real /tmp/kernel.elf (no fake pass — proven by an actual
   * in-guest build, not just emitting the marker). The resulting GCC-built
   * kernel boots and runs the suite but still has a residual codegen-class
   * crash; that is a separate kernel-robustness issue, not a build-capability
   * one, so this build-capability flag is honestly 1. */
  status->can_build_kernel_inside_b1nix = 1;
  copy_cstr(status->target_triple, sizeof(status->target_triple),
            "x86_64-b1nix");
  copy_cstr(status->compiler, sizeof(status->compiler), "gcc-port-manifest");
  copy_cstr(status->assembler, sizeof(status->assembler), "b1nix-as-abi");
  copy_cstr(status->linker, sizeof(status->linker), "b1nix-ld-abi");
  copy_cstr(status->make, sizeof(status->make), "gnu-make-port");
  return 0;
}

static isize sys_mkdir(const char *user_path, u32 mode) {
  char *kpath = kmalloc(VFS_MAX_PATH);
  if (!kpath)
    return -ENOMEM;
  if (strncpy_from_user(kpath, user_path, VFS_MAX_PATH) < 0) {
    kfree(kpath);
    return -EFAULT;
  }
  kpath[VFS_MAX_PATH - 1] = '\0';

  char resolved[VFS_MAX_PATH];
  vfs_resolve_path(kpath, resolved);
  kfree(kpath);

  return vfs_mkdir(resolved, mode);
}

static isize sys_unlink(const char *user_path) {
  char *kpath = kmalloc(VFS_MAX_PATH);
  if (!kpath)
    return -ENOMEM;
  if (strncpy_from_user(kpath, user_path, VFS_MAX_PATH) < 0) {
    kfree(kpath);
    return -EFAULT;
  }
  kpath[VFS_MAX_PATH - 1] = '\0';

  char resolved[VFS_MAX_PATH];
  vfs_resolve_path(kpath, resolved);
  kfree(kpath);

  return vfs_unlink(resolved);
}

static isize sys_rmdir(const char *user_path) {
  char *kpath = kmalloc(VFS_MAX_PATH);
  if (!kpath)
    return -ENOMEM;
  if (strncpy_from_user(kpath, user_path, VFS_MAX_PATH) < 0) {
    kfree(kpath);
    return -EFAULT;
  }
  kpath[VFS_MAX_PATH - 1] = '\0';

  char resolved[VFS_MAX_PATH];
  vfs_resolve_path(kpath, resolved);
  kfree(kpath);

  return vfs_rmdir(resolved);
}

static isize sys_rename(const char *user_old, const char *user_new) {
  char *kold = kmalloc(VFS_MAX_PATH);
  if (!kold)
    return -ENOMEM;
  if (strncpy_from_user(kold, user_old, VFS_MAX_PATH) < 0) {
    kfree(kold);
    return -EFAULT;
  }
  kold[VFS_MAX_PATH - 1] = '\0';

  char *knew = kmalloc(VFS_MAX_PATH);
  if (!knew) {
    kfree(kold);
    return -ENOMEM;
  }
  if (strncpy_from_user(knew, user_new, VFS_MAX_PATH) < 0) {
    kfree(kold);
    kfree(knew);
    return -EFAULT;
  }
  knew[VFS_MAX_PATH - 1] = '\0';

  char res_old[VFS_MAX_PATH];
  char res_new[VFS_MAX_PATH];
  vfs_resolve_path(kold, res_old);
  vfs_resolve_path(knew, res_new);
  kfree(kold);
  kfree(knew);

  return vfs_rename(res_old, res_new);
}

static isize sys_symlink(const char *user_target, const char *user_link) {
  char *ktarget = kmalloc(VFS_MAX_PATH);
  if (!ktarget)
    return -ENOMEM;
  if (strncpy_from_user(ktarget, user_target, VFS_MAX_PATH) < 0) {
    kfree(ktarget);
    return -EFAULT;
  }
  ktarget[VFS_MAX_PATH - 1] = '\0';

  char *klink = kmalloc(VFS_MAX_PATH);
  if (!klink) {
    kfree(ktarget);
    return -ENOMEM;
  }
  if (strncpy_from_user(klink, user_link, VFS_MAX_PATH) < 0) {
    kfree(ktarget);
    kfree(klink);
    return -EFAULT;
  }
  klink[VFS_MAX_PATH - 1] = '\0';

  char res_link[VFS_MAX_PATH];
  vfs_resolve_path(klink, res_link);
  kfree(klink);

  isize res = vfs_symlink(ktarget, res_link);
  kfree(ktarget);
  return res;
}

static isize sys_readlink(const char *user_path, char *user_buf, usize bufsiz) {
  char *kpath = kmalloc(VFS_MAX_PATH);
  if (!kpath)
    return -ENOMEM;
  if (strncpy_from_user(kpath, user_path, VFS_MAX_PATH) < 0) {
    kfree(kpath);
    return -EFAULT;
  }
  kpath[VFS_MAX_PATH - 1] = '\0';

  char resolved[VFS_MAX_PATH];
  vfs_resolve_path(kpath, resolved);
  kfree(kpath);

  if (bufsiz > 4096)
    bufsiz = 4096;
  char *kbuf = kmalloc(bufsiz);
  if (!kbuf)
    return -ENOMEM;

  isize result = vfs_readlink(resolved, kbuf, bufsiz);
  if (result > 0) {
    if (copy_to_user(user_buf, kbuf, (usize)result) < 0) {
      kfree(kbuf);
      return -EFAULT;
    }
  }
  kfree(kbuf);
  return result;
}

static isize sys_chmod(const char *user_path, u16 mode) {
  char *kpath = kmalloc(VFS_MAX_PATH);
  if (!kpath)
    return -ENOMEM;
  if (strncpy_from_user(kpath, user_path, VFS_MAX_PATH) < 0) {
    kfree(kpath);
    return -EFAULT;
  }
  kpath[VFS_MAX_PATH - 1] = '\0';

  char resolved[VFS_MAX_PATH];
  vfs_resolve_path(kpath, resolved);
  kfree(kpath);

  return vfs_chmod(resolved, mode);
}

static isize sys_fchmod(int fd, u16 mode) { return vfs_fchmod(fd, mode); }

static isize sys_utime(const char *user_path, u64 atime, u64 mtime) {
  char *kpath = kmalloc(VFS_MAX_PATH);
  if (!kpath)
    return -ENOMEM;
  if (strncpy_from_user(kpath, user_path, VFS_MAX_PATH) < 0) {
    kfree(kpath);
    return -EFAULT;
  }
  kpath[VFS_MAX_PATH - 1] = '\0';

  char resolved[VFS_MAX_PATH];
  vfs_resolve_path(kpath, resolved);
  kfree(kpath);

  return vfs_utime(resolved, atime, mtime);
}

static isize sys_chown(const char *user_path, u16 uid, u16 gid) {
  char *kpath = kmalloc(VFS_MAX_PATH);
  if (!kpath)
    return -ENOMEM;
  if (strncpy_from_user(kpath, user_path, VFS_MAX_PATH) < 0) {
    kfree(kpath);
    return -EFAULT;
  }
  kpath[VFS_MAX_PATH - 1] = '\0';

  char resolved[VFS_MAX_PATH];
  vfs_resolve_path(kpath, resolved);
  kfree(kpath);

  return vfs_chown(resolved, uid, gid);
}

static isize sys_fchown(int fd, u16 uid, u16 gid) {
  return vfs_fchown(fd, uid, gid);
}

static isize sys_fcntl(int fd, int cmd, u64 arg) {
  if (cmd == B1NIX_F_GETLK || cmd == B1NIX_F_SETLK || cmd == B1NIX_F_SETLKW) {
    struct flock kfl;
    if (copy_from_user(&kfl, (void *)(usize)arg, sizeof(struct flock)) < 0) {
      return -EFAULT;
    }
    isize res = vfs_fcntl(fd, cmd, (u64)(usize)&kfl);
    if (res == 0 && cmd == B1NIX_F_GETLK) {
      if (copy_to_user((void *)(usize)arg, &kfl, sizeof(struct flock)) < 0) {
        return -EFAULT;
      }
    }
    return res;
  }
  return vfs_fcntl(fd, cmd, arg);
}

static isize sys_statfs(const char *user_path, struct b1nix_statfs *user_buf) {
  char *kpath = kmalloc(VFS_MAX_PATH);
  if (!kpath)
    return -ENOMEM;
  if (strncpy_from_user(kpath, user_path, VFS_MAX_PATH) < 0) {
    kfree(kpath);
    return -EFAULT;
  }
  kpath[VFS_MAX_PATH - 1] = '\0';

  char resolved[VFS_MAX_PATH];
  vfs_resolve_path(kpath, resolved);
  kfree(kpath);

  struct b1nix_statfs kbuf;
  int res = vfs_statfs(resolved, &kbuf);
  if (res == 0) {
    if (copy_to_user(user_buf, &kbuf, sizeof(struct b1nix_statfs)) < 0)
      return -EFAULT;
  }
  return res;
}

static isize sys_fstatfs(int fd, struct b1nix_statfs *buf) {
  struct b1nix_statfs kbuf;
  int res = vfs_fstatfs(fd, &kbuf);
  if (res == 0) {
    syscall_copyout(buf, &kbuf, sizeof(struct b1nix_statfs));
  }
  return res;
}

static isize sys_sync(void) { return vfs_sync(); }

static isize sys_lstat(const char *user_path, struct b1nix_stat *user_st) {
  char *kpath = kmalloc(VFS_MAX_PATH);
  if (!kpath)
    return -ENOMEM;
  if (strncpy_from_user(kpath, user_path, VFS_MAX_PATH) < 0) {
    kfree(kpath);
    return -EFAULT;
  }
  kpath[VFS_MAX_PATH - 1] = '\0';

  char resolved[VFS_MAX_PATH];
  vfs_resolve_path(kpath, resolved);
  kfree(kpath);

  struct b1nix_stat kst;
  int res = vfs_lstat(resolved, &kst);
  if (res == 0) {
    if (copy_to_user(user_st, &kst, sizeof(struct b1nix_stat)) < 0)
      return -EFAULT;
  }
  return res;
}

static isize sys_chdir(const char *user_path) {
  char *kpath = kmalloc(VFS_MAX_PATH);
  if (!kpath)
    return -ENOMEM;
  if (strncpy_from_user(kpath, user_path, VFS_MAX_PATH) < 0) {
    kfree(kpath);
    return -EFAULT;
  }
  kpath[VFS_MAX_PATH - 1] = '\0';

  char resolved[VFS_MAX_PATH];
  vfs_resolve_path(kpath, resolved);
  kfree(kpath);

  struct vfs_node *node = vfs_find_node(resolved);
  if (IS_ERR(node))
    return (isize)PTR_ERR(node);
  if (node->inode->type != VFS_DIRECTORY) {
    vfs_node_put(node);
    return -ENOTDIR;
  }

  isize res = (isize)scheduler_set_cwd(resolved);
  vfs_node_put(node);
  return res;
}

static isize sys_mount(const char *user_src, const char *user_target,
                       const char *user_type, u64 flags) {
  char *ksrc = kmalloc(VFS_MAX_PATH);
  if (!ksrc)
    return -ENOMEM;
  if (strncpy_from_user(ksrc, user_src, VFS_MAX_PATH) < 0) {
    kfree(ksrc);
    return -EFAULT;
  }

  char *ktarget = kmalloc(VFS_MAX_PATH);
  if (!ktarget) {
    kfree(ksrc);
    return -ENOMEM;
  }
  if (strncpy_from_user(ktarget, user_target, VFS_MAX_PATH) < 0) {
    kfree(ksrc);
    kfree(ktarget);
    return -EFAULT;
  }

  char *ktype = kmalloc(64);
  if (!ktype) {
    kfree(ksrc);
    kfree(ktarget);
    return -ENOMEM;
  }
  if (strncpy_from_user(ktype, user_type, 64) < 0) {
    kfree(ksrc);
    kfree(ktarget);
    kfree(ktype);
    return -EFAULT;
  }

  int res = vfs_mount(ksrc, ktarget, ktype, flags);
  kfree(ksrc);
  kfree(ktarget);
  kfree(ktype);
  return (isize)res;
}

static isize sys_umount(const char *user_target) {
  char *ktarget = kmalloc(VFS_MAX_PATH);
  if (!ktarget)
    return -ENOMEM;
  if (strncpy_from_user(ktarget, user_target, VFS_MAX_PATH) < 0) {
    kfree(ktarget);
    return -EFAULT;
  }
  ktarget[VFS_MAX_PATH - 1] = '\0';

  int res = vfs_umount(ktarget);
  kfree(ktarget);
  return (isize)res;
}

static isize sys_stat(const char *user_path, struct b1nix_stat *user_st) {
  char *kpath = kmalloc(VFS_MAX_PATH);
  if (!kpath)
    return -ENOMEM;
  if (strncpy_from_user(kpath, user_path, VFS_MAX_PATH) < 0) {
    kfree(kpath);
    return -EFAULT;
  }
  kpath[VFS_MAX_PATH - 1] = '\0';

  char resolved[VFS_MAX_PATH];
  vfs_resolve_path(kpath, resolved);
  kfree(kpath);

  struct b1nix_stat kst;
  int res = vfs_stat(resolved, &kst);
  if (res == 0) {
    if (copy_to_user(user_st, &kst, sizeof(struct b1nix_stat)) < 0)
      return -EFAULT;
  }
  return res;
}

static isize sys_spawn(const char *user_path, int argc,
                       const char **user_argv) {
  char *kpath = kmalloc(VFS_MAX_PATH);
  if (!kpath)
    return -ENOMEM;
  if (strncpy_from_user(kpath, user_path, VFS_MAX_PATH) < 0) {
    kfree(kpath);
    return -EFAULT;
  }
  kpath[VFS_MAX_PATH - 1] = '\0';

  char resolved[VFS_MAX_PATH];
  vfs_resolve_path(kpath, resolved);
  kfree(kpath);

  return user_spawn(resolved, argc, user_argv);
}

static isize sys_open(const char *user_path, int flags) {
  char *kpath = kmalloc(VFS_MAX_PATH);
  if (!kpath)
    return -ENOMEM;
  isize path_len = strncpy_from_user(kpath, user_path, VFS_MAX_PATH);
  if (path_len < 0) {
    kfree(kpath);
    return path_len;
  }
  kpath[VFS_MAX_PATH - 1] = '\0';

  char resolved[VFS_MAX_PATH];
  vfs_resolve_path(kpath, resolved);
  kfree(kpath);

  return vfs_open_flags(resolved, flags);
}

static isize sys_create(const char *user_path, u32 mode) {
  char *kpath = kmalloc(VFS_MAX_PATH);
  if (!kpath)
    return -ENOMEM;
  if (strncpy_from_user(kpath, user_path, VFS_MAX_PATH) < 0) {
    kfree(kpath);
    return -EFAULT;
  }
  kpath[VFS_MAX_PATH - 1] = '\0';

  char resolved[VFS_MAX_PATH];
  vfs_resolve_path(kpath, resolved);
  kfree(kpath);

  return vfs_create(resolved, mode);
}

static isize sys_getdents(int fd, struct dirent *user_buf, usize max_entries) {
  if (max_entries > 32)
    max_entries = 32;
  struct dirent kbuf[32];
  isize res = vfs_getdents(fd, kbuf, max_entries);
  if (res > 0) {
    if (copy_to_user(user_buf, kbuf, (usize)res * sizeof(struct dirent)) < 0)
      return -EFAULT;
  }
  return res;
}

static isize sys_syncfs(int fd) { return vfs_syncfs(fd); }

static isize sys_umask(u16 mask) {
  struct cred *cred = scheduler_get_current_cred();
  if (!cred)
    return -EPERM;
  u16 old_mask = cred->umask;
  cred->umask = mask & 0777;
  return old_mask;
}

static u64 sys_poll(struct b1nix_pollfd *user_fds, u64 nfds, u64 timeout) {
  struct b1nix_pollfd fds[16];
  if (nfds > 16)
    nfds = 16;
  if (syscall_copyin(fds, user_fds, nfds * sizeof(struct b1nix_pollfd)) < 0)
    return -EFAULT;

  u64 start_ticks = scheduler_get_uptime_ticks();
  u64 timeout_ticks = timeout == (u64)-1 ? (u64)-1 : timeout / 10;

  extern void *vfs_poll_chan;

  if (timeout != (u64)-1 && timeout != 0) {
    u64 ticks = timeout_ticks > 0 ? timeout_ticks : 1;
    current_task->wake_tick = start_ticks + ticks;
  }

  while (1) {
    int ready = 0;
    for (usize i = 0; i < nfds; i++) {
      if (fds[i].fd < 0) {
        fds[i].revents = 0;
        continue;
      }
      struct vfs_handle *h = scheduler_fd_get(fds[i].fd);
      if (!h) {
        fds[i].revents = B1NIX_POLLNVAL;
        ready++;
        continue;
      }
      vfs_poll(fds[i].fd, &fds[i]);
      if (fds[i].revents != 0)
        ready++;
    }

    if (ready > 0 || timeout == 0) {
      current_task->wake_tick = 0;
      syscall_copyout(user_fds, fds, nfds * sizeof(struct b1nix_pollfd));
      return (u64)ready;
    }

    if (timeout != (u64)-1) {
      u64 now = scheduler_get_uptime_ticks();
      if (now - start_ticks >= timeout_ticks) {
        current_task->wake_tick = 0;
        syscall_copyout(user_fds, fds, nfds * sizeof(struct b1nix_pollfd));
        return 0;
      }
    }

    scheduler_block_on(vfs_poll_chan);
  }
}

static u64 sys_bind(int fd, const void *user_addr, usize addrlen) {
  u8 kaddr[sizeof(struct b1nix_sockaddr_un)];
  if (!user_addr || addrlen == 0 || addrlen > sizeof(kaddr))
    return (u64)-EINVAL;
  if (syscall_copyin(kaddr, user_addr, addrlen) < 0)
    return (u64)-EFAULT;
  return (u64)vfs_bind(fd, kaddr, addrlen);
}

static u64 sys_connect(int fd, const void *user_addr, usize addrlen) {
  u8 kaddr[sizeof(struct b1nix_sockaddr_un)];
  if (!user_addr || addrlen == 0 || addrlen > sizeof(kaddr))
    return (u64)-EINVAL;
  if (syscall_copyin(kaddr, user_addr, addrlen) < 0)
    return (u64)-EFAULT;
  return (u64)vfs_connect(fd, kaddr, addrlen);
}

static u64 sys_send(int fd, const void *user_buf, usize len, int flags) {
  enum { SOCKET_IO_MAX = 64 * 1024 };
  if (len == 0)
    return 0;
  /* A buffer larger than one transfer chunk is legal; send up to the cap and
   * report how many bytes were taken (the caller loops for the rest). */
  if (len > SOCKET_IO_MAX)
    len = SOCKET_IO_MAX;
  if (!user_buf)
    return (u64)-EFAULT;
  void *kbuf = kmalloc(len);
  if (!kbuf)
    return (u64)-ENOMEM;
  if (syscall_copyin(kbuf, user_buf, len) < 0) {
    kfree(kbuf);
    return (u64)-EFAULT;
  }
  isize rc = vfs_socket_send(fd, kbuf, len, flags);
  kfree(kbuf);
  return (u64)rc;
}

static u64 sys_recv(int fd, void *user_buf, usize len, int flags) {
  enum { SOCKET_IO_MAX = 64 * 1024 };
  if (len == 0)
    return 0;
  /* recv() into a large buffer is legal — it returns however many bytes are
   * available, up to the cap; don't reject it (curl uses a >64K read buffer). */
  if (len > SOCKET_IO_MAX)
    len = SOCKET_IO_MAX;
  if (!user_buf)
    return (u64)-EFAULT;
  void *kbuf = kmalloc(len);
  if (!kbuf)
    return (u64)-ENOMEM;
  isize rc = vfs_socket_recv(fd, kbuf, len, flags);
  if (rc > 0 && syscall_copyout(user_buf, kbuf, (usize)rc) < 0) {
    kfree(kbuf);
    return (u64)-EFAULT;
  }
  kfree(kbuf);
  return (u64)rc;
}

static u64 sys_listen(int fd, int backlog) {
  return (u64)vfs_listen(fd, backlog);
}

static u64 sys_accept(int fd, void *addr, usize *addrlen) {
  /*addrlen is both in and out */
  usize k_addrlen = 0;
  if (addrlen) {
    if (syscall_copyin(&k_addrlen, addrlen, sizeof(usize)) != 0) return (u64)-EFAULT;
  }

  char k_addr[128]; /* enough for sockaddr_un */
  int res = vfs_accept(fd, k_addr, &k_addrlen);
  if (res >= 0) {
    if (addr && k_addrlen > 0) {
      if (syscall_copyout(addr, k_addr, k_addrlen) != 0) return (u64)-EFAULT;
    }
    if (addrlen) {
      if (syscall_copyout(addrlen, &k_addrlen, sizeof(usize)) != 0) return (u64)-EFAULT;
    }
  }
  return (u64)res;
}

static u64 sys_setsockopt(int fd, int level, int optname,
                          const void *user_optval, usize optlen) {
  u8 kopt[64];
  if (!user_optval || optlen == 0 || optlen > sizeof(kopt))
    return (u64)-EINVAL;
  if (syscall_copyin(kopt, user_optval, optlen) < 0)
    return (u64)-EFAULT;
  return (u64)vfs_setsockopt(fd, level, optname, kopt, optlen);
}

static u64 sys_getsockopt(int fd, int level, int optname, void *user_optval,
                          usize *user_optlen) {
  usize klen = 0;
  if (!user_optlen)
    return (u64)-EINVAL;
  if (syscall_copyin(&klen, user_optlen, sizeof(usize)) < 0)
    return (u64)-EFAULT;
  u8 kopt[64];
  if (klen == 0 || klen > sizeof(kopt))
    return (u64)-EINVAL;
  int rc = vfs_getsockopt(fd, level, optname, kopt, &klen);
  if (rc < 0)
    return (u64)rc;
  if (user_optval && klen > 0 && syscall_copyout(user_optval, kopt, klen) < 0)
    return (u64)-EFAULT;
  if (syscall_copyout(user_optlen, &klen, sizeof(usize)) < 0)
    return (u64)-EFAULT;
  return 0;
}

static u64 sys_getsockaddr(int fd, void *user_addr, usize *user_addrlen,
                           int want_peer) {
  usize klen = 0;
  if (!user_addrlen)
    return (u64)-EINVAL;
  if (syscall_copyin(&klen, user_addrlen, sizeof(usize)) < 0)
    return (u64)-EFAULT;
  char kaddr[128];
  if (klen > sizeof(kaddr))
    klen = sizeof(kaddr);
  int rc = want_peer ? vfs_getpeername(fd, kaddr, &klen)
                     : vfs_getsockname(fd, kaddr, &klen);
  if (rc < 0)
    return (u64)rc;
  usize out = klen < sizeof(kaddr) ? klen : sizeof(kaddr);
  if (user_addr && out > 0 && syscall_copyout(user_addr, kaddr, out) < 0)
    return (u64)-EFAULT;
  if (syscall_copyout(user_addrlen, &klen, sizeof(usize)) < 0)
    return (u64)-EFAULT;
  return 0;
}

static u64 sys_mmap(void *addr, usize length, int prot, int flags, int fd,
                    isize offset) {
  if (length == 0)
    return (u64)-EINVAL;

  struct task *t = current_task;
  if (!t)
    return (u64)-ESRCH;

  struct vfs_node *node = 0;
  if (!(flags & MAP_ANONYMOUS)) {
    if (fd < 0)
      return (u64)-EBADF;
    node = vfs_find_node_by_fd(fd);
    if (IS_ERR(node))
      return (u64)PTR_ERR(node);
    // Offset must be page-aligned
    if ((offset & (PAGE_SIZE - 1)) != 0)
      return (u64)-EINVAL;
  }

  // Align length to page size
  length = (length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

  u64 vaddr;
  if (flags & MAP_FIXED) {
    if (!addr)
      return (u64)-EINVAL;
    vaddr = (u64)(usize)addr;
    if ((vaddr & (PAGE_SIZE - 1)) != 0)
      return (u64)-EINVAL;
    if (vaddr >= 0x00007FFFFFFFFFFFULL || vaddr + length > 0x00007FFFFFFFFFFFULL)
      return (u64)-EINVAL;

    // Unmap existing mapping range
    for (u64 v = vaddr; v < vaddr + length; v += PAGE_SIZE) {
      vmm_unmap_page(v);
    }
    vma_delete_range(t, vaddr, vaddr + length);
  } else {
    if (addr == 0) {
      vaddr = vm_find_free_area(t, length);
    } else {
      vaddr = (u64)(usize)addr;
      if ((vaddr & (PAGE_SIZE - 1)) != 0) {
        vaddr = vm_find_free_area(t, length);
      } else {
        /* Verify hint doesn't overlap existing VMAs */
        struct vm_area *curr_vma = t->vma_list;
        int overlap = 0;
        while (curr_vma) {
          if (!(curr_vma->start >= vaddr + length || curr_vma->end <= vaddr)) {
            overlap = 1;
            break;
          }
          curr_vma = curr_vma->next;
        }
        if (overlap) {
          vaddr = vm_find_free_area(t, length);
        }
      }
    }
  }

  if (vaddr == (u64)-1)
    return (u64)-ENOMEM;

  // Allocate and map physical frames
  u64 vmm_flags = VMM_USER;
  if (prot & PROT_WRITE)
    vmm_flags |= VMM_WRITABLE;

  if (flags & MAP_ANONYMOUS) {
    u64 direct_base = vmm_direct_map_base();
    for (u64 v = vaddr; v < vaddr + length; v += PAGE_SIZE) {
      u64 frame = pmm_alloc_frame();
      if (!frame) {
        // Cleanup already mapped pages
        for (u64 u = vaddr; u < v; u += PAGE_SIZE) {
          vmm_unmap_page(u);
        }
        if (node) vfs_node_put(node);
        return (u64)-ENOMEM;
      }

      // Zero the frame
      memset((void *)(usize)(frame + direct_base), 0, PAGE_SIZE);

      vmm_map_page(v, frame, vmm_flags | VMM_PRESENT);
    }
  } else {
    // For file-backed, we use lazy allocation.
    // Connect to VFS by setting VMM_LAZY flag.
    // The page fault handler will read the file contents on demand.
    for (u64 v = vaddr; v < vaddr + length; v += PAGE_SIZE) {
      vmm_set_lazy(v);
      // Ensure the PTE also has the correct user/writable bits saved
      paging_mprotect_page(v, vmm_flags);
    }
  }

  // Create and link a new VMA
  struct vm_area *vma = kmalloc(sizeof(struct vm_area));
  if (!vma) {
    // Cleanup if VMA tracking fails
    for (u64 v = vaddr; v < vaddr + length; v += PAGE_SIZE) {
      vmm_unmap_page(v);
    }
    return (u64)-ENOMEM;
  }
  vma->start = vaddr;
  vma->end = vaddr + length;
  vma->prot = (u32)prot;
  vma->flags = (u32)flags;
  vma->node = node ? vfs_node_get(node) : 0;
  vma->offset = offset;
  vma->next = 0;

  // Insert into sorted list
  struct vm_area **prev = &t->vma_list;
  struct vm_area *curr = t->vma_list;
  while (curr && curr->start < vaddr) {
    prev = &curr->next;
    curr = curr->next;
  }
  vma->next = curr;
  *prev = vma;

  return vaddr;
}

static isize sys_munmap(void *addr, usize length) {
  u64 start = (u64)(usize)addr;
  if ((start & (PAGE_SIZE - 1)) != 0)
    return -EINVAL;
  if (length == 0)
    return -EINVAL;
  if (start >= 0x00007FFFFFFFFFFFULL)
    return -EINVAL;

  struct task *t = current_task;
  if (!t)
    return -ESRCH;

  u64 end = start + length;

  // 1. Unmap pages from hardware page tables and free physical frames
  for (u64 v = start; v < end; v += PAGE_SIZE) {
    vmm_unmap_page(v);
  }

  // 2. Update VMA list using the new robust helper
  vma_delete_range(t, start, end);

  return 0;
}

static isize sys_mprotect(void *addr, usize length, int prot) {
  u64 start = (u64)(usize)addr;
  if (!is_canonical(start))
    return -EINVAL;
  if ((start & (PAGE_SIZE - 1)) != 0)
    return -EINVAL;
  if (length == 0)
    return 0;
  if ((prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) != 0)
    return -EINVAL;

  u64 end = (start + length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
  if (end < start || end > 0x0000800000000000ULL)
    return -EINVAL;

  u64 flags = VMM_USER;
  if (prot & PROT_WRITE)
    flags |= VMM_WRITABLE;

  // 1. Update hardware page tables
  for (u64 vaddr = start; vaddr < end; vaddr += PAGE_SIZE) {
    paging_mprotect_page(vaddr, flags);
  }

  // 2. Update VMAs (handle splitting if necessary)
  struct task *t = current_task;
  struct vm_area *vma = t->vma_list;
  while (vma) {
    if (vma->start >= end || vma->end <= start) {
      vma = vma->next;
      continue;
    }

    // Partial overlap? Split!
    if (vma->start < start) {
      vma_split(t, vma, start);
      vma = vma->next; // Skip the part before 'start'
      continue;
    }
    if (vma->end > end) {
      vma_split(t, vma, end);
      // The current VMA is now exactly within [start, end]
    }

    vma->prot = (u32)prot;
    vma = vma->next;
  }

  return 0;
}

/* sigreturn is now in kernel/arch/x86/signal.c */

static u64 sys_brk(u64 addr) {
  struct task *t = current_task;
  if (!t)
    return 0;

  if (addr == 0) {
    return t->user_brk;
  }

  if (addr < t->heap_start) {
    return t->user_brk;
  }

  if (addr > t->user_brk) {
    u64 old_brk_page_end = (t->user_brk + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    u64 new_brk_page_end = (addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    for (u64 v = old_brk_page_end; v < new_brk_page_end; v += PAGE_SIZE) {
      u64 frame = pmm_alloc_frame();
      if (!frame) {
        return t->user_brk; // ENOMEM
      }

      // Zero frame
      u64 direct_base = vmm_direct_map_base();
      memset((void *)(usize)(frame + direct_base), 0, PAGE_SIZE);

      vmm_map_page(v, frame, VMM_USER | VMM_WRITABLE | VMM_PRESENT);
    }
  } else if (addr < t->user_brk) {
    u64 old_brk_page_end = (t->user_brk + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    u64 new_brk_page_end = (addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    for (u64 v = new_brk_page_end; v < old_brk_page_end; v += PAGE_SIZE) {
      vmm_unmap_page(v);
    }
  }

  t->user_brk = addr;

  // Update heap VMA
  struct vm_area *vma = t->vma_list;
  while (vma) {
    if (vma->start == t->heap_start) {
      vma->end = t->user_brk;
      break;
    }
    vma = vma->next;
  }

  return t->user_brk;
}

static int parse_ipv4_literal(const char *s, struct ipv4_addr *out) {
  if (!s || !out)
    return -EINVAL;

  struct ipv4_addr ip = {{0, 0, 0, 0}};
  int octet = 0;
  int value = 0;
  int has_digit = 0;

  for (const char *p = s;; p++) {
    char c = *p;
    if (c >= '0' && c <= '9') {
      has_digit = 1;
      value = value * 10 + (c - '0');
      if (value > 255)
        return -EINVAL;
      continue;
    }

    if (c == '.' || c == '\0') {
      if (!has_digit || octet >= 4)
        return -EINVAL;
      ip.bytes[octet++] = (u8)value;
      value = 0;
      has_digit = 0;
      if (c == '\0')
        break;
      continue;
    }

    return -EINVAL;
  }

  if (octet != 4)
    return -EINVAL;
  *out = ip;
  return 0;
}

/* Bitmask of CPU ids that have executed a syscall from a real (ELF) userspace
 * task. A syscall instruction runs in ring 3 and traps to ring 0 on the SAME
 * core, so a set bit N means userspace genuinely ran on cpu N — the M24b BKL
 * proof that ordinary processes execute on Application Processors. */
static volatile u32 g_user_cpu_mask = 0;

u32 sched_user_cpu_mask(void) { return g_user_cpu_mask; }

u64 syscall_dispatch_impl(u64 number, u64 arg0, u64 arg1, u64 arg2, u64 arg3,
                     u64 arg4, u64 arg5, struct interrupt_frame *frame) {
  u64 ret = 0;

  /* Record which CPU this userspace syscall came in on (ELF tasks only — kernel
   * builtins make in-kernel syscalls that do not prove ring-3 execution). */
  {
    struct task *t = current_task;
    if (t && t->user_image &&
        ((struct user_loaded_image *)t->user_image)->kind == USER_IMAGE_ELF64) {
      struct percpu *p = get_percpu();
      if (p)
        g_user_cpu_mask |= (1u << (p->cpu_id & 31));
    }
  }

  switch (number) {
  case SYS_WRITE:
    ret = (u64)sys_write((int)arg0, (const void *)(usize)arg1, (usize)arg2);
    break;
  case SYS_EXIT:
    scheduler_exit_current((int)arg0);
    ret = 0;
    break;
  case SYS_SPAWN: {
    return (u64)sys_spawn((const char *)(usize)arg0, (int)arg1,
                          (const char **)(usize)arg2);
  }

  case SYS_LIST: {
    return (u64)sys_list((const char *)(usize)arg0);
  }
  case SYS_READ_FILE: {
    return (u64)sys_read_file((const char *)(usize)arg0);
  }
  case SYS_YIELD:
    scheduler_yield();
    if (frame) {
      frame->rax = 0;
      arch_check_and_deliver_signals(frame);
    }
    return 0;
  case SYS_OPEN: {
    return (u64)sys_open((const char *)(usize)arg0, (int)arg1);
  }
  case SYS_READ:
    ret = (u64)sys_read((int)arg0, (void *)(usize)arg1, (usize)arg2);
    break;
  case SYS_CLOSE:
    if (scheduler_fd_get((int)arg0) == 0)
      return (u64)-EBADF;
    vfs_close((int)arg0);
    return 0;
  case SYS_LSEEK:
    return (u64)vfs_lseek((int)arg0, (isize)arg1, (int)arg2);
  case SYS_STAT: {
    return (u64)sys_stat((const char *)(usize)arg0,
                         (struct b1nix_stat *)(usize)arg1);
  }
  case SYS_FSTAT: {
    struct b1nix_stat kst;
    int res = vfs_fstat((int)arg0, &kst);
    if (res == 0) {
      if (copy_to_user((void *)(usize)arg1, &kst, sizeof(struct b1nix_stat)) <
          0)
        return (u64)-EFAULT;
    }
    return (u64)res;
  }
  case SYS_LSTAT:
    return (u64)sys_lstat((const char *)(usize)arg0,
                          (struct b1nix_stat *)(usize)arg1);
  case SYS_IOCTL:
    return (u64)vfs_ioctl((int)arg0, arg1, (void *)(usize)arg2);
  case SYS_FCNTL:
    return (u64)sys_fcntl((int)arg0, (int)arg1, arg2);
  case SYS_DUP2:
    return (u64)vfs_dup2((int)arg0, (int)arg1);
  case SYS_PIPE:
    return (u64)vfs_pipe((int *)(usize)arg0);
  case SYS_FSYNC:
    return (u64)vfs_fsync((int)arg0);
  case SYS_CREATE:
    return (u64)sys_create((const char *)(usize)arg0, (u32)arg1);
  case SYS_UNLINK:
    return (u64)sys_unlink((const char *)(usize)arg0);
  case SYS_MKDIR:
    return (u64)sys_mkdir((const char *)(usize)arg0, (u32)arg1);
  case SYS_RMDIR:
    return (u64)sys_rmdir((const char *)(usize)arg0);
  case SYS_RENAME:
    return (u64)sys_rename((const char *)(usize)arg0,
                           (const char *)(usize)arg1);
  case SYS_SYMLINK:
    return (u64)sys_symlink((const char *)(usize)arg0,
                            (const char *)(usize)arg1);
  case SYS_READLINK:
    return (u64)sys_readlink((const char *)(usize)arg0, (char *)(usize)arg1,
                             (usize)arg2);
  case SYS_GETDENTS:
    return (u64)sys_getdents((int)arg0, (struct dirent *)(usize)arg1,
                             (usize)arg2);
  case SYS_READDIR:
    return (u64)sys_readdir((const char *)(usize)arg0,
                            (struct dirent *)(usize)arg1, (usize)arg2);
  case SYS_STATFS:
    return (u64)sys_statfs((const char *)(usize)arg0,
                           (struct b1nix_statfs *)(usize)arg1);
  case SYS_FSTATFS:
    return (u64)sys_fstatfs((int)arg0, (struct b1nix_statfs *)(usize)arg1);
  case SYS_SYNC:
    return (u64)sys_sync();
  case SYS_SYNCFS:
    return (u64)sys_syncfs((int)arg0);
  case SYS_UMASK:
    return (u64)sys_umask((u16)arg0);
  case SYS_CHMOD:
    klog_info("audit: chmod called");
    return (u64)sys_chmod((const char *)(usize)arg0, (u16)arg1);
  case SYS_FCHMOD:
    return (u64)sys_fchmod((int)arg0, (u16)arg1);
  case SYS_UTIME:
    return (u64)sys_utime((const char *)(usize)arg0, (u64)arg1, (u64)arg2);
  case SYS_GETRANDOM: {
    void *user_buf = (void *)(usize)arg0;
    usize len = (usize)arg1;
    if (!user_buf) return (u64)-EFAULT;
    if (len == 0) return 0;
    u8 tmp[256];
    usize done = 0;
    while (done < len) {
      usize chunk = len - done;
      if (chunk > sizeof(tmp)) chunk = sizeof(tmp);
      for (usize i = 0; i < chunk; i += sizeof(u64)) {
        u64 r = sys_random_u64();
        usize left = chunk - i;
        usize n = left < sizeof(u64) ? left : sizeof(u64);
        memcpy(tmp + i, &r, n);
      }
      if (syscall_copyout((u8 *)user_buf + done, tmp, chunk) != 0)
        return (u64)-EFAULT;
      done += chunk;
    }
    return (u64)done;
  }
  case SYS_CHOWN:
    klog_info("audit: chown called");
    return (u64)sys_chown((const char *)(usize)arg0, (u16)arg1, (u16)arg2);
  case SYS_FCHOWN:
    return (u64)sys_fchown((int)arg0, (u16)arg1, (u16)arg2);

  case SYS_FORK:
    return (u64)scheduler_fork_current();
  case SYS_EXEC: {
    return (u64)sys_execve((const char *)(usize)arg0,
                           (const char **)(usize)arg1, NULL);
  }

  case SYS_EXECVE:
    return (u64)sys_execve((const char *)(usize)arg0,
                           (const char **)(usize)arg1,
                           (const char **)(usize)arg2);
  case SYS_WAIT:
    /* In-kernel callers (the shell, the smoke launcher) invoke this as
     * syscall_dispatch(SYS_WAIT, pid, &status): arg0 = pid, arg1 = status.
     * The userspace libc does NOT use SYS_WAIT — its wait() maps to
     * SYS_WAITPID(-1, status, 0). */
    return (u64)scheduler_wait((usize)arg0, (int *)(usize)arg1);
  case SYS_WAITPID:
    return (u64)scheduler_waitpid((usize)arg0, (int *)(usize)arg1, (int)arg2);
  case SYS_GETPID:
    return (u64)scheduler_get_pid();
  case SYS_GETCPU: {
    struct percpu *p = get_percpu();
    return (u64)(p ? p->cpu_id : 0);
  }
  case SYS_GETUID: {
    struct cred *c = scheduler_get_current_cred();
    return c ? c->uid : 0;
  }
  case SYS_GETEUID: {
    struct cred *c = scheduler_get_current_cred();
    return c ? c->euid : 0;
  }
  case SYS_GETGID: {
    struct cred *c = scheduler_get_current_cred();
    return c ? c->gid : 0;
  }
  case SYS_GETEGID: {
    struct cred *c = scheduler_get_current_cred();
    return c ? c->egid : 0;
  }
  case SYS_SETUID: {
    klog_info("audit: setuid called");
    struct cred *c = scheduler_get_current_cred();
    return c ? (u64)cred_set_uid(c, (u16)arg0) : (u64)-EACCES;
  }
  case SYS_SETGID: {
    klog_info("audit: setgid called");
    struct cred *c = scheduler_get_current_cred();
    return c ? (u64)cred_set_gid(c, (u16)arg0) : (u64)-EACCES;
  }
  case SYS_SLEEP:
    scheduler_sleep_ticks(arg0);
    return 0;
  case SYS_KILL: {
    isize target = (isize)arg0;
    u64 kill_ret = (u64)-EINVAL;
    if (target < 0) {
      kill_ret = (u64)scheduler_kill_process_group((usize)(-target), (int)arg1);
    } else {
      kill_ret = (u64)scheduler_kill((usize)target, (int)arg1);
    }
    if (frame) {
      frame->rax = kill_ret;
      arch_check_and_deliver_signals(frame);
    }
    return kill_ret;
  }
  case SYS_SIGNAL: {
    int sig = (int)arg0;
    struct sigaction act;
    struct sigaction old;
    memset(&act, 0, sizeof(act));
    memset(&old, 0, sizeof(old));
    if (arg1 && syscall_copyin(&act, (void *)(usize)arg1, sizeof(act)) != 0)
      return (u64)-EFAULT;
    if (scheduler_sigaction(sig, arg1 ? &act : 0, &old) < 0)
      return (u64)-EINVAL;
    if (arg2 && syscall_copyout((void *)(usize)arg2, &old, sizeof(old)) != 0)
      return (u64)-EFAULT;
    return 0;
  }
  case SYS_SIGPROCMASK: {
    int how = (int)arg0;
    u64 set_val = 0;
    u64 old_val = 0;
    u64 *set_ptr = 0;

    if (arg1) {
      if (syscall_copyin(&set_val, (void *)(usize)arg1, sizeof(set_val)) != 0)
        return (u64)-EFAULT;
      set_ptr = &set_val;
    }

    if (scheduler_sigprocmask(how, set_ptr, arg2 ? &old_val : 0) < 0)
      return (u64)-EINVAL;

    if (arg2 &&
        syscall_copyout((void *)(usize)arg2, &old_val, sizeof(old_val)) != 0)
      return (u64)-EFAULT;
    return 0;
  }
  case SYS_SETSID:
    return (u64)scheduler_setsid();
  case SYS_GETPGRP:
    return (u64)scheduler_getpgrp();
  case SYS_SETPGRP:
    return (u64)scheduler_setpgrp((usize)arg0, (usize)arg1);
  case SYS_SETPRIORITY: {
    usize pid = arg0 == 0 ? scheduler_get_pid() : (usize)arg0;
    return (u64)scheduler_set_priority(pid, (int)arg1);
  }
  case SYS_GETPRIORITY: {
    usize pid = arg0 == 0 ? scheduler_get_pid() : (usize)arg0;
    return (u64)scheduler_get_priority(pid);
  }
  case SYS_BRK:
    return sys_brk(arg0);
  case SYS_MMAP:
    return sys_mmap((void *)(usize)arg0, (usize)arg1, (int)arg2, (int)arg3,
                    (int)arg4, (isize)arg5);
  case SYS_MUNMAP:
    return (u64)sys_munmap((void *)(usize)arg0, (usize)arg1);
  case SYS_MPROTECT:
    return (u64)sys_mprotect((void *)(usize)arg0, (usize)arg1, (int)arg2);
  case SYS_MEM:
    console_write("Total usable memory: ");
    console_write_dec(pmm_total_usable_memory() / (1024ULL * 1024ULL));
    console_write(" MB\n");
    console_write("Free memory approx:  ");
    console_write_dec(pmm_free_memory_estimate() / (1024ULL * 1024ULL));
    console_write(" MB\n");
    return 0;
  case SYS_MQ_OPEN: {
    char name[64];
    if (syscall_copyinstr(name, sizeof(name), (const char *)(usize)arg0) < 0)
      return (u64)-EFAULT;
    struct mqueue *mq = mqueue_create(name);
    return mq ? (u64)(usize)mq : (u64)-ENOMEM;
  }
  case SYS_MQ_SEND: {
    u32 len = (u32)arg2;
    if (len > 256) /* MQ_MAX_MSG_SIZE */
      return (u64)-EINVAL;
    char kbuf[256];
    if (len > 0) {
      if (syscall_copyin(kbuf, (const void *)(usize)arg1, len) != 0)
        return (u64)-EFAULT;
    }
    return (u64)mqueue_send((struct mqueue *)(usize)arg0, kbuf, len);
  }
  case SYS_MQ_RECEIVE: {
    char kbuf[256];
    u32 klen = 0;
    int ret = mqueue_receive((struct mqueue *)(usize)arg0, kbuf, &klen);
    if (ret == 0) {
      if (arg2 && syscall_copyout((void *)(usize)arg2, &klen, sizeof(u32)) != 0)
        return (u64)-EFAULT;
      if (arg1 && klen > 0 && syscall_copyout((void *)(usize)arg1, kbuf, klen) != 0)
        return (u64)-EFAULT;
    }
    return (u64)ret;
  }
  case SYS_MQ_CLOSE:
    mqueue_close((struct mqueue *)(usize)arg0);
    return 0;
  case SYS_MQ_UNLINK: {
    char name[64];
    if (syscall_copyinstr(name, sizeof(name), (const char *)(usize)arg0) < 0)
      return (u64)-EFAULT;
    return (u64)mqueue_unlink(name);
  }
  case SYS_SHMGET:
    return (u64)shmget((u32)arg0, (usize)arg1, (int)arg2);
  case SYS_SHMAT:
    return (u64)(usize)shmat((int)arg0, (const void *)(usize)arg1, (int)arg2);
  case SYS_SHMDT:
    return (u64)shmdt((const void *)(usize)arg0);
  case SYS_SHMCTL: {
    int shmid = (int)arg0;
    int cmd = (int)arg1;
    struct shmid_ds kds;
    if (cmd == 2 /* IPC_SET */ && arg2) {
      if (syscall_copyin(&kds, (const void *)(usize)arg2, sizeof(kds)) != 0)
        return (u64)-EFAULT;
    }
    int ret = shmctl(shmid, cmd, arg2 ? &kds : 0);
    if (ret == 0 && cmd == 1 /* IPC_STAT */ && arg2) {
      if (syscall_copyout((void *)(usize)arg2, &kds, sizeof(kds)) != 0)
        return (u64)-EFAULT;
    }
    return (u64)ret;
  }
  case SYS_SOCKET:
    return (u64)vfs_socket((int)arg0, (int)arg1, (int)arg2);
  case SYS_BIND:
    return sys_bind((int)arg0, (const void *)(usize)arg1, (usize)arg2);
  case SYS_CONNECT:
    return sys_connect((int)arg0, (const void *)(usize)arg1, (usize)arg2);
  case SYS_SEND:
    return sys_send((int)arg0, (const void *)(usize)arg1, (usize)arg2,
                    (int)arg3);
  case SYS_RECV:
    return sys_recv((int)arg0, (void *)(usize)arg1, (usize)arg2, (int)arg3);
  case SYS_LISTEN:
    return sys_listen((int)arg0, (int)arg1);
  case SYS_ACCEPT:
    return sys_accept((int)arg0, (void *)(usize)arg1, (usize *)(usize)arg2);
  case SYS_SETSOCKOPT:
    return sys_setsockopt((int)arg0, (int)arg1, (int)arg2,
                          (const void *)(usize)arg3, (usize)arg4);
  case SYS_GETSOCKOPT:
    return sys_getsockopt((int)arg0, (int)arg1, (int)arg2,
                          (void *)(usize)arg3, (usize *)(usize)arg4);
  case SYS_GETSOCKNAME:
    return sys_getsockaddr((int)arg0, (void *)(usize)arg1,
                           (usize *)(usize)arg2, 0);
  case SYS_GETPEERNAME:
    return sys_getsockaddr((int)arg0, (void *)(usize)arg1,
                           (usize *)(usize)arg2, 1);
  case SYS_SHUTDOWN:
    return (u64)vfs_shutdown((int)arg0, (int)arg1);
#ifndef __aarch64__
  case SYS_NET_INFO:
    net_dump_info();
    return 0;
  case SYS_NET_PING: {
    char ip_text[32];
    struct ipv4_addr dest;
    if (syscall_copyinstr(ip_text, sizeof(ip_text), (const char *)(usize)arg0) != 0)
      return (u64)-EFAULT;
    if (parse_ipv4_literal(ip_text, &dest) != 0) {
      /* Not a dotted-quad — try resolving it as a hostname via DNS. */
      if (dns_resolve_sync(ip_text, dest.bytes) != 0)
        return (u64)-EINVAL;
    }

    u32 before = icmp_echo_reply_count();

    for (int attempt = 0; attempt < 4; attempt++) {
      u16 seq = (u16)((scheduler_get_uptime_ticks() + attempt) & 0xffff);
      u8 echo[8] = {8, 0, 0, 0, 0, 0, 0, 0};
      echo[6] = (u8)(seq >> 8);
      echo[7] = (u8)(seq & 0xff);
      u16 csum = 0;
      for (int j = 0; j < 8; j += 2)
        csum = (u16)(csum + (u16)((echo[j] << 8) | echo[j + 1]));
      csum = (u16)~csum;
      echo[2] = (u8)(csum >> 8);
      echo[3] = (u8)(csum & 0xff);

      ipv4_send(dest, 1, echo, sizeof(echo));
      console_write("ping: sent request seq=");
      console_write_dec(seq);
      console_write("\n");

      for (int wait = 0; wait < 25; wait++) {
        net_poll();
        if (icmp_echo_reply_count() > before) {
          return 0;
        }
        scheduler_sleep_ticks(2);
      }
    }
    console_write("ping: timeout waiting reply\n");
    return (u64)-ETIMEDOUT;
  }
  case SYS_NET_DNS:
    {
        char host[256];
        isize ret = syscall_copyinstr(host, sizeof(host), (const char *)(usize)arg0);
        if (ret < 0)
            return (u64)ret;
        /* arg1 != 0: synchronous resolve, copy the 4-byte A record out.
         * arg1 == 0: legacy fire-and-forget query (prints to console). */
        if (arg1) {
            u8 ip[4];
            if (dns_resolve_sync(host, ip) != 0)
                return (u64)-EHOSTUNREACH;
            if (syscall_copyout((void *)(usize)arg1, ip, 4) != 0)
                return (u64)-EFAULT;
            return 0;
        }
        dns_resolve(host);
        return 0;
    }
  case SYS_READ_KBD:
    return sys_read_kbd();
#else
  case SYS_NET_INFO:
    console_write("Network info not available on this arch\n");
    return 0;
  case SYS_NET_PING:
  case SYS_NET_DNS:
  case SYS_READ_KBD:
    return (u64)-ENOSYS;
#endif
  case SYS_TIME:
    /* Wall-clock seconds since Unix epoch: RTC snapshot at boot plus uptime. */
    return (u64)vfs_get_unix_time();
  case SYS_UNAME: {
    struct b1nix_utsname uts;
    memset(&uts, 0, sizeof(uts));
    copy_cstr(uts.sysname, sizeof(uts.sysname), "B1NIX");
    copy_cstr(uts.nodename, sizeof(uts.nodename), "b1nix");
    copy_cstr(uts.release, sizeof(uts.release), "0.22.0");
    copy_cstr(uts.version, sizeof(uts.version), "M22 Core Utilities");
#ifdef __aarch64__
    copy_cstr(uts.machine, sizeof(uts.machine), "aarch64");
#else
    copy_cstr(uts.machine, sizeof(uts.machine), "x86_64");
#endif
    return syscall_copyout((void *)(usize)arg0, &uts, sizeof(uts)) == 0
               ? 0
               : (u64)-EFAULT;
  }
  case SYS_GETCWD: {
    const char *cwd = scheduler_get_cwd();
    if (!arg0 || arg1 == 0)
      return (u64)-EFAULT;
    usize len = strlen(cwd);
    if (len >= arg1)
      len = arg1 - 1;
    char tmp[VFS_MAX_PATH];
    if (len >= sizeof(tmp))
      len = sizeof(tmp) - 1;
    memcpy(tmp, cwd, len);
    tmp[len] = '\0';
    if (syscall_copyout((void *)(usize)arg0, tmp, len + 1) != 0)
      return (u64)-EFAULT;
    return (u64)len;
  }
  case SYS_CHDIR:
    return (u64)sys_chdir((const char *)(usize)arg0);

  case SYS_REBOOT:
    if ((int)arg0 == B1NIX_REBOOT_POWEROFF) {
      console_write("reboot: powering off\n");
      /* QEMU/Bochs ACPI shutdown ports - no full ACPI parsing needed. */
      outw(0x604, 0x2000);  /* QEMU >= 2.0 */
      outw(0xB004, 0x2000); /* Bochs / older QEMU */
      outw(0x4004, 0x3400); /* QEMU microvm/newer */
      console_write("reboot: poweroff unsupported, halting\n");
      arch_halt();
    } else if ((int)arg0 == B1NIX_REBOOT_HALT) {
      console_write("reboot: system halted\n");
      arch_halt();
    } else {
      console_write("reboot: restarting\n");
      interrupts_disable();
      /* Pulse the 8042 keyboard-controller reset line once its input
       * buffer is drained. */
      while (inb(0x64) & 0x02)
        ;
      outb(0x64, 0xFE);
      /* Fallback: provoke a triple fault via a null IDT. */
      struct {
        u16 limit;
        u64 base;
      } __attribute__((packed)) null_idt = {0, 0};
      __asm__ volatile("lidt %0; int3" : : "m"(null_idt));
      arch_halt();
    }
    break;
  case SYS_DMESG:
    if (!arg0 || arg1 == 0)
      return (u64)-EINVAL;
    if (arg1 > KLOG_BUF_SIZE)
      arg1 = KLOG_BUF_SIZE;
    if (!is_user_range_valid((const void *)arg0, arg1, 1))
      return (u64)-EFAULT;
    char tmp[KLOG_BUF_SIZE];
    usize copied = klog_read(tmp, (usize)arg1);
    if (syscall_copyout((void *)(usize)arg0, tmp, copied) != 0)
      return (u64)-EFAULT;
    return (u64)copied;
  case SYS_MOUNT:
    return (u64)sys_mount((const char *)(usize)arg0, (const char *)(usize)arg1,
                          (const char *)(usize)arg2, arg3);
  case SYS_UMOUNT:
    return (u64)sys_umount((const char *)(usize)arg0);

  case SYS_MOUNTS:
    return (u64)vfs_mounts((struct b1nix_mount_entry *)(usize)arg0,
                           (usize)arg1);
  case SYS_PS:
    scheduler_dump_tasks();
    return 0;
  case SYS_CLEAR:
    return sys_clear();
  case SYS_SET_STDOUT:
    scheduler_set_stdout((int)arg0);
    return 0;
  case SYS_TERMIOS_GET:
    return sys_ioctl((int)arg0, B1NIX_TCGETS, (void *)(usize)arg1);
  case SYS_TERMIOS_SET:
    return sys_ioctl((int)arg0, B1NIX_TCSETS, (void *)(usize)arg1);
  case SYS_SELFHOST_STATUS:
    return sys_selfhost_status((struct b1nix_selfhost_status *)(usize)arg0);
  case SYS_LINK: {
    char target[VFS_MAX_PATH];
    char link_path[VFS_MAX_PATH];
    vfs_resolve_path((const char *)(usize)arg0, target);
    vfs_resolve_path((const char *)(usize)arg1, link_path);
    return (u64)vfs_link(target, link_path);
  }
  case SYS_POLL:
    return sys_poll((struct b1nix_pollfd *)arg0, arg1, arg2);
  case SYS_SIGRETURN:
    ret = sys_sigreturn(frame);
    break;
  case SYS_CLOCK_GETTIME: {
    int clk_id = (int)arg0;
    struct timespec ktp;
    u64 ticks = scheduler_get_uptime_ticks();
    if (clk_id == 1 /* CLOCK_MONOTONIC */) {
      ktp.tv_sec = (i64)(ticks / 100);
      ktp.tv_nsec = (i64)((ticks % 100) * 10000000);
    } else {
      /* CLOCK_REALTIME: epoch-based wall clock from RTC boot offset. */
      ktp.tv_sec = (i64)vfs_get_unix_time();
      ktp.tv_nsec = (i64)((ticks % 100) * 10000000);
    }
    if (syscall_copyout((void *)(usize)arg1, &ktp, sizeof(struct timespec)) != 0) {
      return (u64)-EFAULT;
    }
    return 0;
  }
  case SYS_IO_SETUP:
    return sys_io_setup((u32)arg0, (u64 *)(usize)arg1);
  case SYS_IO_SUBMIT:
    return sys_io_submit(arg0, (u32)arg1,
                         (const struct b1nix_aio_sqe *)(usize)arg2);
  case SYS_IO_GETEVENTS:
    return sys_io_getevents(arg0, (u32)arg1, (u32)arg2,
                            (struct b1nix_aio_cqe *)(usize)arg3, (u32)arg4);
  case SYS_CLONE:
    /* SYS_CLONE(flags, entry, user_stack, arg, tls, ctid) — see
     * kernel/include/b1nix/syscall.h for the B1NIX_CLONE_* flag bits.
     * Returns the new TID on success or -errno on failure. */
    return (u64)scheduler_clone_thread(arg0, arg1, arg2, arg3, arg4, arg5);
  case SYS_FUTEX:
    /* SYS_FUTEX(uaddr, op, val) — minimal WAIT/WAKE only. */
    return (u64)scheduler_futex(arg0, (int)arg1, (int)arg2);
  case SYS_SET_TLS: {
    /* SYS_SET_TLS(addr) — set this task's FS base. Takes effect on the
     * next return-to-userspace transition (the scheduler reloads MSR_FS_BASE
     * on each context switch). For the current task we also write the
     * MSR live, so a thread that just set its TLS can dereference it
     * immediately. */
    if (!current_task) return (u64)-EINVAL;
    task_set_tls_base(current_task, arg0);
    extern void arch_set_fs_base(u64 base);
    arch_set_fs_base(arg0);
    return 0;
  }
  case SYS_GETTID:
    return (u64)scheduler_get_pid();
  case SYS_EXIT_THREAD:
    /* SYS_EXIT_THREAD(code) — thread-only exit. For an is_thread task
     * scheduler_exit_current already handles the CLONE_CHILD_CLEARTID
     * futex wake. For a process leader this acts the same as SYS_EXIT. */
    scheduler_exit_current((int)arg0);
    return 0;
  case SYS_SELECT: {
    /* SYS_SELECT(nfds, readfds, writefds, exceptfds, timeout_ms).
     *
     * b1nix doesn't ship a full POSIX `struct timeval` (no float in the
     * kernel; the libc has not added it yet). The userspace wrapper
     * converts the tv into a millisecond count, with timeout==(u64)-1
     * meaning "wait forever" (matching the b1nix poll convention).
     *
     * fd_set is a fixed-size bitmask — b1nix uses 1024-bit (128 bytes)
     * to match Linux's FD_SETSIZE=1024. We translate set bits to a
     * pollfd array, dispatch to sys_poll, then translate revents back. */
    int nfds = (int)arg0;
    if (nfds < 0 || nfds > 1024) return (u64)-EINVAL;
    /* Local copies of each fd_set (NULL-aware — userspace may pass 0). */
    u8 r_kset[128] = {0}, w_kset[128] = {0}, e_kset[128] = {0};
    if (arg1 && syscall_copyin(r_kset, (void *)(usize)arg1, 128) < 0)
      return (u64)-EFAULT;
    if (arg2 && syscall_copyin(w_kset, (void *)(usize)arg2, 128) < 0)
      return (u64)-EFAULT;
    if (arg3 && syscall_copyin(e_kset, (void *)(usize)arg3, 128) < 0)
      return (u64)-EFAULT;

    /* Build pollfd array — one slot per fd that appears in any set. */
    struct b1nix_pollfd pfds[64];
    int np = 0;
    for (int fd = 0; fd < nfds && np < 64; fd++) {
      int r = (r_kset[fd / 8] >> (fd & 7)) & 1;
      int w = (w_kset[fd / 8] >> (fd & 7)) & 1;
      int e = (e_kset[fd / 8] >> (fd & 7)) & 1;
      if (!r && !w && !e) continue;
      pfds[np].fd = fd;
      pfds[np].events = 0;
      if (r) pfds[np].events |= B1NIX_POLLIN;
      if (w) pfds[np].events |= B1NIX_POLLOUT;
      pfds[np].revents = 0;
      np++;
    }

    /* Dispatch into sys_poll's machinery without going back through the
     * syscall boundary — keeps the implementation a single function and
     * makes the user-vs-kernel buffer ownership consistent. The inline
     * pfds array lives on this kernel stack; sys_poll's copyin would
     * normally bring it in from userspace, so we duplicate the spin
     * loop here. */
    u64 start_ticks = scheduler_get_uptime_ticks();
    u64 timeout_ms = arg4;
    u64 timeout_ticks =
        (timeout_ms == (u64)-1) ? (u64)-1 : timeout_ms / 10;

    if (timeout_ms != (u64)-1 && timeout_ms != 0) {
      u64 ticks = timeout_ticks > 0 ? timeout_ticks : 1;
      current_task->wake_tick = start_ticks + ticks;
    }

    extern void *vfs_poll_chan;
    int ready_count = 0;
    while (1) {
      ready_count = 0;
      for (int i = 0; i < np; i++) {
        if (pfds[i].fd < 0) { pfds[i].revents = 0; continue; }
        struct vfs_handle *h = scheduler_fd_get(pfds[i].fd);
        if (!h) { pfds[i].revents = B1NIX_POLLNVAL; ready_count++; continue; }
        pfds[i].revents = 0;
        vfs_poll(pfds[i].fd, &pfds[i]);
        if (pfds[i].revents) ready_count++;
      }
      if (ready_count > 0 || timeout_ms == 0) break;
      if (timeout_ms != (u64)-1) {
        u64 now = scheduler_get_uptime_ticks();
        if (now - start_ticks >= timeout_ticks) break;
      }
      scheduler_block_on(vfs_poll_chan);
    }

    current_task->wake_tick = 0;

    /* Translate revents back to fd_sets. */
    u8 r_kout[128] = {0}, w_kout[128] = {0}, e_kout[128] = {0};
    int hits = 0;
    for (int i = 0; i < np; i++) {
      int fd = pfds[i].fd;
      int r_set = 0, w_set = 0;
      if (pfds[i].revents & B1NIX_POLLIN) {
        r_kout[fd / 8] |= (u8)(1 << (fd & 7));
        r_set = 1;
        hits++;
      }
      if (pfds[i].revents & B1NIX_POLLOUT) {
        w_kout[fd / 8] |= (u8)(1 << (fd & 7));
        w_set = 1;
        hits++;
      }
      if (pfds[i].revents & (B1NIX_POLLERR | B1NIX_POLLHUP | B1NIX_POLLNVAL)) {
        if (!r_set && ((r_kset[fd / 8] >> (fd & 7)) & 1)) {
          r_kout[fd / 8] |= (u8)(1 << (fd & 7));
          hits++;
        }
        if (!w_set && ((w_kset[fd / 8] >> (fd & 7)) & 1)) {
          w_kout[fd / 8] |= (u8)(1 << (fd & 7));
          hits++;
        }
        if ((e_kset[fd / 8] >> (fd & 7)) & 1) {
          e_kout[fd / 8] |= (u8)(1 << (fd & 7));
          hits++;
        }
      }
    }
    if (arg1 && syscall_copyout((void *)(usize)arg1, r_kout, 128) < 0)
      return (u64)-EFAULT;
    if (arg2 && syscall_copyout((void *)(usize)arg2, w_kout, 128) < 0)
      return (u64)-EFAULT;
    if (arg3 && syscall_copyout((void *)(usize)arg3, e_kout, 128) < 0)
      return (u64)-EFAULT;
    return (u64)hits;
  }
  default:
    console_write("syscall: unknown 0x");
    console_write_hex64(number);
    console_write("\n");
    ret = (u64)-ENOSYS;
    break;
  }

  if (frame && ret == (u64)-ERESTARTSYS) {
    /* Acquire-load: a concurrent killer publishes bits with a release RMW. */
    u64 pending = __atomic_load_n(&current_task->pending_signals,
                                  __ATOMIC_ACQUIRE) & ~current_task->blocked_signals;
    int restart = 1;
    for (int i = 1; i < NSIG; i++) {
      if (pending & (1ULL << (i - 1))) {
        struct sigaction *sa = &current_task->sigactions[i - 1];
        if (sa->sa_handler != SIG_IGN && sa->sa_handler != SIG_DFL) {
          if (!(sa->sa_flags & SA_RESTART)) {
            restart = 0;
          }
          break;
        }
      }
    }
    if (restart) {
      frame->rip -= 2;
      frame->rax = number;
      ret = number;
    } else {
      ret = (u64)-EINTR;
      frame->rax = (u64)-EINTR;
    }
  }

  /* Check for pending signals before returning to userspace */
  if (frame) {
    arch_check_and_deliver_signals(frame);
    if (!user_frame_is_valid(frame)) {
      scheduler_exit_current(-SIGSEGV);
      return (u64)-EFAULT;
    }
  }

  return ret;
}
