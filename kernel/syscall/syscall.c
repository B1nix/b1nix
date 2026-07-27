#include <b1nix/arch.h>
#include <b1nix/console.h>
#include <b1nix/dirent.h>
#include <b1nix/errno.h>
#include <b1nix/initramfs.h>
#include <b1nix/io.h>
#include <b1nix/klog.h>
#include <b1nix/linux_abi.h>
#include <b1nix/mm.h>
#include <b1nix/mqueue.h>
#include <b1nix/net.h>
#include <b1nix/page_cache.h>
#include <b1nix/inotify.h>
#include <b1nix/posix.h>
#include <b1nix/seccomp.h>
#include <b1nix/rtc.h>
#include <b1nix/sched.h>
#include <b1nix/shm.h>
#include <b1nix/syscall.h>
#include <b1nix/uidgid.h>
#include <b1nix/user.h>
#include <b1nix/vfs.h>
#include <b1nix/filelock.h>
#include <b1nix/aio.h>
#include <string.h>
#include <stdio.h>
#include <b1nix/version.h>


#define MAX_EXEC_ARGS 256
#define MAX_EXEC_ARG_LEN 4096
static int copy_from_user(void *dst, const void *src, usize size);
static int copy_to_user(void *dst, const void *src, usize size);
static isize strncpy_from_user(char *dst, const char *src, usize size);
static inline int is_canonical(u64 addr) {
#ifdef __x86_64__
  return ((isize)addr >> 47) == 0 || ((isize)addr >> 47) == -1;
#else
  return (addr >> 32) == 0;
#endif
}

static int syscall_allows_kernel_pointers(void) {
  struct task *t = current_task;
  if (!t || !t->user_image)
    return 1;
  return t->in_kernel_syscall;
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

u64 kernel_random_u64(void) {
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
      (!allow_kernel_ptrs && (u64)u_array >= USER_SPACE_LIMIT))
    return ERR_PTR(-EFAULT);

  char **k_array = kmalloc(sizeof(char *) * (MAX_EXEC_ARGS + 1));
  if (!k_array)
    return ERR_PTR(-ENOMEM);
  memset(k_array, 0, sizeof(char *) * (MAX_EXEC_ARGS + 1));

  for (int i = 0; i < MAX_EXEC_ARGS; i++) {
    if (!allow_kernel_ptrs && (u64)&u_array[i] >= USER_SPACE_LIMIT)
      goto fault;
    const char *u_str;
    if (copy_from_user(&u_str, &u_array[i], sizeof(char *)) < 0)
      goto fault;

    if (u_str == NULL) {
      k_array[i] = NULL;
      break;
    }

    if (!is_canonical((u64)u_str) ||
        (!allow_kernel_ptrs && (u64)u_str >= USER_SPACE_LIMIT))
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
    if (curr >= USER_SPACE_LIMIT) return -EFAULT;

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
  /* Allow kernel pointers during early boot */
  if (!t || !t->user_image) return 1;

  if (end < start) return 0; // Overflow
  if (end > USER_SPACE_LIMIT) return 0; // Not in userspace

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

/* writev(fd, iov, iovcnt) — scatter write. iov is a userspace array of
 * {void *iov_base, size_t iov_len} pairs. */
struct b1nix_iovec { void *iov_base; usize iov_len; };
#define UIO_MAXIOV 1024

static isize sys_writev(int fd, const struct b1nix_iovec *uiov, int iovcnt) {
  if (iovcnt < 0 || iovcnt > UIO_MAXIOV)
    return -EINVAL;
  if (iovcnt == 0)
    return 0;

  isize total = 0;
  char kbuf[4096];
  for (int i = 0; i < iovcnt; i++) {
    struct b1nix_iovec iov;
    if (copy_from_user(&iov, uiov + i, sizeof(iov)) < 0)
      return total > 0 ? total : -EFAULT;
    const char *base = (const char *)iov.iov_base;
    usize len = iov.iov_len;
    if (len == 0)
      continue;

    while (len > 0) {
      usize chunk = len > sizeof(kbuf) ? sizeof(kbuf) : len;
      if (copy_from_user(kbuf, base, chunk) < 0)
        return total > 0 ? total : -EFAULT;
      isize res = vfs_write(fd, kbuf, chunk);
      if (res < 0)
        return total > 0 ? total : res;
      if (res == 0)
        return total;
      total += res;
      base += res;
      len -= (usize)res;
      if (res < (isize)chunk)
        return total;
    }
  }
  return total;
}

/* readv(fd, iov, iovcnt) — scatter read. Mirrors writev logic. */
static isize sys_readv(int fd, const struct b1nix_iovec *uiov, int iovcnt) {
  if (iovcnt < 0 || iovcnt > UIO_MAXIOV)
    return -EINVAL;
  if (iovcnt == 0)
    return 0;

  isize total = 0;
  char kbuf[4096];
  for (int i = 0; i < iovcnt; i++) {
    struct b1nix_iovec iov;
    if (copy_from_user(&iov, uiov + i, sizeof(iov)) < 0)
      return total > 0 ? total : -EFAULT;
    char *base = (char *)iov.iov_base;
    usize len = iov.iov_len;
    if (len == 0)
      continue;

    while (len > 0) {
      usize chunk = len > sizeof(kbuf) ? sizeof(kbuf) : len;
      isize res = vfs_read(fd, kbuf, chunk);
      if (res < 0)
        return total > 0 ? total : res;
      if (res == 0)
        return total;
      if (copy_to_user(base, kbuf, (usize)res) < 0)
        return total > 0 ? total : -EFAULT;
      total += res;
      base += res;
      len -= (usize)res;
      if (res < (isize)chunk)
        return total;
    }
  }
  return total;
}

/* M73: shared fd→fd byte pump backing sendfile/copy_file_range/splice. Reads up
 * to `count` bytes from in_fd and writes them to out_fd through a kernel bounce
 * buffer. When *in_off / *out_off is provided the transfer starts there and the
 * caller's offset variable is advanced, WITHOUT disturbing the fd's own file
 * offset (POSIX sendfile/copy_file_range semantics: an explicit offset argument
 * leaves the descriptor position untouched). A NULL offset pointer means "use
 * and advance the fd's own offset". Returns bytes copied or -errno. */
static isize file_copy_range(int in_fd, u64 *in_off, int out_fd, u64 *out_off,
                             usize count) {
  /* Use positioned I/O (vfs_pread/pwrite) for the explicit-offset side so the
   * shared descriptor's own file offset is never disturbed — thread-safe, unlike
   * an lseek-save-restore that another thread sharing the fd could observe
   * mid-transfer. A NULL offset uses and advances the fd's own offset. */
  char kbuf[4096];
  isize total = 0;
  isize err = 0;
  u64 ipos = in_off ? *in_off : 0;
  u64 opos = out_off ? *out_off : 0;
  while (count > 0) {
    usize chunk = count > sizeof(kbuf) ? sizeof(kbuf) : count;
    isize r = in_off ? vfs_pread(in_fd, kbuf, chunk, ipos)
                     : vfs_read(in_fd, kbuf, chunk);
    if (r < 0) {
      err = r;
      break;
    }
    if (r == 0)
      break; /* EOF on input */
    if (in_off)
      ipos += (u64)r;
    isize w_done = 0;
    while (w_done < r) {
      isize w = out_off
                    ? vfs_pwrite(out_fd, kbuf + w_done, (usize)(r - w_done), opos)
                    : vfs_write(out_fd, kbuf + w_done, (usize)(r - w_done));
      if (w < 0) {
        err = w;
        break;
      }
      if (w == 0)
        break;
      w_done += w;
      if (out_off)
        opos += (u64)w;
    }
    total += w_done;
    count -= (usize)w_done;
    if (err || w_done < r)
      break; /* short/failed write — stop */
    if (r < (isize)chunk)
      break; /* short read = EOF */
  }

  if (in_off)
    *in_off = ipos;
  if (out_off)
    *out_off = opos;
  if (total == 0 && err)
    return err;
  return total;
}

/* sendfile(out_fd, in_fd, off*, count). off (when non-NULL) names the in_fd
 * start offset and receives the new position; the in_fd file offset is left
 * unchanged in that case. out_fd always uses and advances its own offset. */
static isize sys_sendfile(int out_fd, int in_fd, u64 *user_off, usize count) {
  u64 off;
  u64 *poff = 0;
  if (user_off) {
    if (syscall_copyin(&off, user_off, sizeof(off)) < 0)
      return -EFAULT;
    poff = &off;
  }
  isize ret = file_copy_range(in_fd, poff, out_fd, 0, count);
  if (ret >= 0 && poff && syscall_copyout(user_off, &off, sizeof(off)) < 0)
    return -EFAULT;
  return ret;
}

/* copy_file_range(fd_in, off_in*, fd_out, off_out*, len, flags). Both offsets
 * are independently optional; flags must be 0 (no Linux flags defined yet). */
static isize sys_copy_file_range(int fd_in, u64 *user_off_in, int fd_out,
                                 u64 *user_off_out, usize len,
                                 unsigned int flags) {
  if (flags != 0)
    return -EINVAL;
  u64 off_in, off_out;
  u64 *pin = 0, *pout = 0;
  if (user_off_in) {
    if (syscall_copyin(&off_in, user_off_in, sizeof(off_in)) < 0)
      return -EFAULT;
    pin = &off_in;
  }
  if (user_off_out) {
    if (syscall_copyin(&off_out, user_off_out, sizeof(off_out)) < 0)
      return -EFAULT;
    pout = &off_out;
  }
  isize ret = file_copy_range(fd_in, pin, fd_out, pout, len);
  if (ret >= 0) {
    if (pin && syscall_copyout(user_off_in, &off_in, sizeof(off_in)) < 0)
      return -EFAULT;
    if (pout && syscall_copyout(user_off_out, &off_out, sizeof(off_out)) < 0)
      return -EFAULT;
  }
  return ret;
}

/* splice(fd_in, off_in*, fd_out, off_out*, len, flags). Linux requires the
 * offset for a pipe end be NULL; we don't special-case pipes (the copy pump
 * read/writes either kind) but honor the same offset semantics. */
static isize sys_splice(int fd_in, u64 *user_off_in, int fd_out,
                        u64 *user_off_out, usize len, unsigned int flags) {
  (void)flags; /* SPLICE_F_* are advisory (MOVE/NONBLOCK/MORE/GIFT) */
  u64 off_in, off_out;
  u64 *pin = 0, *pout = 0;
  if (user_off_in) {
    if (syscall_copyin(&off_in, user_off_in, sizeof(off_in)) < 0)
      return -EFAULT;
    pin = &off_in;
  }
  if (user_off_out) {
    if (syscall_copyin(&off_out, user_off_out, sizeof(off_out)) < 0)
      return -EFAULT;
    pout = &off_out;
  }
  isize ret = file_copy_range(fd_in, pin, fd_out, pout, len);
  if (ret >= 0) {
    if (pin && syscall_copyout(user_off_in, &off_in, sizeof(off_in)) < 0)
      return -EFAULT;
    if (pout && syscall_copyout(user_off_out, &off_out, sizeof(off_out)) < 0)
      return -EFAULT;
  }
  return ret;
}

/* fallocate(fd, mode, offset, len). mode 0 (allocate) and FALLOC_FL_KEEP_SIZE
 * are honored by extending the file to offset+len when it is shorter (b1nix
 * filesystems allocate on write / have no preallocation primitive, so this is
 * the meaningful guarantee: the bytes exist and are zero). Hole-punching and
 * range collapse/insert/zero are not supported by the underlying drivers. */
static int sys_fallocate(int fd, int mode, u64 offset, u64 len) {
  if (len == 0)
    return -EINVAL;
  /* Only plain allocate (0) and KEEP_SIZE are representable; the rest need
   * driver support b1nix lacks. */
  if (mode & ~FALLOC_FL_KEEP_SIZE)
    return -EOPNOTSUPP;
  struct b1nix_stat st;
  if (vfs_fstat(fd, &st) < 0)
    return -EBADF;
  u64 end = offset + len;
  if (mode & FALLOC_FL_KEEP_SIZE)
    return 0; /* reservation only — no real preallocation to perform */
  if (end > st.st_size)
    return vfs_ftruncate(fd, end);
  return 0; /* already covered */
}

/* statx(dirfd, path, flags, mask, statxbuf). b1nix supports the common forms:
 * an absolute/cwd-relative path (dirfd == AT_FDCWD) and AT_EMPTY_PATH on an fd.
 * It maps the existing stat data into the Linux struct statx layout so glibc /
 * port binaries that prefer statx get real values. */
static int sys_statx(int dirfd, const char *user_path, int flags,
                     unsigned int mask, struct statx *user_buf) {
  struct b1nix_stat st;
  int rc;
  if ((flags & AT_EMPTY_PATH) && (!user_path || user_path[0] == '\0')) {
    rc = vfs_fstat(dirfd, &st);
  } else {
    char kpath[VFS_MAX_PATH];
    if (syscall_copyinstr(kpath, sizeof(kpath), user_path) < 0)
      return -EFAULT;
    /* Only AT_FDCWD (or an absolute path) is resolvable — no per-fd dir base. */
    if (dirfd != AT_FDCWD && kpath[0] != '/')
      return -EBADF;
    rc = (flags & AT_SYMLINK_NOFOLLOW) ? vfs_lstat(kpath, &st)
                                       : vfs_stat(kpath, &st);
  }
  if (rc < 0)
    return rc;

  struct statx sx;
  memset(&sx, 0, sizeof(sx));
  sx.stx_mask = mask & STATX_BASIC_STATS;
  sx.stx_blksize = (u32)st.st_blksize;
  sx.stx_nlink = st.st_nlink;
  sx.stx_uid = st.st_uid;
  sx.stx_gid = st.st_gid;
  sx.stx_mode = (u16)st.st_mode;
  sx.stx_ino = st.st_ino;
  sx.stx_size = st.st_size;
  sx.stx_blocks = st.st_blocks;
  sx.stx_atime.tv_sec = (i64)st.st_atim.tv_sec;
  sx.stx_atime.tv_nsec = (u32)st.st_atim.tv_nsec;
  sx.stx_mtime.tv_sec = (i64)st.st_mtim.tv_sec;
  sx.stx_mtime.tv_nsec = (u32)st.st_mtim.tv_nsec;
  sx.stx_ctime.tv_sec = (i64)st.st_ctim.tv_sec;
  sx.stx_ctime.tv_nsec = (u32)st.st_ctim.tv_nsec;
  sx.stx_btime = sx.stx_ctime; /* no separate birth time — report ctime */
  sx.stx_rdev_major = (u32)(st.st_rdev >> 8);
  sx.stx_rdev_minor = (u32)(st.st_rdev & 0xff);
  sx.stx_dev_major = (u32)(st.st_dev >> 8);
  sx.stx_dev_minor = (u32)(st.st_dev & 0xff);
  if (syscall_copyout(user_buf, &sx, sizeof(sx)) < 0)
    return -EFAULT;
  return 0;
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

/* execveat(dirfd, path, argv, envp, flags) — Linux nr 322. b1nix needs this for
 * musl fexecve(): musl first tries execveat(fd, "", ..., AT_EMPTY_PATH) and only
 * falls back to execing /proc/self/fd/N if that returns ENOSYS. Dropbear re-execs
 * itself per connection via fexecve; without a working execveat the re-exec fails
 * (EBADF) and the SSH session command never runs. */
static u64 sys_execveat(int dirfd, const char *user_path, const char **user_argv,
                        const char **user_envp, int flags) {
  char resolved[VFS_MAX_PATH];

  if (flags & AT_EMPTY_PATH) {
    /* The program to run IS the file already open at dirfd; user_path is empty.
     * Recover its path from the fd and exec that. */
    struct vfs_handle *h = scheduler_fd_get(dirfd);
    if (!h || !h->used || h->kind != VFS_HANDLE_NODE || !h->node)
      return (u64)-EBADF;
    if (vfs_get_node_path(h->node, resolved, VFS_MAX_PATH) < 0)
      return (u64)-EBADF;
  } else {
    char kpath[VFS_MAX_PATH];
    if (strncpy_from_user(kpath, user_path, VFS_MAX_PATH) < 0)
      return (u64)-EFAULT;
    kpath[VFS_MAX_PATH - 1] = '\0';
    if (kpath[0] == '\0')
      return (u64)-ENOENT;
    if (kpath[0] != '/' && dirfd != AT_FDCWD) {
      /* dirfd-relative: join the directory fd's path with the relative path. */
      struct vfs_handle *h = scheduler_fd_get(dirfd);
      if (!h || !h->used || h->kind != VFS_HANDLE_NODE || !h->node)
        return (u64)-EBADF;
      char dir[VFS_MAX_PATH];
      if (vfs_get_node_path(h->node, dir, VFS_MAX_PATH) < 0)
        return (u64)-EBADF;
      char joined[VFS_MAX_PATH];
      snprintf(joined, sizeof(joined), "%s/%s", dir, kpath);
      vfs_resolve_path(joined, resolved);
    } else {
      vfs_resolve_path(kpath, resolved);
    }
    if (resolved[0] == '\0')
      return (u64)-ENOENT;
  }

  char **kargv = copy_user_array(user_argv);
  if (IS_ERR(kargv))
    return (u64)PTR_ERR(kargv);
  char **kenvp = copy_user_array(user_envp);
  if (IS_ERR(kenvp)) {
    free_kernel_array(kargv);
    return (u64)PTR_ERR(kenvp);
  }

  u64 res = (u64)user_execve_current(resolved, (const char **)kargv,
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
#ifdef __x86_64__
  if (frame->cs != 0x23 || frame->ss != 0x1B)
    return 0;
#else
  if (frame->cs != 0x1B || frame->ss != 0x23)
    return 0;
#endif
  if (frame->rip >= USER_SPACE_LIMIT ||
      frame->rsp >= USER_SPACE_LIMIT)
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
  /* Verified self-host capability: the in-guest toolchain path builds kernel
   * translation units and links a real kernel.elf. Clang is now the preferred
   * native frontend; binutils still supplies the in-guest assembler/linker. */
  status->can_build_kernel_inside_b1nix = 1;
  copy_cstr(status->target_triple, sizeof(status->target_triple),
            "x86_64-b1nix");
  copy_cstr(status->compiler, sizeof(status->compiler), "clang-native");
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

/* Linux utimes(2) / utimensat(2) → vfs_utime. utimes passes struct timeval[2]
 * (sec/usec), utimensat struct timespec[2] (sec/nsec) with the UTIME_NOW /
 * UTIME_OMIT nsec sentinels. Only AT_FDCWD (or an absolute path) is resolvable
 * — same restriction as sys_statx above. */
#define LX_UTIME_NOW 0x3fffffffL
#define LX_UTIME_OMIT 0x3ffffffeL
static isize sys_linux_utimensat(int dirfd, const char *user_path,
                                 u64 times_ptr, int is_nsec) {
  char kpath[VFS_MAX_PATH];
  if (!user_path)
    return -EINVAL; /* futimens (path-less) form not supported */
  if (syscall_copyinstr(kpath, sizeof(kpath), user_path) < 0)
    return -EFAULT;
  if (dirfd != AT_FDCWD && kpath[0] != '/')
    return -EBADF;
  char resolved[VFS_MAX_PATH];
  vfs_resolve_path(kpath, resolved);

  u64 now = vfs_get_unix_time();
  u64 atime = now, mtime = now;
  if (times_ptr) {
    u64 tv[4]; /* [0]=a.sec [1]=a.frac [2]=m.sec [3]=m.frac */
    if (syscall_copyin(tv, (void *)(usize)times_ptr, sizeof(tv)) < 0)
      return -EFAULT;
    atime = tv[0];
    mtime = tv[2];
    if (is_nsec) {
      if (tv[1] == LX_UTIME_NOW)
        atime = now;
      if (tv[3] == LX_UTIME_NOW)
        mtime = now;
      if (tv[1] == LX_UTIME_OMIT || tv[3] == LX_UTIME_OMIT) {
        struct b1nix_stat st;
        if (vfs_stat(resolved, &st) == 0) {
          if (tv[1] == LX_UTIME_OMIT)
            atime = st.st_atim.tv_sec;
          if (tv[3] == LX_UTIME_OMIT)
            mtime = st.st_mtim.tv_sec;
        }
      }
    }
  }
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

static isize sys_access(const char *user_path, int mode) {
  if ((mode & ~(R_OK | W_OK | X_OK)) != 0)
    return -EINVAL;

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

  if (mode != 0) {
    const struct cred *cred = scheduler_get_current_cred();
    if (!cred) {
      vfs_node_put(node);
      return -EACCES;
    }

    struct cred access_cred = *cred;
    access_cred.euid = cred->uid;
    access_cred.egid = cred->gid;
    if (access_cred.euid == ROOT_UID && (mode & X_OK) &&
        (node->inode->mode & 0111) == 0) {
      vfs_node_put(node);
      return -EACCES;
    }
    if (!cred_can_access(&access_cred, node->inode->uid, node->inode->gid,
                         node->inode->mode, (u32)mode)) {
      vfs_node_put(node);
      return -EACCES;
    }
  }

  if (mode & 2) { // W_OK
    if (vfs_node_is_readonly(node)) {
      vfs_node_put(node);
      return -EROFS;
    }
  }

  vfs_node_put(node);
  return 0;
}

static isize sys_fchdir(int fd) {
  struct vfs_handle *h = scheduler_fd_get(fd);
  if (!h || !h->used)
    return -EBADF;
  if (h->kind != VFS_HANDLE_NODE || !h->node || !h->node->inode)
    return -EINVAL;
  if (h->node->inode->type != VFS_DIRECTORY)
    return -ENOTDIR;

  char resolved[VFS_MAX_PATH];
  int err = vfs_get_node_path(h->node, resolved, VFS_MAX_PATH);
  if (err < 0)
    return err;

  return (isize)scheduler_set_cwd(resolved);
}

static u64 sys_alarm(unsigned int seconds) {
  if (!current_task)
    return 0;

  u64 current_ticks = scheduler_get_uptime_ticks();
  u64 old_alarm = task_alarm_ticks(current_task);
  u64 remaining = 0;
  if (old_alarm > 0) {
    if (old_alarm > current_ticks) {
      remaining = (old_alarm - current_ticks + 99) / 100;
    } else {
      remaining = 0;
    }
  }

  if (seconds == 0) {
    task_set_alarm_ticks(current_task, 0);
  } else {
    task_set_alarm_ticks(current_task, current_ticks + (u64)seconds * 100);
  }

  return remaining;
}

static u64 sys_sigsuspend(const u64 *user_mask) {
  if (!current_task)
    return (u64)-EINVAL;
  if (!user_mask)
    return (u64)-EFAULT;

  u64 mask;
  if (syscall_copyin(&mask, user_mask, sizeof(u64)) < 0) {
    return (u64)-EFAULT;
  }

  task_set_saved_sigmask(current_task, current_task->blocked_signals, 1);

  u64 new_mask = mask & ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
  current_task->blocked_signals = new_mask;

  u64 flags = interrupts_save();
  while (1) {
    u64 pending = __atomic_load_n(&current_task->pending_signals, __ATOMIC_ACQUIRE) & ~current_task->blocked_signals;
    int has_deliverable = 0;
    for (int i = 1; i <= NSIG; i++) {
      if (pending & (1ULL << (i - 1))) {
        sighandler_t handler = current_task->sigactions[i - 1].sa_handler;
        if (handler == SIG_IGN || (handler == SIG_DFL && (i == SIGCHLD || i == SIGURG || i == SIGWINCH || (i == SIGCONT && current_task->state != TASK_STOPPED)))) {
          __atomic_fetch_and(&current_task->pending_signals, ~(1ULL << (i - 1)), __ATOMIC_RELAXED);
        } else if (handler == SIG_DFL &&
                   (i == SIGSTOP || i == SIGTSTP ||
                    i == SIGTTIN || i == SIGTTOU)) {
          current_task->state = TASK_STOPPED;
          current_task->last_stop_signal = i;
          current_task->stop_report_pending = 1;
          __atomic_fetch_and(&current_task->pending_signals,
                             ~(1ULL << (i - 1)), __ATOMIC_RELAXED);
          scheduler_notify_wait_event(current_task->parent_id);
          interrupts_restore(flags);
          scheduler_yield();
          flags = interrupts_save();
        } else if (handler == SIG_DFL && i == SIGCONT) {
          __atomic_fetch_and(&current_task->pending_signals,
                             ~(1ULL << (i - 1)), __ATOMIC_RELAXED);
        } else {
          has_deliverable = 1;
        }
      }
    }
    if (has_deliverable) {
      break;
    }
    current_task->state = TASK_BLOCKED;
    scheduler_yield();
  }
  interrupts_restore(flags);

  if (task_has_saved_sigmask(current_task)) {
    current_task->blocked_signals = task_saved_sigmask(current_task);
    task_clear_saved_sigmask(current_task);
  }

  return (u64)-EINTR;
}

static isize sys_getrlimit(int resource, struct rlimit *user_rlim) {
  struct rlimit rlim;
  int err = scheduler_getrlimit(resource, &rlim);
  if (err < 0)
    return err;

  if (syscall_copyout(user_rlim, &rlim, sizeof(rlim)) < 0)
    return -EFAULT;

  return 0;
}

static isize sys_setrlimit(int resource, const struct rlimit *user_rlim) {
  struct rlimit rlim;
  if (syscall_copyin(&rlim, user_rlim, sizeof(rlim)) < 0)
    return -EFAULT;

  return scheduler_setrlimit(resource, &rlim);
}

/* ── Extended-attribute syscalls (M44 / wave 7) ──
 * The path and name are NUL-terminated user strings; the value is a raw
 * byte buffer of `size` bytes. `nofollow` selects the l*xattr variant. */
static isize sys_setxattr(const char *user_path, const char *user_name,
                          const void *user_value, usize size, int flags,
                          int nofollow) {
  if (size > XATTR_VALUE_MAX)
    return -E2BIG;
  char *kpath = kmalloc(VFS_MAX_PATH);
  if (!kpath)
    return -ENOMEM;
  char kname[XATTR_NAME_MAX + 1];
  void *kval = 0;
  isize ret;
  if (strncpy_from_user(kpath, user_path, VFS_MAX_PATH) < 0) {
    ret = -EFAULT;
    goto out_path;
  }
  kpath[VFS_MAX_PATH - 1] = '\0';
  if (strncpy_from_user(kname, user_name, sizeof(kname)) < 0) {
    ret = -EFAULT;
    goto out_path;
  }
  if (size) {
    kval = kmalloc(size);
    if (!kval) {
      ret = -ENOMEM;
      goto out_path;
    }
    if (syscall_copyin(kval, user_value, size) < 0) {
      ret = -EFAULT;
      goto out_val;
    }
  }
  char resolved[VFS_MAX_PATH];
  vfs_resolve_path(kpath, resolved);
  ret = vfs_setxattr(resolved, kname, kval, size, flags, nofollow);
out_val:
  if (kval)
    kfree(kval);
out_path:
  kfree(kpath);
  return ret;
}

static isize sys_getxattr(const char *user_path, const char *user_name,
                          void *user_value, usize size, int nofollow) {
  char *kpath = kmalloc(VFS_MAX_PATH);
  if (!kpath)
    return -ENOMEM;
  char kname[XATTR_NAME_MAX + 1];
  isize ret;
  if (strncpy_from_user(kpath, user_path, VFS_MAX_PATH) < 0) {
    ret = -EFAULT;
    goto out;
  }
  kpath[VFS_MAX_PATH - 1] = '\0';
  if (strncpy_from_user(kname, user_name, sizeof(kname)) < 0) {
    ret = -EFAULT;
    goto out;
  }
  char resolved[VFS_MAX_PATH];
  vfs_resolve_path(kpath, resolved);
  if (size == 0) {
    ret = vfs_getxattr(resolved, kname, 0, 0, nofollow); /* size query */
    goto out;
  }
  usize cap = size > XATTR_VALUE_MAX ? XATTR_VALUE_MAX : size;
  void *kbuf = kmalloc(cap);
  if (!kbuf) {
    ret = -ENOMEM;
    goto out;
  }
  ret = vfs_getxattr(resolved, kname, kbuf, cap, nofollow);
  if (ret > 0 && syscall_copyout(user_value, kbuf, (usize)ret) < 0)
    ret = -EFAULT;
  kfree(kbuf);
out:
  kfree(kpath);
  return ret;
}

static isize sys_listxattr(const char *user_path, char *user_list, usize size,
                           int nofollow) {
  char *kpath = kmalloc(VFS_MAX_PATH);
  if (!kpath)
    return -ENOMEM;
  isize ret;
  if (strncpy_from_user(kpath, user_path, VFS_MAX_PATH) < 0) {
    ret = -EFAULT;
    goto out;
  }
  kpath[VFS_MAX_PATH - 1] = '\0';
  char resolved[VFS_MAX_PATH];
  vfs_resolve_path(kpath, resolved);
  if (size == 0) {
    ret = vfs_listxattr(resolved, 0, 0, nofollow); /* size query */
    goto out;
  }
  /* Bound the kernel buffer; the full xattr name set cannot exceed this. */
  usize cap = size > 8192 ? 8192 : size;
  char *kbuf = kmalloc(cap);
  if (!kbuf) {
    ret = -ENOMEM;
    goto out;
  }
  ret = vfs_listxattr(resolved, kbuf, cap, nofollow);
  if (ret > 0 && syscall_copyout(user_list, kbuf, (usize)ret) < 0)
    ret = -EFAULT;
  kfree(kbuf);
out:
  kfree(kpath);
  return ret;
}

static isize sys_removexattr(const char *user_path, const char *user_name,
                             int nofollow) {
  char *kpath = kmalloc(VFS_MAX_PATH);
  if (!kpath)
    return -ENOMEM;
  char kname[XATTR_NAME_MAX + 1];
  isize ret;
  if (strncpy_from_user(kpath, user_path, VFS_MAX_PATH) < 0) {
    ret = -EFAULT;
    goto out;
  }
  kpath[VFS_MAX_PATH - 1] = '\0';
  if (strncpy_from_user(kname, user_name, sizeof(kname)) < 0) {
    ret = -EFAULT;
    goto out;
  }
  char resolved[VFS_MAX_PATH];
  vfs_resolve_path(kpath, resolved);
  ret = vfs_removexattr(resolved, kname, nofollow);
out:
  kfree(kpath);
  return ret;
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

/* M40 — Linux stat/fstat/lstat. Same native lookup as the b1nix calls, but the
 * result is copied out in the Linux x86_64 `struct stat` layout (see
 * linux_stat_from_b1nix). Routed here from the dispatcher for Linux-personality
 * tasks only; native b1nix tasks keep using sys_stat/sys_fstat/sys_lstat. */
static isize sys_linux_stat_family(u64 lnr, u64 arg0, u64 arg1) {
  struct b1nix_stat kst;
  int res;
  if (lnr == LINUX_NR_FSTAT) {
    res = vfs_fstat((int)arg0, &kst);
  } else {
    char *kpath = kmalloc(VFS_MAX_PATH);
    if (!kpath)
      return -ENOMEM;
    if (strncpy_from_user(kpath, (const char *)(usize)arg0, VFS_MAX_PATH) < 0) {
      kfree(kpath);
      return -EFAULT;
    }
    kpath[VFS_MAX_PATH - 1] = '\0';
    char resolved[VFS_MAX_PATH];
    vfs_resolve_path(kpath, resolved);
    kfree(kpath);
    res = (lnr == LINUX_NR_LSTAT) ? vfs_lstat(resolved, &kst)
                                  : vfs_stat(resolved, &kst);
  }
  if (res != 0)
    return res;
  struct linux_stat lst;
  linux_stat_from_b1nix(&lst, &kst);
  if (copy_to_user((void *)(usize)arg1, &lst, sizeof(lst)) < 0)
    return -EFAULT;
  return 0;
}

/* Fill the b1nix uname result. Single source of truth shared by the native
 * SYS_UNAME path and the M40 Linux uname translation. */
static void fill_b1nix_utsname(struct b1nix_utsname *uts) {
  memset(uts, 0, sizeof(*uts));
  copy_cstr(uts->sysname, sizeof(uts->sysname), "B1NIX");
  copy_cstr(uts->nodename, sizeof(uts->nodename), "b1nix");
  copy_cstr(uts->release, sizeof(uts->release), B1NIX_VERSION_STR);
  copy_cstr(uts->version, sizeof(uts->version), "#1 SMP");
#if defined(__aarch64__)
  copy_cstr(uts->machine, sizeof(uts->machine), "aarch64");
#elif defined(__x86_64__)
  copy_cstr(uts->machine, sizeof(uts->machine), "x86_64");
#else
  copy_cstr(uts->machine, sizeof(uts->machine), "i686");
#endif
}

/* M40 — Linux uname. Same data as the native call, copied out in the wider
 * Linux `struct utsname` layout (six 65-byte fields + domainname). */
static isize sys_linux_uname(u64 arg0) {
  struct b1nix_utsname uts;
  fill_b1nix_utsname(&uts);
  struct linux_utsname lx;
  linux_utsname_from_b1nix(&lx, &uts);
  if (copy_to_user((void *)(usize)arg0, &lx, sizeof(lx)) < 0)
    return -EFAULT;
  return 0;
}

/* Map a b1nix dirent type (1=file, 2=device, 3=directory) to a Linux d_type. */
static u8 lx_dirent_type(const struct dirent *d) {
  switch (d->type) {
  case 3:
    return 4; /* DT_DIR */
  case 2:
    return 2; /* DT_CHR (b1nix "device") */
  case 1:
    return 8; /* DT_REG */
  default:
    return d->is_dir ? 4 : 0; /* DT_DIR or DT_UNKNOWN */
  }
}

/* M40 — Linux getdents64. The native getdents returns a fixed-size struct
 * dirent array; Linux expects variable-length linux_dirent64 records and a
 * byte count. Read a batch, repack as many as fit into the user buffer, then
 * rewind the directory index by the unemitted count so nothing is lost. */
static isize sys_linux_getdents64(int fd, u64 user_buf, usize count) {
  isize start = vfs_lseek(fd, 0, B1NIX_SEEK_CUR);
  if (start < 0)
    return start;
  struct dirent kbuf[32];
  isize n = vfs_getdents(fd, kbuf, 32);
  if (n <= 0)
    return n; /* 0 = end of directory, <0 = -errno */

  usize written = 0;
  isize emitted = 0;
  for (isize i = 0; i < n; i++) {
    usize namelen = 0;
    while (namelen < sizeof(kbuf[i].name) && kbuf[i].name[namelen])
      namelen++;
    /* d_name starts at byte 19; record is padded to an 8-byte multiple. */
    usize reclen = (19 + namelen + 1 + 7) & ~(usize)7;
    if (written + reclen > count)
      break; /* no room; leave this and the rest for the next call */

    char rec[19 + sizeof(kbuf[0].name) + 1 + 7];
    for (usize z = 0; z < reclen; z++)
      rec[z] = 0;
    struct linux_dirent64 *de = (struct linux_dirent64 *)rec;
    de->d_ino = (u64)(start + i + 1); /* b1nix dirent has no inode; synthesize */
    de->d_off = (i64)(start + i + 1); /* opaque cookie: index of the next entry */
    de->d_reclen = (u16)reclen;
    de->d_type = lx_dirent_type(&kbuf[i]);
    for (usize z = 0; z < namelen; z++)
      de->d_name[z] = kbuf[i].name[z];
    de->d_name[namelen] = '\0';

    if (copy_to_user((void *)(usize)(user_buf + written), rec, reclen) < 0)
      return -EFAULT;
    written += reclen;
    emitted++;
  }

  /* Consumed n from the handle but only emitted `emitted`; rewind the rest. */
  vfs_lseek(fd, start + emitted, B1NIX_SEEK_SET);
  if (emitted == 0)
    return -EINVAL; /* buffer too small for even one entry */
  return (isize)written;
}

/* M40 — Linux arch_prctl. glibc and TLS-using Linux binaries set the FS base
 * here rather than via a dedicated syscall; b1nix tracks the same per-task FS
 * base (task_set_tls_base), reloaded on the next return to userspace. */
static isize sys_linux_arch_prctl(u64 option, u64 addr) {
  extern void arch_set_fs_base(u64 base);
  switch (option) {
  case LINUX_ARCH_SET_FS:
    task_set_tls_base(current_task, addr);
    /* Linux arch_prctl is synchronous: the FS base must be live on return to
     * userspace, not only after the next context switch (which is when the
     * scheduler otherwise reloads IA32_FS_BASE). Write the MSR now. */
    arch_set_fs_base(addr);
    return 0;
  case LINUX_ARCH_GET_FS: {
    u64 base = task_tls_base(current_task);
    if (copy_to_user((void *)(usize)addr, &base, sizeof(base)) < 0)
      return -EFAULT;
    return 0;
  }
  default:
    return -EINVAL; /* ARCH_SET_GS/ARCH_GET_GS unsupported */
  }
}

/* M40 — Linux rt_sigprocmask. Same as the native call but the sigset_t bit
 * positions are remapped (b1nix and Linux number signals differently). */
static isize sys_linux_rt_sigprocmask(int how, u64 set_ptr, u64 oldset_ptr) {
  /* Linux SIG_UNBLOCK=1/SIG_SETMASK=2 are swapped relative to b1nix
   * (SIG_SETMASK=1/SIG_UNBLOCK=2); SIG_BLOCK=0 matches. */
  int b_how = (how == 1) ? 2 : (how == 2) ? 1 : how;
  u64 b_set = 0, b_old = 0;
  u64 *b_set_p = 0;
  if (set_ptr) {
    u64 lx_set = 0;
    if (syscall_copyin(&lx_set, (void *)(usize)set_ptr, sizeof(lx_set)) != 0)
      return -EFAULT;
    b_set = linux_sigset_to_b1nix(lx_set);
    b_set_p = &b_set;
  }
  if (scheduler_sigprocmask(b_how, b_set_p, oldset_ptr ? &b_old : 0) < 0)
    return -EINVAL;
  if (oldset_ptr) {
    u64 lx_old = b1nix_sigset_to_linux(b_old);
    if (syscall_copyout((void *)(usize)oldset_ptr, &lx_old, sizeof(lx_old)) != 0)
      return -EFAULT;
  }
  return 0;
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

/* True if a caught signal (one with an installed user handler) is deliverable
 * now. Used to make blocking select()/poll() return EINTR, as POSIX requires —
 * b1nix historically never interrupted them. Unlike the waitpid predicate this
 * DOES include SIGCHLD: dropbear's session loop arms a SIGCHLD handler that
 * writes a self-pipe to wake select(), and relies on select() returning EINTR
 * so it can reap the exited child and forward its output/exit-status in order.
 * SIGKILL/SIGSTOP are never caught, so excluding the SIG_DFL/SIG_IGN cases here
 * leaves their default handling to the normal delivery path. */
static int select_poll_signal_pending(void) {
  return scheduler_signal_pending();
}

static u64 sys_poll(struct b1nix_pollfd *user_fds, u64 nfds, u64 timeout) {
  /* 64: displayd alone polls 4 + MAX_CLIENTS(32) = 36 fds; the old cap of 16
   * silently dropped the tail of the array (those fds were never polled and
   * their revents never written back). */
  struct b1nix_pollfd fds[64];
  if (nfds > 64)
    nfds = 64;
  if (syscall_copyin(fds, user_fds, nfds * sizeof(struct b1nix_pollfd)) < 0)
    return -EFAULT;

  u64 start_ticks = scheduler_get_uptime_ticks();
  u64 timeout_ticks = timeout == (u64)-1 ? (u64)-1 : timeout / 10;

  extern void *vfs_poll_chan;

  while (1) {
    /* Publish BLOCKED on vfs_poll_chan BEFORE scanning the fds. Under -smp a
     * wake_all(vfs_poll_chan) from another CPU (data arriving on a socket/pipe)
     * firing between the scan and the sleep would otherwise be lost and the
     * poller would hang forever (the SSH/dropbear poll wedge). The SEQ_CST
     * fence in scheduler_wait_prepare orders the BLOCKED store ahead of the
     * vfs_poll reads, so a racing waker either is seen by the scan or observes
     * our BLOCKED state. */
    scheduler_wait_prepare(vfs_poll_chan);

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

    int timed_out = 0;
    if (ready == 0 && timeout != 0 && timeout != (u64)-1) {
      u64 now = scheduler_get_uptime_ticks();
      if (now - start_ticks >= timeout_ticks)
        timed_out = 1;
    }

    if (ready > 0 || timeout == 0 || timed_out) {
      scheduler_wait_cancel();
      current_task->wake_tick = 0;
      syscall_copyout(user_fds, fds, nfds * sizeof(struct b1nix_pollfd));
      return (u64)ready;
    }

    /* Interrupted by a caught signal? Abort with -ERESTARTSYS so the dispatch
     * tail returns EINTR (or restarts under SA_RESTART) and delivers the
     * handler. Checked with IRQs still disabled (from wait_prepare) so a signal
     * posted concurrently is not missed before we sleep. */
    if (select_poll_signal_pending()) {
      scheduler_wait_cancel();
      current_task->wake_tick = 0;
      return (u64)-ERESTARTSYS;
    }

    /* Re-arm the timer deadline EVERY iteration, after wait_prepare: an
     * explicit wake_all(vfs_poll_chan) (any fs/socket activity — the chan is
     * global) clears wake_tick when it promotes this task, so arming it only
     * once before the loop meant a spurious wake stripped the timeout and the
     * next sleep was unbounded — a poll(10ms) could then sleep tens of
     * seconds until unrelated traffic kicked the chan (netd's reactor wedge,
     * every socket() timing out with ETIMEDOUT meanwhile). */
    if (timeout != (u64)-1 && timeout != 0) {
      u64 ticks = timeout_ticks > 0 ? timeout_ticks : 1;
      current_task->wake_tick = start_ticks + ticks;
    }

    scheduler_wait_commit();
  }
}

static u64 sys_bind(int fd, const void *user_addr, usize addrlen) {
  u8 kaddr[sizeof(struct b1nix_sockaddr_un)];
  if (!user_addr || addrlen == 0 || addrlen > sizeof(kaddr))
    return (u64)-EINVAL;
  memset(kaddr, 0, sizeof(kaddr));
  if (syscall_copyin(kaddr, user_addr, addrlen) < 0)
    return (u64)-EFAULT;
  return (u64)vfs_bind(fd, kaddr, addrlen);
}

static u64 sys_connect(int fd, const void *user_addr, usize addrlen) {
  u8 kaddr[sizeof(struct b1nix_sockaddr_un)];
  if (!user_addr || addrlen == 0 || addrlen > sizeof(kaddr))
    return (u64)-EINVAL;
  memset(kaddr, 0, sizeof(kaddr));
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

/* sendto(fd, buf, len, flags, dest_addr, addrlen). dest_addr may be NULL, in
 * which case this is exactly send(). */
static u64 sys_sendto(int fd, const void *user_buf, usize len, int flags,
                      const void *user_addr, usize addrlen) {
  enum { SOCKET_IO_MAX = 64 * 1024 };
  u8 kaddr[sizeof(struct b1nix_sockaddr_un)];
  usize kaddrlen = 0;
  if (user_addr && addrlen) {
    if (addrlen > sizeof(kaddr))
      return (u64)-EINVAL;
    memset(kaddr, 0, sizeof(kaddr));
    if (syscall_copyin(kaddr, user_addr, addrlen) < 0)
      return (u64)-EFAULT;
    kaddrlen = addrlen;
  }
  if (len == 0)
    return 0;
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
  isize rc = vfs_socket_sendto(fd, kbuf, len, flags,
                               kaddrlen ? (const void *)kaddr : 0, kaddrlen);
  kfree(kbuf);
  return (u64)rc;
}

/* recvfrom(fd, buf, len, flags, src_addr, addrlen*). src_addr/addrlen may be
 * NULL, in which case this is exactly recv(). */
static u64 sys_recvfrom(int fd, void *user_buf, usize len, int flags,
                        void *user_addr, u32 *user_addrlen) {
  enum { SOCKET_IO_MAX = 64 * 1024 };
  u8 kaddr[sizeof(struct b1nix_sockaddr_un)];
  usize kaddrlen = 0;
  u32 uaddrlen = 0;
  if (user_addr && user_addrlen) {
    if (syscall_copyin(&uaddrlen, user_addrlen, sizeof(uaddrlen)) < 0)
      return (u64)-EFAULT;
    kaddrlen = uaddrlen > sizeof(kaddr) ? sizeof(kaddr) : uaddrlen;
    memset(kaddr, 0, sizeof(kaddr));
  }
  if (len == 0)
    return 0;
  if (len > SOCKET_IO_MAX)
    len = SOCKET_IO_MAX;
  if (!user_buf)
    return (u64)-EFAULT;
  void *kbuf = kmalloc(len);
  if (!kbuf)
    return (u64)-ENOMEM;
  usize got = kaddrlen;
  isize rc = vfs_socket_recvfrom(fd, kbuf, len, flags,
                                 kaddrlen ? (void *)kaddr : 0,
                                 kaddrlen ? &got : 0);
  if (rc > 0 && syscall_copyout(user_buf, kbuf, (usize)rc) < 0) {
    kfree(kbuf);
    return (u64)-EFAULT;
  }
  kfree(kbuf);
  if (rc >= 0 && kaddrlen) {
    u32 out = (u32)(got < kaddrlen ? got : kaddrlen);
    if (out && syscall_copyout(user_addr, kaddr, out) < 0)
      return (u64)-EFAULT;
    if (syscall_copyout(user_addrlen, &out, sizeof(out)) < 0)
      return (u64)-EFAULT;
  }
  return (u64)rc;
}

struct syscall_iovec {
  void *iov_base;
  usize iov_len;
};

struct syscall_msghdr {
  void *msg_name;
  u32 msg_namelen;
  struct syscall_iovec *msg_iov;
  int msg_iovlen;
  void *msg_control;
  usize msg_controllen;
  int msg_flags;
};

struct syscall_cmsghdr {
  usize cmsg_len;
  int cmsg_level;
  int cmsg_type;
};

#define K_SOL_SOCKET 1
#define K_SCM_RIGHTS 1
#define K_SCM_CREDENTIALS 2
#define K_MSG_CTRUNC 0x08
#define K_CMSG_ALIGN(n) (((n) + sizeof(usize) - 1) & ~(sizeof(usize) - 1))

static int copyin_message(const struct syscall_msghdr *user_msg,
                          struct syscall_msghdr *msg,
                          struct syscall_iovec *iov, char **payload,
                          usize *payload_len) {
  if (!user_msg || syscall_copyin(msg, user_msg, sizeof(*msg)) < 0)
    return -EFAULT;
  if (msg->msg_iovlen < 1 || msg->msg_iovlen > 16 || !msg->msg_iov)
    return -EINVAL;
  if (syscall_copyin(iov, msg->msg_iov,
                     (usize)msg->msg_iovlen * sizeof(*iov)) < 0)
    return -EFAULT;

  usize total = 0;
  for (int i = 0; i < msg->msg_iovlen; i++) {
    if (iov[i].iov_len > 65536 || total > 65536 - iov[i].iov_len)
      return -EMSGSIZE;
    total += iov[i].iov_len;
  }
  if (total == 0)
    return -EINVAL;
  char *buf = kmalloc(total);
  if (!buf)
    return -ENOMEM;
  *payload = buf;
  *payload_len = total;
  return 0;
}

static u64 sys_sendmsg(int fd, const struct syscall_msghdr *user_msg,
                       int flags) {
  struct syscall_msghdr msg;
  struct syscall_iovec iov[16];
  char *payload = 0;
  usize payload_len = 0;
  int err = copyin_message(user_msg, &msg, iov, &payload, &payload_len);
  if (err < 0)
    return (u64)err;

  usize off = 0;
  for (int i = 0; i < msg.msg_iovlen; i++) {
    if (iov[i].iov_len &&
        syscall_copyin(payload + off, iov[i].iov_base, iov[i].iov_len) < 0) {
      kfree(payload);
      return (u64)-EFAULT;
    }
    off += iov[i].iov_len;
  }

  struct vfs_handle *handles[VFS_SCM_MAX_FDS] = {0};
  usize nhandles = 0;
  int wants_cred = 0;
  if (msg.msg_control && msg.msg_controllen) {
    if (msg.msg_controllen > 512) {
      kfree(payload);
      return (u64)-EINVAL;
    }
    u8 control[512];
    if (syscall_copyin(control, msg.msg_control, msg.msg_controllen) < 0) {
      kfree(payload);
      return (u64)-EFAULT;
    }
    usize pos = 0;
    while (pos + sizeof(struct syscall_cmsghdr) <= msg.msg_controllen) {
      struct syscall_cmsghdr *c = (struct syscall_cmsghdr *)(control + pos);
      if (c->cmsg_len < sizeof(*c) ||
          c->cmsg_len > msg.msg_controllen - pos) {
        err = -EINVAL;
        goto sendmsg_fail;
      }
      usize header_len = K_CMSG_ALIGN(sizeof(*c));
      usize data_len = c->cmsg_len > header_len ? c->cmsg_len - header_len : 0;
      u8 *data = (u8 *)c + sizeof(*c);
      if (c->cmsg_level == K_SOL_SOCKET && c->cmsg_type == K_SCM_RIGHTS) {
        if (data_len == 0 || data_len % sizeof(int) != 0 ||
            data_len / sizeof(int) > VFS_SCM_MAX_FDS - nhandles) {
          err = -EINVAL;
          goto sendmsg_fail;
        }
        int *fds = (int *)data;
        usize count = data_len / sizeof(int);
        for (usize i = 0; i < count; i++) {
          struct vfs_handle *h = scheduler_fd_get_retain(fds[i]);
          if (!h) {
            err = -EBADF;
            goto sendmsg_fail;
          }
          handles[nhandles++] = h;
        }
      } else if (c->cmsg_level == K_SOL_SOCKET &&
                 c->cmsg_type == K_SCM_CREDENTIALS) {
        wants_cred = 1;
      } else {
        err = -EINVAL;
        goto sendmsg_fail;
      }
      usize next = K_CMSG_ALIGN(c->cmsg_len);
      if (next == 0)
        break;
      pos += next;
    }
  }

  struct b1nix_ucred cred;
  struct b1nix_ucred *cred_ptr = 0;
  if (wants_cred) {
    const struct cred *current_cred = scheduler_get_current_cred();
    cred.pid = current_task ? (int)task_tgid(current_task) : 0;
    cred.uid = current_cred ? current_cred->euid : 0;
    cred.gid = current_cred ? current_cred->egid : 0;
    cred_ptr = &cred;
  }
  isize rc;
  if (!nhandles && !cred_ptr && msg.msg_name && msg.msg_namelen) {
    /* sendmsg() with msg_name is sendto() with the address in the header —
     * the destination applies per message, no connect() required. */
    u8 kaddr[sizeof(struct b1nix_sockaddr_un)];
    usize alen = msg.msg_namelen > sizeof(kaddr) ? sizeof(kaddr)
                                                 : msg.msg_namelen;
    memset(kaddr, 0, sizeof(kaddr));
    if (syscall_copyin(kaddr, msg.msg_name, alen) < 0) {
      err = -EFAULT;
      goto sendmsg_fail;
    }
    rc = vfs_socket_sendto(fd, payload, payload_len, flags, kaddr, alen);
  } else {
    rc = vfs_socket_sendmsg(fd, payload, payload_len, flags, handles, nhandles,
                            cred_ptr);
  }
  if (rc >= 0) {
    kfree(payload);
    return (u64)rc;
  }
  err = (int)rc;

sendmsg_fail:
  for (usize i = 0; i < nhandles; i++)
    if (handles[i])
      vfs_handle_release(handles[i]);
  kfree(payload);
  return (u64)err;
}

static u64 sys_recvmsg(int fd, struct syscall_msghdr *user_msg, int flags) {
  struct syscall_msghdr msg;
  struct syscall_iovec iov[16];
  char *payload = 0;
  usize payload_len = 0;
  int err = copyin_message(user_msg, &msg, iov, &payload, &payload_len);
  if (err < 0)
    return (u64)err;

  usize header_space = K_CMSG_ALIGN(sizeof(struct syscall_cmsghdr));
  usize fd_capacity = 0;
  if (msg.msg_control && msg.msg_controllen > header_space)
    fd_capacity = (msg.msg_controllen - header_space) / sizeof(int);
  if (fd_capacity > VFS_SCM_MAX_FDS)
    fd_capacity = VFS_SCM_MAX_FDS;

  int received_fds[VFS_SCM_MAX_FDS];
  usize received_count = 0;
  struct b1nix_ucred cred;
  int has_cred = 0;
  int ctrunc = 0;
  isize rc = vfs_socket_recvmsg(fd, payload, payload_len, flags, received_fds,
                                fd_capacity, &received_count, &cred, &has_cred,
                                &ctrunc);
  if (rc < 0) {
    kfree(payload);
    return (u64)rc;
  }

  usize copied = 0;
  for (int i = 0; i < msg.msg_iovlen && copied < (usize)rc; i++) {
    usize chunk = iov[i].iov_len;
    if (chunk > (usize)rc - copied)
      chunk = (usize)rc - copied;
    if (chunk && syscall_copyout(iov[i].iov_base, payload + copied, chunk) < 0) {
      for (usize j = 0; j < received_count; j++)
        vfs_close(received_fds[j]);
      kfree(payload);
      return (u64)-EFAULT;
    }
    copied += chunk;
  }
  kfree(payload);

  if (msg.msg_name && msg.msg_namelen) {
    u8 zero[128] = {0};
    usize n = msg.msg_namelen < sizeof(zero) ? msg.msg_namelen : sizeof(zero);
    if (syscall_copyout(msg.msg_name, zero, n) < 0)
      return (u64)-EFAULT;
  }

  u8 control[512] = {0};
  usize control_len = 0;
  if (received_count) {
    usize data_len = received_count * sizeof(int);
    usize cmsg_len = header_space + data_len;
    usize space = header_space + K_CMSG_ALIGN(data_len);
    if (space <= msg.msg_controllen) {
      struct syscall_cmsghdr *c = (struct syscall_cmsghdr *)control;
      c->cmsg_len = cmsg_len;
      c->cmsg_level = K_SOL_SOCKET;
      c->cmsg_type = K_SCM_RIGHTS;
      memcpy(control + sizeof(*c), received_fds, data_len);
      control_len = space;
    } else {
      for (usize i = 0; i < received_count; i++)
        vfs_close(received_fds[i]);
      ctrunc = 1;
    }
  }
  if (has_cred) {
    usize cmsg_len = header_space + sizeof(cred);
    usize space = header_space + K_CMSG_ALIGN(sizeof(cred));
    if (control_len + space <= msg.msg_controllen) {
      struct syscall_cmsghdr *c =
          (struct syscall_cmsghdr *)(control + control_len);
      c->cmsg_len = cmsg_len;
      c->cmsg_level = K_SOL_SOCKET;
      c->cmsg_type = K_SCM_CREDENTIALS;
      memcpy((u8 *)c + sizeof(*c), &cred, sizeof(cred));
      control_len += space;
    } else {
      ctrunc = 1;
    }
  }
  if (control_len && syscall_copyout(msg.msg_control, control, control_len) < 0)
    return (u64)-EFAULT;

  msg.msg_controllen = control_len;
  msg.msg_flags = ctrunc ? K_MSG_CTRUNC : 0;
  if (syscall_copyout(user_msg, &msg, sizeof(msg)) < 0)
    return (u64)-EFAULT;
  return (u64)rc;
}

static u64 sys_listen(int fd, int backlog) {
  return (u64)vfs_listen(fd, backlog);
}

/* M57: socketpair(domain, type, protocol, int sv[2]). Allocates the two fds in
 * the kernel, then copies them out; on copyout failure both are closed so no
 * descriptor leaks into a process that never learns its number. */
/* Remap the b1nix signal number embedded in a wait status word to the Linux
 * numbering for a Linux-personality waiter. Termination: signal in bits 0-6;
 * stopped: ((sig<<8)|0x7f). 0xffff (continued) and plain exit statuses pass
 * through unchanged. Without the stopped-case remap, musl's WSTOPSIG saw
 * b1nix SIGTTIN (17) where it expected Linux SIGTTIN (21). */
static int wait_status_to_linux(int kstatus) {
  if ((kstatus & 0xff) == 0x7f && kstatus != 0xffff) {
    int linux_sig = b1nix_signo_to_linux((kstatus >> 8) & 0xff);
    if (linux_sig > 0)
      return (linux_sig << 8) | 0x7f;
  } else if ((kstatus & 0x7f) != 0 && kstatus != 0xffff) {
    int linux_sig = b1nix_signo_to_linux(kstatus & 0x7f);
    if (linux_sig > 0)
      return (kstatus & ~0x7f) | linux_sig;
  }
  return kstatus;
}

static u64 sys_socketpair(int domain, int type, int protocol, int *user_sv) {
  if (!user_sv)
    return (u64)-EFAULT;
  int sv[2];
  int rc = vfs_socketpair(domain, type, protocol, sv);
  if (rc < 0)
    return (u64)rc;
  if (syscall_copyout(user_sv, sv, sizeof(sv)) < 0) {
    vfs_close(sv[0]);
    vfs_close(sv[1]);
    return (u64)-EFAULT;
  }
  return 0;
}

/* socklen_t width differs by personality: Linux/musl socklen_t is 4 bytes,
 * b1nix's native libc used a pointer-sized usize. Copying 8 bytes for a Linux
 * binary reads 4 bytes of garbage into the length AND — far worse — the
 * write-back stomps the 4 bytes NEXT TO the caller's socklen_t (libcurl kept a
 * struct pointer there: nsfb crashed with a non-canonical hash pointer in
 * Curl_multi_will_close after getpeername mangled it). */
static int socklen_is_u32(void) {
  struct task *t = current_task;
  return t && t->user_image &&
         ((struct user_loaded_image *)t->user_image)->personality ==
             PERSONALITY_LINUX;
}
static int socklen_copyin(usize *klen, const void *user) {
  if (socklen_is_u32()) {
    u32 v = 0;
    if (syscall_copyin(&v, user, sizeof(v)) < 0)
      return -1;
    *klen = v;
    return 0;
  }
  return syscall_copyin(klen, user, sizeof(usize)) < 0 ? -1 : 0;
}
static int socklen_copyout(void *user, usize klen) {
  if (socklen_is_u32()) {
    u32 v = (u32)klen;
    return syscall_copyout(user, &v, sizeof(v)) < 0 ? -1 : 0;
  }
  return syscall_copyout(user, &klen, sizeof(usize)) < 0 ? -1 : 0;
}

static u64 sys_accept(int fd, void *addr, usize *addrlen) {
  /*addrlen is both in and out */
  usize k_addrlen = 0;
  if (addrlen) {
    if (socklen_copyin(&k_addrlen, addrlen) != 0) return (u64)-EFAULT;
  }

  char k_addr[128]; /* enough for sockaddr_un */
  int res = vfs_accept(fd, k_addr, &k_addrlen);
  if (res >= 0) {
    if (addr && k_addrlen > 0) {
      if (k_addrlen > sizeof(k_addr)) k_addrlen = sizeof(k_addr);
      if (syscall_copyout(addr, k_addr, k_addrlen) != 0) return (u64)-EFAULT;
    }
    if (addrlen) {
      if (socklen_copyout(addrlen, k_addrlen) != 0) return (u64)-EFAULT;
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
  if (socklen_copyin(&klen, user_optlen) != 0)
    return (u64)-EFAULT;
  u8 kopt[64];
  if (klen == 0 || klen > sizeof(kopt))
    return (u64)-EINVAL;
  int rc = vfs_getsockopt(fd, level, optname, kopt, &klen);
  if (rc < 0)
    return (u64)rc;
  if (user_optval && klen > 0 && syscall_copyout(user_optval, kopt, klen) < 0)
    return (u64)-EFAULT;
  if (socklen_copyout(user_optlen, klen) != 0)
    return (u64)-EFAULT;
  return 0;
}

static u64 sys_getsockaddr(int fd, void *user_addr, usize *user_addrlen,
                           int want_peer) {
  usize klen = 0;
  if (!user_addrlen)
    return (u64)-EINVAL;
  if (socklen_copyin(&klen, user_addrlen) != 0)
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
  if (socklen_copyout(user_addrlen, klen) != 0)
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
#ifdef __x86_64__
      /* A non-FIXED hint is advisory. The low 4 GiB is the bootstrap identity
       * map (2 MB supervisor huge pages cloned into every address space), where
       * a 4 KiB user PTE can't be inserted — an access there faults as a present
       * supervisor page (P=1), which the lazy-map #PF path can't service. So a
       * hint below 4 GiB (e.g. V8's code-cage reservation requesting ~0x6000000)
       * must be ignored and relocated, exactly like vm_find_free_area, which
       * deliberately starts at 0x100000000 for the same reason. */
      if (vaddr != 0 && vaddr < 0x100000000ULL)
        vaddr = 0; /* force the relocation paths below */
#endif
      if (vaddr == 0 || (vaddr & (PAGE_SIZE - 1)) != 0) {
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

  if ((flags & MAP_ANONYMOUS) && ((flags & MAP_NORESERVE) || prot == PROT_NONE)) {
    /* Lazy commit — no frame reserved up front; the page-fault handler's Case 1
     * zero-fills a fresh frame on first touch (anonymous → no VMA node → stays
     * zeroed). Used for two cases:
     *   - MAP_NORESERVE anonymous: defer the physical allocation (documented
     *     NORESERVE semantics, no fake reservation accounting).
     *   - PROT_NONE anonymous: a pure reservation/decommit with no access, so a
     *     physical frame is pointless until the region is mprotect'd to an
     *     accessible mode and touched. Eagerly allocating frames here drained
     *     and corrupted the PMM free list under V8's JIT, whose cage reservations
     *     and OS::DecommitPages (mmap PROT_NONE | MAP_FIXED, no NORESERVE) churn
     *     large PROT_NONE ranges. */
    extern void tlb_shootdown_poll(void);
    /* A pure PROT_NONE reservation needs NO eager per-page PTEs. V8's sandbox
     * does enormous ones — a ~1.4 TiB cage, plus a Smi-range loop that issues
     * 256 back-to-back 4 GiB PROT_NONE reservations (sandbox.cc) — and marking
     * each page would run hundreds of millions of iterations and hang the boot.
     * The #PF handler's anonymous fast path (paging.c "Lazy Allocation for User
     * Heap/Mmap region") already zero-fills any not-present anonymous fault in
     * [0x40000000, USER_SPACE_LIMIT) with no leaf PTE, and every mmap region
     * lands at >= 0x100000000, so the reserved range faults in lazily on first
     * touch with no per-page setup. Just record the VMA below.
     * ponytail: PROT_NONE is not strictly enforced on the skipped range (a wild
     * touch zero-fills instead of SIGSEGV) — but the eager path didn't enforce
     * it either (it set VMM_LAZY and lazily mapped on the fault), so there is no
     * semantic regression. NORESERVE *with* access (prot != PROT_NONE, e.g. a
     * read-only or RW lazy-commit region) keeps the eager path so its protection
     * bits are honored on fault-in. */
    if (prot != PROT_NONE) {
      for (u64 v = vaddr; v < vaddr + length; v += PAGE_SIZE) {
        vmm_set_lazy(v);
        paging_mprotect_page(v, vmm_flags);
        /* A large mmap (V8's multi-GB JIT regions) walks many pages; drain any
         * in-flight cross-CPU TLB shootdown so an initiator on another CPU isn't
         * left spinning until its timeout guard fires (tlb_shootdown_poll is a
         * single load when nothing is pending). */
        tlb_shootdown_poll();
      }
    }
  } else if (flags & MAP_ANONYMOUS) {
    extern void tlb_shootdown_poll(void);
    u64 direct_base = vmm_direct_map_base();
    for (u64 v = vaddr; v < vaddr + length; v += PAGE_SIZE) {
      tlb_shootdown_poll(); /* see the lazy path above */
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
  } else if (node && node->inode && node->inode->type == VFS_DEVICE &&
             (node->inode->mmap_handle_phys_cb || node->inode->mmap_phys_cb)) {
    /* M47 device-memory mmap (/dev/fb0): map the device's physical frames
     * directly, shared across fork (VMM_SHARED bypasses CoW) and refcounted
     * per mapping like SysV shm — munmap/teardown decrement, the device's
     * own reference keeps the frames alive. */
    u64 phys = 0;
    struct vfs_handle *handle = scheduler_fd_get(fd);
    int rc = node->inode->mmap_handle_phys_cb
                 ? node->inode->mmap_handle_phys_cb(
                       handle, (u64)offset, length, &phys)
                 : node->inode->mmap_phys_cb(
                       node, (u64)offset, length, &phys);
    if (rc < 0) /* node is borrowed from the fd table — nothing to put */
      return (u64)rc;
    for (u64 v = vaddr; v < vaddr + length; v += PAGE_SIZE) {
      u64 frame = phys + (v - vaddr);
      vmm_map_page(v, frame, vmm_flags | VMM_SHARED | VMM_PRESENT);
      pmm_ref_frame(frame);
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
  if (vma->node && vma->node->inode && vma->node->inode->mmap_open_cb)
    vma->node->inode->mmap_open_cb(vma->node);
  if (vma->node && vma->node->inode && vma->node->inode->mmap_range_open_cb)
    vma->node->inode->mmap_range_open_cb(vma->node, (u64)offset, length);

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

  if (length > (usize)(0x00007FFFFFFFFFFFULL - start))
    return -EINVAL;
  u64 end = start + ((length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
  if (end < start || end > 0x00007FFFFFFFFFFFULL)
    return -EINVAL;

  // 1. Unmap pages from hardware page tables and free physical frames
  for (u64 v = start; v < end; v += PAGE_SIZE) {
    vmm_unmap_page(v);
  }

  // 2. Update VMA list using the new robust helper
  vma_delete_range(t, start, end);

  return 0;
}

/* Serialize the address-space mutators (mmap / munmap / mprotect) across all
 * threads of every process. The VMA list is a raw singly-linked list shared by
 * every CLONE_VM thread (child->vma_list = parent->vma_list), and none of the
 * mutators locked it: two threads mutating it at once — classically a musl
 * detached thread freeing its own stack (munmap) while the parent starts the
 * next pthread_create (mmap of a fresh stack) — corrupted the next-pointers and
 * hung the following list walk (vm_find_free_area) forever. This is a coarse
 * sleeping mutex (a busy-flag with scheduler_yield, so a mutator may still block
 * on frame reclaim / writeback while holding it): mmap is not a hot path, and
 * correctness beats the lost parallelism. The page-fault handler intentionally
 * does NOT take it — it only reads the list, the pre-existing read/write race is
 * unchanged, and taking a yielding lock in the fault path is unsafe. */
static volatile int g_vma_mutex;
static void vma_mutator_lock(void) {
  while (__sync_lock_test_and_set(&g_vma_mutex, 1))
    scheduler_yield();
}
static void vma_mutator_unlock(void) {
  __sync_lock_release(&g_vma_mutex);
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
  if (end < start || end > USER_SPACE_LIMIT)
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

/* madvise(addr, length, advice). Only the calling process's own mapping is
 * touched. MADV_DONTNEED (and MADV_FREE, see below) drop the backing pages of
 * an anonymous range so the next access lazily refaults to a fresh zeroed page;
 * the hint advices are accepted as no-ops. */
static isize sys_madvise(void *addr, usize length, int advice) {
  u64 start = (u64)(usize)addr;
  if ((start & (PAGE_SIZE - 1)) != 0)
    return -EINVAL; /* POSIX: addr must be page-aligned */
  if (length == 0)
    return 0;
  if (start >= USER_SPACE_LIMIT)
    return -EINVAL;
  if (length > (usize)(USER_SPACE_LIMIT - start))
    return -EINVAL;
  u64 end = (start + length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
  if (end < start || end > USER_SPACE_LIMIT)
    return -EINVAL;

  switch (advice) {
  case MADV_NORMAL:
  case MADV_RANDOM:
  case MADV_SEQUENTIAL:
  case MADV_WILLNEED:
  case MADV_DONTFORK:
  case MADV_DOFORK:
  case MADV_HUGEPAGE:
  case MADV_NOHUGEPAGE:
    /* Accepted hints b1nix does not act on (no prefetch, no fork-inherit
     * control, no transparent hugepages) — legal POSIX no-op. */
    return 0;
  case MADV_DONTNEED:
  case MADV_FREE:
    break;
  default:
    return -EINVAL;
  }

  struct task *t = current_task;
  if (!t)
    return -ESRCH;

  /* The range must lie entirely inside mapped regions (POSIX ENOMEM otherwise).
   * MADV_DONTNEED/FREE here is implemented only for anonymous, non-shared
   * mappings — dropping a file-backed or MAP_SHARED page would discard data, so
   * we leave those untouched (a conservative, data-safe no-op). For anonymous
   * MAP_PRIVATE pages MADV_FREE is treated as MADV_DONTNEED: the page content is
   * discarded and the next access yields a zeroed page (documented simplification
   * — b1nix has no lazy-reclaim queue to keep the old contents on a read). */
  for (u64 v = start; v < end; v += PAGE_SIZE) {
    struct vm_area *vma = t->vma_list;
    struct vm_area *cover = 0;
    while (vma) {
      if (v >= vma->start && v < vma->end) {
        cover = vma;
        break;
      }
      vma = vma->next;
    }
    if (!cover)
      return -ENOMEM; /* unmapped page in range */

    int anon = (cover->flags & MAP_ANONYMOUS) != 0 || cover->node == 0;
    int shared = (cover->flags & MAP_SHARED) != 0;
    if (!anon || shared)
      continue; /* never discard file/shared data */

    u64 vmm_flags = VMM_USER;
    if (cover->prot & PROT_WRITE)
      vmm_flags |= VMM_WRITABLE;

    /* Drop the present frame (vmm_unmap_page frees it + unregisters from the
     * eviction list), then re-arm the page as lazy so the next touch refaults
     * to a fresh zeroed anonymous page (page-fault Case 1). */
    vmm_unmap_page(v);
    vmm_set_lazy(v);
    paging_mprotect_page(v, vmm_flags);
  }

  return 0;
}

/* M72: msync(addr, length, flags). Write back the dirty pages of a file-backed
 * MAP_SHARED mapping to its backing file. A store through such a mapping lands
 * in the page-frame that backs the mapping and sets only the hardware PTE dirty
 * bit — neither the page-cache DIRTY flag (so a clean page-cache entry can be
 * dropped by eviction, losing the write) nor anything fsync(2) keys on. msync
 * walks the range, test-and-clears each page's PTE dirty bit, and for the pages
 * userspace actually wrote, writes the page-frame's contents straight back to
 * the backing file via the inode write_cb (which goes through the block cache,
 * coherent with the read path). Writing the frame directly — rather than via a
 * page-cache lookup that may have been evicted — is what makes the data durable.
 * MS_ASYNC schedules instead of waiting: it just marks the page-cache entry
 * dirty (if still resident) so a later flush/eviction persists it. MS_INVALIDATE
 * is a no-op (mappers share the frame, so views are already coherent). */
static isize sys_msync(void *addr, usize length, int flags) {
  extern int paging_test_and_clear_dirty(u64 pml4_phys, u64 vaddr);
  extern u64 paging_user_frame(u64 pml4_phys, u64 vaddr);
  u64 start = (u64)(usize)addr;
  if (start & (PAGE_SIZE - 1))
    return -EINVAL; /* POSIX: addr must be page-aligned */
  if (flags & ~(MS_ASYNC | MS_SYNC | MS_INVALIDATE))
    return -EINVAL;
  if ((flags & MS_ASYNC) && (flags & MS_SYNC))
    return -EINVAL;
  if (length == 0)
    return 0;
  struct task *t = current_task;
  if (!t)
    return -ESRCH;
  if (start >= USER_SPACE_LIMIT || length > (usize)(USER_SPACE_LIMIT - start))
    return -ENOMEM;
  u64 end = (start + length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

  /* The range must be fully mapped (POSIX ENOMEM otherwise). */
  for (u64 v = start; v < end; v += PAGE_SIZE) {
    struct vm_area *vma = t->vma_list;
    int covered = 0;
    while (vma) {
      if (v >= vma->start && v < vma->end) {
        covered = 1;
        break;
      }
      vma = vma->next;
    }
    if (!covered)
      return -ENOMEM;
  }

  for (struct vm_area *vma = t->vma_list; vma; vma = vma->next) {
    if (vma->end <= start || vma->start >= end)
      continue;
    if (!(vma->flags & MAP_SHARED))
      continue; /* MAP_PRIVATE writes are not written back */
    if (!vma->node || !vma->node->inode ||
        vma->node->inode->type != VFS_FILE)
      continue;
    struct vfs_inode *inode = vma->node->inode;
    if (!inode->write_cb)
      continue;
    u64 lo = vma->start > start ? vma->start : start;
    u64 hi = vma->end < end ? vma->end : end;
    for (u64 v = lo; v < hi; v += PAGE_SIZE) {
      if (!paging_test_and_clear_dirty(t->pml4_phys, v))
        continue;
      u64 file_off = (u64)vma->offset + (v - vma->start);
      u64 file_page = file_off & ~(PAGE_SIZE - 1);

      /* Keep a resident page-cache entry coherent (and dirty for fsync/eviction). */
      struct page_cache_entry *page = page_cache_get_page(inode, file_page);
      if (page) {
        page_cache_mark_dirty(page);
        page_cache_put_page(page);
      }

      if (flags & MS_ASYNC)
        continue; /* scheduled: leave the durable write to flush/eviction */

      /* Synchronous: write the frame straight to the backing file. */
      u64 frame = paging_user_frame(t->pml4_phys, v);
      if (!frame)
        continue;
      void *frame_virt = (void *)(usize)(frame + vmm_direct_map_base());
      usize wsize = PAGE_SIZE;
      if (file_page + PAGE_SIZE > inode->size)
        wsize = (file_page < inode->size) ? (usize)(inode->size - file_page) : 0;
      if (wsize == 0)
        continue;
      struct vfs_node dummy;
      memset(&dummy, 0, sizeof(dummy));
      dummy.inode = inode;
      inode->write_cb(&dummy, file_page, (const char *)frame_virt, wsize, 0);
    }
  }
  return 0;
}

/* sigaltstack(ss, old_ss). Per-process alternate signal stack kept in a
 * scheduler side-table. The kernel honors SA_ONSTACK at signal delivery by
 * placing the signal frame at the top of this stack. */
static isize sys_sigaltstack(const void *user_ss, void *user_old) {
  struct task *t = current_task;
  if (!t)
    return -ESRCH;

  /* Report the current setting first (POSIX: old_ss reflects state before the
   * new ss is applied). SS_ONSTACK is reported when the task is currently
   * executing on its alt stack, derived from the live user SP. */
  if (user_old) {
    kstack_t cur;
    task_get_altstack(t, &cur);
    /* saved_user_rsp holds the user SP (rsp/esp) captured at kernel entry. */
    u64 sp = t->saved_user_rsp;
    if (cur.ss_size != 0 && task_on_altstack(t, sp))
      cur.ss_flags |= SS_ONSTACK;
    if (syscall_copyout(user_old, &cur, sizeof(cur)) < 0)
      return -EFAULT;
  }

  if (user_ss) {
    kstack_t ss;
    if (syscall_copyin(&ss, user_ss, sizeof(ss)) < 0)
      return -EFAULT;
    /* Cannot change the alt stack while executing on it. */
    if (task_altstack_top(t) && task_on_altstack(t, t->saved_user_rsp))
      return -EPERM;
    if (ss.ss_flags & ~(SS_DISABLE | SS_ONSTACK))
      return -EINVAL;
    if (!(ss.ss_flags & SS_DISABLE)) {
      if (ss.ss_size < MINSIGSTKSZ)
        return -ENOMEM;
      if (ss.ss_sp == 0 || ss.ss_sp >= USER_SPACE_LIMIT)
        return -EINVAL;
    }
    if (task_set_altstack(t, &ss) < 0)
      return -ENOMEM;
  }

  return 0;
}

/* sigreturn is now in kernel/arch/x86_64/signal.c */

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

static u64 syscall_dispatch_impl_inner(u64 number, u64 arg0, u64 arg1, u64 arg2, u64 arg3,
                     u64 arg4, u64 arg5, struct interrupt_frame *frame);

u64 syscall_dispatch_impl(u64 number, u64 arg0, u64 arg1, u64 arg2, u64 arg3,
                     u64 arg4, u64 arg5, struct interrupt_frame *frame) {
  u64 ret = syscall_dispatch_impl_inner(number, arg0, arg1, arg2, arg3, arg4, arg5, frame);

  if (frame) {
    if (klog_debug_enabled("signal") && current_task &&
        current_task->user_image &&
        ((struct user_loaded_image *)current_task->user_image)->personality ==
            PERSONALITY_LINUX &&
        (number == 39 || number == LINUX_NR_RT_SIGACTION)) {
      char sigbuf[160];
      snprintf(sigbuf, sizeof(sigbuf),
               "return task=%s pid=%u nr=%llu rip=%p rsp=%p ret=%p",
               current_task->name ? current_task->name : "?",
               (unsigned)current_task->id, (unsigned long long)number,
               (void *)(usize)frame->rip, (void *)(usize)frame->rsp,
               (void *)(usize)ret);
      klog_debug_category("signal", sigbuf);
    }
    if (ret == (u64)-ERESTARTSYS) {
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
        frame->rax = (usize)-EINTR;
      }
    }

    frame->rax = ret;
    arch_check_and_deliver_signals(frame);
    if (!user_frame_is_valid(frame)) {
      scheduler_exit_current(-SIGSEGV);
      return (u64)-EFAULT;
    }
  }

  return ret;
}

static u64 syscall_dispatch_impl_inner(u64 number, u64 arg0, u64 arg1, u64 arg2, u64 arg3,
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

  /* M63: seccomp-bpf. A task that installed a filter has every syscall screened
   * before it runs, using the raw ABI number the caller used (so a Linux-
   * personality task's filter sees Linux numbers, matching Linux semantics).
   * ALLOW returns 0 and the call proceeds; a denial returns the filter's -errno
   * without running the syscall; a KILL verdict terminates the task inside
   * seccomp_filter_syscall and never returns. Only filtered tasks pay any cost. */
  if (frame && seccomp_active()) {
    isize sv = 0;
    if (seccomp_filter_syscall(number, arg0, arg1, arg2, arg3, arg4, arg5,
                               frame, &sv))
      return (u64)sv; /* blocked — sv is the value to return (may be 0) */
  }

  /* M40 — Linux ABI translation. For a task whose image carries the Linux
   * personality, `number` is a Linux x86_64 syscall number; translate it to the
   * b1nix native number before routing. The CPU calling convention is identical,
   * so the args pass straight through. An unmapped Linux call returns -ENOSYS,
   * exactly as Linux returns for an unimplemented syscall. Native b1nix tasks
   * skip this entirely (personality == PERSONALITY_B1NIX). */
  {
    struct task *t = current_task;
    if (t && t->user_image &&
        ((struct user_loaded_image *)t->user_image)->personality ==
            PERSONALITY_LINUX) {
      /* Some calls need a semantic (struct-layout) translation, not just a
       * number remap, because the result struct differs from b1nix's. Handle
       * those here and return their result directly. */
      if (number == LINUX_NR_STAT || number == LINUX_NR_FSTAT ||
          number == LINUX_NR_LSTAT)
        return (u64)sys_linux_stat_family(number, arg0, arg1);
      if (number == LINUX_NR_UNAME)
        return (u64)sys_linux_uname(arg0);
      if (number == LINUX_NR_GETDENTS64)
        return (u64)sys_linux_getdents64((int)arg0, arg1, (usize)arg2);
      if (number == LINUX_NR_ARCH_PRCTL)
        return (u64)sys_linux_arch_prctl(arg0, arg1);
      if (number == LINUX_NR_RT_SIGPROCMASK)
        return (u64)sys_linux_rt_sigprocmask((int)arg0, arg1, arg2);
      /* rt_sigsuspend(130): the wait mask uses Linux signal numbering. Translate
       * it to b1nix numbering, repack into the caller's own buffer (sys_sigsuspend
       * copies the mask back in from user space), then run the native suspend. */
      if (number == 130) {
        if (!arg0)
          return (u64)-EFAULT;
        u64 lx_mask = 0;
        if (syscall_copyin(&lx_mask, (void *)(usize)arg0, sizeof(lx_mask)) < 0)
          return (u64)-EFAULT;
        u64 b_mask = linux_sigset_to_b1nix(lx_mask);
        if (syscall_copyout((void *)(usize)arg0, &b_mask, sizeof(b_mask)) < 0)
          return (u64)-EFAULT;
        return sys_sigsuspend((const u64 *)(usize)arg0);
      }
      /* select(23)/pselect6(270): Linux passes a `struct timeval *` (select)
       * or `struct timespec *` (pselect6) as the timeout, but SYS_SELECT wants
       * an integer millisecond count (NULL => wait forever == (u64)-1). The
       * fd-set/nfds args already line up, so convert the timeout in place and
       * fall through to the table remap -> SYS_SELECT. pselect6's sigmask
       * (arg5) is not yet honored. */
      if (number == 23 || number == 270) {
        if (arg4 == 0) {
          arg4 = (u64)-1;
        } else {
          long tv[2] = {0, 0};
          if (syscall_copyin(tv, (void *)(usize)arg4, sizeof(tv)) < 0)
            return (u64)-EFAULT;
          arg4 = (number == 23)
                     ? (u64)tv[0] * 1000 + (u64)tv[1] / 1000    /* usec */
                     : (u64)tv[0] * 1000 + (u64)tv[1] / 1000000; /* nsec */
        }
      }
      /* fcntl(72) F_GETFL/F_SETFL: the status-flag VALUES differ between the
       * ABIs (Linux O_NONBLOCK=0x800 collides with b1nix O_CLOEXEC=0x800;
       * b1nix O_NONBLOCK=0x4000). Without this, musl code doing
       * fcntl(fd, F_SETFL, O_NONBLOCK) silently set CLOEXEC and left the fd
       * BLOCKING — displayd's client sockets stayed blocking and one silent
       * client froze the whole compositor in recvmsg. */
      if (number == 72 &&
          ((int)arg1 == B1NIX_F_SETFL || (int)arg1 == B1NIX_F_GETFL)) {
        if ((int)arg1 == B1NIX_F_SETFL) {
          u64 b = 0;
          if (arg2 & 0x400) b |= B1NIX_O_APPEND;
          if (arg2 & 0x800) b |= B1NIX_O_NONBLOCK;
          return (u64)sys_fcntl((int)arg0, B1NIX_F_SETFL, b);
        }
        u64 b = (u64)sys_fcntl((int)arg0, B1NIX_F_GETFL, 0);
        if ((isize)b < 0)
          return b;
        u64 lx = b & 3; /* access mode */
        if (b & B1NIX_O_APPEND) lx |= 0x400;
        if (b & B1NIX_O_NONBLOCK) lx |= 0x800;
        return lx;
      }
      /* setitimer(38) / getitimer(36), ITIMER_REAL only — backed by the task
       * alarm (100 Hz ticks) plus a repeat interval so periodic SIGALRM works
       * (busybox ping's send loop). ITIMER_VIRTUAL/PROF are not implemented. */
      if (number == 38 || number == 36) {
        struct lx_itimerval {
          u64 int_sec, int_usec, val_sec, val_usec;
        } itv;
        if ((int)arg0 != 0) /* ITIMER_REAL */
          return (u64)-EINVAL;
        u64 now = scheduler_get_uptime_ticks();
        if (number == 36) { /* getitimer(which, curr) */
          memset(&itv, 0, sizeof(itv));
          u64 dl = task_alarm_ticks(current_task);
          if (dl > now) {
            itv.val_sec = (dl - now) / 100;
            itv.val_usec = ((dl - now) % 100) * 10000;
          }
          u64 iv = task_alarm_interval_ticks(current_task);
          itv.int_sec = iv / 100;
          itv.int_usec = (iv % 100) * 10000;
          if (arg1 &&
              syscall_copyout((void *)(usize)arg1, &itv, sizeof(itv)) < 0)
            return (u64)-EFAULT;
          return 0;
        }
        /* setitimer(which, new, old) */
        struct lx_itimerval old;
        memset(&old, 0, sizeof(old));
        u64 odl = task_alarm_ticks(current_task);
        if (odl > now) {
          old.val_sec = (odl - now) / 100;
          old.val_usec = ((odl - now) % 100) * 10000;
        }
        u64 oiv = task_alarm_interval_ticks(current_task);
        old.int_sec = oiv / 100;
        old.int_usec = (oiv % 100) * 10000;
        if (!arg1)
          return (u64)-EFAULT;
        if (syscall_copyin(&itv, (void *)(usize)arg1, sizeof(itv)) < 0)
          return (u64)-EFAULT;
        u64 val_ticks = itv.val_sec * 100 + itv.val_usec / 10000;
        u64 int_ticks = itv.int_sec * 100 + itv.int_usec / 10000;
        if ((itv.val_sec || itv.val_usec) && val_ticks == 0)
          val_ticks = 1; /* round a sub-tick value up, not to "disarmed" */
        if ((itv.int_sec || itv.int_usec) && int_ticks == 0)
          int_ticks = 1;
        task_set_alarm_interval_ticks(current_task, val_ticks ? int_ticks : 0);
        task_set_alarm_ticks(current_task, val_ticks ? now + val_ticks : 0);
        if (arg2 &&
            syscall_copyout((void *)(usize)arg2, &old, sizeof(old)) < 0)
          return (u64)-EFAULT;
        return 0;
      }
      /* utimes(235): (path, struct timeval[2]); utimensat(280): (dirfd, path,
       * struct timespec[2], flags). */
      if (number == 235)
        return (u64)sys_linux_utimensat(AT_FDCWD, (const char *)(usize)arg0,
                                        arg1, 0);
      if (number == 280)
        return (u64)sys_linux_utimensat((int)arg0, (const char *)(usize)arg1,
                                        arg2, 1);
      /* tkill(tid, sig) / tgkill(tgid, tid, sig): b1nix has no thread-kill, but
       * its tids are task ids, so target the tid directly via scheduler_kill
       * (for a single-threaded process tid == pid). Remap the signo. */
      if (number == LINUX_NR_TKILL)
        return (u64)scheduler_kill((usize)arg0,
                                   linux_signo_to_b1nix((int)arg1));
      if (number == LINUX_NR_TGKILL)
        return (u64)scheduler_kill((usize)arg1,
                                   linux_signo_to_b1nix((int)arg2));
      /* waitid(247): idtype/id/options values match b1nix, but the siginfo
       * layouts differ (b1nix packs 6 ints; Linux is a 128-byte struct with
       * si_errno/si_code swapped and the CLD fields at offset 16). Let
       * scheduler_waitid write its b1nix siginfo into the user's (larger)
       * buffer, read it back, and rewrite it in the Linux layout. */
      if (number == 247) {
        int wr = scheduler_waitid((idtype_t)arg0, (usize)arg1,
                                  (siginfo_t *)(usize)arg2, (int)arg3);
        if (wr < 0)
          return (u64)wr;
        if (arg2) {
          siginfo_t ki;
          if (syscall_copyin(&ki, (void *)(usize)arg2, sizeof(ki)) < 0)
            return (u64)-EFAULT;
          u8 lx[128];
          memset(lx, 0, sizeof(lx));
          *(i32 *)&lx[0] = b1nix_signo_to_linux(ki.si_signo);
          *(i32 *)&lx[4] = ki.si_errno;
          *(i32 *)&lx[8] = ki.si_code;
          *(i32 *)&lx[16] = ki.si_pid;
          *(i32 *)&lx[20] = ki.si_uid;
          *(i32 *)&lx[24] = ki.si_status;
          if (syscall_copyout((void *)(usize)arg2, lx, sizeof(lx)) < 0)
            return (u64)-EFAULT;
        }
        return (u64)wr;
      }
      /* prlimit64(302): (pid, resource, new, old) — self only. RLIMIT_*
       * numbers and struct rlimit {u64 cur, max} match Linux. musl routes
       * both getrlimit and setrlimit through here. */
      if (number == 302) {
        if (arg0 != 0 && arg0 != (u64)scheduler_get_pid())
          return (u64)-EPERM;
        if (arg3) {
          isize gr = sys_getrlimit((int)arg1, (struct rlimit *)(usize)arg3);
          if (gr < 0)
            return (u64)gr;
        }
        if (arg2)
          return (u64)sys_setrlimit((int)arg1,
                                    (const struct rlimit *)(usize)arg2);
        return 0;
      }
      /* SysV shm (shmget 29 / shmat 30 / shmctl 31 / shmdt 67): argument
       * layouts and IPC_RMID/SET/STAT values match; only shmget's create
       * flag BITS differ (Linux IPC_CREAT=0x200, IPC_EXCL=0x400 vs b1nix
       * 0x1000/0x2000). */
      if (number == 29) {
        u64 f = arg2 & 0777;
        if (arg2 & 0x200) f |= IPC_CREAT;
        if (arg2 & 0x400) f |= IPC_EXCL;
        return (u64)shmget((u32)arg0, (usize)arg1, (int)f);
      }
      /* POSIX mq (musl): mq_open(240), mq_unlink(241), mq_timedsend(242),
       * mq_timedreceive(243). b1nix mqds are small table indices that would
       * collide with real fds once musl close()es the mqd, so hand userspace
       * mqd+LXMQ_BASE and strip it on the way back in. */
#define LXMQ_BASE 0x100000
      if (number == 240 || number == 241) {
        char mqname[64];
        if (syscall_copyinstr(mqname, sizeof(mqname),
                              (const char *)(usize)arg0) < 0)
          return (u64)-EFAULT;
        if (number == 241)
          return (u64)mqueue_unlink(mqname);
        int mqd = mqueue_create(mqname);
        return mqd < 0 ? (u64)mqd : (u64)(mqd + LXMQ_BASE);
      }
      if (number == 242 || number == 243) {
        int mqd = (int)arg0 - LXMQ_BASE;
        if (mqd < 0 || mqd >= MQ_MAX_QUEUES)
          return (u64)-EBADF;
        char kbuf[MQ_MAX_MSG_SIZE];
        if (number == 242) { /* mq_timedsend(mqd, ptr, len, prio, abstime) */
          if (arg2 > MQ_MAX_MSG_SIZE)
            return (u64)-EMSGSIZE;
          if (syscall_copyin(kbuf, (const void *)(usize)arg1, (usize)arg2) < 0)
            return (u64)-EFAULT;
          return (u64)mqueue_send(mqd, kbuf, (u32)arg2);
        }
        u32 klen = 0; /* mq_timedreceive(mqd, ptr, maxlen, prio*, abstime) */
        int mrc = mqueue_receive(mqd, kbuf, &klen);
        if (mrc < 0)
          return (u64)mrc;
        if (klen > arg2)
          return (u64)-EMSGSIZE;
        if (syscall_copyout((void *)(usize)arg1, kbuf, klen) < 0)
          return (u64)-EFAULT;
        /* Report the message priority (b1nix mqueues are FIFO, so 0). musl's
         * mq_receive passes msg_prio through as arg3 and expects it filled. */
        if (arg3) {
          u32 prio = 0;
          if (syscall_copyout((void *)(usize)arg3, &prio, sizeof(prio)) < 0)
            return (u64)-EFAULT;
        }
        return (u64)klen;
      }
      if (number == 3 && arg0 >= LXMQ_BASE &&
          arg0 < LXMQ_BASE + MQ_MAX_QUEUES) {
        mqueue_close((int)(arg0 - LXMQ_BASE));
        return 0;
      }
      /* getpriority(140)/setpriority(141): Linux prepends a `which` argument
       * (PRIO_PROCESS=0 only here) and getpriority returns 20-nice so the
       * result is never negative; musl converts it back with 20-ret. */
      if (number == 141) {
        if ((int)arg0 != 0)
          return (u64)-EINVAL;
        usize pid = arg1 ? (usize)arg1 : scheduler_get_pid();
        return (u64)scheduler_set_priority(pid, (int)arg2);
      }
      if (number == 140) {
        if ((int)arg0 != 0)
          return (u64)-EINVAL;
        usize pid = arg1 ? (usize)arg1 : scheduler_get_pid();
        /* scheduler_get_priority already returns the Linux 20-nice encoding
         * (1..40, or a negative errno); musl converts it back with 20-ret.
         * Do NOT apply a second 20- here — that double conversion made
         * getpriority report 20 for a nice-0 task. */
        return (u64)(isize)scheduler_get_priority(pid);
      }
      /* M74 rt_sigqueueinfo(129)(pid, sig, siginfo*): musl sigqueue(3). The
       * payload sits in the Linux siginfo at si_value (offset 24 on x86_64).
       * Extract it, translate the signal, and queue it with the payload so an
       * SA_SIGINFO handler observes si_value. */
      if (number == 129) {
        int b = linux_signo_to_b1nix((int)arg1);
        if (b <= 0)
          return (u64)-EINVAL;
        union sigval v;
        v.sival_ptr = 0;
        if (arg2) {
          u64 sv = 0;
          if (syscall_copyin(&sv, (void *)(usize)(arg2 + 24), sizeof(sv)) < 0)
            return (u64)-EFAULT;
          v.sival_ptr = (void *)(usize)sv;
        }
        if (b >= SIGRTMIN && b <= SIGRTMAX)
          return (u64)scheduler_sigqueue((usize)arg0, b, v, B1NIX_SI_QUEUE);
        return (u64)scheduler_kill((usize)arg0, b);
      }
      /* M74 timer_create(222)(clockid, sigevent*, timer_t*): the Linux sigevent
       * layout is { union sigval value; int signo; int notify; ... } — different
       * field order from b1nix. Translate the signal to b1nix numbering, then
       * drive the native timer path. Only SIGEV_SIGNAL(0) is supported. */
      if (number == 222) {
        if (!arg1 || !arg2)
          return (u64)-EINVAL;
        struct lx_sigevent {
          union sigval value;
          int signo;
          int notify;
        } lsev;
        if (syscall_copyin(&lsev, (void *)(usize)arg1, sizeof(lsev)) < 0)
          return (u64)-EFAULT;
        if (lsev.notify != 0 /* SIGEV_SIGNAL */)
          return (u64)-EINVAL;
        int b = linux_signo_to_b1nix(lsev.signo);
        if (b <= 0)
          return (u64)-EINVAL;
        int id = scheduler_timer_create(b, lsev.value);
        if (id < 0)
          return (u64)(isize)id;
        if (syscall_copyout((void *)(usize)arg2, &id, sizeof(int)) < 0) {
          scheduler_timer_delete(id);
          return (u64)-EFAULT;
        }
        return 0;
      }
      /* klogctl(103)(type, bufp, len): busybox dmesg. Map the READ actions onto
       * klog_read (the native SYS_DMESG path), report the ring capacity for
       * SIZE_BUFFER, and accept the console-control actions as no-ops. */
      if (number == 103) {
        int type = (int)arg0;
        if (type == 10) /* SYSLOG_ACTION_SIZE_BUFFER */
          return (u64)KLOG_BUF_SIZE;
        if (type == 2 || type == 3 || type == 4) { /* READ / READ_ALL / READ_CLEAR */
          u64 buf = arg1, len = arg2;
          if (!buf || len == 0)
            return (u64)-EINVAL;
          if (len > KLOG_BUF_SIZE)
            len = KLOG_BUF_SIZE;
          if (!is_user_range_valid((const void *)(usize)buf, len, 1))
            return (u64)-EFAULT;
          static char klogctl_tmp[KLOG_BUF_SIZE];
          usize copied = klog_read(klogctl_tmp, (usize)len);
          if (syscall_copyout((void *)(usize)buf, klogctl_tmp, copied) != 0)
            return (u64)-EFAULT;
          return (u64)copied;
        }
        return 0; /* open(1)/close(0)/console-level actions: accept */
      }

      /* --- M92: *at() syscall emulation for musl ---
       * musl uses *at() variants (openat, newfstatat, etc.) exclusively. These
       * resolve dirfd + relative path to an absolute path, then delegate to the
       * existing b1nix handler. AT_FDCWD (-100) means "use cwd". */

#define LX_openat          257
#define LX_newfstatat      262
#define LX_unlinkat        263
#define LX_mkdirat         258
#define LX_linkat          265
#define LX_symlinkat       266
#define LX_readlinkat      267
#define LX_fchmodat        268
#define LX_fchownat        260
#define LX_faccessat       269
#define LX_renameat2       316
#define LX_pipe2           293
#define LX_dup3            292
#define LX_ppoll           271
#define LX_pselect6        270
#define LX_accept4         288
#define LX_clock_nanosleep 230
#define LX_set_tid_address 218
#define LX_prlimit64       302
#define LX_set_robust_list 273
#define LX_get_robust_list 274
#define LX_FUTEX           202
#define LX_ioctl           16
#define LX_nanosleep       35

      /* Resolve a dirfd + user path to an absolute kernel path.
       * dirfd == AT_FDCWD (-100) or an absolute path → resolve normally.
       * Otherwise, get the absolute path of dirfd and join with the relative path.
       * Returns 0 on success, -errno on failure. kbuf must be VFS_MAX_PATH. */
      if (number == LX_openat || number == LX_newfstatat ||
          number == LX_unlinkat || number == LX_mkdirat ||
          number == LX_fchmodat || number == LX_fchownat ||
          number == LX_faccessat || number == LX_readlinkat ||
          number == LX_symlinkat || number == LX_renameat2 ||
          number == LX_linkat) {
        int dirfd = (int)arg0;
        const char *user_path = (const char *)(usize)arg1;
        char kpath[VFS_MAX_PATH];
        if (syscall_copyinstr(kpath, sizeof(kpath), user_path) < 0)
          return (u64)-EFAULT;
        /* If path is absolute or dirfd is AT_FDCWD, use as-is. */
        if (kpath[0] != '/' && dirfd != AT_FDCWD) {
          char dirbuf[VFS_MAX_PATH];
          int rc = vfs_fd_abspath(dirfd, dirbuf, sizeof(dirbuf));
          if (rc < 0)
            return (u64)rc;
          /* Join dir path + "/" + relative path. */
          char joined[VFS_MAX_PATH];
          usize dlen = (usize)rc;
          if (dlen + 1 + strlen(kpath) >= sizeof(joined))
            return (u64)-ENAMETOOLONG;
          memcpy(joined, dirbuf, dlen);
          joined[dlen] = '/';
          strcpy(joined + dlen + 1, kpath);
          memcpy(kpath, joined, sizeof(kpath));
        }
        char resolved[VFS_MAX_PATH];
        vfs_resolve_path(kpath, resolved);

        switch (number) {
        case LX_openat: {
          /* arg0=dirfd, arg1=path, arg2=flags, arg3=mode.
           * Translate Linux O_* flags to b1nix flags via an explicit whitelist.
           * Several bits DIVERGE and even COLLIDE between the two ABIs, so a
           * pass-through with per-bit patching is unsafe:
           *   - Linux O_NONBLOCK (04000 = 0x800) == b1nix O_CLOEXEC (0x800)
           *   - Linux O_CLOEXEC  (02000000 = 0x80000) != b1nix O_CLOEXEC
           *   - Linux O_DIRECT   (040000 = 0x4000) == b1nix O_NONBLOCK (0x4000)
           * Build the b1nix flag set from recognized Linux bits only; unknown
           * Linux bits (O_NOCTTY/O_SYNC/O_DIRECT/O_NOFOLLOW/...) are dropped so
           * they can't accidentally set an unrelated b1nix flag. The low two
           * bits (O_RDONLY/O_WRONLY/O_RDWR) are identical in both ABIs. */
          int lf = (int)arg2;
          int flags = lf & 0x3; /* access mode (shared) */
          if (lf & 0100)     flags |= B1NIX_O_CREAT;     /* Linux 0x40  */
          if (lf & 0200)     flags |= B1NIX_O_EXCL;      /* Linux 0x80  */
          if (lf & 01000)    flags |= B1NIX_O_TRUNC;     /* Linux 0x200 */
          if (lf & 02000)    flags |= B1NIX_O_APPEND;    /* Linux 0x400 */
          if (lf & (04000 | 040000 | 0100000))
            flags |= B1NIX_O_NONBLOCK;  /* Linux, native, or musl O_NONBLOCK */
          if (lf & 0200000)  flags |= B1NIX_O_DIRECTORY; /* Linux 0x10000 (shared) */
          if (lf & 02000000) flags |= B1NIX_O_CLOEXEC;   /* Linux 0x80000 -> 0x800 */
          return (u64)vfs_open_flags(resolved, flags);
        }
        case LX_newfstatat: {
          /* arg0=dirfd, arg1=path, arg2=statbuf, arg3=flags.
           * AT_SYMLINK_NOFOLLOW = 0x100. */
          int flags = (int)arg3;
          struct b1nix_stat st;
          int rc;
          if (flags & 0x100) /* AT_SYMLINK_NOFOLLOW */
            rc = vfs_lstat(resolved, &st);
          else
            rc = vfs_stat(resolved, &st);
          if (rc < 0)
            return (u64)rc;
          /* Translate to Linux struct stat layout. */
          struct linux_stat lst;
          linux_stat_from_b1nix(&lst, &st);
          if (syscall_copyout((void *)(usize)arg2, &lst, sizeof(lst)) < 0)
            return (u64)-EFAULT;
          return 0;
        }
        case LX_unlinkat: {
          /* arg0=dirfd, arg1=path, arg2=flags.
           * AT_REMOVEDIR = 0x200. */
          int flags = (int)arg2;
          if (flags & 0x200) /* AT_REMOVEDIR → rmdir */
            return (u64)vfs_rmdir(resolved);
          return (u64)vfs_unlink(resolved);
        }
        case LX_mkdirat:
          return (u64)vfs_mkdir(resolved, (int)arg2);
        case LX_linkat: {
          /* arg0=olddirfd, arg1=oldpath, arg2=newdirfd, arg3=newpath, arg4=flags.
           * resolved = oldpath. Need to resolve newpath from (arg2, arg3). */
          char new_kpath[VFS_MAX_PATH];
          if (syscall_copyinstr(new_kpath, sizeof(new_kpath),
                                (const char *)(usize)arg3) < 0)
            return (u64)-EFAULT;
          if (new_kpath[0] != '/' && (int)arg2 != AT_FDCWD) {
            char new_dirbuf[VFS_MAX_PATH];
            int nrc = vfs_fd_abspath((int)arg2, new_dirbuf, sizeof(new_dirbuf));
            if (nrc < 0)
              return (u64)nrc;
            char new_joined[VFS_MAX_PATH];
            usize ndlen = (usize)nrc;
            if (ndlen + 1 + strlen(new_kpath) >= sizeof(new_joined))
              return (u64)-ENAMETOOLONG;
            memcpy(new_joined, new_dirbuf, ndlen);
            new_joined[ndlen] = '/';
            strcpy(new_joined + ndlen + 1, new_kpath);
            memcpy(new_kpath, new_joined, sizeof(new_kpath));
          }
          char new_resolved[VFS_MAX_PATH];
          vfs_resolve_path(new_kpath, new_resolved);
          return (u64)vfs_link(resolved, new_resolved);
        }
        case LX_symlinkat: {
          /* Linux symlinkat(target, newdirfd, linkpath):
           *   arg0 = target (the CONTENT of the symlink)
           *   arg1 = newdirfd
           *   arg2 = linkpath (where to create the symlink)
           * The outer code resolves (arg0=dirfd, arg1=path) which is WRONG for
           * symlinkat — arg0 is the target, not a dirfd. Re-resolve properly. */
          char target[VFS_MAX_PATH];
          if (syscall_copyinstr(target, sizeof(target),
                                (const char *)(usize)arg0) < 0)
            return (u64)-EFAULT;
          /* Resolve linkpath from (arg1=newdirfd, arg2=linkpath). */
          char lk_kpath[VFS_MAX_PATH];
          if (syscall_copyinstr(lk_kpath, sizeof(lk_kpath),
                                (const char *)(usize)arg2) < 0)
            return (u64)-EFAULT;
          if (lk_kpath[0] != '/' && (int)arg1 != AT_FDCWD) {
            char lk_dirbuf[VFS_MAX_PATH];
            int lrc = vfs_fd_abspath((int)arg1, lk_dirbuf, sizeof(lk_dirbuf));
            if (lrc < 0)
              return (u64)lrc;
            char lk_joined[VFS_MAX_PATH];
            usize ldlen = (usize)lrc;
            if (ldlen + 1 + strlen(lk_kpath) >= sizeof(lk_joined))
              return (u64)-ENAMETOOLONG;
            memcpy(lk_joined, lk_dirbuf, ldlen);
            lk_joined[ldlen] = '/';
            strcpy(lk_joined + ldlen + 1, lk_kpath);
            memcpy(lk_kpath, lk_joined, sizeof(lk_kpath));
          }
          char lk_resolved[VFS_MAX_PATH];
          vfs_resolve_path(lk_kpath, lk_resolved);
          return (u64)vfs_symlink(target, lk_resolved);
        }
        case LX_readlinkat: {
          /* arg0=dirfd, arg1=path, arg2=buf, arg3=bufsize. */
          char linkbuf[VFS_MAX_PATH];
          int rc = vfs_readlink(resolved, linkbuf, sizeof(linkbuf));
          if (rc < 0)
            return (u64)rc;
          usize len = (usize)rc;
          usize bufsize = (usize)arg3;
          if (len >= bufsize)
            len = bufsize - 1;
          if (syscall_copyout((void *)(usize)arg2, linkbuf, len) < 0)
            return (u64)-EFAULT;
          /* NUL-terminate if space allows. */
          if (len < bufsize) {
            char nul = '\0';
            syscall_copyout((void *)(usize)(arg2 + len), &nul, 1);
          }
          return (u64)len;
        }
        case LX_fchmodat:
          return (u64)vfs_chmod(resolved, (u16)arg2);
        case LX_fchownat:
          return (u64)vfs_chown(resolved, (u16)arg2, (u16)arg3);
        case LX_faccessat: {
          /* arg0=dirfd, arg1=path, arg2=mode, arg3=flags.
           * Linux faccessat uses the real mode (R_OK=4, W_OK=2, X_OK=1, F_OK=0).
           * b1nix SYS_ACCESS takes (path, mode) too. Ignore flags (AT_EACCESS). */
          (void)arg3;
          return (u64)sys_access(resolved, (int)arg2);
        }
        case LX_renameat2: {
          /* arg0=olddirfd, arg1=oldpath, arg2=newdirfd, arg3=newpath, arg4=flags.
           * resolved = oldpath. Need to resolve newpath from (arg2, arg3). */
          char new_kpath[VFS_MAX_PATH];
          if (syscall_copyinstr(new_kpath, sizeof(new_kpath),
                                (const char *)(usize)arg3) < 0)
            return (u64)-EFAULT;
          if (new_kpath[0] != '/' && (int)arg2 != AT_FDCWD) {
            char new_dirbuf[VFS_MAX_PATH];
            int nrc = vfs_fd_abspath((int)arg2, new_dirbuf, sizeof(new_dirbuf));
            if (nrc < 0)
              return (u64)nrc;
            char new_joined[VFS_MAX_PATH];
            usize ndlen = (usize)nrc;
            if (ndlen + 1 + strlen(new_kpath) >= sizeof(new_joined))
              return (u64)-ENAMETOOLONG;
            memcpy(new_joined, new_dirbuf, ndlen);
            new_joined[ndlen] = '/';
            strcpy(new_joined + ndlen + 1, new_kpath);
            memcpy(new_kpath, new_joined, sizeof(new_kpath));
          }
          char new_resolved[VFS_MAX_PATH];
          vfs_resolve_path(new_kpath, new_resolved);
          /* arg4=flags: RENAME_NOREPLACE(1), RENAME_EXCHANGE(2).
           * b1nix doesn't support these — silently ignore for now. */
          return (u64)vfs_rename(resolved, new_resolved);
        }
        default:
          break;
        }
#undef LX_openat
#undef LX_newfstatat
#undef LX_unlinkat
#undef LX_mkdirat
#undef LX_linkat
#undef LX_symlinkat
#undef LX_readlinkat
#undef LX_fchmodat
#undef LX_fchownat
#undef LX_faccessat
#undef LX_renameat2
      }

      /* --- M92: wrapper syscalls (thin shims over existing b1nix handlers) --- */
      if (number == LX_pipe2) {
        /* pipe2(fds, flags): call pipe, then set FD_CLOEXEC if requested. */
        int kfds[2];
        int rc = (int)vfs_pipe(kfds);
        if (rc < 0)
          return (u64)rc;
        if (arg1 & 02000000) { /* O_CLOEXEC */
          sys_fcntl(kfds[0], B1NIX_F_SETFD, B1NIX_FD_CLOEXEC);
          sys_fcntl(kfds[1], B1NIX_F_SETFD, B1NIX_FD_CLOEXEC);
        }
        if (syscall_copyout((void *)(usize)arg0, kfds, sizeof(kfds)) < 0)
          return (u64)-EFAULT;
        return 0;
      }
      if (number == LX_dup3) {
        /* dup3(oldfd, newfd, flags): dup2 + set FD_CLOEXEC if flags has it. */
        if ((int)arg0 == (int)arg1)
          return (u64)-EINVAL;
        int rc = (int)vfs_dup2((int)arg0, (int)arg1);
        if (rc < 0)
          return (u64)rc;
        if (arg2 & 02000000) /* O_CLOEXEC */
          sys_fcntl((int)arg1, B1NIX_F_SETFD, B1NIX_FD_CLOEXEC);
        return (u64)rc;
      }
      if (number == LX_ppoll) {
        /* ppoll(fds, nfds, timeout_ts, sigmask, sigsetsize).
         * Ignore sigmask — just call poll. timeout_ts is a pointer to
         * struct timespec (seconds + nanoseconds), convert to ms. */
        int nfds = (int)arg1;
        u64 timeout_ms = (u64)-1; /* infinite */
        if (arg2) {
          /* struct timespec: tv_sec (8 bytes), tv_nsec (8 bytes). */
          u64 tv_sec = 0, tv_nsec = 0;
          syscall_copyin(&tv_sec, (void *)(usize)arg2, sizeof(tv_sec));
          syscall_copyin(&tv_nsec, (void *)(usize)(arg2 + 8), sizeof(tv_nsec));
          timeout_ms = tv_sec * 1000 + tv_nsec / 1000000;
        }
        return (u64)sys_poll((void *)(usize)arg0, nfds, (int)timeout_ms);
      }
      if (number == LX_pselect6) {
        /* pselect6(nfds, readfds, writefds, exceptfds, timeout_ts, sigmask).
         * Ignore sigmask — convert fd_sets to pollfds and use the poll loop.
         * timeout_ts is a pointer to struct timespec, convert to ms. */
        int nfds = (int)arg0;
        if (nfds < 0 || nfds > 1024)
          return (u64)-EINVAL;
        u64 timeout_ms = (u64)-1;
        if (arg4) {
          u64 tv_sec = 0, tv_nsec = 0;
          syscall_copyin(&tv_sec, (void *)(usize)arg4, sizeof(tv_sec));
          syscall_copyin(&tv_nsec, (void *)(usize)(arg4 + 8), sizeof(tv_nsec));
          timeout_ms = tv_sec * 1000 + tv_nsec / 1000000;
        }
        u8 r_kset[128] = {0}, w_kset[128] = {0}, e_kset[128] = {0};
        if (arg1 && syscall_copyin(r_kset, (void *)(usize)arg1, 128) < 0)
          return (u64)-EFAULT;
        if (arg2 && syscall_copyin(w_kset, (void *)(usize)arg2, 128) < 0)
          return (u64)-EFAULT;
        if (arg3 && syscall_copyin(e_kset, (void *)(usize)arg3, 128) < 0)
          return (u64)-EFAULT;
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
        /* Use the same inline poll loop as SYS_SELECT. */
        u64 start_ticks = scheduler_get_uptime_ticks();
        u64 timeout_ticks = timeout_ms == (u64)-1 ? (u64)-1 : timeout_ms / 10;
        if (timeout_ms != (u64)-1 && timeout_ms != 0) {
          u64 ticks = timeout_ticks > 0 ? timeout_ticks : 1;
          current_task->wake_tick = start_ticks + ticks;
        }
        extern void *vfs_poll_chan;
        int ready_count = 0;
        while (1) {
          scheduler_wait_prepare(vfs_poll_chan);
          ready_count = 0;
          for (int i = 0; i < np; i++) {
            if (pfds[i].fd < 0) { pfds[i].revents = 0; continue; }
            struct vfs_handle *h = scheduler_fd_get(pfds[i].fd);
            if (!h) { pfds[i].revents = B1NIX_POLLNVAL; ready_count++; continue; }
            pfds[i].revents = 0;
            vfs_poll(pfds[i].fd, &pfds[i]);
            if (pfds[i].revents) ready_count++;
          }
          int timed_out = 0;
          if (ready_count == 0 && timeout_ms != 0 && timeout_ms != (u64)-1) {
            u64 now = scheduler_get_uptime_ticks();
            if (now - start_ticks >= timeout_ticks) timed_out = 1;
          }
          if (ready_count > 0 || timeout_ms == 0 || timed_out) {
            scheduler_wait_cancel();
            break;
          }
          if (select_poll_signal_pending()) {
            scheduler_wait_cancel();
            return (u64)-EINTR;
          }
          /* Re-arm every iteration — an explicit wake clears wake_tick (see
           * sys_poll for the full unbounded-sleep failure this prevents). */
          if (timeout_ms != (u64)-1 && timeout_ms != 0) {
            u64 ticks = timeout_ticks > 0 ? timeout_ticks : 1;
            current_task->wake_tick = start_ticks + ticks;
          }
          scheduler_block_on(vfs_poll_chan);
        }
        /* Write back fd_sets with revents. */
        memset(r_kset, 0, sizeof(r_kset));
        memset(w_kset, 0, sizeof(w_kset));
        memset(e_kset, 0, sizeof(e_kset));
        for (int i = 0; i < np; i++) {
          if (pfds[i].revents && pfds[i].fd >= 0) {
            int fd = pfds[i].fd;
            if (pfds[i].revents & (B1NIX_POLLIN | B1NIX_POLLHUP | B1NIX_POLLERR))
              r_kset[fd / 8] |= (1 << (fd & 7));
            if (pfds[i].revents & (B1NIX_POLLOUT | B1NIX_POLLERR))
              w_kset[fd / 8] |= (1 << (fd & 7));
            if (pfds[i].revents & B1NIX_POLLERR)
              e_kset[fd / 8] |= (1 << (fd & 7));
          }
        }
        if (arg1 && syscall_copyout((void *)(usize)arg1, r_kset, 128) < 0)
          return (u64)-EFAULT;
        if (arg2 && syscall_copyout((void *)(usize)arg2, w_kset, 128) < 0)
          return (u64)-EFAULT;
        if (arg3 && syscall_copyout((void *)(usize)arg3, e_kset, 128) < 0)
          return (u64)-EFAULT;
        return (u64)ready_count;
      }
      if (number == LX_accept4) {
        /* accept4(sockfd, addr, addrlen, flags): accept + set FD_CLOEXEC
         * and O_NONBLOCK from flags. */
        int rc = (int)vfs_accept((int)arg0, (void *)(usize)arg1,
                                  (usize *)(usize)arg2);
        if (rc < 0)
          return (u64)rc;
        if (arg3 & 02000000) /* O_CLOEXEC */
          sys_fcntl(rc, B1NIX_F_SETFD, B1NIX_FD_CLOEXEC);
        if (arg3 & 04000) /* O_NONBLOCK */
          sys_fcntl(rc, B1NIX_F_SETFL, B1NIX_O_NONBLOCK);
        return (u64)rc;
      }
      if (number == LX_clock_nanosleep) {
        /* clock_nanosleep(clock_id, flags, request, remain).
         * flags=0 → relative sleep. Convert the timespec to ticks and sleep. */
        (void)arg0; /* clock_id — ignore, use relative */
        (void)arg3; /* remain — not implemented */
        struct timespec ts;
        if (syscall_copyin(&ts, (const void *)(usize)arg2, sizeof(ts)) != 0)
          return (u64)-EFAULT;
        u64 ticks = (u64)ts.tv_sec * 100 + (u64)ts.tv_nsec / 10000000;
        if (ticks == 0) ticks = 1;
        scheduler_sleep_ticks(ticks);
        return 0;
      }
      if (number == LX_nanosleep) {
        /* Linux nanosleep uses the same timespec ABI as native nanosleep.
         * Handle it here and return: merely rewriting `number` to the native
         * value (235) used to fall through into the Linux→b1nix table lookup
         * below, which does not know 235 as a *Linux* number → every Linux
         * nanosleep returned -ENOSYS. */
        struct timespec ts;
        if (syscall_copyin(&ts, (const void *)(usize)arg0, sizeof(ts)) != 0)
          return (u64)-EFAULT;
        if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000L)
          return (u64)-EINVAL;
        u64 ticks = (u64)ts.tv_sec * 100 + (u64)ts.tv_nsec / 10000000;
        if (ticks == 0) ticks = 1;
        scheduler_sleep_ticks(ticks);
        if (arg1) {
          struct timespec rem = {0, 0};
          if (syscall_copyout((void *)(usize)arg1, &rem, sizeof(rem)) != 0)
            return (u64)-EFAULT;
        }
        return 0;
      }
      if (number == LX_set_tid_address) {
        /* set_tid_address(tidptr): store the clear_child_tid pointer, return
         * the current TID. musl uses this for pthread thread management. */
        task_set_child_tid_clear(current_task, (u64)arg0);
        return (u64)scheduler_get_pid();
      }
      /* M92: set_thread_area (Linux NR 205) — legacy GDT-based TLS for x86.
       * musl's __set_thread_area calls this during __init_tls on x86_64.
       * b1nix uses arch_prctl(ARCH_SET_FS) for TLS instead, so this is a
       * no-op that returns success. */
      if (number == 205)
        return 0;
      /* membarrier(324): no-op on b1nix (no cross-CPU lazy-TLB tricks in
       * userspace); CMD_QUERY returns 0 = "no commands supported", others
       * succeed trivially on this single-address-space-coherent kernel. */
      if (number == 324)
        return 0;
      if (number == LX_prlimit64) {
        /* prlimit64(pid, resource, new_limit, old_limit).
         * Simplified: only support getrlimit (new_limit=0). */
        if (arg2)
          return (u64)-ENOSYS; /* set not implemented yet */
        return (u64)sys_getrlimit((int)arg1, (void *)(usize)arg3);
      }
      if (number == LX_set_robust_list) {
        /* set_robust_list(head, len): store the robust futex list head.
         * Stub — return 0 (b1nix doesn't use robust futexes yet). */
        return 0;
      }
      if (number == LX_get_robust_list) {
        /* get_robust_list(pid, head_ptr, len_ptr): not implemented. */
        return (u64)-ENOSYS;
      }
      /* Linux clone(2) has a different argument layout than b1nix SYS_CLONE:
       *   Linux: clone(flags, user_stack, parent_tidptr, child_tidptr, tls)
       *   b1nix: SYS_CLONE(flags, entry, user_stack, arg, tls, ctid)
       * musl's pthread_create calls clone with Linux layout. Translate. */
      if (number == 56) { /* LX_clone */
        u64 flags = arg0;
        u64 user_stack = arg1;
        u64 parent_tid = arg2;
        u64 child_tid = arg3;
        u64 tls_val = arg4;
        /* For musl pthread: CLONE_VM|CLONE_FS|CLONE_FILES|CLONE_SIGHAND|
         * CLONE_THREAD|CLONE_SETTLS|CLONE_PARENT_SETTID|CLONE_CHILD_SETTID|
         * CLONE_CHILD_CLEARTID.
         * The "entry" is the user_stack itself (musl sets up the stack to
         * return to pthread_start), arg is 0 (already on the stack). */
        u64 b1nix_flags = flags;
        /* Add CLONE_PARENT_SETTID and CLONE_CHILD_SETTID if not already set. */
        if (parent_tid)
          b1nix_flags |= 0x100000; /* CLONE_PARENT_SETTID */
        if (child_tid)
          b1nix_flags |= 0x1000000; /* CLONE_CHILD_SETTID */
        /* M92: musl's __clone stores the start_routine pointer in r9 before
         * calling syscall. The child expects to resume at the parent's RIP
         * (right after the syscall instruction in musl __clone), with rax=0
         * and r9 = start_routine. arg5 is the saved r9 from the interrupt
         * frame. We pass frame->rip as the entry so the child thread
         * continues at the correct instruction. */
        u64 start_func = arg5; /* musl puts fn in r9 before syscall */
        u64 child_entry = frame ? frame->rip : user_stack;
        return (u64)scheduler_clone_thread(b1nix_flags, child_entry,
                                           user_stack, 0, tls_val, child_tid,
                                           parent_tid, child_tid, start_func);
      }
      /* Futex with extended ops for musl (WAIT_BITSET, WAKE_BITSET, REQUEUE,
       * CMP_REQUEUE, PRIVATE_FLAG). The dispatcher routes futex through
       * SYS_FUTEX after the table lookup, but the extended ops need special
       * handling here because the table entry uses the same native number. */
      if (number == LX_FUTEX) {
        int op = (int)arg1;
        int base_op = op & 0x7F; /* strip PRIVATE_FLAG (0x80) */
        /* FUTEX_WAIT_BITSET(9) → treat as WAIT, ignoring val3 (signal mask). */
        if (base_op == 9)
          return (u64)scheduler_futex(arg0, B1NIX_FUTEX_WAIT, (int)arg2, arg3);
        /* FUTEX_WAKE_BITSET(10) → treat as WAKE. */
        if (base_op == 10)
          return (u64)scheduler_futex(arg0, B1NIX_FUTEX_WAKE, (int)arg2, 0);
        /* FUTEX_REQUEUE(4): wake val waiters on uaddr, requeue val2 to uaddr2. */
        if (base_op == 4) {
          int woken = (int)scheduler_futex(arg0, B1NIX_FUTEX_WAKE, (int)arg2, 0);
          if (woken < 0)
            return (u64)woken;
          /* Requeue remaining from arg2 count: wake min(val2, remaining) on
           * uaddr2. For now, do a simple wake on uaddr2. */
          int extra = (int)arg3 > woken ? (int)arg3 - woken : 0;
          if (extra > 0) {
            int rq = (int)scheduler_futex(arg4, B1NIX_FUTEX_WAKE, extra, 0);
            if (rq > 0)
              woken += rq;
          }
          return (u64)woken;
        }
        /* FUTEX_CMP_REQUEUE(8): same as REQUEUE but check *uaddr == val3 first. */
        if (base_op == 8) {
          int cur = 0;
          if (syscall_copyin(&cur, (void *)(usize)arg0, sizeof(int)) < 0)
            return (u64)-EFAULT;
          if (cur != (int)arg5)
            return (u64)-EAGAIN;
          return (u64)scheduler_futex(arg0, B1NIX_FUTEX_WAKE, (int)arg2, 0);
        }
        /* Default: pass through to native futex handler. */
        return (u64)scheduler_futex(arg0, op, (int)arg2, arg3);
      }

      /* Signal-number remap: b1nix signo values differ from Linux. rt_sigaction
       * takes the signo in arg0; kill takes it in arg1. Remap in place, then let
       * the table route to SYS_SIGNAL / SYS_KILL. Signo 0 (kill existence check)
       * maps to 0 and is left intact. */
      if (number == LINUX_NR_RT_SIGACTION) {
        /* Linux struct sigaction (152B) → b1nix struct sigaction (32B).
         * Field order matches but sa_mask size differs: Linux 128B vs b1nix 8B.
         * Also need to remap the signo in sa_mask bits. */
        int lx_sig = (int)arg0;
        int b_sig = linux_signo_to_b1nix(lx_sig);
        if (b_sig == 0 && lx_sig != 0)
          return (u64)-EINVAL;
        struct linux_sigaction {
          u64 sa_handler;
          u64 sa_flags;
          u64 sa_restorer;
          u8 sa_mask[128]; /* Linux __sigset_t (up to 128 bytes) */
        } lx_act, lx_old;
        /* rt_sigaction(sig, act, oldact, sigsetsize): the user structs are
         * 24 bytes of header + sigsetsize bytes of mask — musl/glibc pass
         * sigsetsize == 8, so the whole struct is 32 bytes. Copying a fixed
         * 152 bytes out for oldact overruns the caller's 32-byte stack buffer
         * by 120 bytes and wipes its saved return addresses (ash crashed at
         * rip=0 right after setsignal for exactly this reason). */
        u64 lx_setsz = arg3 ? arg3 : 8;
        if (lx_setsz > sizeof(lx_act.sa_mask))
          return (u64)-EINVAL;
        usize lx_size = 3 * sizeof(u64) + (usize)lx_setsz;
        struct sigaction b_act, b_old;
        memset(&lx_act, 0, sizeof(lx_act));
        memset(&lx_old, 0, sizeof(lx_old));
        memset(&b_act, 0, sizeof(b_act));
        memset(&b_old, 0, sizeof(b_old));
        if (arg1) {
          if (syscall_copyin(&lx_act, (void *)(usize)arg1, lx_size) < 0)
            return (u64)-EFAULT;
          b_act.sa_handler = (sighandler_t)(usize)lx_act.sa_handler;
          b_act.sa_flags = lx_act.sa_flags;
          b_act.sa_restorer = (void (*)(void))(usize)lx_act.sa_restorer;
          /* Convert Linux 128-byte sigset to b1nix u64 sigset. */
          u64 lx_sigset = 0;
          memcpy(&lx_sigset, lx_act.sa_mask, sizeof(lx_sigset));
          b_act.sa_mask = linux_sigset_to_b1nix(lx_sigset);
        }
        {
          char sigbuf[192];
          snprintf(sigbuf, sizeof(sigbuf),
                   "rt_sigaction task=%s lx=%d b=%d set=%p handler=%p restorer=%p old=%p",
                   current_task ? current_task->name : "?",
                   lx_sig, b_sig, (void *)(usize)arg1,
                   (void *)(usize)lx_act.sa_handler,
                   (void *)(usize)lx_act.sa_restorer,
                   (void *)(usize)arg2);
          klog_debug_category("signal", sigbuf);
        }
        if (scheduler_sigaction(b_sig, arg1 ? &b_act : 0, &b_old) < 0)
          return (u64)-EINVAL;
        if (arg2) {
          lx_old.sa_handler = (u64)(usize)b_old.sa_handler;
          lx_old.sa_flags = b_old.sa_flags;
          lx_old.sa_restorer = (u64)(usize)b_old.sa_restorer;
          u64 b_sigset = b_old.sa_mask;
          u64 lx_sigset = b1nix_sigset_to_linux(b_sigset);
          memset(lx_old.sa_mask, 0, sizeof(lx_old.sa_mask));
          memcpy(lx_old.sa_mask, &lx_sigset, sizeof(lx_sigset));
          klog_debug_category("signal", "rt_sigaction before old copyout");
          if (syscall_copyout((void *)(usize)arg2, &lx_old, lx_size) < 0)
            return (u64)-EFAULT;
          klog_debug_category("signal", "rt_sigaction after old copyout");
        }
        klog_debug_category("signal", "rt_sigaction return");
        return 0;
      } else if (number == LINUX_NR_KILL && arg1 != 0)
        arg1 = (u64)linux_signo_to_b1nix((int)arg1);

      /* signalfd4: translate the Linux sigset_t mask to b1nix signal numbers
       * in-place so the SYS_SIGNALFD4 handler sees a b1nix-compatible mask. */
      if (number == LINUX_NR_SIGNALFD4 && arg1 != 0) {
        u64 lx_mask = 0;
        if (syscall_copyin(&lx_mask, (void *)(usize)arg1, sizeof(lx_mask)) == 0) {
          u64 b_mask = linux_sigset_to_b1nix(lx_mask);
          if (syscall_copyout((void *)(usize)arg1, &b_mask, sizeof(b_mask)) < 0)
            return (u64)-EFAULT;
        }
        /* Linux signalfd4 has (fd, mask, sigsetsize, flags), while the
         * native handler consumes (fd, mask, flags). */
        /* musl passes sizeof(sigset_t), which is 128 bytes even though the
         * b1nix signal set currently uses the low 64 bits. */
        if (arg2 != sizeof(u64) && arg2 != 128)
          return (u64)-EINVAL;
        arg2 = arg3;
      }

      /* M92: termios struct translation for Linux tasks.
       * Linux struct termios (44B): c_iflag(4), c_oflag(4), c_cflag(4),
       * c_lflag(4), c_line(1), c_cc[19](19). Total 44.
       * b1nix struct b1nix_termios (48B): c_iflag(4), c_oflag(4), c_cflag(4),
       * c_lflag(4), c_cc[32](32). Total 48.
       * Field order differs after c_lflag. Translate on TCGETS/TCSETS. */
      if (number == LX_ioctl) {
        int fd = (int)arg0;
        u64 request = arg1;
        /* Linux TCGETS=0x5401, TCSETS=0x5402 — same as b1nix. */
        struct linux_termios {
          u32 c_iflag;
          u32 c_oflag;
          u32 c_cflag;
          u32 c_lflag;
          u8 c_line;
          u8 c_cc[19];
        };
        /* NOTE: device ioctl handlers (pty, serial tty) read/write their
         * termios argument with syscall_copyin/copyout, which reject kernel
         * pointers. So we cannot hand them a kernel-stack scratch struct —
         * we repack through the caller's own (user) buffer and dispatch with
         * the user pointer. arg2 points at musl's struct termios (>=48B), so
         * a 48-byte b1nix_termios fits. */
        if (request == B1NIX_TCGETS) {
          /* Device fills the user buffer with b1nix layout, then we convert
           * it to Linux layout in place. */
          int rc = (int)vfs_ioctl(fd, B1NIX_TCGETS, (void *)(usize)arg2);
          if (rc < 0)
            return (u64)rc;
          struct b1nix_termios bt;
          if (syscall_copyin(&bt, (void *)(usize)arg2, sizeof(bt)) < 0)
            return (u64)-EFAULT;
          struct linux_termios lt;
          lt.c_iflag = bt.c_iflag;
          lt.c_oflag = bt.c_oflag;
          lt.c_cflag = bt.c_cflag;
          lt.c_lflag = bt.c_lflag;
          lt.c_line = 0;
          memset(lt.c_cc, 0, sizeof(lt.c_cc));
          usize copy = sizeof(lt.c_cc) < sizeof(bt.c_cc)
                           ? sizeof(lt.c_cc)
                           : sizeof(bt.c_cc);
          memcpy(lt.c_cc, bt.c_cc, copy);
          if (syscall_copyout((void *)(usize)arg2, &lt, sizeof(lt)) < 0)
            return (u64)-EFAULT;
          return 0;
        }
        if (request == B1NIX_TCSETS || request == B1NIX_TCSETSW ||
            request == B1NIX_TCSETSF) {
          struct linux_termios lt;
          if (syscall_copyin(&lt, (void *)(usize)arg2, sizeof(lt)) < 0)
            return (u64)-EFAULT;
          struct b1nix_termios bt;
          memset(&bt, 0, sizeof(bt));
          bt.c_iflag = lt.c_iflag;
          bt.c_oflag = lt.c_oflag;
          bt.c_cflag = lt.c_cflag;
          bt.c_lflag = lt.c_lflag;
          memcpy(bt.c_cc, lt.c_cc, sizeof(lt.c_cc));
          /* Repack the b1nix layout back into the user buffer so the device's
           * user-validated copyin succeeds, then dispatch with the user ptr.
           * TCSADRAIN/TCSAFLUSH map to TCSETS (no output buffering).
           *
           * tcsetattr(3) takes a `const struct termios *`, so the caller's
           * buffer must come back unchanged: save the original bytes and
           * restore them afterwards. Leaving the b1nix layout behind shifted
           * the caller's copy by one byte (c_cc[0] became c_cc[1], i.e. VINTR
           * read back as VQUIT), so a second tcsetattr with the same struct
           * installed the wrong control characters. */
          u8 user_saved[sizeof(struct b1nix_termios)];
          if (syscall_copyin(user_saved, (void *)(usize)arg2,
                             sizeof(user_saved)) < 0)
            return (u64)-EFAULT;
          if (syscall_copyout((void *)(usize)arg2, &bt, sizeof(bt)) < 0)
            return (u64)-EFAULT;
          int src = vfs_ioctl(fd, B1NIX_TCSETS, (void *)(usize)arg2);
          if (syscall_copyout((void *)(usize)arg2, user_saved,
                              sizeof(user_saved)) < 0)
            return (u64)-EFAULT;
          return (u64)src;
        }
      }
      /* reboot(magic1, magic2, cmd, arg): the command Linux passes in arg2 is a
       * magic constant; SYS_REBOOT reads its own command from arg0. An
       * unrecognised command is EINVAL rather than a silent restart. */
      if (number == LINUX_NR_REBOOT) {
        switch ((u32)arg2) {
        case LINUX_REBOOT_CMD_POWER_OFF: arg0 = B1NIX_REBOOT_POWEROFF; break;
        case LINUX_REBOOT_CMD_HALT:      arg0 = B1NIX_REBOOT_HALT;     break;
        case LINUX_REBOOT_CMD_RESTART:   arg0 = B1NIX_REBOOT_RESTART;  break;
        default: return (u64)-EINVAL;
        }
      }
      /* xattr: Linux picks follow/don't-follow by syscall number, b1nix by a
       * trailing `nofollow` argument the caller never passes. Supply it here so
       * the handler does not read an argument register the caller never set. */
      switch (number) {
      case LINUX_NR_SETXATTR:     arg5 = 0; break;
      case LINUX_NR_LSETXATTR:    arg5 = 1; break;
      case LINUX_NR_GETXATTR:     arg4 = 0; break;
      case LINUX_NR_LGETXATTR:    arg4 = 1; break;
      case LINUX_NR_LISTXATTR:    arg3 = 0; break;
      case LINUX_NR_LLISTXATTR:   arg3 = 1; break;
      case LINUX_NR_REMOVEXATTR:  arg2 = 0; break;
      case LINUX_NR_LREMOVEXATTR: arg2 = 1; break;
      default: break;
      }
      u32 native = linux_syscall_to_b1nix(number);
      if (native == LINUX_SYS_UNMAPPED) {
        console_write("linux-abi: unmapped syscall ");
        console_write(linux_syscall_name(number));
        console_write(" (nr=");
        console_write_dec(number);
        console_write(") -> -ENOSYS\n");
        return (u64)-ENOSYS;
      }
      number = native;
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
    /* ioctl request codes are 32-bit; _IOR codes with the top bit set (e.g.
     * TIOCGPTN=0x80045430) arrive sign-extended through musl's `int request`.
     * Truncate so handlers' `case 0x80045430:` labels match. */
    return (u64)vfs_ioctl((int)arg0, (u32)arg1, (void *)(usize)arg2);
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
  case SYS_GETDENTS64:
    /* Native entry to the Linux getdents64 byte layout (variable-length
     * records with d_reclen), used by ports that read directories via the
     * Linux dirent ABI (e.g. Chromium base/files/dir_reader_linux). */
    return (u64)sys_linux_getdents64((int)arg0, arg1, (usize)arg2);
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
#ifdef __x86_64__
    return (u64)sys_utime((const char *)(usize)arg0, (u64)arg1, (u64)arg2);
#else
    /* i386: each 64-bit timestamp arrives split as lo/hi 32-bit args so that
     * post-2038 (>32-bit) times survive the int $0x80 register ABI. */
    return (u64)sys_utime((const char *)(usize)arg0,
                          (u64)(u32)arg1 | ((u64)(u32)arg2 << 32),
                          (u64)(u32)arg3 | ((u64)(u32)arg4 << 32));
#endif
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
        u64 r = kernel_random_u64();
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
  case SYS_EXECVEAT:
    return sys_execveat((int)arg0, (const char *)(usize)arg1,
                        (const char **)(usize)arg2,
                        (const char **)(usize)arg3, (int)arg4);
  case SYS_WAIT: {
    int kstatus = 0;
    u64 wr = (u64)scheduler_wait((usize)arg0, &kstatus);
    if ((isize)wr >= 0 && current_task && current_task->user_image &&
        ((struct user_loaded_image *)current_task->user_image)->personality == PERSONALITY_LINUX)
      kstatus = wait_status_to_linux(kstatus);
    if ((isize)wr >= 0 && arg1 && syscall_copyout((void *)(usize)arg1, &kstatus, sizeof(kstatus)) != 0) {
      return (u64)-EFAULT;
    }
    return wr;
  }
  case SYS_WAITPID: {
    int kstatus = 0;
    u64 wr = (u64)scheduler_waitpid((usize)arg0, &kstatus, (int)arg2);
    if ((isize)wr >= 0 && current_task && current_task->user_image &&
        ((struct user_loaded_image *)current_task->user_image)->personality == PERSONALITY_LINUX)
      kstatus = wait_status_to_linux(kstatus);
    if ((isize)wr >= 0 && arg1 && syscall_copyout((void *)(usize)arg1, &kstatus, sizeof(kstatus)) != 0) {
      return (u64)-EFAULT;
    }
    /* ERESTARTSYS conversion and signal delivery handled by the wrapper. */
    return wr;
  }
  case SYS_GETPID:
    return (u64)scheduler_get_pid();
  case SYS_GETPPID:
    return (u64)(current_task ? current_task->parent_id : 0);
  case SYS_SIGSUSPEND: {
    /* Signal delivery handled by the wrapper after we return. */
    u64 r = sys_sigsuspend((const u64 *)(usize)arg0);
    return r;
  }
  case SYS_ALARM:
    return (u64)sys_alarm((unsigned int)arg0);
  case SYS_FCHDIR:
    return (u64)sys_fchdir((int)arg0);
  case SYS_ACCESS:
    return (u64)sys_access((const char *)(usize)arg0, (int)arg1);
  case SYS_FTRUNCATE: {
#ifdef __x86_64__
    return (u64)vfs_ftruncate((int)arg0, arg1);
#else
    u64 len = ((u64)arg2 << 32) | (u32)arg1;
    return (u64)vfs_ftruncate((int)arg0, len);
#endif
  }
  case SYS_DUP:
    return (u64)vfs_dup((int)arg0);
  case SYS_GETRLIMIT:
    return (u64)sys_getrlimit((int)arg0, (struct rlimit *)(usize)arg1);
  case SYS_SETRLIMIT:
    return (u64)sys_setrlimit((int)arg0, (const struct rlimit *)(usize)arg1);
  case SYS_SETXATTR:
    return (u64)sys_setxattr((const char *)(usize)arg0,
                             (const char *)(usize)arg1,
                             (const void *)(usize)arg2, (usize)arg3,
                             (int)arg4, (int)arg5);
  case SYS_GETXATTR:
    return (u64)sys_getxattr((const char *)(usize)arg0,
                             (const char *)(usize)arg1, (void *)(usize)arg2,
                             (usize)arg3, (int)arg4);
  case SYS_LISTXATTR:
    return (u64)sys_listxattr((const char *)(usize)arg0, (char *)(usize)arg1,
                              (usize)arg2, (int)arg3);
  case SYS_REMOVEXATTR:
    return (u64)sys_removexattr((const char *)(usize)arg0,
                                (const char *)(usize)arg1, (int)arg2);
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
  case SYS_SETEUID: {
    klog_info("audit: seteuid called");
    struct cred *c = scheduler_get_current_cred();
    if (!c) return (u64)-EACCES;
    int rc = cred_set_euid(c, (u16)arg0);
    return rc == 0 ? 0 : (u64)-EPERM;
  }
  case SYS_SETEGID: {
    klog_info("audit: setegid called");
    struct cred *c = scheduler_get_current_cred();
    if (!c) return (u64)-EACCES;
    int rc = cred_set_egid(c, (u16)arg0);
    return rc == 0 ? 0 : (u64)-EPERM;
  }
  case SYS_SETREUID: {
    klog_info("audit: setreuid called");
    struct cred *c = scheduler_get_current_cred();
    if (!c) return (u64)-EACCES;
    return cred_setreuid(c, (int)(isize)arg0, (int)(isize)arg1) == 0
               ? 0
               : (u64)-EPERM;
  }
  case SYS_SETREGID: {
    klog_info("audit: setregid called");
    struct cred *c = scheduler_get_current_cred();
    if (!c) return (u64)-EACCES;
    return cred_setregid(c, (int)(isize)arg0, (int)(isize)arg1) == 0
               ? 0
               : (u64)-EPERM;
  }
  case SYS_SETRESUID: {
    struct cred *c = scheduler_get_current_cred();
    if (!c) return (u64)-EACCES;
    int ruid = (int)(isize)arg0;
    int euid = (int)(isize)arg1;
    int suid = (int)(isize)arg2;
    if (ruid < -1 || ruid > 0xFFFF || euid < -1 || euid > 0xFFFF ||
        suid < -1 || suid > 0xFFFF)
      return (u64)-EINVAL;
    return cred_setresuid(c, ruid, euid, suid) == 0
               ? 0
               : (u64)-EPERM;
  }
  case SYS_SETRESGID: {
    struct cred *c = scheduler_get_current_cred();
    if (!c) return (u64)-EACCES;
    int rgid = (int)(isize)arg0;
    int egid = (int)(isize)arg1;
    int sgid = (int)(isize)arg2;
    if (rgid < -1 || rgid > 0xFFFF || egid < -1 || egid > 0xFFFF ||
        sgid < -1 || sgid > 0xFFFF)
      return (u64)-EINVAL;
    return cred_setresgid(c, rgid, egid, sgid) == 0
               ? 0
               : (u64)-EPERM;
  }
  case SYS_WAITID:
    return (u64)scheduler_waitid((idtype_t)arg0, (usize)arg1,
                                 (siginfo_t *)(usize)arg2, (int)arg3);
  case SYS_TIMES: {
    struct tms *user_tms = (struct tms *)(usize)arg0;
    if (user_tms) {
      struct tms k_tms;
      k_tms.tms_utime = (clock_t)task_utime(current_task);
      k_tms.tms_stime = (clock_t)task_stime(current_task);
      k_tms.tms_cutime = (clock_t)task_cutime(current_task);
      k_tms.tms_cstime = (clock_t)task_cstime(current_task);
      if (syscall_copyout(user_tms, &k_tms, sizeof(struct tms)) < 0) {
        return (u64)-EFAULT;
      }
    }
    return (u64)scheduler_get_uptime_ticks();
  }
  case SYS_GETRUSAGE: {
    int who = (int)arg0;
    struct rusage *user_ru = (struct rusage *)(usize)arg1;
    if (!user_ru) {
      return (u64)-EFAULT;
    }
    struct rusage k_ru;
    memset(&k_ru, 0, sizeof(struct rusage));

    if (who == RUSAGE_SELF) {
      u64 utime = task_utime(current_task);
      u64 stime = task_stime(current_task);
      k_ru.ru_utime.tv_sec = (i64)(utime / 100);
      k_ru.ru_utime.tv_usec = (i64)((utime % 100) * 10000);
      k_ru.ru_stime.tv_sec = (i64)(stime / 100);
      k_ru.ru_stime.tv_usec = (i64)((stime % 100) * 10000);
    } else if (who == RUSAGE_CHILDREN) {
      u64 cutime = task_cutime(current_task);
      u64 cstime = task_cstime(current_task);
      k_ru.ru_utime.tv_sec = (i64)(cutime / 100);
      k_ru.ru_utime.tv_usec = (i64)((cutime % 100) * 10000);
      k_ru.ru_stime.tv_sec = (i64)(cstime / 100);
      k_ru.ru_stime.tv_usec = (i64)((cstime % 100) * 10000);
    } else if (who == RUSAGE_THREAD) {
      u64 utime = task_utime(current_task);
      u64 stime = task_stime(current_task);
      k_ru.ru_utime.tv_sec = (i64)(utime / 100);
      k_ru.ru_utime.tv_usec = (i64)((utime % 100) * 10000);
      k_ru.ru_stime.tv_sec = (i64)(stime / 100);
      k_ru.ru_stime.tv_usec = (i64)((stime % 100) * 10000);
    } else {
      return (u64)-EINVAL;
    }

    if (syscall_copyout(user_ru, &k_ru, sizeof(struct rusage)) < 0) {
      return (u64)-EFAULT;
    }
    return 0;
  }
  case SYS_GETPGID:
    return (u64)scheduler_getpgid((usize)arg0);
  case SYS_GETGROUPS: {
    struct cred *c = scheduler_get_current_cred();
    if (!c) return (u64)-EACCES;
    int size = (int)arg0;
    u32 *user_list = (u32 *)(usize)arg1;
    if (size == 0) {
      return (u64)c->ngroups;
    }
    if (size < c->ngroups) {
      return (u64)-EINVAL;
    }
    if (!user_list) return (u64)-EFAULT;
    u32 k_list[MAX_GROUPS];
    for (int i = 0; i < c->ngroups && i < MAX_GROUPS; i++) {
      k_list[i] = c->groups[i];
    }
    if (syscall_copyout(user_list, k_list, c->ngroups * sizeof(u32)) != 0) {
      return (u64)-EFAULT;
    }
    return (u64)c->ngroups;
  }
  case SYS_SETGROUPS: {
    klog_info("audit: setgroups called");
    struct cred *c = scheduler_get_current_cred();
    if (!c) return (u64)-EACCES;
    if (c->euid != ROOT_UID && !cred_has_cap(c, CAP_SETGID)) {
      return (u64)-EPERM;
    }
    usize size = (usize)arg0;
    const u32 *user_list = (const u32 *)(usize)arg1;
    if (size > MAX_GROUPS) {
      return (u64)-EINVAL;
    }
    if (size > 0 && !user_list) return (u64)-EFAULT;
    u32 k_list[MAX_GROUPS];
    if (size > 0) {
      if (syscall_copyin(k_list, user_list, size * sizeof(u32)) != 0) {
        return (u64)-EFAULT;
      }
    }
    for (usize i = 0; i < size; i++) {
      c->groups[i] = (u16)k_list[i];
    }
    c->ngroups = (int)size;
    return 0;
  }
  case SYS_SLEEP:
    scheduler_sleep_ticks(arg0);
    return 0;
  case SYS_NANOSLEEP: {
    struct timespec ts;
    if (syscall_copyin(&ts, (const void *)(usize)arg0, sizeof(ts)) != 0)
      return (u64)-EFAULT;
    if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000L)
      return (u64)-EINVAL;
    u64 ticks = (u64)ts.tv_sec * 100 + (u64)ts.tv_nsec / 10000000;
    if (ticks == 0) ticks = 1;
    scheduler_sleep_ticks(ticks);
    if (arg1) {
      struct timespec rem = {0, 0};
      if (syscall_copyout((void *)(usize)arg1, &rem, sizeof(rem)) != 0)
        return (u64)-EFAULT;
    }
    return 0;
  }
  case SYS_KILL: {
    /* POSIX kill(2) pid decoding: 0 = caller's process group, -1 = every
     * process the caller may signal, < -1 = process group |pid|, > 0 = that
     * process. The old code sent pid 0 to a task lookup (always failing) and
     * pid -1 to process group 1. */
    isize target = (isize)arg0;
    u64 kill_ret;
    if (target == 0) {
      kill_ret = (u64)scheduler_kill_process_group(scheduler_getpgrp(),
                                                   (int)arg1);
    } else if (target == -1) {
      kill_ret = (u64)scheduler_kill_all((int)arg1);
    } else if (target < 0) {
      kill_ret = (u64)scheduler_kill_process_group((usize)(-target), (int)arg1);
    } else {
      kill_ret = (u64)scheduler_kill((usize)target, (int)arg1);
    }
    /* Signal delivery handled by the wrapper. */
    return kill_ret;
  }
  case SYS_RT_TGSIGQUEUEINFO: {
    /* rt_tgsigqueueinfo(tgid, tid, sig, siginfo): Linux lets a process send a
     * signal to one of its own threads with attached siginfo. b1nix has no
     * per-thread siginfo delivery, so we honor the common in-tree use (LLVM's
     * crash handler re-raising a fault on itself) by re-raising the signal to
     * the calling process. The siginfo payload (arg3) is intentionally dropped
     * — exactly the semantics of raise(3), which callers fall back to. */
    int rsig = (int)arg2;
    if (rsig <= 0 || rsig >= 64)
      return (u64)-EINVAL;
    /* Re-raise to self; signal delivery is performed by the wrapper. */
    return (u64)scheduler_kill((usize)scheduler_get_pid(), rsig);
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
  case SYS_SIGQUEUE: {
    /* sigqueue(pid, sig, sival). RT signals queue with the payload delivered to
     * an SA_SIGINFO handler as si_value; a standard signal is posted via the
     * normal coalescing path (no payload). */
    int sig = (int)arg1;
    union sigval v;
    v.sival_ptr = (void *)(usize)arg2;
    if (SIG_IS_RT(sig))
      return (u64)scheduler_sigqueue((usize)arg0, sig, v, B1NIX_SI_QUEUE);
    return (u64)scheduler_kill((usize)arg0, sig);
  }
  case SYS_TIMER_CREATE: {
    /* timer_create(clockid, struct sigevent*, timer_t*). Only SIGEV_SIGNAL is
     * supported; the clock id is accepted but the tick is the single time base. */
    struct k_sigevent {
      int sigev_notify;
      int sigev_signo;
      union sigval sigev_value;
    } sev;
    if (!arg1 || !arg2)
      return (u64)-EINVAL;
    if (syscall_copyin(&sev, (void *)(usize)arg1, sizeof(sev)) < 0)
      return (u64)-EFAULT;
    if (sev.sigev_notify != 0 /* SIGEV_SIGNAL */)
      return (u64)-EINVAL;
    int id = scheduler_timer_create(sev.sigev_signo, sev.sigev_value);
    if (id < 0)
      return (u64)id;
    if (syscall_copyout((void *)(usize)arg2, &id, sizeof(int)) < 0) {
      scheduler_timer_delete(id);
      return (u64)-EFAULT;
    }
    return 0;
  }
  case SYS_TIMER_SETTIME: {
    /* timer_settime(id, flags, const itimerspec*, itimerspec*). Times convert to
     * 100 Hz ticks; it_value all-zero disarms. TIMER_ABSTIME (flags&1) is treated
     * relative (the smoke uses relative arming). */
    struct k_timespec { i64 tv_sec; i64 tv_nsec; };
    struct k_itimerspec { struct k_timespec it_interval; struct k_timespec it_value; } its;
    if (!arg2)
      return (u64)-EINVAL;
    if (syscall_copyin(&its, (void *)(usize)arg2, sizeof(its)) < 0)
      return (u64)-EFAULT;
    u64 first = (u64)its.it_value.tv_sec * 100 + (u64)its.it_value.tv_nsec / 10000000;
    u64 interval = (u64)its.it_interval.tv_sec * 100 + (u64)its.it_interval.tv_nsec / 10000000;
    /* A non-zero requested time shorter than one tick still arms (1 tick). */
    if (first == 0 && (its.it_value.tv_sec || its.it_value.tv_nsec))
      first = 1;
    if (interval == 0 && (its.it_interval.tv_sec || its.it_interval.tv_nsec))
      interval = 1;
    u64 old_rem = 0, old_int = 0;
    int rc = scheduler_timer_settime((int)arg0, first, interval, &old_rem, &old_int);
    if (rc < 0)
      return (u64)rc;
    if (arg3) {
      struct k_itimerspec old;
      old.it_value.tv_sec = (i64)(old_rem / 100);
      old.it_value.tv_nsec = (i64)((old_rem % 100) * 10000000);
      old.it_interval.tv_sec = (i64)(old_int / 100);
      old.it_interval.tv_nsec = (i64)((old_int % 100) * 10000000);
      if (syscall_copyout((void *)(usize)arg3, &old, sizeof(old)) < 0)
        return (u64)-EFAULT;
    }
    return 0;
  }
  case SYS_TIMER_GETTIME: {
    struct k_timespec { i64 tv_sec; i64 tv_nsec; };
    struct k_itimerspec { struct k_timespec it_interval; struct k_timespec it_value; } its;
    u64 rem = 0, interval = 0;
    int rc = scheduler_timer_gettime((int)arg0, &rem, &interval);
    if (rc < 0)
      return (u64)rc;
    its.it_value.tv_sec = (i64)(rem / 100);
    its.it_value.tv_nsec = (i64)((rem % 100) * 10000000);
    its.it_interval.tv_sec = (i64)(interval / 100);
    its.it_interval.tv_nsec = (i64)((interval % 100) * 10000000);
    if (!arg1 || syscall_copyout((void *)(usize)arg1, &its, sizeof(its)) < 0)
      return (u64)-EFAULT;
    return 0;
  }
  case SYS_TIMER_DELETE:
    return (u64)scheduler_timer_delete((int)arg0);
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
  case SYS_GETSID:
    return (u64)scheduler_getsid((usize)arg0);
  case SYS_GETPGRP:
    return (u64)scheduler_getpgrp();
  case SYS_SETPGRP:
    return (u64)scheduler_setpgrp((usize)arg0, (usize)arg1);
  case SYS_SETPRIORITY: {
    /* Linux setpriority(int which, id_t who, int prio): args are
     * arg0=which, arg1=who, arg2=prio. Only PRIO_PROCESS is supported; who==0
     * means the calling process. The old code read arg0 as the pid and arg1 as
     * the value, so it stored `who` (0) as the nice value and ignored the real
     * prio in arg2 — every nice()/setpriority() collapsed to nice 0, breaking
     * nice biasing (M46 nice-biasing). */
    usize who = (usize)arg1;
    int prio = (int)arg2;
    usize pid = who == 0 ? scheduler_get_pid() : who;
    return (u64)scheduler_set_priority(pid, prio);
  }
  case SYS_GETPRIORITY: {
    /* Linux getpriority(int which, id_t who): arg0=which, arg1=who. */
    usize who = (usize)arg1;
    usize pid = who == 0 ? scheduler_get_pid() : who;
    /* scheduler_get_priority already returns the Linux 20-nice encoding. */
    return (u64)(isize)scheduler_get_priority(pid);
  }
  case SYS_BRK:
    return sys_brk(arg0);
  case SYS_MMAP: {
    vma_mutator_lock();
    u64 r = sys_mmap((void *)(usize)arg0, (usize)arg1, (int)arg2, (int)arg3,
                     (int)arg4, (isize)arg5);
    vma_mutator_unlock();
    return r;
  }
  case SYS_MUNMAP: {
    vma_mutator_lock();
    u64 r = (u64)sys_munmap((void *)(usize)arg0, (usize)arg1);
    vma_mutator_unlock();
    return r;
  }
  case SYS_MPROTECT: {
    vma_mutator_lock();
    u64 r = (u64)sys_mprotect((void *)(usize)arg0, (usize)arg1, (int)arg2);
    vma_mutator_unlock();
    return r;
  }
  case SYS_MADVISE:
    return (u64)sys_madvise((void *)(usize)arg0, (usize)arg1, (int)arg2);
  case SYS_MINCORE: {
    /* SYS_MINCORE(addr, length, vec): vec[i] bit0 = the page at
     * addr + i*PAGE_SIZE is present in this address space (page-table walk).
     * POSIX requires a page-aligned addr. */
    u64 addr = arg0;
    usize length = (usize)arg1;
    u8 *uvec = (u8 *)(usize)arg2;
    if (addr & (PAGE_SIZE - 1)) return (u64)-EINVAL;
    if (!uvec) return (u64)-EFAULT;
    usize pages = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    u8 batch[512];
    usize done = 0;
    while (done < pages) {
      usize n = pages - done;
      if (n > sizeof(batch)) n = sizeof(batch);
      for (usize i = 0; i < n; i++) {
        u64 va = addr + (u64)(done + i) * PAGE_SIZE;
        batch[i] = (vmm_virt_to_phys((void *)(usize)va) != 0) ? 1 : 0;
      }
      if (syscall_copyout(uvec + done, batch, n) < 0) return (u64)-EFAULT;
      done += n;
    }
    return 0;
  }
  case SYS_SIGALTSTACK:
    return (u64)sys_sigaltstack((const void *)(usize)arg0,
                                (void *)(usize)arg1);
  /* --- M73: modern I/O & introspection --- */
  case SYS_SENDFILE:
    return (u64)sys_sendfile((int)arg0, (int)arg1, (u64 *)(usize)arg2,
                             (usize)arg3);
  case SYS_COPY_FILE_RANGE:
    return (u64)sys_copy_file_range((int)arg0, (u64 *)(usize)arg1, (int)arg2,
                                    (u64 *)(usize)arg3, (usize)arg4,
                                    (unsigned int)arg5);
  case SYS_SPLICE:
    return (u64)sys_splice((int)arg0, (u64 *)(usize)arg1, (int)arg2,
                           (u64 *)(usize)arg3, (usize)arg4,
                           (unsigned int)arg5);
  case SYS_FALLOCATE:
    return (u64)sys_fallocate((int)arg0, (int)arg1, arg2, arg3);
  case SYS_STATX:
    return (u64)sys_statx((int)arg0, (const char *)(usize)arg1, (int)arg2,
                          (unsigned int)arg3, (struct statx *)(usize)arg4);
  case SYS_MSYNC:
    return (u64)sys_msync((void *)(usize)arg0, (usize)arg1, (int)arg2);
  /* --- M63: seccomp-bpf --- */
  case SYS_SECCOMP: {
    unsigned int op = (unsigned int)arg0;
    if (op == SECCOMP_SET_MODE_FILTER)
      return (u64)seccomp_set_mode_filter((u32)arg1, (const void *)(usize)arg2);
    if (op == SECCOMP_SET_MODE_STRICT)
      return (u64)seccomp_set_mode_strict();
    return (u64)-EINVAL;
  }
  case SYS_PRCTL: {
    int option = (int)arg0;
    if (option == PR_SET_SECCOMP) {
      if (arg1 == SECCOMP_MODE_STRICT)
        return (u64)seccomp_set_mode_strict();
      if (arg1 == SECCOMP_MODE_FILTER)
        return (u64)seccomp_set_mode_filter(0, (const void *)(usize)arg2);
      return (u64)-EINVAL;
    }
    if (option == PR_SET_NO_NEW_PRIVS)
      return (u64)seccomp_set_no_new_privs();
    if (option == PR_GET_NO_NEW_PRIVS)
      return (u64)seccomp_get_no_new_privs();
    return (u64)-EINVAL; /* other prctl options unsupported */
  }
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
    /* Returns a table index (mqd), not a kernel pointer — see mqueue.h. */
    return (u64)mqueue_create(name);
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
    return (u64)mqueue_send((int)arg0, kbuf, len);
  }
  case SYS_MQ_RECEIVE: {
    char kbuf[256];
    u32 klen = 0;
    int ret = mqueue_receive((int)arg0, kbuf, &klen);
    if (ret == 0) {
      if (arg2 && syscall_copyout((void *)(usize)arg2, &klen, sizeof(u32)) != 0)
        return (u64)-EFAULT;
      if (arg1 && klen > 0 && syscall_copyout((void *)(usize)arg1, kbuf, klen) != 0)
        return (u64)-EFAULT;
    }
    return (u64)ret;
  }
  case SYS_MQ_CLOSE:
    mqueue_close((int)arg0);
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
  case SYS_SOCKETPAIR:
    return sys_socketpair((int)arg0, (int)arg1, (int)arg2,
                          (int *)(usize)arg3);
  case SYS_BIND:
    return sys_bind((int)arg0, (const void *)(usize)arg1, (usize)arg2);
  case SYS_CONNECT:
    return sys_connect((int)arg0, (const void *)(usize)arg1, (usize)arg2);
  case SYS_SEND:
    return sys_send((int)arg0, (const void *)(usize)arg1, (usize)arg2,
                    (int)arg3);
  case SYS_RECV:
    return sys_recv((int)arg0, (void *)(usize)arg1, (usize)arg2, (int)arg3);
  case SYS_SENDTO:
    return sys_sendto((int)arg0, (const void *)(usize)arg1, (usize)arg2,
                      (int)arg3, (const void *)(usize)arg4, (usize)arg5);
  case SYS_RECVFROM:
    return sys_recvfrom((int)arg0, (void *)(usize)arg1, (usize)arg2, (int)arg3,
                        (void *)(usize)arg4, (u32 *)(usize)arg5);
  case SYS_SENDMSG:
    return sys_sendmsg((int)arg0,
                       (const struct syscall_msghdr *)(usize)arg1, (int)arg2);
  case SYS_RECVMSG:
    return sys_recvmsg((int)arg0, (struct syscall_msghdr *)(usize)arg1,
                       (int)arg2);
  case SYS_MEMFD_CREATE: {
    char name[64];
    if (syscall_copyinstr(name, sizeof(name), (const char *)(usize)arg0) < 0)
      return (u64)-EFAULT;
    return (u64)vfs_memfd_create(name, (u32)arg1);
  }
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
      if (dns_resolve_sync(ip_text, dest.bytes) != 0)
        return (u64)-EINVAL;
    }

    u32 before = icmp_echo_reply_count();
    u8 echo[8] = {8, 0, 0, 0, 0, 0, 0, 0};
    u16 seq = (u16)(scheduler_get_uptime_ticks() & 0xffff);
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

    for (int wait = 0; wait < 50; wait++) {
      if (icmp_echo_reply_count() > before) {
        return 0;
      }
      scheduler_sleep_ticks(2);
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
        if (arg1) {
            u8 ip[4];
            if (dns_resolve_sync(host, ip) != 0)
                return (u64)-EHOSTUNREACH;
            if (syscall_copyout((void *)(usize)arg1, ip, 4) != 0)
                return (u64)-EFAULT;
        } else {
            dns_resolve(host);
        }
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
    fill_b1nix_utsname(&uts);
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

  case SYS_REBOOT: {
    /* Privileged: like Linux reboot()/CAP_SYS_BOOT, only root may halt or
     * reboot the machine. A NULL cred is an in-kernel caller (init kthread),
     * which is allowed. This is why /bin/{halt,reboot,poweroff,shutdown} are
     * NOT setuid — they are plain root-owned binaries. */
    const struct cred *rb_cred = scheduler_get_current_cred();
    if (rb_cred && rb_cred->euid != ROOT_UID)
      return (u64)-EPERM;
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
  }
  case SYS_DMESG:
    if (!arg0 || arg1 == 0)
      return (u64)-EINVAL;
    if (arg1 > KLOG_BUF_SIZE)
      arg1 = KLOG_BUF_SIZE;
    if (!is_user_range_valid((const void *)arg0, arg1, 1))
      return (u64)-EFAULT;
    /* static, not stack: KLOG_BUF_SIZE is now 64 KiB which would overflow the
     * kernel stack. dmesg is rare so the shared buffer's only hazard is a
     * concurrent dmesg garbling output — never a crash. */
    static char tmp[KLOG_BUF_SIZE];
    usize copied = klog_read(tmp, (usize)arg1);
    if (syscall_copyout((void *)(usize)arg0, tmp, copied) != 0)
      return (u64)-EFAULT;
    return (u64)copied;
  case SYS_SYSINFO: {
    /* Linux struct sysinfo (sysinfo(2)). Mirror byte-for-byte the userspace
     * <sys/sysinfo.h>: native `unsigned long` on both sides (32-bit i686 /
     * 64-bit x86_64). b1nix has no per-buffer-cache accounting or swap-size
     * API, so bufferram/sharedram/swap are 0; mem_unit stays 1 (bytes) so
     * BusyBox free does no scaling. */
    struct k_sysinfo {
      long uptime;
      unsigned long loads[3];
      unsigned long totalram;
      unsigned long freeram;
      unsigned long sharedram;
      unsigned long bufferram;
      unsigned long totalswap;
      unsigned long freeswap;
      unsigned short procs;
      unsigned short pad;
      unsigned long totalhigh;
      unsigned long freehigh;
      unsigned int mem_unit;
      char _f[20 - 2 * sizeof(long) - sizeof(int)];
    } info;
    memset(&info, 0, sizeof(info));
    info.uptime = (long)(scheduler_get_uptime_ticks() / 100u);
    info.totalram = (unsigned long)pmm_total_usable_memory();
    info.freeram = (unsigned long)pmm_free_memory_estimate();
    info.procs = (unsigned short)scheduler_task_count();
    info.mem_unit = 1;
    if (!arg0)
      return (u64)-EFAULT;
    if (syscall_copyout((void *)(usize)arg0, &info, sizeof(info)) != 0)
      return (u64)-EFAULT;
    return 0;
  }
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
    /* Through the tail so a -ERESTARTSYS from an interrupting signal becomes
     * -EINTR (or a restart) and the pending handler is delivered. */
    ret = sys_poll((struct b1nix_pollfd *)arg0, arg1, arg2);
    break;
  case SYS_SIGRETURN:
    ret = sys_sigreturn(frame);
    break;
  case SYS_CLOCK_GETTIME: {
    int clk_id = (int)arg0;
    struct timespec ktp;
    u64 ticks = scheduler_get_uptime_ticks();
    /* Monotonic family: CLOCK_MONOTONIC(1), CLOCK_PROCESS_CPUTIME_ID(2),
     * CLOCK_THREAD_CPUTIME_ID(3), CLOCK_MONOTONIC_RAW(4),
     * CLOCK_MONOTONIC_COARSE(6), CLOCK_BOOTTIME(7) -> uptime monotonic clock.
     * b1nix has no separate boot/raw clocks and no fine-grained per-task CPU
     * accounting, so the CPU-time ids are a best-effort monotonic value (added
     * for the Chromium port; values match Linux). CLOCK_REALTIME(0) and
     * CLOCK_REALTIME_COARSE(5) -> wall clock. */
    if (clk_id == 1 || clk_id == 2 || clk_id == 3 || clk_id == 4 ||
        clk_id == 6 || clk_id == 7) {
      ktp.tv_sec = (i64)(ticks / 100);
      ktp.tv_nsec = (i64)((ticks % 100) * 10000000);
    } else {
      /* CLOCK_REALTIME / CLOCK_REALTIME_COARSE: epoch-based wall clock. */
      ktp.tv_sec = (i64)vfs_get_unix_time();
      ktp.tv_nsec = (i64)((ticks % 100) * 10000000);
    }
    if (syscall_copyout((void *)(usize)arg1, &ktp, sizeof(struct timespec)) != 0) {
      return (u64)-EFAULT;
    }
    return 0;
  }
  case SYS_SETTIMEOFDAY: {
    /* SYS_SETTIMEOFDAY(const struct timeval *tv) — set the wall clock. Only
     * root may step the clock (POSIX EPERM otherwise). Backed by the RTC time
     * offset so every subsequent time read reflects the new value. */
    struct cred *c = scheduler_get_current_cred();
    if (!c || (c->euid != ROOT_UID && !cred_has_cap(c, CAP_SYS_TIME)))
      return (u64)-EPERM;
    if (!arg0) return (u64)-EFAULT;
    struct timeval tv;
    if (syscall_copyin(&tv, (const void *)(usize)arg0, sizeof(tv)) != 0)
      return (u64)-EFAULT;
    if (tv.tv_sec < 0 || tv.tv_usec < 0 || tv.tv_usec >= 1000000)
      return (u64)-EINVAL;
    rtc_set_unix_time((u64)tv.tv_sec);
    return 0;
  }
  case SYS_SCHED_GETAFFINITY: {
    /* sched_getaffinity(pid, cpusetsize, mask). b1nix does not pin userspace
     * tasks to CPUs, so every task may run on any online CPU — report the set
     * of online CPUs (bits 0..online-1). Returns the number of bytes written,
     * matching the Linux raw-syscall convention. */
    extern int get_online_cpu_count(void);
    usize cpusetsize = (usize)arg1;
    void *umask = (void *)(usize)arg2;
    if (!umask || cpusetsize == 0) return (u64)-EINVAL;
    int online = get_online_cpu_count();
    if (online < 1) online = 1;
    u8 kmask[128];
    usize n = cpusetsize < sizeof(kmask) ? cpusetsize : sizeof(kmask);
    memset(kmask, 0, n);
    for (int c = 0; c < online && (usize)(c / 8) < n; c++)
      kmask[c / 8] |= (u8)(1u << (c % 8));
    if (syscall_copyout(umask, kmask, n) != 0) return (u64)-EFAULT;
    return (u64)n;
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
    return (u64)scheduler_clone_thread(arg0, arg1, arg2, arg3, arg4, arg5, 0, 0, 0);
  case SYS_FUTEX:
    /* SYS_FUTEX(uaddr, op, val, timeout_ms) — WAIT/WAKE. timeout_ms>0 arms a
     * relative deadline on WAIT (returns -ETIMEDOUT on expiry); 0 = forever. */
    return (u64)scheduler_futex(arg0, (int)arg1, (int)arg2, arg3);
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
  case SYS_GET_TLS_INFO: {
    /* SYS_GET_TLS_INFO(info, image_out, image_cap) — expose the running image's
     * PT_TLS template so the libc can build a per-thread ELF TLS block in
     * pthread_create (the kernel only sets up the main thread's TLS at exec).
     * info (struct b1nix_tls_info: memsz, filesz, align) is filled if non-NULL;
     * if image_out is non-NULL up to image_cap bytes of the .tdata init image
     * are copied out. Returns 0, or -errno. */
    struct task *t = current_task;
    if (!t || !t->user_image) return (u64)-EINVAL;
    struct user_loaded_image *img = (struct user_loaded_image *)t->user_image;
    if (arg0) {
      struct { u64 memsz, filesz, align; } info = {
          img->tls_memsz, img->tls_filesz,
          img->tls_align ? img->tls_align : 8};
      if (syscall_copyout((void *)(usize)arg0, &info, sizeof(info)) < 0)
        return (u64)-EFAULT;
    }
    if (arg1 && img->tls_data && img->tls_filesz) {
      u64 n = img->tls_filesz < arg2 ? img->tls_filesz : arg2;
      if (syscall_copyout((void *)(usize)arg1, img->tls_data, (usize)n) < 0)
        return (u64)-EFAULT;
    }
    return 0;
  }
  case SYS_DL_PHDR_INFO: {
    /* SYS_DL_PHDR_INFO(buf, cap) — copy out the loaded-module table (the
     * executable + every shared library) that backs dl_iterate_phdr. Each entry
     * is {u64 base, u64 phdr_vaddr, u64 phnum, char name[96]} matching userspace's
     * struct b1nix_dl_module. Up to `cap` entries are written to `buf`; the return
     * value is the TOTAL module count so the caller can detect truncation. The
     * libc dl_iterate_phdr uses this so the libgcc_s.so unwinder can locate each
     * module's PT_GNU_EH_FRAME (cross-DSO C++ exception unwinding). */
    struct task *t = current_task;
    if (!t || !t->user_image) return 0;
    struct user_loaded_image *img = (struct user_loaded_image *)t->user_image;
    usize total = img->dl_module_count;
    if (arg0 && arg1) {
      struct {
        u64 base, phdr_vaddr, phnum, eh_frame_va;
        char name[USER_DL_MODULE_NAME_MAX];
      } e;
      usize n = total < arg1 ? total : arg1;
      for (usize i = 0; i < n; i++) {
        e.base = img->dl_modules[i].base;
        e.phdr_vaddr = img->dl_modules[i].phdr_vaddr;
        e.phnum = img->dl_modules[i].phnum;
        e.eh_frame_va = img->dl_modules[i].eh_frame_va;
        memcpy(e.name, img->dl_modules[i].name, USER_DL_MODULE_NAME_MAX);
        if (syscall_copyout((void *)(usize)(arg0 + i * sizeof(e)), &e,
                            sizeof(e)) < 0)
          return (u64)-EFAULT;
      }
    }
    return (u64)total;
  }
  case SYS_FD_PATH: {
    /* SYS_FD_PATH(fd, buf, size) — write the absolute path of an open fd into
     * buf (NUL-terminated), returning the path length. Backs libc's *at()
     * emulation: openat/unlinkat with a real dirfd resolve the dirfd to its path
     * here, then join the relative component (b1nix has no per-fd-base path
     * resolver in the kernel path walker). */
    int fd = (int)arg0;
    char kbuf[VFS_MAX_PATH];
    usize cap = (usize)arg2;
    if (cap == 0)
      return (u64)-EINVAL;
    if (cap > sizeof(kbuf))
      cap = sizeof(kbuf);
    int rc = vfs_fd_abspath(fd, kbuf, cap);
    if (rc < 0)
      return (u64)rc;
    if (syscall_copyout((void *)(usize)arg1, kbuf, (usize)rc + 1) < 0)
      return (u64)-EFAULT;
    return (u64)rc;
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
    int select_eintr = 0;
    while (1) {
      /* Publish BLOCKED before the fd scan — same SMP lost-wakeup fix as
       * sys_poll above (the SSH/select wedge). */
      scheduler_wait_prepare(vfs_poll_chan);
      ready_count = 0;
      for (int i = 0; i < np; i++) {
        if (pfds[i].fd < 0) { pfds[i].revents = 0; continue; }
        struct vfs_handle *h = scheduler_fd_get(pfds[i].fd);
        if (!h) { pfds[i].revents = B1NIX_POLLNVAL; ready_count++; continue; }
        pfds[i].revents = 0;
        vfs_poll(pfds[i].fd, &pfds[i]);
        if (pfds[i].revents) ready_count++;
      }
      int timed_out = 0;
      if (ready_count == 0 && timeout_ms != 0 && timeout_ms != (u64)-1) {
        u64 now = scheduler_get_uptime_ticks();
        if (now - start_ticks >= timeout_ticks) timed_out = 1;
      }
      if (ready_count > 0 || timeout_ms == 0 || timed_out) {
        scheduler_wait_cancel();
        break;
      }
      /* Interrupted by a caught signal (incl. SIGCHLD)? Bail to the tail with
       * -ERESTARTSYS — dropbear's session loop expects select() to return EINTR
       * so it can reap the exited child and forward its output in order. */
      if (select_poll_signal_pending()) {
        scheduler_wait_cancel();
        select_eintr = 1;
        break;
      }
      /* Re-arm every iteration — an explicit wake clears wake_tick (see
       * sys_poll for the full unbounded-sleep failure this prevents). */
      if (timeout_ms != (u64)-1 && timeout_ms != 0) {
        u64 ticks = timeout_ticks > 0 ? timeout_ticks : 1;
        current_task->wake_tick = start_ticks + ticks;
      }
      scheduler_wait_commit();
    }

    current_task->wake_tick = 0;
    if (select_eintr) {
      ret = (u64)-ERESTARTSYS;
      break;
    }

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

  /* --- M56: event-loop & IPC primitives (grouped to ease merging) --- */
  case SYS_EVENTFD2:
    return (u64)vfs_eventfd((unsigned int)arg0, (int)arg1);
  case SYS_EPOLL_CREATE1:
    return (u64)vfs_epoll_create((int)arg0);
  case SYS_EPOLL_CTL: {
    /* epoll_ctl(epfd, op, fd, event). EPOLL_CTL_DEL ignores event. */
    struct b1nix_epoll_event kev;
    struct b1nix_epoll_event *kevp = 0;
    if (arg3) {
      if (syscall_copyin(&kev, (void *)(usize)arg3, sizeof(kev)) < 0)
        return (u64)-EFAULT;
      kevp = &kev;
    }
    return (u64)vfs_epoll_ctl((int)arg0, (int)arg1, (int)arg2, kevp);
  }
  case SYS_EPOLL_WAIT: {
    /* epoll_wait(epfd, events, maxevents, timeout_ms). */
    int maxevents = (int)arg2;
    if (maxevents <= 0)
      return (u64)-EINVAL;
    enum { EPOLL_BATCH = 64 };
    if (maxevents > EPOLL_BATCH)
      maxevents = EPOLL_BATCH;
    if (!arg1)
      return (u64)-EFAULT;
    struct b1nix_epoll_event kbuf[EPOLL_BATCH];
    int n = vfs_epoll_wait((int)arg0, kbuf, maxevents, (int)arg3);
    if (n < 0)
      return (u64)(isize)n;
    if (n > 0 &&
        syscall_copyout((void *)(usize)arg1, kbuf,
                        (usize)n * sizeof(struct b1nix_epoll_event)) < 0)
      return (u64)-EFAULT;
    return (u64)n;
  }
  case SYS_TIMERFD_CREATE:
    return (u64)vfs_timerfd_create((int)arg0, (int)arg1);
  case SYS_TIMERFD_SETTIME: {
    /* timerfd_settime(fd, flags, new_value, old_value). */
    struct b1nix_itimerspec newv, oldv;
    if (!arg2)
      return (u64)-EINVAL;
    if (syscall_copyin(&newv, (void *)(usize)arg2, sizeof(newv)) < 0)
      return (u64)-EFAULT;
    int rc = vfs_timerfd_settime((int)arg0, (int)arg1, &newv,
                                 arg3 ? &oldv : 0);
    if (rc < 0)
      return (u64)(isize)rc;
    if (arg3 && syscall_copyout((void *)(usize)arg3, &oldv, sizeof(oldv)) < 0)
      return (u64)-EFAULT;
    return 0;
  }
  case SYS_SIGNALFD4: {
    /* signalfd4(fd, sigmask_ptr, flags). mask is a pointer to a 64-bit
     * signal bitmask (Linux/musl convention). Copy it from userspace. */
    u64 mask_val = 0;
    if (syscall_copyin(&mask_val, (void *)(usize)arg1, sizeof(mask_val)) < 0)
      return (u64)-EFAULT;
    return (u64)vfs_signalfd((int)arg0, mask_val, (int)arg2);
  }

  case SYS_INOTIFY_INIT1:
    return (u64)vfs_inotify_init1((int)arg0);
  case SYS_INOTIFY_ADD_WATCH:
    return (u64)vfs_inotify_add_watch((int)arg0, (const char *)(usize)arg1,
                                      (u32)arg2);
  case SYS_INOTIFY_RM_WATCH:
    return (u64)vfs_inotify_rm_watch((int)arg0, (int)arg1);

  case SYS_EXIT_GROUP:
    scheduler_exit_group((int)arg0);
    ret = 0;
    break;

  case SYS_SET_TID_ADDRESS:
    /* set_tid_address(tidptr): store the clear-child-tid pointer and return
     * the calling thread's TID.  musl calls this during __init_tls. */
    task_set_child_tid_clear(current_task, (u64)arg0);
    ret = (u64)scheduler_get_pid();
    break;

  case SYS_WRITEV:
    return (u64)sys_writev((int)arg0, (const struct b1nix_iovec *)(usize)arg1,
                           (int)arg2);
  case SYS_READV:
    return (u64)sys_readv((int)arg0, (const struct b1nix_iovec *)(usize)arg1,
                          (int)arg2);

  default:
    console_write("syscall: unknown 0x");
    console_write_hex64(number);
    console_write("\n");
    ret = (u64)-ENOSYS;
    break;
  }

  /* ERESTARTSYS conversion, frame->rax publication, and signal delivery are
   * all handled uniformly by the outer syscall_dispatch_impl wrapper. */

  return ret;
}
