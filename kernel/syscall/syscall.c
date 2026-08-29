#include <b1nix/arch.h>
#include <b1nix/console.h>
#include <b1nix/cgroup.h>
#include <b1nix/dirent.h>
#include <b1nix/errno.h>
#include <b1nix/kprintf.h>
#include <b1nix/initramfs.h>
#include <b1nix/io.h>
#include <b1nix/klog.h>
#include <b1nix/kmsg.h>
#include <b1nix/linux_abi.h>
#include <b1nix/mm.h>

/* arch/x86_64/tlb.c; a no-op on a single-CPU boot. */
void tlb_shootdown_all(void);
#include <b1nix/mqueue.h>
#include <b1nix/namespace.h>
#include <b1nix/net.h>
#include <b1nix/page_cache.h>
#include <b1nix/inotify.h>
#include <b1nix/posix.h>
#include <b1nix/seccomp.h>
#include <b1nix/rtc.h>
#include <b1nix/ptrace.h>
#include <b1nix/rseq.h>
#include <b1nix/sched.h>
#include <b1nix/blk.h>
#include <b1nix/shm.h>
#include <b1nix/sysv_ipc.h>
#include <b1nix/sock_filter.h>
#include <b1nix/syscall.h>
#include <b1nix/uidgid.h>
#include <b1nix/user.h>
#include <b1nix/module.h>
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

/* Make every page of a user range genuinely writable, or refuse.
 *
 * The VMA list says what a program is ALLOWED to do; the page tables say what
 * the CPU will actually permit, and the two disagree routinely -- a
 * copy-on-write page inside a PROT_WRITE mapping is read-only in the tables,
 * and so is one whose region was later downgraded. memcpy obeys the tables, so
 * a copyout validated against the list alone stores into a read-only page from
 * ring 0. A user-mode store there is an ordinary fault the handler either
 * services or reports as SIGSEGV; the same store from the kernel arrived as an
 * unhandled exception and PANICKED the machine -- seen clearing a dying
 * thread's tid word, which musl points at its own thread-list lock, so the
 * crash landed on every threaded program that exited at the wrong moment.
 *
 * Resolving it here rather than inside the copy is deliberate twice over: the
 * caller gets the EFAULT it already knows how to handle, and the fault is taken
 * at a point of this function's choosing rather than in the middle of a memcpy
 * some callers reach from a context where faulting is not allowed. */
static int user_range_prepare_write(void *user_dst, usize size) {
  extern u64 vmm_query_leaf_pte(u64 vaddr);
  const u64 need = VMM_PRESENT | VMM_USER | VMM_WRITABLE;
  u64 start = (u64)(usize)user_dst;
  u64 end = start + size;

  if (!current_task || !current_task->user_image)
    return 1; /* early boot writes go to kernel memory */
  for (u64 v = start & ~(u64)(PAGE_SIZE - 1); v < end; v += PAGE_SIZE) {
    u64 pte = vmm_query_leaf_pte(v);

    /* Only the one case that is fatal is handled here. A page that is absent,
     * lazy, or under a huge entry with no leaf of its own faults from the copy
     * itself and the handler services it exactly as it always has; taking those
     * faults early instead would double the work on the hottest path in the
     * kernel for no gain. What the handler cannot survive is a store to a page
     * that IS present and IS read-only, because that arrives as a supervisor
     * protection fault with nothing to fix up. */
    if (!(pte & VMM_PRESENT) || (pte & need) == need)
      continue;
    if (!(pte & VMM_USER))
      return 0;
    /* Ask for the same fault a store from ring 3 would take -- which breaks a
     * copy-on-write sharing, or fails because the page really is read-only.
     *
     * More than once, because the handler answers some faults by resolving one
     * layer and asking to be called again (a huge identity page is split, a
     * translation another CPU changed is flushed and retried). A single call
     * therefore proves nothing about whether the page can be written; only the
     * table does, and only after the handler has stopped making progress. */
    int rc = 0;
    for (int attempt = 0; attempt < 4; attempt++) {
      /* PF_PRESENT matters: the handler decides which kind of fault this is
       * from the error code, not from the entry. Without it the page is taken
       * for an absent one and served by the anonymous fast path, which sees a
       * present leaf, calls the fault already handled and returns success --
       * leaving the entry exactly as read-only as it found it. The code the CPU
       * would have reported is the code to pass. */
      rc = vmm_handle_page_fault(v, PF_USER | PF_WRITE | PF_PRESENT);
      pte = vmm_query_leaf_pte(v);
      if ((pte & need) == need || rc < 0)
        break;
    }
    if ((pte & need) != need) {
      /* Bounded, because this is the report of a write the kernel was asked to
       * make into memory the program itself could not write. Silence here is
       * what turned it into an unexplained EFAULT far from its cause. */
      static unsigned refused;

      if (refused < 8) {
        char line[128];

        refused++;
        snprintf(line, sizeof(line),
                 "copyout: refusing write to read-only user page va 0x%llx "
                 "pte 0x%llx rc %d task %s\n",
                 (unsigned long long)v, (unsigned long long)pte, rc,
                 current_task->name ? current_task->name : "?");
        console_write(line);
      }
      return 0;
    }
  }
  return 1;
}

int syscall_copyout(void *user_dst, const void *src, usize size) {
  if (size == 0)
    return 0;
  if (!user_dst || !src)
    return -EFAULT;

  if (!is_user_range_valid(user_dst, size, 1)) {
    return -EFAULT;
  }
  if (!user_range_prepare_write(user_dst, size)) {
    return -EFAULT;
  }

  /* Every write the kernel makes into a traced process, with its size.
   *
   * A heap that fails its own consistency check has had something written
   * before the block, and the kernel is one of the few writers that can do that
   * without the program noticing. b1nix.trace-copyout turns this on; it is
   * paired with the syscall trace, so each write sits under the call that made
   * it. */
  extern int syscall_trace_active(void);
  if (syscall_trace_active()) {
    char cl[96];

    snprintf(cl, sizeof(cl), "  copyout %p +%llu", user_dst,
             (unsigned long long)size);
    klog_debug_category("syscall", cl);
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

  /* Fast path: ask the page tables, not the VMA list.
   *
   * The list walk below is O(number of mappings), and it ran on every copy in
   * or out of userspace. A browser holds thousands of mappings and makes
   * hundreds of thousands of copying calls in a start-up, which put a plain
   * clock_gettime at about a hundred microseconds — the syscall did almost
   * nothing and spent all of it walking a list. A page that is present, marked
   * user, and (for a write) writable is by construction covered by a mapping
   * with those permissions, so the tables answer the same question in four
   * loads. Anything else — a lazy page, a COW page, a hole — falls through to
   * the walk, which stays the authority. */
  {
    extern u64 vmm_query_leaf_pte(u64 vaddr);
    u64 need = VMM_PRESENT | VMM_USER | (write ? VMM_WRITABLE : 0);
    u64 v = start & ~(u64)(PAGE_SIZE - 1);
    int resident = 1;

    for (; v < end; v += PAGE_SIZE) {
      if ((vmm_query_leaf_pte(v) & need) != need) {
        resident = 0;
        break;
      }
    }
    if (resident)
      return 1;
  }

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

  /* A driver that copies out itself gets the caller's pointer, unbounced. */
  if (vfs_read_is_direct(fd))
    return vfs_read_user(fd, buf, count);

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

  /* write(fd, buf, 0) moves no bytes — except on a message-oriented socket,
   * where Linux puts an empty message on the wire. Returning 0 without sending
   * anything loses a message the receiver is waiting for. */
  if (count == 0) {
    if (!vfs_socket_sends_empty_messages(fd))
      return 0;
    isize res = vfs_write(fd, kbuf, 0);
    return res < 0 ? res : 0;
  }

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

    /* Same rule as sys_read: the driver owns the copy, so the segment's own
     * pointer goes straight through. Leaving this on the bounce path would
     * have made readv the one call that still failed with EFAULT. */
    if (vfs_read_is_direct(fd)) {
      isize res = vfs_read_user(fd, base, len);
      if (res < 0)
        return total > 0 ? total : res;
      total += res;
      if (res < (isize)len)
        return total;
      continue;
    }

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

/* Bytes moved per iteration of the fd→fd pump below. Linux's splice moves a
 * full pipe — 16 pages — in one go, and this is that same 64 KiB. */
#define FILE_COPY_CHUNK (64 * 1024)

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
  /* The bounce buffer is a heap allocation, not a stack array: this loop is
   * the whole of sendfile/copy_file_range/splice, and at one 4 KiB round trip
   * per iteration a large copy pays a read and a write syscall's worth of VFS
   * work per page. Linux moves a pipeful — 16 pages, 64 KiB — per iteration,
   * which is the size asked for here; the 4 KiB stack buffer stays as the
   * fallback for when the heap cannot spare it, so the path never fails for
   * want of the larger buffer. */
  char stack_buf[4096];
  usize bufsz = count < FILE_COPY_CHUNK ? count : FILE_COPY_CHUNK;
  if (bufsz < sizeof(stack_buf))
    bufsz = sizeof(stack_buf);
  char *kbuf = (bufsz > sizeof(stack_buf)) ? (char *)kmalloc(bufsz) : 0;
  if (!kbuf) {
    kbuf = stack_buf;
    bufsz = sizeof(stack_buf);
  }
  isize total = 0;
  isize err = 0;
  u64 ipos = in_off ? *in_off : 0;
  u64 opos = out_off ? *out_off : 0;
  while (count > 0) {
    usize chunk = count > bufsz ? bufsz : count;
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

  if (kbuf != stack_buf)
    kfree(kbuf);
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
/* Resolve a dirfd-relative path the way the *at() syscalls define it: an
 * absolute path or AT_FDCWD is used as-is, anything else is joined onto the
 * directory the fd names. b1nix has no per-fd resolution in the VFS, so the
 * join happens here on top of vfs_fd_abspath(). Returns 0 or -errno. */
static int syscall_resolve_at(int dirfd, const char *kpath, char *out,
                              usize outsz) {
  if (!kpath || !out || outsz == 0)
    return -EINVAL;
  if (kpath[0] == '/' || dirfd == AT_FDCWD) {
    if (strlen(kpath) >= outsz)
      return -ENAMETOOLONG;
    strcpy(out, kpath);
    return 0;
  }
  char dirbuf[VFS_MAX_PATH];
  int rc = vfs_fd_abspath(dirfd, dirbuf, sizeof(dirbuf));
  if (rc < 0)
    return rc;
  usize dlen = (usize)rc;
  if (dlen + 1 + strlen(kpath) >= outsz)
    return -ENAMETOOLONG;
  memcpy(out, dirbuf, dlen);
  out[dlen] = '/';
  strcpy(out + dlen + 1, kpath);
  return 0;
}

static int sys_statx(int dirfd, const char *user_path, int flags,
                     unsigned int mask, struct statx *user_buf) {
  struct b1nix_stat st;
  int rc;
  /* The path the answer is about, kept so the mount id can be worked out from
   * it below. Both branches produce one: AT_EMPTY_PATH names a descriptor, and
   * a descriptor has an absolute path. */
  char resolved[VFS_MAX_PATH];
  resolved[0] = '\0';
  if ((flags & AT_EMPTY_PATH) && (!user_path || user_path[0] == '\0')) {
    rc = vfs_fstat(dirfd, &st);
    if (vfs_fd_abspath(dirfd, resolved, sizeof(resolved)) < 0)
      resolved[0] = '\0';
  } else {
    char kpath[VFS_MAX_PATH];
    if (syscall_copyinstr(kpath, sizeof(kpath), user_path) < 0)
      return -EFAULT;
    int arc = syscall_resolve_at(dirfd, kpath, resolved, sizeof(resolved));
    if (arc < 0)
      return arc;
    rc = (flags & AT_SYMLINK_NOFOLLOW) ? vfs_lstat(resolved, &st)
                                       : vfs_stat(resolved, &st);
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

  /* The mount id, when the caller asked for it.
   *
   * Reported only when it is really known: the mask bit is what tells the
   * caller the field means something, and setting it over a zero would be
   * worse than leaving it clear -- systemd would then go looking in
   * /proc/self/mountinfo for a row numbered 0, find none, and draw a
   * conclusion from it. Both the 32-bit and the 64-bit form get the same
   * number, which is the one /proc/<pid>/mountinfo prints; b1nix does not
   * reuse mount ids within a boot, so it satisfies what MNT_ID_UNIQUE
   * promises.
   *
   * This is how systemd asks whether a path is a mount point, and it asks
   * before it will mount anything: without an answer it reported "Failed to
   * determine whether /proc is a mount point" for each API filesystem in turn
   * and then "Failed to mount API filesystems", and PID 1 exited. */
  if (mask & (STATX_MNT_ID | STATX_MNT_ID_UNIQUE)) {
    int mid = resolved[0] ? vfs_mount_id_for_path(resolved) : 0;
    if (mid > 0) {
      sx.stx_mnt_id = (u64)mid;
      sx.stx_mask |= mask & (STATX_MNT_ID | STATX_MNT_ID_UNIQUE);
    }
  }

  /* Whether this path is the root of a mount.
   *
   * stx_attributes_mask is the promise: it names the bits this kernel knows
   * how to report, and a caller reads stx_attributes only for bits that appear
   * in it. Both were left at zero, which says "this kernel cannot tell you
   * anything about attributes" -- and since systemd 256 that is the only
   * question systemd asks about mount points, the name_to_handle_at(2) and
   * mountinfo fallbacks having been removed. It answered -EUNATCH for each of
   * /proc, /sys, /dev, /dev/shm and /run, reported "Failed to mount API
   * filesystems", and exited before it had printed one line about itself.
   *
   * Unlike stx_mnt_id above this is NOT conditional on a mask bit: Linux fills
   * stx_attributes on every statx, and the mask is what tells the caller the
   * value means something. */
  if (resolved[0]) {
    sx.stx_attributes_mask = STATX_ATTR_MOUNT_ROOT;
    if (vfs_path_is_mount_root(resolved))
      sx.stx_attributes |= STATX_ATTR_MOUNT_ROOT;
  }
  /* No path -- an AT_EMPTY_PATH call on a descriptor with no name -- means the
   * question cannot be answered, and the mask stays empty rather than claiming
   * an answer of "no". */
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
  int rc = vfs_ioctl(fd, request, arg);

  /* Which request on which descriptor was refused as "not a terminal".
   *
   * "errno 25 from syscall 16" names ioctl and no more, and a terminal answers
   * a dozen requests through several handlers. util-linux's login takes an
   * ENOTTY, carries on, and then uses a buffer it never filled -- so the
   * request code and the descriptor are the whole question, and neither the
   * errno trace nor the VT layer's own trace could answer it once the refusal
   * came from somewhere other than the VT. `b1nix.trace-ioctl`. */
  if (rc == -ENOTTY && bootinfo_has_flag("b1nix.trace-ioctl")) {
    console_write("ioctl: ENOTTY req=0x");
    console_write_hex64(request);
    console_write(" fd=");
    console_write_dec((u64)(u32)fd);
    if (current_task) {
      console_write(" by ");
      console_write(current_task->name);
    }
    console_write("\n");
  }
  return (u64)rc;
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
  /* M98: the GNU Make port is retired; /bin/make is bmake (NetBSD make). */
  copy_cstr(status->make, sizeof(status->make), "bmake-port");
  return 0;
}

/* Translate Linux O_* open flags to b1nix flags via an explicit whitelist.
 * Several bits DIVERGE and even COLLIDE between the two ABIs, so a pass-through
 * with per-bit patching is unsafe:
 *   - Linux O_NONBLOCK (04000 = 0x800) == b1nix O_CLOEXEC (0x800)
 *   - Linux O_CLOEXEC  (02000000 = 0x80000) != b1nix O_CLOEXEC
 *   - Linux O_DIRECT   (040000 = 0x4000) == b1nix O_NONBLOCK (0x4000)
 * O_NOFOLLOW and O_PATH do not diverge -- b1nix uses Linux's values for both --
 * and they carry meaning a path walker depends on, so they are translated
 * rather than dropped.
 * Build the b1nix flag set from recognized Linux bits only; unknown Linux bits
 * (O_NOCTTY/O_SYNC/O_DIRECT/O_NOFOLLOW/...) are dropped so they cannot set an
 * unrelated b1nix flag. The low two bits (O_RDONLY/O_WRONLY/O_RDWR) are
 * identical in both ABIs. */
static int linux_open_flags_to_b1nix(int lf) {
  int flags = lf & 0x3; /* access mode (shared) */
  if (lf & 0100)     flags |= B1NIX_O_CREAT;     /* Linux 0x40  */
  if (lf & 0200)     flags |= B1NIX_O_EXCL;      /* Linux 0x80  */
  if (lf & 01000)    flags |= B1NIX_O_TRUNC;     /* Linux 0x200 */
  if (lf & 02000)    flags |= B1NIX_O_APPEND;    /* Linux 0x400 */
  /* Linux O_NONBLOCK (04000), plus 040000 for callers that pass b1nix-native
   * flags (b1nix O_NONBLOCK == 0x4000 == 040000) through the Linux path.
   * 0100000 is NOT included: that is Linux O_LARGEFILE, which musl ORs into
   * EVERY open() (see __sys_open_cp in musl's src/internal/syscall.h). Mapping
   * it to O_NONBLOCK made every musl open non-blocking — harmless on regular
   * files, but it turned a FIFO's blocking open into an instant ENXIO. On
   * 64-bit O_LARGEFILE is a no-op, so it is simply dropped. */
  if (lf & (04000 | 040000))
    flags |= B1NIX_O_NONBLOCK;
  if (lf & 0200000)  flags |= B1NIX_O_DIRECTORY; /* Linux 0x10000 (shared) */
  if (lf & 0400000)  flags |= B1NIX_O_NOFOLLOW;  /* Linux 0x20000 (shared) */
  if (lf & 010000000) flags |= B1NIX_O_PATH;     /* Linux 0x200000 (shared) */
  if (lf & 02000000) flags |= B1NIX_O_CLOEXEC;   /* Linux 0x80000 -> 0x800 */
  return flags;
}

/* mknod(2): the kernel side of mkfifo(3). Only S_IFIFO and S_IFREG are
 * creatable — see vfs_mknod for why. */
static isize sys_mknod(const char *user_path, u32 mode, u64 dev) {
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

  return vfs_mknod(resolved, mode, dev);
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
  char resolved[VFS_MAX_PATH];

  /*
   * The path-less form IS futimens(3).
   *
   * utimensat(fd, NULL, ...) is how the C library implements futimens, so
   * refusing it with EINVAL refuses every futimens in every program. eudev's
   * udevd touches /run/udev/queue after each event that way; the refusal was
   * not fatal to it, so it retried -- forever, at the top of its event loop,
   * printing "could not touch /run/udev/queue: Invalid argument" and starving
   * everything else on the machine. A daemon spinning is what an unimplemented
   * syscall looks like from the outside.
   */
  if (!user_path) {
    if (vfs_fd_abspath(dirfd, resolved, sizeof(resolved)) < 0)
      return -EBADF;
  } else {
    if (syscall_copyinstr(kpath, sizeof(kpath), user_path) < 0)
      return -EFAULT;
    /* And a relative path against a directory descriptor, which is what the
     * *at() family exists for -- it used to be EBADF. */
    if (syscall_resolve_at(dirfd, kpath, resolved, sizeof(resolved)) < 0)
      return -EBADF;
  }

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

/* access(2) on a path already in kernel memory. The *at() shim resolves
 * dirfd-relative paths into a kernel buffer, so it needs this entry point —
 * handing that buffer to the user-pointer variant made every faccessat() call
 * fail with EFAULT. */
static isize sys_access_kpath(const char *kpath, int mode) {
  if ((mode & ~(R_OK | W_OK | X_OK)) != 0)
    return -EINVAL;

  char resolved[VFS_MAX_PATH];
  vfs_resolve_path(kpath, resolved);

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

static isize sys_access(const char *user_path, int mode) {
  char *kpath = kmalloc(VFS_MAX_PATH);
  if (!kpath)
    return -ENOMEM;
  if (strncpy_from_user(kpath, user_path, VFS_MAX_PATH) < 0) {
    kfree(kpath);
    return -EFAULT;
  }
  kpath[VFS_MAX_PATH - 1] = '\0';
  isize rc = sys_access_kpath(kpath, mode);
  kfree(kpath);
  return rc;
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

/* ── Wall time to scheduler ticks ───────────────────────────────────────────
 *
 * At the rate the timer was ACTUALLY programmed with, never a written-out
 * constant. Every conversion below used a hardcoded 100, and the LAPIC timer
 * has been armed at 1 kHz since it could be calibrated -- so every POSIX
 * timer, every alarm(2) and every setitimer(2) in the machine fired TEN TIMES
 * EARLY. `timeout 120 systemctl daemon-reload` killed systemctl after twelve
 * seconds and reported a timeout; `timeout 25 systemctl start` gave up after
 * two and a half. A working daemon is indistinguishable from a hung one when
 * the clock the test is held to runs ten times fast, so this was not one bug
 * but a wrong answer given to every question anything asked with a timeout.
 *
 * sched.h says this in as many words -- "written as a bare count it silently
 * became ten times shorter when the timer moved from 100 Hz to 1 kHz" -- and
 * SCHED_MS_TO_TICKS exists for it. These call sites predate it.
 *
 * Saturating, not wrapping: a deadline further out than the counter can hold
 * means "never", and wrapping turns never into now -- which is how a timerfd
 * armed for TIME_T_MAX once fired immediately.
 */
#define SC_TICKS_MAX ((u64)1 << 62)

static u64 sc_tick_hz(void) {
  u64 hz = SCHED_TICKS_PER_SEC;

  return hz ? hz : 100;
}

/* seconds + a fraction (nsec with frac_per_sec 1e9, usec with 1e6) to ticks,
 * rounded UP so a wait is never shorter than the caller asked for. */
static u64 sc_time_to_ticks(u64 sec, u64 frac, u64 frac_per_sec) {
  u64 hz = sc_tick_hz();

  if (sec > SC_TICKS_MAX / hz)
    return SC_TICKS_MAX;
  u64 t = sec * hz;
  u64 add = (frac * hz + (frac_per_sec - 1)) / frac_per_sec;
  if (t > SC_TICKS_MAX - add)
    return SC_TICKS_MAX;
  return t + add;
}

static void sc_ticks_to_time(u64 ticks, u64 *sec, u64 *frac,
                             u64 frac_per_sec) {
  u64 hz = sc_tick_hz();

  *sec = ticks / hz;
  *frac = ((ticks % hz) * frac_per_sec) / hz;
}

static u64 sys_alarm(unsigned int seconds) {
  if (!current_task)
    return 0;

  u64 current_ticks = scheduler_get_uptime_ticks();
  u64 old_alarm = task_alarm_ticks(current_task);
  u64 remaining = 0;
  if (old_alarm > 0) {
    if (old_alarm > current_ticks) {
      /* alarm(2) reports the remainder in whole seconds, rounded up. */
      u64 hz = sc_tick_hz();
      remaining = (old_alarm - current_ticks + hz - 1) / hz;
    } else {
      remaining = 0;
    }
  }

  if (seconds == 0) {
    task_set_alarm_ticks(current_task, 0);
  } else {
    task_set_alarm_ticks(current_task,
                         current_ticks +
                             sc_time_to_ticks((u64)seconds, 0, 1000000000ull));
  }

  return remaining;
}

/* M86: resolve one of the CPU-time clocks to nanoseconds.
 *
 * Positive ids are the two fixed clocks: CLOCK_PROCESS_CPUTIME_ID(2) is the
 * caller's whole thread group, CLOCK_THREAD_CPUTIME_ID(3) is the calling
 * thread. Negative ids are the dynamic clocks clock_getcpuclockid(3) and
 * pthread_getcpuclockid(3) return, in Linux's encoding:
 *
 *   clockid = (~pid_or_tid << 3) | (perthread ? 4 : 0) | which
 *   which: 0 = PROF (user+system), 1 = VIRT (user only), 2 = SCHED (user+system)
 *
 * so `~(clockid >> 3)` recovers the pid (arithmetic shift — the id is
 * negative), and the low three bits say what to report. Returns -1 for an id
 * that names no live task. */
static isize sys_cpu_clock_ns(int clk_id, u64 *out_ns) {
  u64 u = 0, s = 0;
  if (clk_id == 2) { /* CLOCK_PROCESS_CPUTIME_ID */
    task_group_cputime_ns(current_task, &u, &s);
    *out_ns = u + s;
    return 0;
  }
  if (clk_id == 3) { /* CLOCK_THREAD_CPUTIME_ID */
    *out_ns = task_utime_ns(current_task) + task_stime_ns(current_task);
    return 0;
  }
  if (clk_id >= 0)
    return -1;

  int which = clk_id & 3;
  int perthread = (clk_id & 4) != 0;
  usize id = (usize)(~(clk_id >> 3));
  if (id == 0)
    return -1;
  struct task *t = scheduler_task_by_pid(id);
  if (!t)
    return -1;
  if (perthread) {
    u = task_utime_ns(t);
    s = task_stime_ns(t);
  } else {
    task_group_cputime_ns(t, &u, &s);
  }
  *out_ns = (which == 1) ? u : (u + s); /* VIRT is user time only */
  return 0;
}

/* clock_getres(2). A tick-driven clock's honest resolution is one tick, and
 * the tick is whatever the timer was programmed with — reporting Linux's 1 ns
 * would be a lie a caller can act on (poll loops sized from the resolution),
 * and so would reporting a 10 ms tick on a kernel running at 1 kHz. The
 * CPU-time clocks are different: M86 accounts them from the TSC, so their
 * resolution really is nanoseconds. */
static isize sys_clock_getres(int clk_id, struct timespec *user_res) {
  int is_cpu_clock = (clk_id < 0 || clk_id == 2 || clk_id == 3);
  if (clk_id > 7)
    return -EINVAL;
  if (clk_id < 0) {
    /* A dynamic CPU clock must name a live task, exactly as clock_gettime
     * requires — otherwise the id is not a clock at all. */
    u64 probe = 0;
    if (sys_cpu_clock_ns(clk_id, &probe) < 0)
      return -EINVAL;
  }
  if (!user_res)
    return 0; /* Linux allows a NULL res: the call then only validates clk_id */
  struct timespec res;
  res.tv_sec = 0;
  /* Report what the clock actually does. A program that reads 10 ms here and
   * then measures in microseconds concludes its own measurements are noise —
   * and one told 1 ns by a clock that moves in 10 ms steps is misled the other
   * way. COARSE clocks stay on the tick and say so. */
  if (is_cpu_clock)
    res.tv_nsec = 1;
  else if (clk_id == 5 || clk_id == 6) /* the *_COARSE pair */
    res.tv_nsec = (i64)(1000000000ULL / sc_tick_hz());
  else
    res.tv_nsec = arch_tsc_clock_ready() ? 1
                                        : (i64)(1000000000ULL / sc_tick_hz());
  if (syscall_copyout(user_res, &res, sizeof(res)) != 0)
    return -EFAULT;
  return 0;
}

/* sigtimedwait(2): wait for one of the signals in `set` to become pending,
 * consume it WITHOUT running its handler, and return its number. This is how a
 * program that manages signals synchronously (openrc-init's shutdown path, any
 * signalfd-less daemon) reads them. Polls on the scheduler tick: signal
 * delivery already wakes a blocked task, and a tick-granular wait matches the
 * clock resolution the rest of the timing syscalls report.
 *
 * Returns the b1nix signal number, -EAGAIN on timeout, or -EINTR when a signal
 * OUTSIDE the set becomes deliverable (POSIX). */
static isize sys_sigtimedwait_kernel(u64 set, const struct timespec *user_ts) {
  if (!current_task)
    return -EFAULT;
  /* SIGKILL/SIGSTOP can never be waited for, exactly as they cannot be caught. */
  set &= ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
  if (!set)
    return -EINVAL;

  int has_timeout = 0;
  u64 deadline = 0;
  if (user_ts) {
    struct timespec ts;
    if (syscall_copyin(&ts, user_ts, sizeof(ts)) < 0)
      return -EFAULT;
    if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000)
      return -EINVAL;
    has_timeout = 1;
    deadline = scheduler_get_ticks() +
               sc_time_to_ticks((u64)ts.tv_sec, (u64)ts.tv_nsec, 1000000000ull);
  }

  for (;;) {
    u64 pending =
        __atomic_load_n(&current_task->pending_signals, __ATOMIC_ACQUIRE);
    u64 wanted = pending & set;
    if (wanted) {
      for (int sig = 1; sig < NSIG; sig++) {
        if (!(wanted & (1ULL << (sig - 1))))
          continue;
        __atomic_fetch_and(&current_task->pending_signals,
                           ~(1ULL << (sig - 1)), __ATOMIC_RELAXED);
        return sig;
      }
    }
    /* A deliverable signal that is NOT in the set aborts the wait so its
     * handler can run. */
    if (pending & ~current_task->blocked_signals & ~set)
      return -EINTR;
    if (has_timeout && scheduler_get_ticks() >= deadline)
      return -EAGAIN;
    scheduler_block_on_timeout(&current_task->pending_signals, 1);
  }
}

/* Native entry point: the set comes from userspace in b1nix numbering. */
static isize sys_sigtimedwait(const u64 *user_set,
                              const struct timespec *user_ts) {
  if (!user_set)
    return -EFAULT;
  u64 set;
  if (syscall_copyin(&set, user_set, sizeof(u64)) < 0)
    return -EFAULT;
  return sys_sigtimedwait_kernel(set, user_ts);
}


/* ── M95: init_module(2) / finit_module(2) / delete_module(2) ────────────────
 * Loading code into ring 0 is the most privileged operation there is, so all
 * three require CAP_SYS_MODULE (root has it through the full capability set).
 * A .ko is a few tens of KiB; the ceiling keeps a bogus length from asking the
 * heap for gigabytes (kmalloc panics on OOM in this kernel). */
#define MODULE_IMAGE_MAX (16ULL * 1024ULL * 1024ULL)
#define MODULE_PARAMS_MAX 256

static int module_caller_privileged(void) {
  struct cred *c = scheduler_get_current_cred();
  return c && (c->euid == ROOT_UID || cred_has_cap(c, CAP_SYS_MODULE));
}

/* Copy the (optional) NUL-terminated parameter string in from userspace. */
static int module_copy_params(const char *user_params, char *out, usize cap) {
  out[0] = '\0';
  if (!user_params)
    return 0;
  if (syscall_copyinstr(out, cap, user_params) < 0)
    return -EFAULT;
  return 0;
}

static isize sys_init_module(const void *user_image, u64 len,
                             const char *user_params) {
  if (!module_caller_privileged())
    return -EPERM;
  if (!user_image || len == 0 || len > MODULE_IMAGE_MAX)
    return -EINVAL;
  char params[MODULE_PARAMS_MAX];
  int rc = module_copy_params(user_params, params, sizeof(params));
  if (rc != 0)
    return rc;
  char *image = kmalloc((usize)len);
  if (!image)
    return -ENOMEM;
  if (syscall_copyin(image, user_image, (usize)len) < 0) {
    kfree(image);
    return -EFAULT;
  }
  rc = module_load_image(image, (usize)len, params);
  kfree(image);
  return rc;
}

static isize sys_finit_module(int fd, const char *user_params, u32 flags) {
  (void)flags;
  if (!module_caller_privileged())
    return -EPERM;
  char params[MODULE_PARAMS_MAX];
  int rc = module_copy_params(user_params, params, sizeof(params));
  if (rc != 0)
    return rc;

  struct b1nix_stat st;
  if (vfs_fstat(fd, &st) != 0)
    return -EBADF;
  if (st.st_size == 0 || (u64)st.st_size > MODULE_IMAGE_MAX)
    return -EINVAL;
  usize size = (usize)st.st_size;
  char *image = kmalloc(size);
  if (!image)
    return -ENOMEM;
  usize got = 0;
  while (got < size) {
    isize n = vfs_pread(fd, image + got, size - got, (u64)got);
    if (n < 0) {
      kfree(image);
      return -EIO;
    }
    if (n == 0)
      break;
    got += (usize)n;
  }
  if (got != size) {
    kfree(image);
    return -EIO;
  }
  rc = module_load_image(image, size, params);
  kfree(image);
  return rc;
}

static isize sys_delete_module(const char *user_name, u32 flags) {
  if (!module_caller_privileged())
    return -EPERM;
  char name[MODULE_NAME_MAX];
  if (!user_name)
    return -EFAULT;
  if (syscall_copyinstr(name, sizeof(name), user_name) < 0)
    return -EFAULT;
  return module_unload(name, flags);
}

/* Suspend until a deliverable signal arrives, running with `mask` blocked.
 * Split out of sys_sigsuspend so callers that already hold the mask in kernel
 * memory (Linux pause(2), which suspends with the CURRENT mask and has no user
 * buffer to read one from) can use it directly. */
static u64 sigsuspend_with_mask(u64 mask) {
  if (!current_task)
    return (u64)-EINVAL;

  task_set_saved_sigmask(current_task, current_task->blocked_signals, 1);

  u64 new_mask = mask & ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
  current_task->blocked_signals = new_mask;

  u64 flags = interrupts_save();
  while (1) {
    u64 pending = __atomic_load_n(&current_task->pending_signals, __ATOMIC_ACQUIRE) & ~current_task->blocked_signals;
    int has_deliverable = 0;
    for (int i = 1; i < NSIG; i++) {
      if (pending & (1ULL << (i - 1))) {
        sighandler_t handler = current_task->sigactions[i - 1].sa_handler;
        if (handler == SIG_IGN || (handler == SIG_DFL && (i == SIGCHLD || i == SIGURG || i == SIGWINCH || (i == SIGCONT && current_task->state != TASK_STOPPED)))) {
          __atomic_fetch_and(&current_task->pending_signals, ~(1ULL << (i - 1)), __ATOMIC_RELAXED);
        } else if (handler == SIG_DFL &&
                   (i == SIGSTOP || i == SIGTSTP ||
                    i == SIGTTIN || i == SIGTTOU) &&
                   ptrace_is_traced(current_task)) {
          /* A traced task must stop through ptrace, not here: stopping it in
           * the middle of sigsuspend/pause leaves its tracer with a task in
           * TASK_STOPPED that ptrace never parked, so every PTRACE_GETREGS
           * against it fails with ESRCH. Leave the signal pending and end the
           * wait — the syscall-return path (arch_check_and_deliver_signals)
           * parks it with a complete ring-3 register frame. */
          has_deliverable = 1;
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
    /* No channel: this wait ends on a signal, and the previous wait's channel
     * must not be left behind to catch a wake meant for it. */
    current_task->wait_chan = 0;
    current_task->state = TASK_BLOCKED;
    scheduler_yield();
  }
  interrupts_restore(flags);

  /* The temporary mask stays installed. It is the return to ring 3 that puts
   * the original back — either through the signal frame, once a handler has
   * been entered with the wait mask in force, or through the restore in
   * arch_check_and_deliver_signals when nothing was delivered. Undoing it here
   * re-blocked the very signal that ended the wait, so the handler never ran
   * and the caller looped on sigsuspend forever. */
  return (u64)-EINTR;
}

/* Fault in every page of [start, end) in the calling task's address space and
 * confirm it is present afterwards. Used by mlock/mlockall so the call only
 * succeeds once the memory really is resident — a lock record on a page that
 * was never populated would be a promise the kernel could not keep. Returns 0
 * when the whole range is resident, -1 if any page could not be populated
 * (unmapped hole, PROT_NONE reservation, or out of memory). */
static int mlock_populate(u64 start, u64 end) {
  if (!current_task || !current_task->pml4_phys)
    return -1;
  for (u64 va = start; va < end; va += PAGE_SIZE) {
    if (paging_user_frame(current_task->pml4_phys, va))
      continue;
    /* Not-present, user-mode read fault — the demand pager's own entry point
     * (anonymous zero-fill, file-backed page-in, or swap-in). */
    if (vmm_handle_page_fault(va, PF_USER) != 0)
      return -1;
    if (!paging_user_frame(current_task->pml4_phys, va))
      return -1;
  }
  return 0;
}

/* Install a wait mask for the duration of one blocking call.
 *
 * epoll_pwait, ppoll and pselect exist for one reason: a program that keeps a
 * signal blocked everywhere else wants it deliverable while — and only while —
 * it is parked in the wait. b1nix dropped the mask argument on the floor and
 * ran the plain wait instead, so for any program using that idiom the signal
 * was never deliverable at all. A terminal emulator blocking SIGTERM and
 * unblocking it across its epoll could not be asked to quit: SIGTERM sat
 * pending while the process ran on, and only SIGKILL ended it.
 *
 * The original mask comes back the same way sigsuspend's does — through the
 * signal frame once a handler is entered, or through the return to ring 3 when
 * nothing was delivered. Restoring it here would re-block the signal before it
 * could be acted on, which is the whole defect over again.
 *
 * mask is in Linux numbering, as every caller of this path is. */
static void syscall_wait_mask_install(u64 lx_mask) {
  if (!current_task)
    return;
  /* Never overwrite a mask already put aside. A wait interrupted by a signal
   * comes back through this same entry point when the call is restarted, and a
   * second save would store the temporary mask as if it were the caller's own
   * — the real one would then never come back and the process would run for
   * the rest of its life with everything but the wait signal blocked. */
  if (task_has_saved_sigmask(current_task))
    return;
  u64 b_mask = linux_sigset_to_b1nix(lx_mask);
  task_set_saved_sigmask(current_task, current_task->blocked_signals, 1);
  current_task->blocked_signals =
      b_mask & ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
}

static u64 sys_sigsuspend(const u64 *user_mask) {
  if (!current_task)
    return (u64)-EINVAL;
  if (!user_mask)
    return (u64)-EFAULT;

  u64 mask;
  if (syscall_copyin(&mask, user_mask, sizeof(u64)) < 0)
    return (u64)-EFAULT;

  return sigsuspend_with_mask(mask);
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
  /* source and type are both allowed to be NULL: a propagation change
   * (mount(NULL, "/", NULL, MS_REC|MS_SHARED, NULL) — the first thing systemd
   * does as PID 1) and a remount name neither. Copying from a NULL pointer
   * reported EFAULT, which systemd treats as fatal for the propagation call. */
  if (!user_src)
    ksrc[0] = '\0';
  else if (strncpy_from_user(ksrc, user_src, VFS_MAX_PATH) < 0) {
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

  /* The operations that name no filesystem, in the order Linux checks them.
   * Each of these takes a target that already exists and changes something
   * about it, so none of them reaches the filesystem type at all.
   *
   * The order is Linux's do_mount() and it is not a matter of taste: REMOUNT is
   * tested BEFORE BIND, because `MS_BIND|MS_REMOUNT|MS_RDONLY` is how every
   * caller turns an existing bind read-only -- it is what systemd's
   * ProtectHostname=, ProtectKernelTunables= and ReadOnlyPaths= all come down
   * to. Tested the other way round it was taken for a fresh bind of a NULL
   * source, which resolves to `/`, and binding a directory onto
   * /proc/sys/kernel/domainname answered ENOTDIR -- which systemd reports as
   * "Failed to set up mount namespacing" and treats as fatal, so
   * systemd-udevd never started. */
  int done = 1, dres = 0;
  const char *dop = "";
  if (flags & MS_REMOUNT) {
    dop = "remount";
    dres = vfs_remount(ktarget, flags);
  } else if (flags & MS_BIND) {
    dop = "bind";
    dres = vfs_bind_mount(ksrc, ktarget, flags);
  } else if (flags & MS_PROPAGATION_MASK) {
    dop = "propagation";
    dres = vfs_set_propagation(ktarget, flags);
  } else if (flags & MS_MOVE) {
    /* switch_root calls mount(".", "/", NULL, MS_MOVE, NULL); the "." is
     * relative to the caller's cwd, so both go through the resolver. */
    dop = "move";
    dres = vfs_move_mount(ksrc, ktarget);
  } else {
    done = 0;
  }
  if (done) {
    if (bootinfo_has_flag("b1nix.trace-mount")) {
      char line[320];
      snprintf(line, sizeof(line), "mount[%s]: '%s' -> '%s' flags=0x%llx = %d",
               dop, ksrc, ktarget, (unsigned long long)flags, dres);
      klog_info(line);
    }
    kfree(ksrc);
    kfree(ktarget);
    return (isize)dres;
  }

  char *ktype = kmalloc(64);
  if (!ktype) {
    kfree(ksrc);
    kfree(ktarget);
    return -ENOMEM;
  }
  if (!user_type)
    ktype[0] = '\0';
  else if (strncpy_from_user(ktype, user_type, 64) < 0) {
    kfree(ksrc);
    kfree(ktarget);
    kfree(ktype);
    return -EFAULT;
  }

  int res = vfs_mount(ksrc, ktarget, ktype, flags);
  /* Which mounts an init system actually asks for, and what it got. A mount
   * that fails non-fatally (systemd tries three variants of cgroup2 before
   * falling back) is invisible from the guest side. */
  if (bootinfo_has_flag("b1nix.trace-mount")) {
    char line[256];
    snprintf(line, sizeof(line), "mount: '%s' -> '%s' type='%s' flags=0x%llx = %d",
             ksrc, ktarget, ktype, (unsigned long long)flags, res);
    klog_info(line);
  }
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

/*
 * readahead(fd, offset, count) -- pull a file's contents into the cache before
 * anything asks for them.
 *
 * It is real work rather than an accepted hint: the pages are read through the
 * ordinary VFS path, so they land in the same page/block cache a later read
 * will hit, and a caller that asks for a megabyte pays for a megabyte here
 * instead of paying for it a block at a time later. Returning 0 without
 * reading would be indistinguishable to the caller and a lie to anyone timing
 * it, so this reads.
 *
 * Linux semantics kept: EBADF for a bad descriptor, EINVAL for a descriptor
 * that cannot be read this way, and 0 on success. A short read at end-of-file
 * is success -- there was simply less to warm than asked for.
 */
static isize sys_readahead(int fd, u64 offset, usize count) {
  if (fd < 0)
    return -EBADF;
  if (count == 0)
    return 0;

  /* One block-sized staging buffer, reused: the point is to populate the
   * cache, and the bytes themselves are thrown away. 64 KiB keeps the number
   * of VFS round trips low without asking the heap for anything awkward. */
  enum { RA_CHUNK = 65536 };
  char *buf = kmalloc(RA_CHUNK);
  if (!buf)
    return -ENOMEM;

  usize done = 0;
  isize rc = 0;
  while (done < count) {
    usize want = count - done;
    if (want > RA_CHUNK)
      want = RA_CHUNK;
    isize got = vfs_pread(fd, buf, want, offset + done);
    if (got < 0) {
      rc = got;
      break;
    }
    if (got == 0) /* end of file: nothing left to warm, and that is not an error */
      break;
    done += (usize)got;
  }
  kfree(buf);
  return rc < 0 ? rc : 0;
}

static isize sys_pivot_root(const char *user_new, const char *user_old) {
  /* Replacing the root of every process is CAP_SYS_ADMIN territory, and Linux
   * gates it there too. */
  struct cred *c = scheduler_get_current_cred();
  if (c && c->euid != ROOT_UID && !cred_has_cap(c, CAP_SYS_ADMIN))
    return -EPERM;

  char *knew = kmalloc(VFS_MAX_PATH);
  char *kold = kmalloc(VFS_MAX_PATH);
  if (!knew || !kold) {
    kfree(knew);
    kfree(kold);
    return -ENOMEM;
  }
  if (strncpy_from_user(knew, user_new, VFS_MAX_PATH) < 0 ||
      strncpy_from_user(kold, user_old, VFS_MAX_PATH) < 0) {
    kfree(knew);
    kfree(kold);
    return -EFAULT;
  }
  knew[VFS_MAX_PATH - 1] = '\0';
  kold[VFS_MAX_PATH - 1] = '\0';

  int res = vfs_pivot_root(knew, kold);
  kfree(knew);
  kfree(kold);
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
/* System node/domain name. sethostname(2)/setdomainname(2) update these, and
 * uname(2), /proc/sys/kernel/{hostname,domainname} and /sys/kernel/hostname all
 * read them, so a hostname set at boot (OpenRC's `hostname` service) is visible
 * everywhere a Linux program looks for it.
 *
 * M109: the pair lives in the caller's UTS namespace
 * (kernel/sched/namespace.c). Because every reader in the tree comes through
 * these two functions, routing them through the namespace is what makes
 * `unshare -u` change the name for the caller and for nobody else. */
void kernel_hostname_get(char *buf, usize len) {
  namespace_uts_get_host(buf, len);
}

void kernel_domainname_get(char *buf, usize len) {
  namespace_uts_get_domain(buf, len);
}

int kernel_hostname_set(const char *name) {
  return namespace_uts_set_host(name);
}

int kernel_domainname_set(const char *name) {
  return namespace_uts_set_domain(name);
}

/* ── M109: PID-namespace translation at the syscall boundary ──────────────
 *
 * Inside the kernel a task has exactly one id. A PID namespace is a view of
 * those ids, so the translation belongs here, where a number crosses between
 * the kernel and a task that may be looking at a private numbering — not in
 * the scheduler, which would then have to carry a namespace with every id.
 *
 * With no namespace in play both helpers return their argument, so the normal
 * path costs one predictable branch (namespace_active() is a plain load). */

/* A pid arriving FROM userspace. Returns 0 for "no task of that number in the
 * caller's namespace", which every caller turns into ESRCH. */
static usize ns_pid_in(u64 user_pid) {
  return namespace_pid_from_user((usize)user_pid);
}

/* A pid on its way OUT to userspace. Errors (<= 0) pass through untouched. */
static u64 ns_pid_out(u64 kernel_pid) {
  if ((isize)kernel_pid <= 0)
    return kernel_pid;
  usize v = namespace_pid_to_user((usize)kernel_pid);
  /* A task the caller cannot name should never reach here — it can only be a
   * descendant, and every descendant is numbered in every ancestor namespace.
   * Report 0 rather than leaking the kernel's own id if it ever does. */
  return (u64)v;
}

/* The CLONE_NEW* flags b1nix has namespaces for. clone(2) may ask for them at
 * the same time as it makes the child, and until now it did not get them:
 * scheduler_fork_clone inherits the parent's namespaces and the flags were
 * dropped on the floor.
 *
 * Dropping them is not a missing feature, it is a wrong answer. A caller that
 * asked for a private mount namespace and was given the shared one goes on to
 * remount things "for itself" -- and every one of those mounts is everybody's.
 * systemd forks its generators with CLONE_NEWNS and then remounts the root
 * read-only inside what it believes is its own namespace: the root really went
 * read-only, PID 1's own log descriptor started answering EROFS, and the boot
 * went silent from that point on with no error anywhere to say why. */
#define CLONE_NS_FLAGS                                                         \
  (B1NIX_CLONE_NEWNS | B1NIX_CLONE_NEWUTS | B1NIX_CLONE_NEWNET)

/* Prepare, in the parent, the namespaces a clone(CLONE_NEW*) asks for.
 *
 * It must happen here and not in the child: a forked child does not return
 * through this C code at all -- it resumes at x86_fork_child_trampoline and
 * goes straight back to ring 3 -- and even if it did, it is runnable the
 * instant the fork returns, so anything done to it afterwards can be too late.
 *
 * CLONE_NEWPID is refused rather than ignored. On clone it means the child is
 * pid 1 of a fresh numbering, which is not what this kernel's unshare-shaped
 * machinery does, and a process that believes it got a private pid namespace
 * and did not is worse off than one told plainly that it cannot have one. */
static int clone_prepare_namespaces(u64 flags) {
  /* The namespaces this kernel does not have, refused rather than ignored --
   * the same answer unshare(2) already gives, so the two calls agree about
   * what exists.
   *
   * Ignoring CLONE_NEWUSER was not a harmless omission. systemd probes for
   * id-mapped mounts by cloning into a user namespace and then writing the
   * child's /proc/<pid>/uid_map; with the flag dropped it got an ordinary
   * child, and the write failed with ENOENT because there is no such file.
   * That ENOENT is fatal to the unit -- "Failed to set up special execution
   * directory in /run" -- so journald could not start. Told plainly that user
   * namespaces do not exist, systemd records "not supported" and carries on. */
  if (flags & (B1NIX_CLONE_NEWUSER | B1NIX_CLONE_NEWIPC |
               B1NIX_CLONE_NEWCGROUP | B1NIX_CLONE_NEWPID))
    return -EINVAL;
  if (!(flags & CLONE_NS_FLAGS))
    return 0;
  return namespace_child_prepare(flags);
}

/* ── M109: unshare(2) / setns(2) ────────────────────────────────────────── */
static isize sys_unshare(u64 flags) {
  struct cred *c = scheduler_get_current_cred();
  if (!c || !cred_has_cap(c, CAP_SYS_ADMIN))
    return -EPERM;
  return namespace_unshare(flags);
}

static isize sys_setns(int fd, int nstype) {
  struct cred *c = scheduler_get_current_cred();
  if (!c || !cred_has_cap(c, CAP_SYS_ADMIN))
    return -EPERM;

  /* The descriptor is a /proc/<pid>/ns/<kind> handle, which is what nsenter(1)
   * opens; it carries the namespace it named at open() time. */
  u32 pin = 0;
  int rc = vfs_fd_ns_pin(fd, &pin);
  if (rc != 0)
    return rc;
  int kind = VFS_NS_PIN_KIND(pin);
  u32 id = VFS_NS_PIN_ID(pin);

  /* A non-zero nstype is the caller telling us what it believes the handle is;
   * disagreeing with it is an error, not something to paper over. */
  if (nstype != 0) {
    int want = -1;
    switch ((unsigned)nstype) {
    case B1NIX_CLONE_NEWUTS: want = NS_UTS; break;
    case B1NIX_CLONE_NEWNS:  want = NS_MNT; break;
    case B1NIX_CLONE_NEWPID: want = NS_PID; break;
    case B1NIX_CLONE_NEWNET: want = NS_NET; break;
    default: return -EINVAL;
    }
    if (want != kind)
      return -EINVAL;
  }
  return namespace_setns(kind, id);
}

static void fill_b1nix_utsname(struct b1nix_utsname *uts) {
  memset(uts, 0, sizeof(*uts));
  copy_cstr(uts->sysname, sizeof(uts->sysname), "B1NIX");
  kernel_hostname_get(uts->nodename, sizeof(uts->nodename));
  copy_cstr(uts->release, sizeof(uts->release), B1NIX_RELEASE_STR);
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
  /* A process running under the Linux personality is told it is on Linux, and
   * told a Linux kernel version.
   *
   * This is not decoration. glibc's ld.so refuses to start with "FATAL: kernel
   * too old" unless the release string parses as at least the version its
   * NT_GNU_ABI_TAG note asks for (3.2.0 for every Debian binary), and a
   * distribution's shell scripts branch on `uname -s`. b1nix's own version is
   * kept in the string rather than hidden, so a boot log still identifies the
   * kernel that is actually running. Native-personality callers keep seeing
   * "B1NIX" and the bare version. */
  copy_cstr(lx.sysname, sizeof(lx.sysname), "Linux");
  copy_cstr(lx.release, sizeof(lx.release),
            B1NIX_RELEASE_STR);
  copy_cstr(lx.version, sizeof(lx.version), "#1 SMP b1nix");
  kernel_domainname_get(lx.domainname, sizeof(lx.domainname));
  if (copy_to_user((void *)(usize)arg0, &lx, sizeof(lx)) < 0)
    return -EFAULT;
  return 0;
}

/* The caller's user registers, for a clone(2) child that must resume with them
 * (see struct clone_user_regs). */
static void clone_regs_from_frame(struct clone_user_regs *r,
                                  const struct interrupt_frame *f) {
  memset(r, 0, sizeof(*r));
  if (!f)
    return;
  r->rbx = f->rbx;
  r->rcx = f->rcx;
  r->rdx = f->rdx;
  r->rsi = f->rsi;
  r->rdi = f->rdi;
  r->rbp = f->rbp;
  r->r8 = f->r8;
  r->r9 = f->r9;
  r->r10 = f->r10;
  r->r11 = f->r11;
  r->r12 = f->r12;
  r->r13 = f->r13;
  r->r14 = f->r14;
  r->r15 = f->r15;
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
/* getdents(2) (Linux nr 78) — the pre-64-bit record:
 *   { u64 d_ino; i64 d_off; u16 d_reclen; char d_name[]; '\0'; u8 d_type; }
 * with d_type living in the LAST byte of the record rather than in a header
 * field. Same directory walk as getdents64, different packing; `legacy`
 * selects it. */
static isize sys_linux_getdents_common(int fd, u64 user_buf, usize count,
                                       int legacy);

static isize sys_linux_getdents64(int fd, u64 user_buf, usize count) {
  return sys_linux_getdents_common(fd, user_buf, count, 0);
}

/* getdents(2)/getdents64(2).
 *
 * The directory cursor is OPAQUE: a filesystem is free to make it a byte
 * position (ext2 does, because an unlink renumbers nothing that way), so this
 * shim must never compute one. It used to read a batch of 32 and then set the
 * cursor to `start + emitted` — index arithmetic that both destroyed the
 * filesystem's cookie and reintroduced positional semantics, which is why a
 * `rm -rf` of a directory larger than one batch skipped an entry per deletion
 * and left the directory un-removable.
 *
 * So: fetch ONE entry at a time and let the handle's own cursor advance. An
 * entry that does not fit is pushed back by seeking to the cursor saved before
 * it was read, and each record's d_off is the cursor AFTER that entry — a real
 * per-entry cookie, which is what seekdir(3) is supposed to receive. The cost
 * is one VFS call per entry instead of per batch; for ext2 that is a block-cache
 * hit, and correctness here outranks the saving. */
static isize sys_linux_getdents_common(int fd, u64 user_buf, usize count,
                                       int legacy) {
  isize start = vfs_lseek(fd, 0, B1NIX_SEEK_CUR);
  if (start < 0)
    return start;

  usize written = 0;
  isize emitted = 0;
  for (;;) {
    isize before = vfs_lseek(fd, 0, B1NIX_SEEK_CUR);
    if (before < 0)
      break;
    struct dirent kbuf[1];
    isize n = vfs_getdents(fd, kbuf, 1);
    if (n < 0)
      return emitted ? (isize)written : n;
    if (n == 0)
      break; /* end of directory */
    isize after = vfs_lseek(fd, 0, B1NIX_SEEK_CUR);
    isize i = 0;
    usize namelen = 0;
    while (namelen < sizeof(kbuf[i].name) && kbuf[i].name[namelen])
      namelen++;
    /* getdents64: d_name starts at byte 19. getdents: d_name starts at byte
     * 18 and the type byte is appended after the name's NUL. Both records are
     * padded to an 8-byte multiple. */
    usize reclen = legacy ? ((18 + namelen + 2 + 7) & ~(usize)7)
                          : ((19 + namelen + 1 + 7) & ~(usize)7);
    if (written + reclen > count) {
      /* No room: push this entry back so the next call re-reads it. */
      vfs_lseek(fd, before, B1NIX_SEEK_SET);
      break;
    }

    char rec[19 + sizeof(kbuf[0].name) + 2 + 7];
    for (usize z = 0; z < reclen; z++)
      rec[z] = 0;
    /* d_ino: the filesystem's inode number when it has one, otherwise the
     * entry's index — never 0, which some tools read as "deleted". */
    u64 d_ino = kbuf[i].ino ? kbuf[i].ino : (u64)(after);
    if (legacy) {
      *(u64 *)&rec[0] = d_ino;                 /* d_ino  */
      *(i64 *)&rec[8] = (i64)after;            /* d_off: cursor after this entry */
      *(u16 *)&rec[16] = (u16)reclen;          /* d_reclen */
      for (usize z = 0; z < namelen; z++)
        rec[18 + z] = kbuf[i].name[z];
      rec[18 + namelen] = '\0';
      rec[reclen - 1] = (char)lx_dirent_type(&kbuf[i]);
    } else {
      struct linux_dirent64 *de = (struct linux_dirent64 *)rec;
      de->d_ino = d_ino;
      de->d_off = (i64)after; /* opaque cookie: the cursor after this entry */
      de->d_reclen = (u16)reclen;
      de->d_type = lx_dirent_type(&kbuf[i]);
      for (usize z = 0; z < namelen; z++)
        de->d_name[z] = kbuf[i].name[z];
      de->d_name[namelen] = '\0';
    }

    if (copy_to_user((void *)(usize)(user_buf + written), rec, reclen) < 0)
      return -EFAULT;
    written += reclen;
    emitted++;
  }

  if (emitted == 0) {
    /* Either the directory ended (cursor did not move) or the caller's buffer
     * cannot hold even one record. */
    isize now = vfs_lseek(fd, 0, B1NIX_SEEK_CUR);
    return (now == start) ? 0 : -EINVAL;
  }
  return (isize)written;
}

/* True when the caller is not privileged — small helper for the calls that
 * gate one option on root rather than the whole syscall. */
static int c_euid_not_root(void) {
  struct cred *c = scheduler_get_current_cred();
  return !(c && (c->euid == ROOT_UID || cred_has_cap(c, CAP_SYS_ADMIN)));
}

/* ── name_to_handle_at(2) / open_by_handle_at(2) ─────────────────────────────
 * A file handle is an opaque token a program can store and later re-open
 * without keeping a descriptor. b1nix filesystems have no inode-to-path map,
 * so the kernel remembers the resolved path AND the inode it named, and puts
 * the table index in the handle. The inode is what makes the handle honest: if
 * the path now names a different file — it was replaced, or the slot was
 * reused — open_by_handle_at reports ESTALE instead of quietly opening the
 * wrong file. Handles are valid for the lifetime of the boot; Linux only
 * promises validity while the filesystem stays mounted. */
#define FILE_HANDLE_MAX 128
#define FILE_HANDLE_TYPE 0x62316e78 /* 'b1nx' */

struct file_handle_slot {
  char path[VFS_MAX_PATH];
  u64 ino;
  u32 generation;
  int used;
};

static struct file_handle_slot g_file_handles[FILE_HANDLE_MAX];
static spinlock_t g_file_handle_lock = SPINLOCK_INIT;

/* struct file_handle { u32 handle_bytes; int handle_type; u8 f_handle[]; } */
static isize sys_linux_name_to_handle_at(int dirfd, const char *user_path,
                                         u64 user_handle, u64 user_mount_id,
                                         int flags) {
  if (!user_handle)
    return -EFAULT;
  char kpath[VFS_MAX_PATH], resolved[VFS_MAX_PATH];
  if (syscall_copyinstr(kpath, sizeof(kpath), user_path) < 0)
    return -EFAULT;
  if (kpath[0] != '/' && dirfd != AT_FDCWD) {
    char dirbuf[VFS_MAX_PATH], joined[VFS_MAX_PATH];
    int rc = vfs_fd_abspath(dirfd, dirbuf, sizeof(dirbuf));
    if (rc < 0)
      return rc;
    usize dlen = (usize)rc;
    if (dlen + 1 + strlen(kpath) >= sizeof(joined))
      return -ENAMETOOLONG;
    memcpy(joined, dirbuf, dlen);
    joined[dlen] = '/';
    strcpy(joined + dlen + 1, kpath);
    memcpy(kpath, joined, sizeof(kpath));
  }
  vfs_resolve_path(kpath, resolved);

  struct b1nix_stat st;
  int sr = (flags & 0x400 /* AT_SYMLINK_FOLLOW */) ? vfs_stat(resolved, &st)
                                                   : vfs_lstat(resolved, &st);
  if (sr < 0)
    return sr;

  u32 handle_bytes = 0;
  if (syscall_copyin(&handle_bytes, (const void *)(usize)user_handle,
                     sizeof(handle_bytes)) < 0)
    return -EFAULT;
  /* The payload is one 32-bit table index. Linux's contract: too small a
   * buffer reports EOVERFLOW after writing the required size. */
  if (handle_bytes < sizeof(u32)) {
    u32 need = sizeof(u32);
    if (syscall_copyout((void *)(usize)user_handle, &need, sizeof(need)) < 0)
      return -EFAULT;
    return -EOVERFLOW;
  }

  /* The generation distinguishes this file from a later one that reuses the
   * inode number — vfs_stat has no field for it, so read it off the node. */
  u32 generation = 0;
  {
    struct vfs_node *gn = vfs_find_node(resolved);
    if (gn && !IS_ERR(gn)) {
      generation = gn->inode->generation;
      vfs_node_put(gn);
    }
  }

  u64 flags_irq;
  spin_lock_irqsave(&g_file_handle_lock, &flags_irq);
  int slot = -1;
  for (int i = 0; i < FILE_HANDLE_MAX; i++) {
    if (g_file_handles[i].used && g_file_handles[i].ino == st.st_ino &&
        g_file_handles[i].generation == generation &&
        strcmp(g_file_handles[i].path, resolved) == 0) {
      slot = i; /* same file: hand back the same handle */
      break;
    }
  }
  if (slot < 0) {
    for (int i = 0; i < FILE_HANDLE_MAX; i++) {
      if (g_file_handles[i].used)
        continue;
      strncpy(g_file_handles[i].path, resolved, VFS_MAX_PATH - 1);
      g_file_handles[i].path[VFS_MAX_PATH - 1] = '\0';
      g_file_handles[i].ino = st.st_ino;
      g_file_handles[i].generation = generation;
      g_file_handles[i].used = 1;
      slot = i;
      break;
    }
  }
  spin_unlock_irqrestore(&g_file_handle_lock, flags_irq);
  if (slot < 0)
    return -ENOSPC;

  struct {
    u32 handle_bytes;
    i32 handle_type;
    u32 payload;
  } out = {sizeof(u32), FILE_HANDLE_TYPE, (u32)slot};
  if (syscall_copyout((void *)(usize)user_handle, &out, sizeof(out)) < 0)
    return -EFAULT;
  if (user_mount_id) {
    /* The real id of the mount this path lives on, which is what mountinfo's
     * first field carries: a caller asks for it in order to find that row. */
    i32 mount_id = (i32)vfs_mount_id_for_path(resolved);
    if (syscall_copyout((void *)(usize)user_mount_id, &mount_id,
                        sizeof(mount_id)) < 0)
      return -EFAULT;
  }
  return 0;
}

static isize sys_linux_open_by_handle_at(int mount_fd, u64 user_handle,
                                         int flags) {
  (void)mount_fd; /* single mount namespace: any descriptor identifies it */
  if (!user_handle)
    return -EFAULT;
  struct {
    u32 handle_bytes;
    i32 handle_type;
    u32 payload;
  } in;
  if (syscall_copyin(&in, (const void *)(usize)user_handle, sizeof(in)) < 0)
    return -EFAULT;
  if (in.handle_type != FILE_HANDLE_TYPE || in.handle_bytes < sizeof(u32))
    return -ESTALE;

  char path[VFS_MAX_PATH];
  u64 want_ino;
  u32 want_gen;
  u64 flags_irq;
  spin_lock_irqsave(&g_file_handle_lock, &flags_irq);
  if (in.payload >= FILE_HANDLE_MAX || !g_file_handles[in.payload].used) {
    spin_unlock_irqrestore(&g_file_handle_lock, flags_irq);
    return -ESTALE;
  }
  strncpy(path, g_file_handles[in.payload].path, VFS_MAX_PATH - 1);
  path[VFS_MAX_PATH - 1] = '\0';
  want_ino = g_file_handles[in.payload].ino;
  want_gen = g_file_handles[in.payload].generation;
  spin_unlock_irqrestore(&g_file_handle_lock, flags_irq);

  /* The handle names a FILE, not a path: if the path now resolves to a
   * different inode the file the caller asked for is gone. */
  struct b1nix_stat st;
  if (vfs_stat(path, &st) < 0)
    return -ESTALE;
  if (want_ino && st.st_ino != want_ino)
    return -ESTALE;
  {
    /* Same number, different file: the generation catches inode reuse. */
    struct vfs_node *gn = vfs_find_node(path);
    u32 gen = 0;
    if (gn && !IS_ERR(gn)) {
      gen = gn->inode->generation;
      vfs_node_put(gn);
    }
    if (gen != want_gen)
      return -ESTALE;
  }

  return vfs_open_flags(path, linux_open_flags_to_b1nix(flags));
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

#define POLL_STACK_FDS 64

/* poll(2) and its high-resolution siblings.
 *
 * ppoll and pselect are given a struct timespec and were served by rounding it
 * to whole milliseconds, so every sub-millisecond wait a program asked for
 * became either zero — a poll loop that spins — or a full millisecond. The
 * deadline is kept in nanoseconds on the same counter clock_gettime answers
 * from; only the sleep between scans is expressed in ticks, and it is never
 * shorter than the wait that remains. */
static u64 sys_poll_ns(struct b1nix_pollfd *user_fds, u64 nfds,
                       u64 timeout_ns, int infinite) {
  /* As many descriptors as the caller actually passed.
   *
   * The array used to be a fixed 64 on the kernel stack and anything past it
   * was dropped — silently, without EINVAL: those descriptors were never
   * polled and their revents never written back, so an event loop watching
   * more than 64 things simply never heard about the rest and waited forever.
   * An event loop with hundreds of descriptors is ordinary (a browser's is),
   * so the common case stays on the stack and a larger set is allocated. */
  struct b1nix_pollfd stack_fds[POLL_STACK_FDS];
  struct b1nix_pollfd *fds = stack_fds;
  struct b1nix_pollfd *heap_fds = 0;

  if (nfds > sched_fd_limit())
    return -EINVAL; /* more than a process can even have open */
  if (nfds > POLL_STACK_FDS) {
    heap_fds = kmalloc(nfds * sizeof(struct b1nix_pollfd));
    if (!heap_fds)
      return -ENOMEM;
    fds = heap_fds;
  }
  if (syscall_copyin(fds, user_fds, nfds * sizeof(struct b1nix_pollfd)) < 0) {
    kfree(heap_fds);
    return -EFAULT;
  }

  u64 tick_ns = 1000000000ull / (u64)sched_tick_hz();
  u64 deadline_ns = infinite ? 0 : arch_tsc_monotonic_ns() + timeout_ns;

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
    if (ready == 0 && !infinite && timeout_ns != 0 &&
        arch_tsc_monotonic_ns() >= deadline_ns)
      timed_out = 1;

    if (ready > 0 || (!infinite && timeout_ns == 0) || timed_out) {
      scheduler_wait_cancel();
      current_task->wake_tick = 0;
      syscall_copyout(user_fds, fds, nfds * sizeof(struct b1nix_pollfd));
      kfree(heap_fds);
      return (u64)ready;
    }

    /* Interrupted by a caught signal? Abort with -ERESTARTSYS so the dispatch
     * tail returns EINTR (or restarts under SA_RESTART) and delivers the
     * handler. Checked with IRQs still disabled (from wait_prepare) so a signal
     * posted concurrently is not missed before we sleep. */
    if (select_poll_signal_pending()) {
      scheduler_wait_cancel();
      current_task->wake_tick = 0;
      kfree(heap_fds);
      return (u64)-ERESTARTSYS;
    }

    /* Re-arm the timer deadline EVERY iteration, after wait_prepare: an
     * explicit wake_all(vfs_poll_chan) (any fs/socket activity — the chan is
     * global) clears wake_tick when it promotes this task, so arming it only
     * once before the loop meant a spurious wake stripped the timeout and the
     * next sleep was unbounded — a poll(10ms) could then sleep tens of
     * seconds until unrelated traffic kicked the chan (netd's reactor wedge,
     * every socket() timing out with ETIMEDOUT meanwhile). */
    if (!infinite && timeout_ns != 0) {
      u64 now_ns = arch_tsc_monotonic_ns();
      u64 rest_ns = deadline_ns > now_ns ? deadline_ns - now_ns : 0;
      u64 ticks = (rest_ns + tick_ns - 1) / tick_ns;
      if (ticks == 0)
        ticks = 1;
      current_task->wake_tick = scheduler_get_uptime_ticks() + ticks;
    }

    scheduler_wait_commit();
  }
}

/* poll(2): the timeout is whole milliseconds, (u64)-1 meaning "no timeout". */
static u64 sys_poll(struct b1nix_pollfd *user_fds, u64 nfds, u64 timeout) {
  if (timeout == (u64)-1)
    return sys_poll_ns(user_fds, nfds, 0, 1);
  return sys_poll_ns(user_fds, nfds, timeout * 1000000ull, 0);
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
  if (len == 0) {
    /* See sys_write: an empty datagram is a message, not a no-op. */
    if (!vfs_socket_sends_empty_messages(fd))
      return 0;
    isize rc0 = vfs_socket_send(fd, "", 0, flags);
    return (u64)(rc0 < 0 ? rc0 : 0);
  }
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
  if (len == 0) {
    /* See sys_write: an empty datagram is a message, not a no-op. */
    if (!vfs_socket_sends_empty_messages(fd))
      return 0;
    isize rc0 = vfs_socket_sendto(fd, "", 0, flags,
                                  kaddrlen ? (const void *)kaddr : 0, kaddrlen);
    return (u64)(rc0 < 0 ? rc0 : 0);
  }
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
#define K_SCM_TIMESTAMP 29
#define K_SCM_TIMESTAMPNS 35
#define K_MSG_CTRUNC 0x08
#define K_CMSG_ALIGN(n) (((n) + sizeof(usize) - 1) & ~(sizeof(usize) - 1))

/* How many iovecs one sendmsg/recvmsg may carry.
 *
 * This is the size of the arrays the callers put on the KERNEL STACK, and the
 * bound copyin_message validates against — the two must be the same number.
 * They were not: the limit was raised to 64 (chromium's Mojo channel writes
 * more than sixteen in one message) while the buffers stayed at 16, so a
 * message with seventeen iovecs copied 1 KiB of caller-controlled data over a
 * 256-byte stack array. That is a kernel stack overflow reachable from any
 * process that sends a large message. */
#define SYSCALL_IOV_MAX 64

static int copyin_message(const struct syscall_msghdr *user_msg,
                          struct syscall_msghdr *msg,
                          struct syscall_iovec *iov, char **payload,
                          usize *payload_len) {
  if (!user_msg || syscall_copyin(msg, user_msg, sizeof(*msg)) < 0)
    return -EFAULT;
  /* IOV_MAX is 1024 on Linux; sixteen was our own invention and chromium's
   * Mojo channel writes more than that in one message. */
  if (msg->msg_iovlen < 1 || msg->msg_iovlen > SYSCALL_IOV_MAX ||
      !msg->msg_iov)
    return -EINVAL;
  if (syscall_copyin(iov, msg->msg_iov,
                     (usize)msg->msg_iovlen * sizeof(*iov)) < 0)
    return -EFAULT;

  usize total = 0;
  for (int i = 0; i < msg->msg_iovlen; i++) {
    /* A message the size of a frame's metadata or a font list passes here
     * routinely; 64 KiB turned those into EMSGSIZE, which Mojo reads as a dead
     * channel rather than something to split. */
    if (iov[i].iov_len > (1u << 20) || total > (1u << 20) - iov[i].iov_len)
      return -EMSGSIZE;
    total += iov[i].iov_len;
  }
  /* A message with no bytes in it is a message. Linux sends and receives
   * zero-length datagrams, and the ancillary data they carry is the whole
   * point of some of them -- a receiver takes the sender's identity from the
   * SCM_CREDENTIALS attached to an empty message. Refusing an iovec of total
   * length zero with EINVAL made that impossible to express. */
  if (total == 0) {
    *payload = 0;
    *payload_len = 0;
    return 0;
  }
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
  struct syscall_iovec iov[SYSCALL_IOV_MAX];
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
    /* Room for the descriptor arrays real senders attach — see
     * VFS_SCM_MAX_FDS. */
    if (msg.msg_controllen > 1024) {
      kfree(payload);
      return (u64)-EINVAL;
    }
    u8 control[1024];
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
  struct syscall_iovec iov[SYSCALL_IOV_MAX];
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

  if (flags & B1NIX_MSG_CMSG_CLOEXEC)
    for (usize i = 0; i < received_count; i++)
      scheduler_fd_flags_set(received_fds[i], B1NIX_FD_CLOEXEC);

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

  /* msg_name: the sender of this datagram. It used to be filled with zeros,
   * which quietly broke every caller that checks who answered — musl's
   * resolver discards a reply unless msg_name matches the nameserver it
   * queried, so name resolution failed system-wide with the answer sitting
   * in the buffer. Falls back to zeros only when the socket cannot say. */
  if (msg.msg_name && msg.msg_namelen) {
    u8 src[128] = {0};
    usize cap = msg.msg_namelen < sizeof(src) ? msg.msg_namelen : sizeof(src);
    usize n = vfs_socket_last_srcaddr(fd, src, cap);
    if (syscall_copyout(msg.msg_name, src, cap) < 0)
      return (u64)-EFAULT;
    if (n)
      msg.msg_namelen = (u32)n;
  }

  u8 control[1024] = {0};
  usize control_len = 0;
  if (received_count) {
    usize data_len = received_count * sizeof(int);
    usize cmsg_len = header_space + data_len;
    usize space = header_space + K_CMSG_ALIGN(data_len);
    /* Both bounds. msg_controllen is the caller's buffer; `control` is this
     * function's, on the kernel stack, and a caller is free to name a
     * msg_controllen larger than it. Checking only the caller's let a large
     * enough request write past the end of ours. */
    if (space <= msg.msg_controllen && space <= sizeof(control)) {
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
  /* SCM_TIMESTAMP: when the socket asked for arrival stamps, every received
   * message carries the moment it landed in the receive buffer. */
  {
    int ts_mode = vfs_socket_timestamp_enabled(fd);
    u64 stamp = ts_mode ? vfs_socket_last_timestamp_usec(fd) : 0;
    if (ts_mode && stamp) {
      /* SO_TIMESTAMP carries a struct timeval, SO_TIMESTAMPNS a timespec. */
      i64 val[2];
      usize vlen;
      int ctype;
      if (ts_mode == 1) {
        val[0] = (i64)(stamp / 1000000ull);
        val[1] = (i64)(stamp % 1000000ull);
        ctype = K_SCM_TIMESTAMP;
        vlen = sizeof(val);
      } else {
        val[0] = (i64)(stamp / 1000000ull);
        val[1] = (i64)((stamp % 1000000ull) * 1000ull);
        ctype = K_SCM_TIMESTAMPNS;
        vlen = sizeof(val);
      }
      usize cmsg_len = header_space + vlen;
      usize space = header_space + K_CMSG_ALIGN(vlen);
      if (control_len + space <= msg.msg_controllen &&
          control_len + space <= sizeof(control)) {
        struct syscall_cmsghdr *c =
            (struct syscall_cmsghdr *)(control + control_len);
        c->cmsg_len = cmsg_len;
        c->cmsg_level = K_SOL_SOCKET;
        c->cmsg_type = ctype;
        memcpy((u8 *)c + sizeof(*c), val, vlen);
        control_len += space;
      } else {
        ctrunc = 1;
      }
    }
  }
  if (has_cred) {
    usize cmsg_len = header_space + sizeof(cred);
    usize space = header_space + K_CMSG_ALIGN(sizeof(cred));
    if (control_len + space <= msg.msg_controllen &&
        control_len + space <= sizeof(control)) {
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
    /* A syscall trace stop reports SIGTRAP|0x80 (PTRACE_O_TRACESYSGOOD); bit 7
     * is a marker, not part of the signal number, so it is set aside across the
     * remap and put back afterwards. */
    int raw = (kstatus >> 8) & 0xff;
    int sysgood = raw & 0x80;
    int linux_sig = b1nix_signo_to_linux(raw & 0x7f);
    if (linux_sig > 0)
      linux_sig |= sysgood;
    /* Bits 16-23 carry the ptrace event code (SIGTRAP | event << 8 sits in the
     * high half of a stopped status). Only the signal number is remapped —
     * dropping the event byte would make every PTRACE_EVENT_* stop look like a
     * plain SIGTRAP to a Linux-personality tracer. */
    if (linux_sig > 0)
      return (kstatus & 0x00ff0000) | (linux_sig << 8) | 0x7f;
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
  /* addrlen is both in and out: in, the room the caller has; out, how long the
   * address really is. The two are NOT the same number, and writing the second
   * one's worth of bytes into the first one's buffer is memory corruption —
   * dbus-daemon accepts with a 16-byte `struct sockaddr` and got a 110-byte
   * sockaddr_un written over its stack frame ("*** stack smashing detected
   * ***", SIGABRT, the system bus gone). Linux truncates the copy and reports
   * the untruncated length, which is how a caller knows it needs a bigger
   * buffer. */
  usize user_cap = 0;
  if (addrlen) {
    if (socklen_copyin(&user_cap, addrlen) != 0) return (u64)-EFAULT;
  }

  char k_addr[128]; /* enough for sockaddr_un */
  /* What the socket layer is told it may write is OUR buffer, not the
   * caller's: a caller is free to name a capacity larger than 128. */
  usize k_addrlen = user_cap > sizeof(k_addr) ? sizeof(k_addr) : user_cap;
  int res = vfs_accept(fd, k_addr, &k_addrlen);
  if (res >= 0) {
    if (k_addrlen > sizeof(k_addr)) k_addrlen = sizeof(k_addr);
    usize out = k_addrlen < user_cap ? k_addrlen : user_cap;
    if (addr && out > 0) {
      if (syscall_copyout(addr, k_addr, out) != 0) return (u64)-EFAULT;
    }
    if (addrlen) {
      if (socklen_copyout(addrlen, k_addrlen) != 0) return (u64)-EFAULT;
    }
  }
  return (u64)res;
}

/* SOL_SOCKET / SO_ATTACH_FILTER, whose optval is a struct sock_fprog holding a
 * pointer to the instruction array in userspace. Every other option's value is
 * self-contained, so this is the one that has to be resolved here rather than
 * in the socket layer. */
#define SYS_SOL_SOCKET_LEVEL 1
#define SYS_SO_ATTACH_FILTER 26

static u64 sys_setsockopt_attach_filter(int fd, const void *user_optval,
                                        usize optlen) {
  struct sock_fprog_user fp;
  if (optlen < sizeof(fp))
    return (u64)-EINVAL;
  if (syscall_copyin(&fp, user_optval, sizeof(fp)) < 0)
    return (u64)-EFAULT;
  if (fp.len == 0 || fp.len > BPF_MAXINSNS || fp.filter == 0)
    return (u64)-EINVAL;

  usize bytes = (usize)fp.len * sizeof(struct sock_filter_insn);
  struct sock_filter_insn *insns = (struct sock_filter_insn *)kmalloc(bytes);
  if (!insns)
    return (u64)-ENOMEM;
  u64 rc;
  if (syscall_copyin(insns, (const void *)(usize)fp.filter, bytes) < 0)
    rc = (u64)-EFAULT;
  else
    rc = (u64)vfs_sock_attach_filter(fd, insns, fp.len);
  kfree(insns);
  return rc;
}

static u64 sys_setsockopt(int fd, int level, int optname,
                          const void *user_optval, usize optlen) {
  u8 kopt[64];
  if (!user_optval || optlen == 0)
    return (u64)-EINVAL;
  if (level == SYS_SOL_SOCKET_LEVEL && optname == SYS_SO_ATTACH_FILTER)
    return sys_setsockopt_attach_filter(fd, user_optval, optlen);
  if (optlen > sizeof(kopt))
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
  usize user_cap = klen;
  if (klen > sizeof(kaddr))
    klen = sizeof(kaddr);
  int rc = want_peer ? vfs_getpeername(fd, kaddr, &klen)
                     : vfs_getsockname(fd, kaddr, &klen);
  if (rc < 0)
    return (u64)rc;
  if (klen > sizeof(kaddr))
    klen = sizeof(kaddr);
  /* Truncate to what the caller offered; report the real length (see
   * sys_accept). */
  usize out = klen < user_cap ? klen : user_cap;
  if (user_addr && out > 0 && syscall_copyout(user_addr, kaddr, out) < 0)
    return (u64)-EFAULT;
  if (socklen_copyout(user_addrlen, klen) != 0)
    return (u64)-EFAULT;
  return 0;
}

/*
 * Do this process's mappings still describe a partition of its address space?
 *
 * Two VMAs covering one address is not a cosmetic bookkeeping error: every
 * lookup takes the first match, so from that moment the kernel answers
 * questions about that address using whichever mapping happens to be earlier in
 * the list — the wrong protection, the wrong backing, the wrong lifetime. It
 * has cost this project a compositor's heap, and it leaves no trace at the
 * moment it happens.
 *
 * Enabled by b1nix.vma-check because it is quadratic in the number of
 * mappings; a process with a hundred of them pays it on every mmap.
 */
static void vma_audit(const char *where) {
  static int reported;
  struct task *t = current_task;

  if (reported >= 8 || !t || !bootinfo_has_flag("b1nix.vma-check"))
    return;
  for (struct vm_area *a = t->vma_list; a; a = a->next) {
    for (struct vm_area *b = a->next; b; b = b->next) {
      if (a->end <= b->start || b->end <= a->start)
        continue;
      reported++;
      console_write("vma-check: ");
      console_write(where);
      console_write(" left 0x");
      console_write_hex64(a->start);
      console_write("-0x");
      console_write_hex64(a->end);
      console_write(" overlaps 0x");
      console_write_hex64(b->start);
      console_write("-0x");
      console_write_hex64(b->end);
      console_write(" in ");
      console_write(t->name ? t->name : "?");
      console_write("\n");
      return;
    }
  }
}

/* Sleep for exactly as long as asked, to the precision the clock allows.
 *
 * Whole scheduler ticks are slept; the remainder is waited out against the
 * calibrated counter. Rounding the whole request up to a tick — which all
 * three sleep entry points used to do — made every sub-tick sleep cost ten
 * milliseconds, so a usleep(500) waited twenty times longer than it asked.
 * One such call is invisible; a loop of them is not, and userspace is full of
 * them: retry loops, poll intervals, backoffs, the drain loop of a handoff.
 * Measured on one: five hundred iterations that should have taken half a
 * second took seven and a half.
 *
 * The wait is bounded — only a remainder under two milliseconds is spun out,
 * so a long sleep never burns a CPU and the most that can be burned is the
 * tail of one tick — and it is interruptible, because a sleep that ignores a
 * signal is a worse bug than a slow one.
 *
 * Returns the number of ticks actually slept, so a caller can report the
 * remainder of an interrupted sleep.
 */
static u64 syscall_sleep_timespec(const struct timespec *ts, u64 *ticks_asked) {
  u64 tick_ns = 1000000000ULL / (u64)sched_tick_hz();
  u64 total_ns = (u64)ts->tv_sec * 1000000000ULL + (u64)ts->tv_nsec;
  u64 start_ticks = scheduler_get_uptime_ticks();
  u64 deadline_ns = arch_tsc_monotonic_ns() + total_ns;
  u64 asked_ticks = (total_ns + tick_ns - 1) / tick_ns;
  /* A sleep is bounded by BOTH clocks, and by whichever says "enough" first.
   *
   * The nanosecond counter is the precise one and decides the normal case. It
   * is not, however, allowed to be the only way out: if it advances slower than
   * the scheduler's ticks, the deadline is never reached and the loop below
   * sleeps for ever, waking and re-sleeping a few ticks at a time. That is not
   * hypothetical -- `sleep 3` inside the KDE image never returned, and the task
   * dump showed a wake_tick five ticks ahead of the current tick, sixty seconds
   * apart, every time. So the tick count the caller asked for is a ceiling.
   * Two ticks of slack keep the ns deadline the one that normally fires, so
   * sub-tick precision is unaffected. */
  u64 tick_deadline = start_ticks + asked_ticks + 2;

  if (ticks_asked)
    *ticks_asked = asked_ticks;

  /* Sleep the ticks, then finish on the clock.
   *
   * A tick-count sleep returns on the next tick boundary, so it can come back
   * early — asking for five milliseconds and being given four is a sleep that
   * did not happen, and callers that time anything with it are wrong. The
   * remainder is therefore waited out against the counter.
   *
   * It has to be waited out by sleeping again, not by spinning. A tick sleep
   * also ends early whenever anything promotes the task to READY, which any
   * wake path does, and the residue is then not the sub-tick tail this waits
   * for but the whole rest of the sleep. Spun on the counter, a `sleep 20`
   * that was woken once burns a core for twenty seconds with the task never
   * yielding: on a single-CPU guest nothing else runs for that whole time and
   * the machine reads as wedged, which is exactly how it read.
   *
   * So: whole ticks go back to the scheduler, and only the last sub-tick
   * fragment — bounded by one tick, a millisecond at the rate this kernel
   * programs — is spun. Whether re-entry is allowed at all is the scheduler's
   * answer, not ours: see scheduler_sleep_ticks_state. */
  for (;;) {
    u64 now_ns = arch_tsc_monotonic_ns();
    if (now_ns >= deadline_ns)
      break;
    if (scheduler_get_uptime_ticks() >= tick_deadline)
      break;
    if (scheduler_signal_pending_any())
      break;
    u64 rest_ticks = (deadline_ns - now_ns) / tick_ns;
    if (!rest_ticks) {
      __asm__ volatile("pause");
      continue;
    }
    /* Ask the scheduler to sleep and let IT decide whether this task may:
     * reading our own state here and calling on the answer is a race, because
     * another CPU stops or kills us in between. That race panicked the guest
     * from this very loop. */
    int slept = scheduler_sleep_ticks_state(rest_ticks, 0);
    if (slept == SLEEP_GONE)
      break;
    if (slept == SLEEP_RETRY)
      scheduler_yield(); /* stopped or blocked: give up the CPU, then re-check
                          * the deadline — a sleep survives SIGSTOP/SIGCONT
                          * instead of returning short. */
  }
  return scheduler_get_uptime_ticks() - start_ticks;
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

    /* A fixed mapping placed inside the break replaces pages somebody is
     * using; it is legal, and it is also how live data disappears when the
     * caller did not mean to hit the heap. */
    if (bootinfo_has_flag("b1nix.vma-check") && t->heap_start &&
        vaddr < t->user_brk && vaddr + length > t->heap_start) {
      console_write("mmap-fixed inside brk: 0x");
      console_write_hex64(vaddr);
      console_write("+0x");
      console_write_hex64((u64)length);
      console_write(" heap 0x");
      console_write_hex64(t->heap_start);
      console_write("-0x");
      console_write_hex64(t->user_brk);
      console_write(" ");
      console_write(t->name ? t->name : "?");
      console_write("\n");
    }
    /* Unmap the range this MAP_FIXED replaces, batched: same reason as
     * munmap's loop — one flush per batch instead of one per page. */
    {
      /* Collect the frames, flush, and only then release them.
       *
       * This used the inline-freeing unmap, which hands each frame back to the
       * allocator while the other CPUs still have the old translation cached —
       * the flush came afterwards, once per batch. In that window a sibling
       * thread's write goes to a frame that has already been given to somebody
       * else, and the damage surfaces far away: a compositor died with two
       * pixels of a terminal's background colour sitting in its allocator's
       * metadata. munmap has done it in the right order for a while; a
       * MAP_FIXED that replaces a live mapping is the same operation and must
       * do the same thing. */
      enum { FIXED_BATCH = 64 };
      u64 frames[FIXED_BATCH];
      u64 stop = vaddr + length;

      for (u64 v = vaddr; v < stop;) {
        usize n = (usize)((stop - v) / PAGE_SIZE);

        if (n > FIXED_BATCH)
          n = FIXED_BATCH;
        usize nframes = vmm_unmap_range_collect(v, n, frames);
        tlb_shootdown_all();
        for (usize k = 0; k < nframes; k++)
          pmm_free_frame(frames[k]);
        v += (u64)n * PAGE_SIZE;
      }
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
        /* Verify hint doesn't overlap existing VMAs. Under the list lock: a
         * walk racing an insert can follow a pointer being rewritten. */
        u64 ovflags;
        int overlap = 0;
        vma_list_lock(&ovflags);
        for (struct vm_area *curr_vma = t->vma_list; curr_vma;
             curr_vma = curr_vma->next) {
          if (!(curr_vma->start >= vaddr + length || curr_vma->end <= vaddr)) {
            overlap = 1;
            break;
          }
        }
        vma_list_unlock(ovflags);
        if (overlap) {
          vaddr = vm_find_free_area(t, length);
        }
      }
    }
  }

  /* Never return a range that is already somebody's.
   *
   * The free-area search deliberately runs without the VMA list lock — it is
   * the longest walk in the kernel and holding a lock across it stalls every
   * fault on every CPU. That is only sound while nothing else relinks the list
   * underneath it, and "nothing else" is an assumption, not a guarantee. If the
   * answer overlaps a live mapping, the caller is handed memory that already
   * belongs to someone, and the two owners overwrite each other's data with no
   * fault and no clue. Verify under the lock and search again; say so, because
   * a retry here means the assumption above did not hold.
   *
   * Behind b1nix.vma-check: the verification walks the whole mapping list with
   * the lock held, and a browser holds thousands of mappings and makes tens of
   * thousands of calls — paid on every one, it cost twenty times the start-up
   * time. It has never once fired, so it is a check to run when the placement
   * is in question, not on every mmap forever. */
  if (vaddr != (u64)-1 && !(flags & MAP_FIXED) &&
      bootinfo_has_flag("b1nix.vma-check")) {
    for (int attempt = 0; attempt < 4; attempt++) {
      u64 cvflags;
      int overlap = 0;

      vma_list_lock(&cvflags);
      for (struct vm_area *v = t->vma_list; v; v = v->next) {
        if (v->start >= vaddr + length || v->end <= vaddr)
          continue;
        overlap = 1;
        break;
      }
      vma_list_unlock(cvflags);
      if (!overlap)
        break;
      {
        static unsigned reported;

        if (reported < 8) {
          reported++;
          console_write("mmap: placement landed on a live mapping at 0x");
          console_write_hex64(vaddr);
          console_write(" len 0x");
          console_write_hex64((u64)length);
          console_write(" in ");
          console_write(t->name ? t->name : "?");
          console_write("\n");
        }
      }
      vaddr = vm_find_free_area(t, length);
      if (vaddr == (u64)-1)
        break;
    }
  }

  if (vaddr == (u64)-1) {
    /* An mmap that fails is the one event a large allocator does not survive:
     * it maps its arenas up front and treats a refusal as unrecoverable, so
     * the process aborts with no message of its own. Say which request could
     * not be placed — silence here reads as a crash with no cause. */
    console_write("mmap: no free area for 0x");
    console_write_hex64((u64)length);
    console_write(" bytes (flags=0x");
    console_write_hex64((u64)(unsigned)flags);
    console_write(") task=");
    console_write_dec(t->id);
    console_write("\n");
    return (u64)-ENOMEM;
  }

  // Allocate and map physical frames
  u64 vmm_flags = vmm_user_flags_from_prot(prot);

  /* Anonymous memory is faulted in, not handed over.
   *
   * This branch used to be taken only for MAP_NORESERVE: every other anonymous
   * mapping allocated a frame, zeroed it and installed a leaf for every page of
   * the request, before the caller had touched any of them. musl's allocator
   * maps in hundreds of kilobytes at a time and writes to a fraction of it, so
   * the work was mostly wasted — and mremap made it visibly absurd: it mmaps a
   * destination, and every page that mmap had just allocated and zeroed was
   * freed again microseconds later by the move that overwrites them. (That
   * freeing is also what the "destination arrived holding pages left by a
   * previous mapping" report was seeing: not a foreign mapping's leftovers at
   * all, but the destination's own eager allocation.)
   *
   * The fault handler zero-fills any absent anonymous page in this range
   * already, and does it from the pre-zeroed pool with a shared read-only zero
   * page until the first write — so lazy is not merely cheaper, it is what
   * gives the mapping copy-on-write behaviour for pages that are only read.
   *
   * b1nix.eager-anon restores the old behaviour for a run that wants to
   * compare. */
  if ((flags & MAP_ANONYMOUS) && !bootinfo_has_flag("b1nix.eager-anon")) {
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
        console_write("mmap: out of physical frames at 0x");
        console_write_hex64(v);
        console_write(" of 0x");
        console_write_hex64((u64)length);
        console_write(" bytes\n");
        return (u64)-ENOMEM;
      }

      // Zero the frame
      memset((void *)(usize)(frame + direct_base), 0, PAGE_SIZE);

      vmm_map_page(v, frame, vmm_flags | VMM_PRESENT);
    }
  } else if (node && node->inode && node->inode->type == VFS_DEVICE &&
             node->inode->mmap_handle_page_phys_cb) {
    /* M100: scatter-gather device memory. The pages backing the range need not
     * be physically adjacent, so resolve one page at a time instead of
     * extrapolating from a single base. */
    struct vfs_handle *handle = scheduler_fd_get(fd);
    for (u64 v = vaddr; v < vaddr + length; v += PAGE_SIZE) {
      u64 phys = 0;
      int rc = node->inode->mmap_handle_page_phys_cb(
          handle, (u64)offset + (v - vaddr), &phys);
      if (rc < 0 || !phys) {
        for (u64 u = vaddr; u < v; u += PAGE_SIZE)
          vmm_unmap_page(u);
        return (u64)(rc < 0 ? rc : -EINVAL);
      }
      vmm_map_page(v, phys, vmm_flags | VMM_SHARED | VMM_PRESENT);
      pmm_ref_frame(phys);
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
    console_write("mmap: kmalloc(vma) failed\n");
    return (u64)-ENOMEM;
  }
  /* Large mappings, with the address asked for beside the address given.
   *
   * An allocator that places its own pools cares where a mapping lands, not
   * only that it succeeded: it asks for a region, checks the result against
   * the range it requires, and treats a hint it did not get as a failure to
   * allocate — reporting "out of memory" while every mmap in the log returned
   * success. Printing both halves is what distinguishes the two. */
  /* Any mapping placed below 4 GiB, whatever its size.
   *
   * That range is the kernel's identity window — 2 MiB supervisor pages — and
   * userspace has no business being handed an address inside it. A pointer
   * from there reached the browser's allocator and faulted on a page that is
   * present but not user-accessible; whether the kernel handed it out is the
   * question this answers. */
  if (vaddr < 0x100000000ull) {
    console_write("mmap: LOW address handed out: 0x");
    console_write_hex64(vaddr);
    console_write(" len 0x");
    console_write_hex64((u64)length);
    console_write(" want 0x");
    console_write_hex64((u64)(usize)addr);
    console_write(" task=");
    console_write_dec(t->id);
    console_write("\n");
  }
  if (length >= (1u << 20) && bootinfo_has_flag("b1nix.trace-mmap")) {
    console_write("mmap: want 0x");
    console_write_hex64((u64)(usize)addr);
    console_write(" got 0x");
    console_write_hex64(vaddr);
    console_write(" len 0x");
    console_write_hex64((u64)length);
    console_write(" flags 0x");
    console_write_hex64((u64)(unsigned)flags);
    console_write(" prot 0x");
    console_write_hex64((u64)(unsigned)prot);
    console_write(" task=");
    console_write_dec(t->id);
    console_write("\n");
  }

  vma->start = vaddr;
  vma->end = vaddr + length;
  vma->prot = (u32)prot;
  vma->flags = (u32)flags;
  vma->node = node ? vfs_node_get(node) : 0;
  vma->offset = offset;
  vma->next = 0;
  vma_insert(t, vma);
  if (vma->node && vma->node->inode && vma->node->inode->mmap_open_cb)
    vma->node->inode->mmap_open_cb(vma->node);
  if (vma->node && vma->node->inode && vma->node->inode->mmap_range_open_cb)
    vma->node->inode->mmap_range_open_cb(vma->node, (u64)offset, length);

  scheduler_sync_vma_head(t->pml4_phys, t->vma_list);
  vma_audit("mmap");
  return vaddr;
}

void vma_trace_record(const char *what, u64 start, u64 end);
int vma_trace_faults_enabled(void);


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

  /* Unmap in batches, one cross-CPU flush each.
   *
   * vmm_unmap_page tells every other core about ONE page: a global lock, an
   * inter-processor interrupt, and a spin until all of them answer. A browser
   * unmapping tens of megabytes at a time paid that per 4 KiB — measured at
   * 69 ms a call, the single largest system-call cost of its start-up. The
   * pages are retired exactly as before; only the flush is shared. */
  {
    /* One broadcast TLB shootdown per large chunk, not per sixty-four pages.
     *
     * The shootdown is an IPI to every other CPU taken under a global lock and
     * waited for; at a batch of sixty-four pages a browser tearing down a
     * gigabyte of arena paid four thousand of them, and one such munmap was
     * measured at ~1.8 s — a hundred seconds of a two-minute start-up spent in
     * this loop alone. The frames cannot be returned to the allocator before
     * the shootdown (a stale entry on another CPU would then name a reused
     * page), so they are collected first, released after, and the chunk is
     * sized by the array that holds them: 16 MiB of address space per round,
     * from a 32 KiB heap allocation, with the old stack-sized batch as the
     * fallback when the heap has nothing to spare. */
    /* 4096 pages per round meant one broadcast shootdown per 16 MiB, and a
     * browser releasing a gigabyte still paid sixty-four of them — measured at
     * ~109 ms for such a call. A round of 65536 pages covers 256 MiB for one
     * shootdown, at the cost of a 512 KiB frame array that is only allocated
     * when the range is actually that large. */
    enum { MUNMAP_BATCH_MAX = 65536, MUNMAP_BATCH_MIN = 64 };
    u64 stack_frames[MUNMAP_BATCH_MIN];
    usize batch = (usize)((end - start) / PAGE_SIZE);

    if (batch > MUNMAP_BATCH_MAX)
      batch = MUNMAP_BATCH_MAX;
    u64 *frames = (batch > MUNMAP_BATCH_MIN)
                      ? (u64 *)kmalloc(batch * sizeof(u64))
                      : 0;
    if (!frames) {
      frames = stack_frames;
      batch = MUNMAP_BATCH_MIN;
    }

    for (u64 v = start; v < end;) {
      usize n = (usize)((end - v) / PAGE_SIZE);

      if (n > batch)
        n = batch;
      usize nframes = vmm_unmap_range_collect(v, n, frames);
      tlb_shootdown_all();
      for (usize i = 0; i < nframes; i++)
        pmm_free_frame(frames[i]);
      v += (u64)n * PAGE_SIZE;
    }
    if (frames != stack_frames)
      kfree(frames);
  }

  vma_trace_record("munmap", start, end);
  // 2. Update VMA list using the new robust helper
  vma_delete_range(t, start, end);

  scheduler_sync_vma_head(t->pml4_phys, t->vma_list);
  vma_audit("munmap");
  return 0;
}

/* An mmap return that is really an errno: the last page of the address space
 * is not a mapping any caller could have asked for. */
static int mmap_failed(u64 r) { return r >= (u64)-4095; }

/* Linux mremap flags, as musl passes them. */
#define MREMAP_MAYMOVE 1
#define MREMAP_FIXED   2

/*
 * mremap(old, old_len, new_len, flags, new_addr) — resize a mapping.
 *
 * musl's realloc reaches for this on every block large enough to have been
 * mmap'd rather than taken from a bin, and returns failure when the call does:
 * without it, no program can grow a large buffer. That is not an abstract gap —
 * it is why bpkg could unpack a small package and not a two-megabyte one, and
 * why the failure looked like a corrupt download rather than a missing syscall.
 *
 * Shrinking releases the tail in place. Growing — with MREMAP_MAYMOVE, which is
 * what a libc asks for — takes a fresh mapping, copies the contents over and
 * releases the original. The copy goes through the user-access helpers rather
 * than a bare memcpy: the source pages may still be lazily unmapped, and this is
 * the path that knows how to fault them in.
 *
 * MREMAP_FIXED is refused. It moves a mapping onto an address of the caller's
 * choosing, unmapping whatever was there, and nothing here needs it — better a
 * clear EINVAL than a half-implementation that silently loses a mapping.
 */
/* One line per mremap for the first calls of a boot: how it was served and
 * the sizes involved. Off after 32 lines — the point is the pattern, and the
 * console is a serial line. */
static void mremap_note(const char *how, u64 from_len, u64 to_len) {
  static unsigned seen;

  if (seen >= 32 || !console_level_enabled(LOGLEVEL_DEBUG))
    return;
  seen++;
  k_dbg("mremap", "%s 0x%lx -> 0x%lx", how, (unsigned long)from_len,
        (unsigned long)to_len);
}

static u64 sys_mremap(void *old_addr, usize old_len, usize new_len, int flags,
                      void *new_addr) {
  (void)new_addr;
  struct task *t = current_task;
  u64 old_start = (u64)(usize)old_addr;

  if (!t)
    return (u64)-ESRCH;
  /* Diagnostic: refuse the call entirely. A libc that cannot resize a mapping
   * copies through malloc instead, which is slower and completely independent
   * of this code — so a fault that survives the flag is not this syscall's. */
  if (bootinfo_has_flag("b1nix.no-mremap"))
    return (u64)-ENOSYS;
  if ((old_start & (PAGE_SIZE - 1)) != 0 || new_len == 0)
    return (u64)-EINVAL;
  if (flags & ~(MREMAP_MAYMOVE))
    return (u64)-EINVAL;

  old_len = (old_len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
  new_len = (new_len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
  if (old_len == 0)
    return (u64)-EINVAL;

  /* The range has to be one mapping of this process, whole. A request spanning
   * two mappings, or part of none, is a caller error rather than something to
   * guess at. */
  u64 lvflags;
  vma_list_lock(&lvflags);
  struct vm_area *vma = t->vma_list;
  while (vma && !(vma->start <= old_start && vma->end >= old_start + old_len))
    vma = vma->next;
  vma_list_unlock(lvflags);
  if (!vma)
    return (u64)-EFAULT;

  if (new_len == old_len)
    return old_start;

  if (new_len < old_len) {
    isize rc = sys_munmap((void *)(usize)(old_start + new_len),
                          old_len - new_len);
    return rc < 0 ? (u64)rc : old_start;
  }

  /*
   * Grow in place when the space above the mapping is free.
   *
   * The invariant that matters is one mapping per allocation, not a fresh
   * address: a second VMA abutting the first is what broke shrinking (a later
   * call naming the whole range found no single mapping covering it). Growing
   * this VMA's end keeps it one mapping, and the new pages arrive the way
   * every other anonymous page does — on the fault that first touches them.
   *
   * Without this every growth copied the whole allocation through a 64 KiB
   * kernel bounce buffer, faulting in each page on the way, while holding the
   * address-space mutex every other thread needs. Chromium's allocator grows
   * its arenas this way over and over: one such call took minutes, and the
   * rest of the browser waited on the mutex for all of it.
   */
  if (!vma->node && vma->end >= old_start + new_len) {
    /* The mapping is already big enough — the caller asked to grow a range
     * that sits inside a larger one. Nothing to do but say where it is. */
    mremap_note("fits", old_len, new_len);
    return old_start;
  }
  if (vma->end == old_start + old_len && !vma->node) {
    u64 want_end = old_start + new_len;
    int clear = want_end <= USER_SPACE_LIMIT && want_end > old_start;

    u64 gvflags;
    vma_list_lock(&gvflags);
    for (struct vm_area *v = t->vma_list; v && clear; v = v->next) {
      if (v == vma)
        continue;
      if (v->start < want_end && v->end > vma->end)
        clear = 0;
    }
    vma_list_unlock(gvflags);
    if (clear) {
      vma->end = want_end;
      scheduler_sync_vma_head(t->pml4_phys, t->vma_list);
      mremap_note("grow", old_len, new_len);
      return old_start;
    }
  }
  mremap_note("move", old_len, new_len);

  if (!(flags & MREMAP_MAYMOVE))
    return (u64)-ENOMEM;

  /* Diagnostic: refuse only the move, keeping shrink and grow-in-place.
   *
   * b1nix.no-mremap switches off the whole call, which also removes the
   * mapping churn the caller does instead — so a fault that disappears with it
   * has two possible homes. This narrows it to the entry-moving path alone:
   * the caller falls back to allocate-copy-free, which exercises mmap and
   * munmap just as hard. */
  if (bootinfo_has_flag("b1nix.mremap-no-move"))
    return (u64)-ENOMEM;

  /* MAP_NORESERVE: nothing must be allocated into the destination. Every page
   * of it is either overwritten by the move below or faulted in later by the
   * caller, so anything installed here is allocated, zeroed and freed again
   * within the same call. */
  u64 fresh = sys_mmap(0, new_len, (int)vma->prot,
                       MAP_ANONYMOUS | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
  if (mmap_failed(fresh))
    return fresh;
  /* This allocation does not go through the dispatcher, so it would otherwise
   * be missing from the trace — and it is the very range whose contents are in
   * question. */
  vma_trace_record("mremap-dst", fresh, fresh + new_len);
  paging_move_range(old_start, fresh, old_len);

  (void)sys_munmap((void *)(usize)old_start, old_len);
  return fresh;
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
/* One lock per address space, not one for the machine.
 *
 * A single global flag meant an mmap anywhere stalled an mmap everywhere: a
 * browser process starting threads blocked the compositor's next allocation,
 * on a completely unrelated list. Threads of one process share their VMA list
 * — that list is what needs serialising — and processes share nothing here.
 *
 * The address space is named by its PML4 frame, which is exactly what the
 * CLONE_VM threads have in common. A small table of slots is enough: it is
 * indexed by hashing that frame, so unrelated spaces almost never collide, and
 * when they do the cost is the old behaviour for those two alone. */
#define VMA_LOCK_SLOTS 16
static volatile int g_vma_mutex[VMA_LOCK_SLOTS];

/* Who holds each slot.
 *
 * A yielding lock that is only ever released by the code that took it is
 * released by nothing at all when that code's task dies first — and because
 * the slot is shared by every address space hashing to it, the casualty is not
 * only that process: every other one that hashes the same way spins in
 * vma_mutator_lock forever, which is a guest that goes silent with no panic
 * and no clue. Recording the owner makes the leak both reportable and
 * repairable: the exit path hands back whatever the task still holds. */
static struct task *g_vma_mutex_owner[VMA_LOCK_SLOTS];

static unsigned vma_lock_slot(void) {
  u64 space = current_task ? current_task->pml4_phys : 0;

  /* The frame number, folded — consecutive PML4 frames must not land in one
   * slot, and the low twelve bits are always zero. */
  space >>= 12;
  space ^= space >> 8;
  return (unsigned)(space % VMA_LOCK_SLOTS);
}

/* The slot is returned rather than recomputed on release: execve replaces the
 * address space mid-call, and recomputing would then unlock a different one. */
static unsigned vma_mutator_lock(void) {
  unsigned slot = vma_lock_slot();
  unsigned long spins = 0;

  while (__sync_lock_test_and_set(&g_vma_mutex[slot], 1)) {
    scheduler_yield();
    /* Say who is being waited for, once, before the wait becomes a hang that
     * has to be diagnosed from the outside. The count is generous: a mutator
     * can legitimately block on frame reclaim or writeback while holding this,
     * and yields are cheap. */
    if (++spins == 2000000ul) {
      struct task *owner = g_vma_mutex_owner[slot];

      console_write("vma: mutator lock slot ");
      console_write_dec(slot);
      console_write(" held for a very long time by ");
      if (owner) {
        console_write(owner->name ? owner->name : "?");
        console_write(" pid ");
        console_write_dec(owner->id);
        console_write(" state ");
        console_write_dec((u64)owner->state);
      } else {
        console_write("nobody on record");
      }
      console_write("; waiter pid ");
      console_write_dec(current_task ? current_task->id : 0);
      console_write("\n");
      /* The owner's pid and state say that it is stuck, and nothing about
       * where. The task dump names the syscall every task is in and the
       * channel it is waiting on, so the one line that reports the stall also
       * carries the answer — the alternative is a guest that has gone silent
       * and a second run with an instrument added. Printed once, with the
       * warning, for the same reason the warning is printed once. */
      scheduler_dump_tasks();
    }
  }
  g_vma_mutex_owner[slot] = current_task;
  return slot;
}
static void vma_mutator_unlock(unsigned slot) {
  g_vma_mutex_owner[slot] = 0;
  __sync_lock_release(&g_vma_mutex[slot]);
}

/* Give back any address-space mutex this task still holds.
 *
 * Called from the exit path. A task that dies inside mmap — killed by the
 * group leader's exit_group, or by a fault — would otherwise leave the slot
 * set forever. */
void syscall_release_vma_locks(struct task *t) {
  if (!t)
    return;
  for (unsigned i = 0; i < VMA_LOCK_SLOTS; i++) {
    if (g_vma_mutex_owner[i] != t)
      continue;
    g_vma_mutex_owner[i] = 0;
    __sync_lock_release(&g_vma_mutex[i]);
    console_write("vma: released a mutator lock left held by a dying task, pid ");
    console_write_dec(t->id);
    console_write("\n");
  }
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

  u64 flags = vmm_user_flags_from_prot(prot);

  // 1. Update hardware page tables
  {
    extern void paging_mprotect_range(u64 start, u64 end, u64 flags);

    paging_mprotect_range(start, end, flags);
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

  scheduler_sync_vma_head(t->pml4_phys, t->vma_list);
  vma_audit("mprotect");
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
  case MADV_FREE:
    /*
     * A hint, and only a hint.
     *
     * MADV_FREE says the caller no longer needs the contents *and will not be
     * surprised if they survive*: the kernel may reclaim the pages when it is
     * short of memory, and any write before that cancels the offer. Discarding
     * them immediately — which is what this used to do, by falling through to
     * MADV_DONTNEED — is a different promise, and a caller that reads back what
     * it just told us it might not need gets zeros instead of its data.
     *
     * b1nix has no lazy-reclaim queue, so the honest implementation of the hint
     * is to keep the pages. The memory is still accounted to the process and
     * still freed by munmap; only the opportunistic reclaim is missing, which
     * costs footprint rather than correctness.
     */
    return 0;
  case MADV_DONTNEED:
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
  /* Walk the range one mapping at a time, not one page at a time.
   *
   * The old loop searched the whole VMA list for every page and then unmapped
   * that page on its own, which meant a broadcast TLB shootdown per 4 KiB: a
   * discard of a few megabytes cost tens of milliseconds, all of it IPI. The
   * pages of one mapping share its flags, so the lookup belongs outside the
   * loop, and the shootdown belongs once per run — with the frames released
   * only after it, since a frame handed back early can be reused while another
   * CPU still has a stale translation to it. */
  enum { MADV_BATCH = 512 };
  u64 frames[MADV_BATCH];

  for (u64 v = start; v < end;) {
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

    u64 seg_end = cover->end < end ? cover->end : end;
    int anon = (cover->flags & MAP_ANONYMOUS) != 0 || cover->node == 0;
    int shared = (cover->flags & MAP_SHARED) != 0;

    if (!anon || shared) {
      v = seg_end; /* never discard file/shared data */
      continue;
    }

    u64 vmm_flags = vmm_user_flags_from_prot((int)cover->prot);

    while (v < seg_end) {
      usize n = (usize)((seg_end - v) / PAGE_SIZE);

      if (n > MADV_BATCH)
        n = MADV_BATCH;
      usize nframes = vmm_unmap_range_collect(v, n, frames);
      tlb_shootdown_all();
      for (usize i = 0; i < nframes; i++)
        pmm_free_frame(frames[i]);
      /* Re-arm each page as lazy so the next touch refaults to a fresh zeroed
       * anonymous page (page-fault Case 1).
       *
       * One call per page, not two: vmm_set_lazy and paging_mprotect_page each
       * walk four levels, and this runs while the caller — PartitionAlloc,
       * returning memory to the system — holds its global lock, so every other
       * thread in the browser waits behind the second walk for nothing. */
      for (usize i = 0; i < n; i++)
        vmm_set_lazy_flags(v + (u64)i * PAGE_SIZE, vmm_flags);
      v += (u64)n * PAGE_SIZE;
    }
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
      /* Never over a page that is already there.
       *
       * A fresh frame here destroys whatever the page held, and "already
       * mapped" is not a hypothetical: the break is shared by every thread of
       * the process, so two of them growing it can each believe the range is
       * new. Mapping only what is genuinely absent makes the race harmless
       * instead of silently fatal. */
      if (paging_leaf_pte(v) & 1)
        continue;
      u64 frame = pmm_alloc_frame();
      if (!frame) {
        return t->user_brk; // ENOMEM
      }

      // Zero frame
      u64 direct_base = vmm_direct_map_base();
      memset((void *)(usize)(frame + direct_base), 0, PAGE_SIZE);

      /* The heap is data: writable, never executable. */
      vmm_map_page(v, frame,
                   vmm_user_flags_from_prot(PROT_READ | PROT_WRITE) |
                       VMM_PRESENT);
    }
  } else if (addr < t->user_brk) {
    u64 old_brk_page_end = (t->user_brk + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    u64 new_brk_page_end = (addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    for (u64 v = new_brk_page_end; v < old_brk_page_end; v += PAGE_SIZE) {
      vmm_unmap_page(v);
    }
  }

  u64 old_brk = t->user_brk;

  t->user_brk = addr;

  /*
   * Describe the break as mappings, without assuming it is one of them.
   *
   * It used to find "the heap VMA" by its start address and stretch that to the
   * new break. musl puts a guard page at the very front of the break — an
   * mmap(PROT_NONE, MAP_FIXED) at exactly heap_start — and from then on the VMA
   * starting there is the guard. Growing the break then stretched *that* over
   * the whole heap: a PROT_NONE mapping covering live data, overlapping the
   * real heap mapping still inside it.
   *
   * Everything downstream reads a range's properties off the first VMA that
   * covers it, so from that moment the heap was, to the rest of the kernel, an
   * inaccessible lazy anonymous region — and a fault in it was answered with a
   * fresh zero page. That is how a compositor's allocator metadata came to be
   * zeroed while it was still in use.
   *
   * So: on growth, cover only what is not already covered by somebody else; on
   * shrink, remove exactly what was given back. Several VMAs describing one
   * break is not a problem — nothing requires it to be a single mapping.
   */
  if (addr > old_brk) {
    u64 from = (old_brk + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    u64 to = (addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    while (from < to) {
      u64 chunk_end = to;

      /* The first mapping that stands in the way bounds this piece. */
      for (struct vm_area *v = t->vma_list; v; v = v->next) {
        if (v->end <= from || v->start >= chunk_end)
          continue;
        if (v->start <= from) {
          /* Already described by someone else — skip past it. */
          from = v->end;
          chunk_end = from;
          break;
        }
        chunk_end = v->start;
      }
      if (from >= chunk_end) {
        if (from >= to)
          break;
        continue;
      }

      struct vm_area *heap = kzalloc(sizeof(struct vm_area));

      if (!heap)
        break;
      heap->start = from;
      heap->end = chunk_end;
      heap->prot = PROT_READ | PROT_WRITE;
      heap->flags = MAP_PRIVATE | MAP_ANONYMOUS;
      /* In address order, like every other insertion: the list is walked by
       * address in several places and a front-inserted node breaks them. */
      {
        struct vm_area **link = &t->vma_list;

        while (*link && (*link)->start < heap->start)
          link = &(*link)->next;
        heap->next = *link;
        *link = heap;
      }
      from = chunk_end;
    }
  } else if (addr < old_brk) {
    u64 from = (addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    u64 to = (old_brk + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    if (from < to)
      vma_delete_range(t, from, to);
  }

  /* Every change to the break, when asked. A shrink followed by a growth
   * hands back pages whose contents are gone, and that is indistinguishable
   * from corruption to whoever was using them. */
  if (bootinfo_has_flag("b1nix.vma-check")) {
    console_write("brk: 0x");
    console_write_hex64(old_brk);
    console_write(" -> 0x");
    console_write_hex64(addr);
    console_write(addr < old_brk ? " SHRINK " : " grow ");
    console_write(t->name ? t->name : "?");
    console_write("\n");
  }
  /* And the break belongs to the address space, so every thread sharing it
   * must see the new value. */
  scheduler_sync_brk(t->pml4_phys, t->heap_start, t->user_brk);
  vma_audit("brk");
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

/* Set while a traced task is inside a system call, so the copy helpers can
 * report what they wrote without each of them re-reading the command line. */
static int g_trace_in_call;

int syscall_trace_active(void) { return g_trace_in_call; }

static u64 syscall_dispatch_traced(u64 number, u64 arg0, u64 arg1, u64 arg2,
                                   u64 arg3, u64 arg4, u64 arg5,
                                   struct interrupt_frame *frame);

/* M86: CPU-time accounting boundary. A syscall with a frame came from ring 3,
 * so the interval that ends here is user time and everything until the return
 * is system time. Kernel-internal callers pass frame == 0 and are already
 * inside a system-time interval. */
/* ── System-call profile ────────────────────────────────────────────────────
 * Counts and cycles per call number, printed by the watchdog. Off unless
 * b1nix.sysprof is on the command line. */
#define SYSPROF_SLOTS 512
static u64 g_sysprof_count[SYSPROF_SLOTS];
static u64 g_sysprof_cycles[SYSPROF_SLOTS];

static inline u64 syscall_prof_now(void) {
  u32 lo, hi;

  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return ((u64)hi << 32) | lo;
}

static int syscall_prof_enabled(void) {
  static int on = -1;

  if (on < 0)
    on = bootinfo_has_flag("b1nix.sysprof") ? 1 : 0;
  return on;
}

static void syscall_prof_account(u32 nr, u64 cycles) {
  if (nr >= SYSPROF_SLOTS)
    return;
  __atomic_fetch_add(&g_sysprof_count[nr], 1, __ATOMIC_RELAXED);
  __atomic_fetch_add(&g_sysprof_cycles[nr], cycles, __ATOMIC_RELAXED);
}

/* The ten call numbers that have cost the most, newest totals each time. A
 * running total rather than an interval: the question is what the start-up
 * spends its life in, not what it did in the last thirty seconds. */
/* Start a fresh interval, so two reads bracket one run. */
void syscall_prof_reset(void) {
  for (int n = 0; n < SYSPROF_SLOTS; n++) {
    __atomic_store_n(&g_sysprof_count[n], 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_sysprof_cycles[n], 0, __ATOMIC_RELAXED);
  }
}

void syscall_prof_dump(void) {
  if (!syscall_prof_enabled())
    return;
  console_write("sysprof (nr count Mcycles):");
  for (int rank = 0; rank < 12; rank++) {
    u32 best = 0;
    u64 bestc = 0;

    for (u32 n = 0; n < SYSPROF_SLOTS; n++) {
      u64 c = __atomic_load_n(&g_sysprof_cycles[n], __ATOMIC_RELAXED);

      if (c > bestc && !(g_sysprof_count[n] & (1ull << 63))) {
        bestc = c;
        best = n;
      }
    }
    if (!bestc)
      break;
    console_write(" ");
    console_write_dec(best);
    console_write(":");
    console_write_dec(__atomic_load_n(&g_sysprof_count[best], __ATOMIC_RELAXED));
    console_write(":");
    console_write_dec(bestc / 1000000);
    /* Mark it consumed for this pass, then restore below. */
    __atomic_fetch_or(&g_sysprof_count[best], 1ull << 63, __ATOMIC_RELAXED);
  }
  for (u32 n = 0; n < SYSPROF_SLOTS; n++)
    __atomic_fetch_and(&g_sysprof_count[n], ~(1ull << 63), __ATOMIC_RELAXED);
  console_write("\n");
}

u64 syscall_dispatch_impl(u64 number, u64 arg0, u64 arg1, u64 arg2, u64 arg3,
                          u64 arg4, u64 arg5, struct interrupt_frame *frame) {
  if (!frame)
    return syscall_dispatch_traced(number, arg0, arg1, arg2, arg3, arg4, arg5,
                                   frame);
  /* Remember where userspace called from. A thread that blocks in here is
   * invisible to both the fault reporter and the tick sampler, so this is what
   * makes its user stack walkable while it is still parked. Two stores. */
  if (current_task)
    task_set_syscall_entry(current_task, frame->rip, frame->rbp);
  sched_acct_enter_kernel();
  u64 r = syscall_dispatch_traced(number, arg0, arg1, arg2, arg3, arg4, arg5,
                                  frame);
  sched_acct_leave_kernel();
  return r;
}

static u64 syscall_dispatch_traced(u64 number, u64 arg0, u64 arg1, u64 arg2,
                                   u64 arg3, u64 arg4, u64 arg5,
                                   struct interrupt_frame *frame) {
  /* M80: PTRACE_SYSCALL entry stop. The plain-load ptrace_any_traced() gate
   * keeps an untraced system at one memory read per syscall. A tracer that
   * rewrites registers during the stop is obeyed: the number and arguments are
   * re-read from the frame afterwards, which is what makes syscall
   * interception (strace -e inject, seccomp-style rewrites) actually work. */
  if (frame && current_task && ptrace_any_traced()) {
    ptrace_syscall_stop(current_task, frame, 0);
    number = frame->rax;
    arg0 = frame->rdi;
    arg1 = frame->rsi;
    arg2 = frame->rdx;
    arg3 = frame->r10;
    arg4 = frame->r8;
    arg5 = frame->r9;
  }

  /*
   * Every call one process makes, when asked.
   *
   * b1nix.debug=syscall turns it on and b1nix.trace-task=<substring> chooses
   * whose calls to print — tracing everything drowns the serial line and
   * changes the timing of what is being investigated. Written for hunting a
   * corrupted heap: the interesting evidence is the call that wrote to user
   * memory just before a libc's own consistency check fired.
   */
  /* Record it before anything else can fail: the sequence a thread made on the
   * way into a wait is the part comparable with the same program traced on a
   * working kernel. */
  {
    extern void scheduler_note_syscall(u32 nr);

    scheduler_note_syscall((u32)number);
  }

  int trace_this = 0;
  /* Resolved once. The gate is on the hot path of every system call, and both
   * the category check and the filter parse the kernel command line. */
  static int trace_enabled = -1;
  static char want[32];

  if (trace_enabled < 0) {
    trace_enabled = klog_debug_enabled("syscall") &&
                    bootinfo_get_kv("b1nix.trace-task", want, sizeof(want));
  }

  if (frame && current_task && trace_enabled) {
    {
      const char *nm = current_task->name ? current_task->name : "";

      /* A comma-separated list, because a process and the threads it creates
       * carry different names and the interesting sequence spans both. */
      for (usize p = 0; want[p] && !trace_this;) {
        usize e = p;

        while (want[e] && want[e] != ',')
          e++;
        for (usize i = 0; nm[i] && !trace_this; i++)
          if (strncmp(nm + i, want + p, e - p) == 0)
            trace_this = 1;
        p = want[e] ? e + 1 : e;
      }
    }
  }

  /*
   * A watch on one user address, checked either side of the call.
   *
   * b1nix.watch-user=<hex> names it. Written to catch the syscall that damages
   * a libc global — the corruption shows up much later, inside the allocator,
   * and by then nothing says who wrote it. Only traced tasks pay for this.
   */
  u64 watch_addr = 0, watch_before = 0;

  if (trace_this) {
    char w[24];

    if (bootinfo_get_kv("b1nix.watch-user", w, sizeof(w))) {
      for (const char *c = w; *c; c++) {
        u64 d;

        if (*c >= '0' && *c <= '9') d = (u64)(*c - '0');
        else if (*c >= 'a' && *c <= 'f') d = (u64)(*c - 'a' + 10);
        else if (*c >= 'A' && *c <= 'F') d = (u64)(*c - 'A' + 10);
        else break;
        watch_addr = watch_addr * 16 + d;
      }
      if (watch_addr &&
          syscall_copyin(&watch_before, (void *)(usize)watch_addr, 8) != 0)
        watch_addr = 0;
    }
  }

  g_trace_in_call = trace_this && bootinfo_has_flag("b1nix.trace-copyout");
  /* Where the time actually goes.
   *
   * A start-up that takes minutes where it takes a second on Linux is not
   * explained by reading the code; the profile says which call to look at.
   * Two counters per call number, a rdtsc either side, and the watchdog prints
   * the worst offenders. The cost is one serialising read per system call, paid
   * only when b1nix.sysprof asked for it. */
  u64 prof_t0 = syscall_prof_enabled() ? syscall_prof_now() : 0;
  u64 ret = syscall_dispatch_impl_inner(number, arg0, arg1, arg2, arg3, arg4, arg5, frame);
  if (prof_t0)
    syscall_prof_account((u32)number, syscall_prof_now() - prof_t0);
  /* Which call answered a given errno, when a program reports one and does not
   * say what it asked. `b1nix.trace-errno=<n>`: an error is a number in a log
   * message until the call that produced it has a name. */
  {
    /* Read from the command line ONCE. Asking bootinfo per system call means
     * scanning the whole cmdline string on every entry into the kernel, which
     * is not a diagnostic cost -- it is slow enough to stall the machine. */
    enum { TRACE_ERRNO_ALL = 0xffffffffu - 1 };
    static u32 trace_errno = (u32)-1;
    /* Optional pid filter. ENOENT in particular is answered thousands of times
     * in a normal boot -- every probe for a file that is not there is one --
     * and printing them all through the serial console changes the timings
     * being investigated and buries the one line that matters.
     * `b1nix.trace-errno-pid=<pid>` narrows the trace to a single task; 0 (the
     * default) keeps every task, as before. */
    static u32 trace_errno_pid = (u32)-1;
    if (trace_errno == (u32)-1) {
      /* `b1nix.trace-errno=all` reports EVERY failing call rather than one
       * chosen code. Guessing the code first is only possible when the program
       * says which one it got, and a manager that dies without printing
       * anything says nothing at all -- which is exactly when this is needed.
       * Kept behind the same pid filter, because unfiltered it is thousands of
       * lines a second. */
      char ev[16];
      if (bootinfo_get_kv("b1nix.trace-errno", ev, sizeof(ev)) &&
          strcmp(ev, "all") == 0)
        trace_errno = TRACE_ERRNO_ALL;
      else
        trace_errno = bootinfo_get_u32("b1nix.trace-errno", 0);
      trace_errno_pid = bootinfo_get_u32("b1nix.trace-errno-pid", 0);
    }
    u32 e = trace_errno;
    if (e) {
      u32 pid = (unsigned)(current_task ? current_task->id : 0);
      /* An errno return is a small negative value; anything below -4095 is a
       * pointer or a byte count that happens to have the top bit set. */
      i64 sret = (i64)ret;
      int failed = (e == TRACE_ERRNO_ALL) ? (sret < 0 && sret >= -4095)
                                          : (ret == (u64)(-(i64)e));
      if (failed && (trace_errno_pid == 0 || pid == trace_errno_pid)) {
        if (e == TRACE_ERRNO_ALL)
          e = (u32)(-sret);
        /* The path, where the call has one. "errno 2 from syscall 83" says a
         * mkdir failed; it does not say which directory, and the directory is
         * the whole answer. Linux's numbering puts the path in different
         * arguments for different calls, so the argument is chosen per call
         * rather than guessed -- a wrong guess would dereference an integer. */
        const char *upath = 0;
        switch (number) {
        case 21:  /* access */
        case 59:  /* execve */
        case 83:  /* mkdir */
        case 84:  /* rmdir */
        case 87:  /* unlink */
        case 133: /* mknod */
        case 161: /* chroot */
          upath = (const char *)(usize)arg0;
          break;
        case 254: /* inotify_add_watch */
        case 257: /* openat */
        case 258: /* mkdirat */
        case 262: /* newfstatat */
        case 263: /* unlinkat */
        case 267: /* readlinkat */
        case 269: /* faccessat */
          upath = (const char *)(usize)arg1;
          break;
        default:
          break;
        }
        char pbuf[128];
        pbuf[0] = '\0';
        if (upath && strncpy_from_user(pbuf, upath, sizeof(pbuf)) < 0)
          pbuf[0] = '\0';
        /* The call's name, for a Linux-personality task. A number is a lookup
         * every reader of the log has to do by hand, and the numbering is the
         * one thing about this trace that is not obvious. Only Linux-ABI tasks
         * get a name: for a native task the same integer means something else
         * entirely, and a confidently wrong name is worse than none. */
        const char *cname = "";
        if (current_task && current_task->user_image &&
            ((struct user_loaded_image *)current_task->user_image)
                    ->personality == PERSONALITY_LINUX)
          cname = linux_syscall_name(number);
        char el[256];
        if (pbuf[0])
          snprintf(el, sizeof(el),
                   "errno %u from syscall %llu %s (pid %u) path=%s",
                   (unsigned)e, (unsigned long long)number, cname,
                   (unsigned)pid, pbuf);
        else
          snprintf(el, sizeof(el), "errno %u from syscall %llu %s (pid %u)",
                   (unsigned)e, (unsigned long long)number, cname,
                   (unsigned)pid);
        klog_info(el);
      }
    }
  }
  g_trace_in_call = 0;

  if (watch_addr) {
    u64 after = 0;

    if (syscall_copyin(&after, (void *)(usize)watch_addr, 8) == 0 &&
        after != watch_before) {
      char wl[160];

      snprintf(wl, sizeof(wl),
               "WATCH pid=%u nr=%llu changed 0x%llx: %llx -> %llx",
               (unsigned)current_task->id, (unsigned long long)number,
               (unsigned long long)watch_addr,
               (unsigned long long)watch_before, (unsigned long long)after);
      klog_debug_category("syscall", wl);
    }
  }

  if (trace_this) {
    char line[192];

    snprintf(line, sizeof(line),
             "pid=%u nr=%llu (%llx, %llx, %llx, %llx, %llx, %llx) = %llx",
             (unsigned)current_task->id, (unsigned long long)number,
             (unsigned long long)arg0, (unsigned long long)arg1,
             (unsigned long long)arg2, (unsigned long long)arg3,
             (unsigned long long)arg4, (unsigned long long)arg5,
             (unsigned long long)ret);
    klog_debug_category("syscall", line);
  }

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
    /* PTRACE_SYSCALL exit stop: the tracer sees the return value in rax and may
     * replace it before the task resumes. */
    if (current_task && ptrace_any_traced()) {
      ptrace_syscall_stop(current_task, frame, 1);
      ret = frame->rax;
    }
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

  /* Which call this task is in, for the task dump. A thread that sits at the
   * same user RIP for minutes is either looping inside one call or repeating
   * it, and only the number tells those apart. */
  task_note_syscall(number);

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
      /* open(2) (Linux nr 2) carries Linux O_* bits, which collide with the
       * b1nix ones — Linux O_NONBLOCK is b1nix O_CLOEXEC and Linux O_DIRECT is
       * b1nix O_NONBLOCK. The plain number map would pass them through
       * unchanged, so a musl open(path, O_WRONLY|O_NONBLOCK) silently became a
       * blocking open with FD_CLOEXEC. Route it through the same whitelist the
       * openat shim uses. */
      if (number == 2) {
        char kp[VFS_MAX_PATH], rp[VFS_MAX_PATH];
        int cs = syscall_copyinstr(kp, sizeof(kp), (const char *)(usize)arg0);
        if (cs < 0)
          return (u64)(isize)cs; /* keep ENAMETOOLONG distinct from EFAULT */
        vfs_resolve_path(kp, rp);
        return (u64)vfs_open_flags_mode(rp, linux_open_flags_to_b1nix((int)arg1),
                                        (u16)arg2);
      }
      if (number == LINUX_NR_RT_SIGPROCMASK)
        return (u64)sys_linux_rt_sigprocmask((int)arg0, arg1, arg2);
      /* rt_sigtimedwait(128): the set uses Linux signal numbering, and the
       * caller gets a Linux siginfo_t back. Translate both ends around the
       * native wait. */
      if (number == 128) {
        if (!arg0)
          return (u64)-EFAULT;
        u64 lx_mask = 0;
        if (syscall_copyin(&lx_mask, (const void *)(usize)arg0,
                           sizeof(lx_mask)) < 0)
          return (u64)-EFAULT;
        u64 b_mask = 0;
        for (int l = 1; l <= 64; l++) {
          if (!(lx_mask & (1ULL << (l - 1))))
            continue;
          int b = linux_signo_to_b1nix(l);
          if (b > 0 && b <= NSIG)
            b_mask |= (1ULL << (b - 1));
        }
        isize wr = sys_sigtimedwait_kernel(b_mask,
                                           (const struct timespec *)(usize)arg2);
        if (wr < 0)
          return (u64)wr;
        int lx_sig = b1nix_signo_to_linux((int)wr);
        if (arg1) {
          /* Minimal Linux siginfo_t: si_signo, si_errno, si_code. */
          i32 si[4] = {lx_sig, 0, 0, 0};
          if (syscall_copyout((void *)(usize)arg1, si, sizeof(si)) != 0)
            return (u64)-EFAULT;
        }
        return (u64)lx_sig;
      }

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
       * alarm (scheduler ticks) plus a repeat interval so periodic SIGALRM works
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
          if (dl > now)
            sc_ticks_to_time(dl - now, &itv.val_sec, &itv.val_usec, 1000000ull);
          u64 iv = task_alarm_interval_ticks(current_task);
          sc_ticks_to_time(iv, &itv.int_sec, &itv.int_usec, 1000000ull);
          if (arg1 &&
              syscall_copyout((void *)(usize)arg1, &itv, sizeof(itv)) < 0)
            return (u64)-EFAULT;
          return 0;
        }
        /* setitimer(which, new, old) */
        struct lx_itimerval old;
        memset(&old, 0, sizeof(old));
        u64 odl = task_alarm_ticks(current_task);
        if (odl > now)
          sc_ticks_to_time(odl - now, &old.val_sec, &old.val_usec, 1000000ull);
        u64 oiv = task_alarm_interval_ticks(current_task);
        sc_ticks_to_time(oiv, &old.int_sec, &old.int_usec, 1000000ull);
        if (!arg1)
          return (u64)-EFAULT;
        if (syscall_copyin(&itv, (void *)(usize)arg1, sizeof(itv)) < 0)
          return (u64)-EFAULT;
        u64 val_ticks = sc_time_to_ticks(itv.val_sec, itv.val_usec, 1000000ull);
        u64 int_ticks = sc_time_to_ticks(itv.int_sec, itv.int_usec, 1000000ull);
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
      /* tkill(tid, sig) / tgkill(tgid, tid, sig): b1nix tids are task ids, so
       * the tid targets a thread directly. tgkill also verifies the thread
       * group, so a tid recycled into another process is rejected rather than
       * signalled (M86). Remap the signo. */
      if (number == LINUX_NR_TKILL) {
        usize tid = namespace_pid_from_user((usize)arg0);
        if (!tid)
          return (u64)-ESRCH;
        return (u64)scheduler_tkill(0, tid, linux_signo_to_b1nix((int)arg1));
      }
      if (number == LINUX_NR_TGKILL) {
        usize tgid = namespace_pid_from_user((usize)arg0);
        usize tid = namespace_pid_from_user((usize)arg1);
        if (!tgid || !tid)
          return (u64)-ESRCH;
        return (u64)scheduler_tkill(tgid, tid,
                                    linux_signo_to_b1nix((int)arg2));
      }
      /* waitid(247): idtype/id/options values match b1nix, but the siginfo
       * layouts differ (b1nix packs 6 ints; Linux is a 128-byte struct with
       * si_errno/si_code swapped and the CLD fields at offset 16). Let
       * scheduler_waitid write its b1nix siginfo into the user's (larger)
       * buffer, read it back, and rewrite it in the Linux layout. */
      if (number == 247) {
        /* waitid(2)'s fourth idtype: the id is a pidfd rather than a pid. */
        enum { LX_P_PIDFD = 3 };
        usize wid = (usize)arg1;
        u64 widtype = arg0;
        /* P_PIDFD: wait for the process a pidfd holds. It is the same wait as
         * P_PID once the descriptor has been read back into a pid -- the
         * difference is that the descriptor cannot have started meaning a
         * different process in the meantime, which is the whole point of
         * having it.
         *
         * systemd runs every generator this way: it forks, takes a pidfd, and
         * waits on the descriptor. Without this the wait answered EINVAL, and
         * PID 1 reported "Failed to wait for
         * /usr/lib/systemd/system-environment-generators/10-arch" and then
         * "Failed to start up manager" -- the boot ended on the first
         * generator Arch ships. */
        if (widtype == LX_P_PIDFD) {
          struct vfs_handle *wh = scheduler_fd_get((int)arg1);
          if (!wh)
            return (u64)-EBADF;
          usize wpid = vfs_pidfd_pid(wh);
          if (!wpid)
            return (u64)-EBADF;
          widtype = P_PID;
          wid = wpid; /* already a kernel pid: no namespace translation */
        } else if ((arg0 == P_PID || arg0 == P_PGID) && arg1 != 0 &&
                   !(wid = namespace_pid_from_user((usize)arg1))) {
          return (u64)-ECHILD;
        }
        int wr = scheduler_waitid((idtype_t)widtype, wid,
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
          *(i32 *)&lx[16] = (i32)namespace_pid_to_user((usize)ki.si_pid);
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
        if (arg0 != 0 && namespace_pid_from_user((usize)arg0) !=
                             scheduler_get_pid())
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
      /* klogctl(103)(type, bufp, len): dmesg, klogd and logread. Served from
       * the structured record ring (kernel/dev/kmsg.c) so a reader gets whole
       * messages with priorities and sequence numbers, and READ really
       * consumes what it returns. */
      if (number == 103) {
        int type = (int)arg0;
        return (u64)kmsg_syslog(type, (char *)(usize)arg1, (int)arg2);
      }

      /* --- M92: *at() syscall emulation for musl ---
       * musl uses *at() variants (openat, newfstatat, etc.) exclusively. These
       * resolve dirfd + relative path to an absolute path, then delegate to the
       * existing b1nix handler. AT_FDCWD (-100) means "use cwd". */

#define LX_openat          257
#define LX_newfstatat      262
#define LX_unlinkat        263
#define LX_mkdirat         258
#define LX_mknodat         259
#define LX_linkat          265
#define LX_symlinkat       266
#define LX_readlinkat      267
#define LX_fchmodat        268
#define LX_fchownat        260
#define LX_faccessat       269
#define LX_faccessat2      439
/* fchmodat2(dirfd, path, mode, flags): fchmodat with the flags argument the
 * original call never had. glibc 2.39 and later issue it for every
 * fchmodat(3) that passes a flag, falling back only when it answers ENOSYS --
 * and a fallback that has to be discovered costs a syscall per call. */
#define LX_fchmodat2       452
/* close_range(first, last, flags): shut a whole range of descriptors in one
 * call. It is how glibc implements closefrom(3) and how systemd closes the
 * descriptors it does not want a child to inherit; without it both walk
 * /proc/self/fd instead, which is a directory read per exec. */
#define LX_close_range     436
/* pidfd_open(pid, flags) and pidfd_send_signal(pidfd, sig, info, flags). A
 * descriptor that names a process rather than a number that identifies one
 * today; see vfs_pidfd_open. */
#define LX_pidfd_open      434
#define LX_pidfd_send_signal 424
#define LX_renameat2       316
/* renameat2 flags (Linux uapi/linux/fs.h). */
#define RENAME_NOREPLACE (1u << 0)
#define RENAME_EXCHANGE  (1u << 1)
#define RENAME_WHITEOUT  (1u << 2)
#define LX_renameat        264
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
#define LX_clone3          435
#define LX_epoll_pwait     281
#define LX_epoll_pwait2    441
/* clone3 flags this kernel does not model, in the high word of clone_args. */
#define LX_CLONE_PIDFD        0x00001000ULL
#define LX_CLONE_DETACHED     0x00400000ULL
#define LX_CLONE_INTO_CGROUP  0x200000000ULL

      /* Resolve a dirfd + user path to an absolute kernel path.
       * dirfd == AT_FDCWD (-100) or an absolute path → resolve normally.
       * Otherwise, get the absolute path of dirfd and join with the relative path.
       * Returns 0 on success, -errno on failure. kbuf must be VFS_MAX_PATH. */
      /* symlinkat(target, newdirfd, linkpath) does NOT have (dirfd, path) as
       * its first two arguments: arg0 is the symlink's CONTENT and arg1 is the
       * descriptor. The generic block below read arg1 as a user string, so
       * every symlinkat copied from the address 0xffffff9c (AT_FDCWD) and
       * returned EFAULT — "Failed to create symlink '/var/lib/dbus/machine-id':
       * Bad address" from systemd-tmpfiles, and the same for every symlink any
       * glibc program creates through the *at form. */
      if (number == LX_symlinkat) {
        char target[VFS_MAX_PATH];
        if (syscall_copyinstr(target, sizeof(target),
                              (const char *)(usize)arg0) < 0)
          return (u64)-EFAULT;
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

      /* close_range(first, last, flags): close every descriptor in the
       * inclusive range. glibc's closefrom(3) and systemd's own
       * close_all_fds() both reach for it before falling back to reading
       * /proc/self/fd, and the fallback costs a directory walk on every exec.
       *
       * CLOSE_RANGE_UNSHARE (bit 1) asks for the descriptor table to be
       * unshared first, which is only meaningful with CLONE_FILES; b1nix does
       * not share tables that way, so there is nothing to unshare and the flag
       * is accepted. CLOSE_RANGE_CLOEXEC (bit 2) marks rather than closes.
       * A descriptor that is not open is not an error -- the range is a range,
       * not a list. */
      /* pidfd_open(pid, flags): take a reference to a process. */
      if (number == LX_pidfd_open)
        return (u64)(isize)vfs_pidfd_open((usize)arg0, (int)arg1);

      /* pidfd_send_signal(pidfd, sig, info, flags): deliver a signal to the
       * process the descriptor holds. The whole reason it exists is that the
       * descriptor cannot have come to mean a different process since it was
       * opened, which kill(2) on a pid cannot promise.
       *
       * `info` is not read: b1nix's signal delivery builds the siginfo itself
       * and has no way to carry a caller-supplied one, so accepting a pointer
       * and quietly ignoring its contents would be worse than refusing it. A
       * NULL info -- what every caller in practice passes, and what systemd
       * passes -- is honoured. */
      if (number == LX_pidfd_send_signal) {
        if (arg3 != 0)
          return (u64)-EINVAL;
        if (arg2 != 0)
          return (u64)-EOPNOTSUPP;
        struct vfs_handle *ph = scheduler_fd_get((int)arg0);
        if (!ph)
          return (u64)-EBADF;
        usize target = vfs_pidfd_pid(ph);
        if (!target)
          return (u64)-EBADF;
        int sig = (int)arg1;
        if (sig < 0 || sig >= NSIG)
          return (u64)-EINVAL;
        if (sig == 0) {
          /* The existence probe: no signal is sent, the answer is whether the
           * process is still there. */
          struct task *tt = scheduler_task_by_pid(target);
          if (!tt || tt->state == TASK_DEAD || tt->state == TASK_REAPING ||
              tt->state == TASK_UNUSED)
            return (u64)-ESRCH;
          return 0;
        }
        return (u64)(isize)scheduler_kill_thread_group_user(target, sig);
      }

      if (number == LX_close_range) {
        enum { LX_CLOSE_RANGE_UNSHARE = 1u << 1,
               LX_CLOSE_RANGE_CLOEXEC = 1u << 2 };
        u32 first = (u32)arg0;
        u32 last = (u32)arg1;
        u32 cr_flags = (u32)arg2;
        if (cr_flags & ~(u32)(LX_CLOSE_RANGE_UNSHARE | LX_CLOSE_RANGE_CLOEXEC))
          return (u64)-EINVAL;
        if (first > last)
          return (u64)-EINVAL;
        usize cap = current_task ? current_task->fd_capacity : 0;
        u64 hi = last;
        if (cap && hi > (u64)cap - 1)
          hi = (u64)cap - 1;
        for (u64 fd = first; cap && fd <= hi; fd++) {
          if (!scheduler_fd_get((int)fd))
            continue;
          if (cr_flags & LX_CLOSE_RANGE_CLOEXEC)
            scheduler_fd_flags_set((int)fd, B1NIX_FD_CLOEXEC);
          else
            vfs_close((int)fd);
        }
        return 0;
      }
      if (number == LX_openat || number == LX_newfstatat ||
          number == LX_unlinkat || number == LX_mkdirat ||
          number == LX_mknodat ||
          number == LX_fchmodat || number == LX_fchmodat2 ||
          number == LX_fchownat ||
          number == LX_faccessat || number == LX_faccessat2 ||
          number == LX_readlinkat ||
          number == LX_renameat2 ||
          number == LX_renameat ||
          number == LX_linkat) {
        int dirfd = (int)arg0;
        const char *user_path = (const char *)(usize)arg1;
        char kpath[VFS_MAX_PATH];
        if (syscall_copyinstr(kpath, sizeof(kpath), user_path) < 0)
          return (u64)-EFAULT;
        /* AT_EMPTY_PATH: an empty path means "the file dirfd already refers
         * to". glibc's fstat() is exactly this — newfstatat(fd, "", &st,
         * AT_EMPTY_PATH) — so without it every library glibc's loader opened
         * failed at "cannot stat shared object": the empty path was appended
         * to the fd's own path, producing "…/libc.so.6/" and -ENOTDIR. */
        if (kpath[0] == '\0') {
          int at_flags;
          switch (number) {
          case LX_newfstatat:
          case LX_faccessat2:
          case LX_fchmodat2:
            at_flags = (int)arg3;
            break;
          /* fchownat(dirfd, path, owner, group, flags): the flags are the
           * FIFTH argument. Reading the fourth found the GROUP id there, which
           * never has AT_EMPTY_PATH set, so every fchownat(fd, "", u, g,
           * AT_EMPTY_PATH) — how systemd copies ownership onto a temporary
           * file — was rejected with ENOENT. systemd-sysusers died on it while
           * writing /etc/gshadow, so the users dbus needs were never created
           * and the bus refused to start. */
          case LX_fchownat:
          case LX_linkat:
            at_flags = (int)arg4;
            break;
          default:
            at_flags = 0;
            break;
          }
          /* fstat(2) on a descriptor: answer from the descriptor itself, not
           * from a path. A pipe, a socket or a deleted file has no path to
           * resolve — that is why `cmd | tail` reported "cannot fstat
           * 'standard input': Bad file descriptor", and why cat's check on
           * its own stdout failed with ENOENT once devtmpfs had been mounted
           * over the /dev the console fd was opened from. */
          if (number == LX_newfstatat && (at_flags & AT_EMPTY_PATH) &&
              dirfd != AT_FDCWD) {
            struct b1nix_stat st;
            int rc = vfs_fstat(dirfd, &st);
            if (rc < 0)
              return (u64)rc;
            struct linux_stat lst;
            linux_stat_from_b1nix(&lst, &st);
            if (syscall_copyout((void *)(usize)arg2, &lst, sizeof(lst)) < 0)
              return (u64)-EFAULT;
            return 0;
          }
          /* Likewise for chown: answer from the descriptor. A path lookup is
           * a fallback that cannot work for a file with no name. */
          if (number == LX_fchownat && (at_flags & AT_EMPTY_PATH) &&
              dirfd != AT_FDCWD)
            return (u64)vfs_fchown(dirfd, (u16)arg2, (u16)arg3);
          if (!(at_flags & AT_EMPTY_PATH) || dirfd == AT_FDCWD)
            return (u64)-ENOENT;
          int rc = vfs_fd_abspath(dirfd, kpath, sizeof(kpath));
          if (rc < 0)
            return (u64)rc;
        }
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
          /* arg0=dirfd, arg1=path, arg2=flags, arg3=mode. */
          return (u64)vfs_open_flags_mode(
              resolved, linux_open_flags_to_b1nix((int)arg2), (u16)arg3);
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
        case LX_mknodat:
          /* arg0=dirfd, arg1=path, arg2=mode, arg3=dev. musl's mkfifo() reaches
           * here on builds without a plain SYS_mknod. */
          return (u64)vfs_mknod(resolved, (u32)arg2, arg3);
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
        /* fchmodat2 carries a flags word fchmodat does not; b1nix has no
         * per-symlink modes, so AT_SYMLINK_NOFOLLOW has nothing to change and
         * the two are the same operation here. AT_EMPTY_PATH is handled by the
         * shared block above. */
        case LX_fchmodat2:
        case LX_fchmodat:
          return (u64)vfs_chmod(resolved, (u16)arg2);
        case LX_fchownat:
          return (u64)vfs_chown(resolved, (u16)arg2, (u16)arg3);
        case LX_faccessat2: /* same shape, flags are just advisory here */
        case LX_faccessat: {
          /* arg0=dirfd, arg1=path, arg2=mode, arg3=flags.
           * Linux faccessat uses the real mode (R_OK=4, W_OK=2, X_OK=1, F_OK=0).
           * b1nix SYS_ACCESS takes (path, mode) too. Ignore flags (AT_EACCESS). */
          (void)arg3;
          return (u64)sys_access_kpath(resolved, (int)arg2);
        }
        /* renameat(264) is renameat2 without the flags argument. */
        case LX_renameat:
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
          /* arg4=flags. These used to be ignored and the plain rename run
           * anyway, which is the one outcome a caller must never get: a
           * program that asks NOT to clobber the destination was told the
           * rename succeeded, having just destroyed the file it was
           * protecting. renameat(264) carries no flags argument at all, so it
           * is only read for renameat2. */
          if (number == LX_renameat2) {
            u32 rflags = (u32)arg4;

            if (rflags & ~(RENAME_NOREPLACE | RENAME_EXCHANGE |
                           RENAME_WHITEOUT))
              return (u64)-EINVAL;
            /* NOREPLACE and EXCHANGE are mutually exclusive by definition:
             * one requires the destination to be absent, the other requires it
             * to be present. */
            if ((rflags & RENAME_NOREPLACE) && (rflags & RENAME_EXCHANGE))
              return (u64)-EINVAL;
            if (rflags & RENAME_NOREPLACE) {
              struct vfs_node *existing = vfs_find_node(new_resolved);

              if (existing) {
                vfs_node_put(existing);
                return (u64)-EEXIST;
              }
            }
            /* EXCHANGE has to be atomic — swapping by hand through two renames
             * is exactly the non-atomicity callers use it to avoid — and
             * WHITEOUT needs an overlay layer this kernel has no notion of.
             * EINVAL is what Linux returns for a filesystem that does not
             * implement them, and it is what makes the caller fall back
             * instead of trusting a swap that never happened. */
            if (rflags & (RENAME_EXCHANGE | RENAME_WHITEOUT))
              return (u64)-EINVAL;
          }
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
#undef LX_renameat
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
      /* epoll_pwait(epfd, events, maxevents, timeout_ms, sigmask, sigsetsize).
       * The argument layout below epoll_wait is identical, so install the wait
       * mask and let the table remap run the wait itself. */
      if (number == LX_epoll_pwait && arg4) {
        u64 lx_mask = 0;
        if (syscall_copyin(&lx_mask, (void *)(usize)arg4, sizeof(lx_mask)) < 0)
          return (u64)-EFAULT;
        syscall_wait_mask_install(lx_mask);
      }
      if (number == LX_ppoll) {
        /* ppoll(fds, nfds, timeout_ts, sigmask, sigsetsize). timeout_ts is a
         * pointer to struct timespec (seconds + nanoseconds), convert to ms. */
        if (arg3) {
          u64 lx_mask = 0;
          if (syscall_copyin(&lx_mask, (void *)(usize)arg3, sizeof(lx_mask)) < 0)
            return (u64)-EFAULT;
          syscall_wait_mask_install(lx_mask);
        }
        int nfds = (int)arg1;
        /* struct timespec: tv_sec (8 bytes), tv_nsec (8 bytes). Kept in
         * nanoseconds — rounding it to milliseconds turned a sub-millisecond
         * wait into a spin or stretched it to a full tick. */
        if (!arg2)
          return (u64)sys_poll_ns((void *)(usize)arg0, nfds, 0, 1);
        u64 tv_sec = 0, tv_nsec = 0;
        if (syscall_copyin(&tv_sec, (void *)(usize)arg2, sizeof(tv_sec)) < 0 ||
            syscall_copyin(&tv_nsec, (void *)(usize)(arg2 + 8),
                           sizeof(tv_nsec)) < 0)
          return (u64)-EFAULT;
        if ((i64)tv_sec < 0 || (i64)tv_nsec < 0 || tv_nsec >= 1000000000ull)
          return (u64)-EINVAL;
        return (u64)sys_poll_ns((void *)(usize)arg0, nfds,
                                tv_sec * 1000000000ull + tv_nsec, 0);
      }
      if (number == LX_pselect6) {
        /* pselect6(nfds, readfds, writefds, exceptfds, timeout_ts, sigmask).
         * Convert fd_sets to pollfds and use the poll loop; timeout_ts is a
         * pointer to struct timespec, kept in nanoseconds. The last argument is not
         * the mask itself but a pointer to {const sigset_t *, size_t} — Linux
         * ran out of register arguments and boxed the pair. */
        if (arg5) {
          struct { u64 ss; u64 len; } box = {0, 0};
          if (syscall_copyin(&box, (void *)(usize)arg5, sizeof(box)) < 0)
            return (u64)-EFAULT;
          if (box.ss) {
            u64 lx_mask = 0;
            if (syscall_copyin(&lx_mask, (void *)(usize)box.ss,
                               sizeof(lx_mask)) < 0)
              return (u64)-EFAULT;
            syscall_wait_mask_install(lx_mask);
          }
        }
        int nfds = (int)arg0;
        if (nfds < 0 || nfds > 1024)
          return (u64)-EINVAL;
        int ps_infinite = 1;
        u64 ps_timeout_ns = 0;
        if (arg4) {
          u64 tv_sec = 0, tv_nsec = 0;
          if (syscall_copyin(&tv_sec, (void *)(usize)arg4, sizeof(tv_sec)) < 0 ||
              syscall_copyin(&tv_nsec, (void *)(usize)(arg4 + 8),
                             sizeof(tv_nsec)) < 0)
            return (u64)-EFAULT;
          if ((i64)tv_sec < 0 || (i64)tv_nsec < 0 || tv_nsec >= 1000000000ull)
            return (u64)-EINVAL;
          ps_infinite = 0;
          ps_timeout_ns = tv_sec * 1000000000ull + tv_nsec;
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
        /* Use the same inline poll loop as SYS_SELECT, on the same nanosecond
         * deadline sys_poll_ns uses. */
        u64 ps_tick_ns = 1000000000ull / (u64)sched_tick_hz();
        u64 ps_deadline_ns =
            ps_infinite ? 0 : arch_tsc_monotonic_ns() + ps_timeout_ns;
        if (!ps_infinite && ps_timeout_ns != 0) {
          u64 ticks = (ps_timeout_ns + ps_tick_ns - 1) / ps_tick_ns;
          if (ticks == 0)
            ticks = 1;
          current_task->wake_tick = scheduler_get_uptime_ticks() + ticks;
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
          if (ready_count == 0 && !ps_infinite && ps_timeout_ns != 0 &&
              arch_tsc_monotonic_ns() >= ps_deadline_ns)
            timed_out = 1;
          if (ready_count > 0 || (!ps_infinite && ps_timeout_ns == 0) ||
              timed_out) {
            scheduler_wait_cancel();
            break;
          }
          if (select_poll_signal_pending()) {
            scheduler_wait_cancel();
            return (u64)-EINTR;
          }
          /* Re-arm every iteration — an explicit wake clears wake_tick (see
           * sys_poll for the full unbounded-sleep failure this prevents). */
          if (!ps_infinite && ps_timeout_ns != 0) {
            u64 now_ns = arch_tsc_monotonic_ns();
            u64 rest_ns =
                ps_deadline_ns > now_ns ? ps_deadline_ns - now_ns : 0;
            u64 ticks = (rest_ns + ps_tick_ns - 1) / ps_tick_ns;
            if (ticks == 0)
              ticks = 1;
            current_task->wake_tick = scheduler_get_uptime_ticks() + ticks;
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
        /* accept4(sockfd, addr, addrlen, flags): accept + set FD_CLOEXEC and
         * O_NONBLOCK from flags.
         *
         * Through sys_accept, NOT straight into vfs_accept with the caller's
         * pointers. Handing user addresses to the VFS meant the peer address
         * was written into the caller's buffer with no regard for how big the
         * caller said it was, and the socklen_t beside it was written as a
         * 64-bit word over a 32-bit field. dbus-daemon accepts with a 16-byte
         * `struct sockaddr` on its stack; both writes ran past it, and glibc's
         * canary caught the wreckage as "*** stack smashing detected ***" —
         * SIGABRT, and the system bus gone on the first connection it
         * served. */
        u64 arc = sys_accept((int)arg0, (void *)(usize)arg1,
                             (usize *)(usize)arg2);
        if ((isize)arc < 0)
          return arc;
        int rc = (int)arc;
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
        (void)syscall_sleep_timespec(&ts, 0);
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
        (void)syscall_sleep_timespec(&ts, 0);
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
        return (u64)namespace_pid_to_user(scheduler_get_pid());
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
         *
         * Only the calling process's own limits are addressable (pid 0 or our
         * own pid); Linux allows other pids under CAP_SYS_RESOURCE, which this
         * kernel does not model. The old value is fetched BEFORE the new one is
         * installed, which is the whole point of the combined call and what
         * glibc's setrlimit64 and every shell's `ulimit` rely on. */
        usize pid = (usize)arg0;
        if (pid != 0 && pid != namespace_pid_to_user(scheduler_get_pid()))
          return (u64)-EPERM;
        if (arg3) {
          isize r = sys_getrlimit((int)arg1, (void *)(usize)arg3);
          if (r < 0)
            return (u64)r;
        }
        if (arg2)
          return (u64)sys_setrlimit((int)arg1, (const void *)(usize)arg2);
        return 0;
      }
      /* epoll_pwait2(epfd, events, maxevents, timespec*, sigmask, sigsetsize)
       * — epoll_pwait with a nanosecond timeout instead of a millisecond one.
       * The wait itself has millisecond resolution here, so the timeout is
       * rounded UP: a caller asking for 100 us must not be told "0", which
       * means "return immediately" and turns a poll loop into a spin. */
      if (number == LX_epoll_pwait2) {
        int timeout_ms = -1;
        if (arg3) {
          struct timespec ts;
          if (syscall_copyin(&ts, (const void *)(usize)arg3, sizeof(ts)) < 0)
            return (u64)-EFAULT;
          if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000L)
            return (u64)-EINVAL;
          u64 ms = (u64)ts.tv_sec * 1000ull + ((u64)ts.tv_nsec + 999999ull) / 1000000ull;
          if (ms > 0x7fffffffull)
            ms = 0x7fffffffull;
          timeout_ms = (int)ms;
        }
        int maxevents = (int)arg2;
        if (maxevents <= 0)
          return (u64)-EINVAL;
        enum { EPOLL_PWAIT2_BATCH = 64 };
        if (maxevents > EPOLL_PWAIT2_BATCH)
          maxevents = EPOLL_PWAIT2_BATCH;
        if (!arg1)
          return (u64)-EFAULT;
        struct b1nix_epoll_event kbuf[EPOLL_PWAIT2_BATCH];
        int n = vfs_epoll_wait((int)arg0, kbuf, maxevents, timeout_ms);
        if (n < 0)
          return (u64)(isize)n;
        if (n > 0 &&
            syscall_copyout((void *)(usize)arg1, kbuf,
                            (usize)n * sizeof(struct b1nix_epoll_event)) < 0)
          return (u64)-EFAULT;
        return (u64)n;
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
      /* clone3(&args, size) — what glibc 2.34+ reaches for first in
       * pthread_create and posix_spawn. It is the same thread creation as
       * clone(2) with the arguments moved into a versioned struct, plus two
       * things the flat call cannot express: the stack is given as the LOW
       * address of the mapping with an explicit size (so the kernel computes
       * the initial SP), and exit_signal is a full word rather than the low
       * byte of the flags. glibc falls back to clone(2) on -ENOSYS, so this is
       * not strictly required — but returning -ENOSYS costs every process a
       * failed syscall at start-up and leaves the newer interface untested. */
      if (number == LX_clone3) {
        struct lx_clone_args {
          u64 flags;
          u64 pidfd;
          u64 child_tid;
          u64 parent_tid;
          u64 exit_signal;
          u64 stack;
          u64 stack_size;
          u64 tls;
          u64 set_tid;
          u64 set_tid_size;
          u64 cgroup;
        } ca;
        usize size = (usize)arg1;
        /* Linux accepts any size from CLONE_ARGS_SIZE_VER0 (64) upwards and
         * requires the tail beyond what it knows to be zero. Copy what we
         * understand and ignore a longer tail. */
        if (size < 64)
          return (u64)-EINVAL;
        memset(&ca, 0, sizeof(ca));
        usize copy = size < sizeof(ca) ? size : sizeof(ca);
        if (syscall_copyin(&ca, (const void *)(usize)arg0, copy) < 0)
          return (u64)-EFAULT;
        /* Not modelled: caller-chosen tids. */
        if (ca.set_tid || ca.set_tid_size)
          return (u64)-EINVAL;
        /* CLONE_INTO_CGROUP names the child's cgroup with a directory
         * descriptor. Without the flag the `cgroup` field means nothing, so a
         * value there is a caller asking for something it did not request. */
        if (!(ca.flags & LX_CLONE_INTO_CGROUP) && ca.cgroup)
          return (u64)-EINVAL;
        if ((ca.flags & LX_CLONE_PIDFD) &&
            (!ca.pidfd || (ca.flags & (B1NIX_CLONE_THREAD | LX_CLONE_DETACHED))))
          return (u64)-EINVAL;
        if (ca.exit_signal > 63)
          return (u64)-EINVAL;
        /* The child's stack pointer is the TOP of the region, as Linux
         * documents: "stack points to the lowest byte". A zero stack means
         * fork-like sharing of the caller's, which the clone path handles. */
        u64 child_sp = ca.stack ? ca.stack + ca.stack_size : 0;
        u64 flags = ca.flags | (ca.exit_signal & 0xff);
        if (!(flags & B1NIX_CLONE_VM) && child_sp == 0) {
          /* CLONE_PIDFD, the clone3 spelling: the descriptor goes into the
           * `pidfd` field of the argument block rather than into parent_tid.
           * Same reason as in clone(2) above -- this is how a modern manager
           * gets a reference to the child it just made. */
          int nsrc = clone_prepare_namespaces(ca.flags);
          if (nsrc < 0)
            return (u64)(isize)nsrc;
          isize kid =
              scheduler_fork_clone(flags, ca.parent_tid, ca.child_tid);
          if (kid < 0)
            namespace_child_prepare_abort();
          /* CLONE_INTO_CGROUP: move the child out of the cgroup it inherited
           * and into the one the descriptor names. Done before the caller is
           * told the pid, so nothing can observe it in the wrong group. */
          if ((ca.flags & LX_CLONE_INTO_CGROUP) && kid > 0)
            (void)cgroup_attach_pid_at_fd((int)ca.cgroup, (usize)kid);
          if ((ca.flags & LX_CLONE_PIDFD) && kid > 0) {
            int pfd = vfs_pidfd_open((usize)kid, 0);
            i32 pfd32 = pfd < 0 ? -1 : (i32)pfd;
            syscall_copyout((void *)(usize)ca.pidfd, &pfd32, sizeof(pfd32));
          }
          return ns_pid_out((u64)kid);
        }
        u64 child_entry = frame ? frame->rip : child_sp;
        struct clone_user_regs uregs;
        clone_regs_from_frame(&uregs, frame);
        return (u64)scheduler_clone_thread(flags, child_entry, child_sp, 0,
                                           ca.tls, ca.child_tid, ca.parent_tid,
                                           ca.child_tid, arg5,
                                           frame ? &uregs : 0);
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
        /*
         * The caller's flags, exactly as given.
         *
         * A pointer argument is not a request to write through it: Linux writes
         * the new thread's id into child_tidptr only for CLONE_CHILD_SETTID,
         * and musl asks for CLONE_CHILD_CLEARTID instead — passing
         * &__thread_list_lock, one of libc's own globals, as that pointer. So
         * inferring SETTID from "the pointer is non-NULL" stamped a thread id
         * over musl's thread-list lock on every pthread_create. The damage
         * surfaced far away, as a heap consistency check firing inside free(),
         * because that is where the wreckage was next touched.
         */
        /* clone(2) without CLONE_VM and without a stack is fork(2): the child
         * gets a copy of the address space and continues on the same stack.
         * scheduler_clone_thread only ever makes threads and rejected a NULL
         * stack outright with -EFAULT, which is what glibc reported as
         * "Cannot fork" — musl never hit it because its fork passes plain
         * SIGCHLD and took the native fork path. */
        if (!(flags & B1NIX_CLONE_VM) && user_stack == 0) {
          /* CLONE_PIDFD: the caller wants a descriptor for the child, and it
           * comes back through the parent_tid argument -- clone(2) reuses that
           * pointer for it, which is why the two flags are mutually exclusive.
           *
           * This is how systemd forks EVERY child: safe_fork_full() asks for a
           * pidfd and then waits for the child through it, in its event loop.
           * The flag used to be ignored, so the pointer was never written and
           * systemd waited on whatever descriptor its variable happened to
           * hold -- which is never the child. The generators ran, exited, and
           * were reaped, and PID 1 sat in epoll_wait for the rest of the boot
           * waiting to be told about it. */
          if (flags & LX_CLONE_PIDFD) {
            if (!parent_tid ||
                (flags & (B1NIX_CLONE_THREAD | LX_CLONE_DETACHED)))
              return (u64)-EINVAL;
            /* The pointer is checked before the fork: a child that exists and
             * a caller that cannot be told its descriptor is the worst of both
             * answers. */
            i32 probe = -1;
            if (syscall_copyout((void *)(usize)parent_tid, &probe,
                                sizeof(probe)) < 0)
              return (u64)-EFAULT;
          }
          int nsrc = clone_prepare_namespaces(flags);
          if (nsrc < 0)
            return (u64)(isize)nsrc;
          isize kid = scheduler_fork_clone(
              flags, (flags & LX_CLONE_PIDFD) ? 0 : parent_tid, child_tid);
          if (kid < 0)
            namespace_child_prepare_abort();
          if ((flags & LX_CLONE_PIDFD) && kid > 0) {
            /* Runs in the parent only: the child returns 0 from the fork. */
            int pfd = vfs_pidfd_open((usize)kid, 0);
            i32 pfd32 = (i32)pfd;
            if (pfd < 0)
              pfd32 = -1;
            syscall_copyout((void *)(usize)parent_tid, &pfd32, sizeof(pfd32));
          }
          return ns_pid_out((u64)kid);
        }

        u64 b1nix_flags = flags;
        /* M92: musl's __clone stores the start_routine pointer in r9 before
         * calling syscall. The child expects to resume at the parent's RIP
         * (right after the syscall instruction in musl __clone), with rax=0
         * and r9 = start_routine. arg5 is the saved r9 from the interrupt
         * frame. We pass frame->rip as the entry so the child thread
         * continues at the correct instruction. */
        u64 start_func = arg5; /* musl puts fn in r9 before syscall */
        u64 child_entry = frame ? frame->rip : user_stack;
        struct clone_user_regs uregs;
        clone_regs_from_frame(&uregs, frame);
        return (u64)scheduler_clone_thread(b1nix_flags, child_entry,
                                           user_stack, 0, tls_val, child_tid,
                                           parent_tid, child_tid, start_func,
                                           frame ? &uregs : 0);
      }
      /* Futex with extended ops for musl (WAIT_BITSET, WAKE_BITSET, REQUEUE,
       * CMP_REQUEUE, PRIVATE_FLAG). The dispatcher routes futex through
       * SYS_FUTEX after the table lookup, but the extended ops need special
       * handling here because the table entry uses the same native number. */
      if (number == LX_FUTEX) {
        int op = (int)arg1;
        int base_op = op & 0x7F; /* strip PRIVATE_FLAG (0x80) */
        /* ...but keep it: a PRIVATE futex is one nobody outside this address
         * space can name, and it is keyed accordingly. Dropping the flag here
         * merged two processes' private locks on the same shared file offset
         * into one key, so a wake in one released a waiter in the other. */
        int futex_priv = (op & 0x80) ? B1NIX_FUTEX_PRIVATE : 0;
        /* Which futex operations this kernel refuses, and how often.
         *
         * A wake that never reaches the futex code is indistinguishable, from
         * the waiter's side, from a wake that was never sent — and both look
         * like the process simply stopping. Counting the refusals says which
         * one is happening, and for which operation. */
        {
          extern void futex_note_op(int base_op, int served);
          int served = (base_op == 0 || base_op == 1 || base_op == 3 ||
                        base_op == 4 || base_op == 9 || base_op == 10);

          futex_note_op(base_op, served);
        }

        /* Record which futex operation, not merely "futex".
         * A ring full of 202s cannot tell a thread that is waiting from one
         * that is waking somebody, and that difference is the whole question
         * when everything is parked. 900 + op stays clear of every real call
         * number. */
        {
          extern void scheduler_note_syscall(u32 nr);

          scheduler_note_syscall(900u + (u32)base_op);
        }
        /* Plain FUTEX_WAIT(0)/FUTEX_WAKE(1), including the PRIVATE_FLAG
         * forms (0x80/0x81) that musl always uses for its internal
         * synchronization. These must be routed with the PRIVATE flag
         * stripped: scheduler_futex only knows B1NIX_FUTEX_WAIT=0/WAKE=1
         * and otherwise returns -EINVAL, which makes musl __wait/__lock
         * busy-spin forever instead of parking (the b1cc pthread wedge). */
        /* FUTEX_WAIT(0) / FUTEX_WAIT_BITSET(9): arg3 is a pointer to struct timespec
         * (or NULL for infinite wait). We MUST copy and parse the timespec from
         * userspace instead of passing the raw pointer address as timeout_ms,
         * which caused 4400-year timeouts for every timed futex wait. */
        if (base_op == 0 || base_op == 9) {
          u64 timeout_ms = 0;
          if (arg3) {
            u64 tv_sec = 0, tv_nsec = 0;
            /* A timespec this kernel cannot read is not "no timeout".
             *
             * Falling through with timeout_ms still zero turned a bounded wait
             * into an unbounded one: the caller asked to be woken in a
             * millisecond and slept until something unrelated happened to
             * disturb it. Report the fault instead, the way Linux does. */
            if (syscall_copyin(&tv_sec, (void *)(usize)arg3, sizeof(tv_sec)) != 0 ||
                syscall_copyin(&tv_nsec, (void *)(usize)(arg3 + 8),
                               sizeof(tv_nsec)) != 0) {
              extern void futex_note_bad_timespec(void);

              futex_note_bad_timespec();
              return (u64)-EFAULT;
            }
            {
              if (base_op == 9) {
                u64 req_ms = tv_sec * 1000 + tv_nsec / 1000000;
                u64 now_ms = 0;
                if (op & 256) {
                  now_ms = vfs_get_unix_time() * 1000;
                } else {
                  now_ms = scheduler_get_uptime_ticks() * (1000ull / sched_tick_hz());
                }
                if (req_ms > now_ms)
                  timeout_ms = req_ms - now_ms;
                else
                  timeout_ms = 1;
              } else {
                timeout_ms = tv_sec * 1000 + tv_nsec / 1000000;
                if (timeout_ms == 0)
                  timeout_ms = 1;
              }
            }
          }
          return (u64)scheduler_futex(arg0, B1NIX_FUTEX_WAIT | futex_priv, (int)arg2, timeout_ms);
        }
        if (base_op == 1 || base_op == 10)
          return (u64)scheduler_futex(arg0, B1NIX_FUTEX_WAKE | futex_priv, (int)arg2, 0);
        /* FUTEX_REQUEUE(3): wake val waiters on uaddr, requeue val2 to
         * uaddr2. FUTEX_CMP_REQUEUE(4) does the same after checking that
         * *uaddr still holds val3. These numbers were swapped here — 4 was
         * treated as REQUEUE and 8, which is TRYLOCK_PI, as CMP_REQUEUE. */
        if (base_op == 3 || base_op == 4) {
          if (base_op == 4) {
            int cur = 0;

            if (syscall_copyin(&cur, (void *)(usize)arg0, sizeof(int)) < 0)
              return (u64)-EFAULT;
            if (cur != (int)arg5)
              return (u64)-EAGAIN;
          }
          /* A real requeue: the waiters move, they are not woken where they
           * are and hoped to be woken again somewhere else. */
          int woken = scheduler_futex_requeue(arg0, arg4, (int)arg2, (int)arg3,
                                              futex_priv ? 1 : 0);

          return (u64)woken;
        }
        /* FUTEX_WAKE_OP(5): change a word, then wake on one or both queues.
         *
         * One call does what a release of two coupled objects otherwise needs
         * two of: it applies an arithmetic operation to *uaddr2, wakes up to
         * `val` waiters on uaddr, and — if the value uaddr2 HELD BEFORE the
         * operation satisfies a comparison — wakes up to `val2` waiters on
         * uaddr2 as well. Qt's semaphores and its read/write locks are built on
         * it, and refusing the op leaves the waiter parked forever: the release
         * that was supposed to reach it reports an error the caller ignores,
         * because on every other system the call cannot fail that way. That is
         * why kwin_wayland stopped dead in a futex with nothing able to wake
         * it, on a plugin scan that touches no hardware at all.
         *
         * val3 packs the whole instruction, as Linux defines it:
         *   bits 28-31  operation (SET/ADD/OR/ANDN/XOR), bit 3 = shift oparg
         *   bits 24-27  comparison (EQ/NE/LT/LE/GT/GE)
         *   bits 12-23  operand
         *   bits  0-11  comparison argument
         */
        if (base_op == 5) {
          u32 encoded = (u32)arg5;
          unsigned fop = (encoded >> 28) & 0xf;
          unsigned fcmp = (encoded >> 24) & 0xf;
          int oparg = (int)((encoded >> 12) & 0xfff);
          int cmparg = (int)(encoded & 0xfff);
          int oldval = 0, newval = 0, woken = 0;

          /* A twelve-bit field is signed in Linux's reading of it. */
          if (oparg & 0x800)
            oparg |= ~0xfff;
          if (cmparg & 0x800)
            cmparg |= ~0xfff;
          if (fop & 8) { /* FUTEX_OP_OPARG_SHIFT */
            fop &= 7;
            if (oparg < 0 || oparg > 31)
              return (u64)-EINVAL;
            oparg = 1 << oparg;
          }

          if (!arg4 || syscall_copyin(&oldval, (void *)(usize)arg4,
                                      sizeof(oldval)) < 0)
            return (u64)-EFAULT;
          switch (fop) {
          case 0: newval = oparg; break;              /* SET  */
          case 1: newval = oldval + oparg; break;     /* ADD  */
          case 2: newval = oldval | oparg; break;     /* OR   */
          case 3: newval = oldval & ~oparg; break;    /* ANDN */
          case 4: newval = oldval ^ oparg; break;     /* XOR  */
          default: return (u64)-ENOSYS;
          }
          if (syscall_copyout((void *)(usize)arg4, &newval, sizeof(newval)) < 0)
            return (u64)-EFAULT;

          woken = scheduler_futex(arg0, B1NIX_FUTEX_WAKE | futex_priv,
                                  (int)arg2, 0);
          if (woken < 0)
            woken = 0;

          {
            int fire = 0;

            switch (fcmp) {
            case 0: fire = (oldval == cmparg); break; /* EQ */
            case 1: fire = (oldval != cmparg); break; /* NE */
            case 2: fire = (oldval <  cmparg); break; /* LT */
            case 3: fire = (oldval <= cmparg); break; /* LE */
            case 4: fire = (oldval >  cmparg); break; /* GT */
            case 5: fire = (oldval >= cmparg); break; /* GE */
            default: return (u64)-ENOSYS;
            }
            if (fire) {
              int more = scheduler_futex(arg4, B1NIX_FUTEX_WAKE | futex_priv,
                                         (int)arg3, 0);

              if (more > 0)
                woken += more;
            }
          }
          return (u64)woken;
        }
        /* The priority-inheritance family: FUTEX_LOCK_PI(6),
         * FUTEX_UNLOCK_PI(7), FUTEX_TRYLOCK_PI(8).
         *
         * A PI futex holds the owner's thread id in its low bits, with
         * FUTEX_WAITERS (bit 31) set while anyone is queued. There is no
         * priority inheritance behind this — the scheduler has no priority to
         * donate — but the locking protocol is the real one, and that is what
         * callers depend on. musl probes for the whole family by taking such a
         * lock on a throwaway word, and returns whatever the kernel said to
         * pthread_mutexattr_setprotocol: pulseaudio, which chromium loads,
         * asserts that the answer is either success or ENOTSUP and aborts the
         * process on anything else.
         *
         * The compare-and-set is not atomic against userspace — it copies in,
         * decides, and copies out — but every path that changes a PI word goes
         * through this call, so two threads racing here are serialised by the
         * syscall itself. */
        if (base_op == 6 || base_op == 7 || base_op == 8) {
          const u32 waiters_bit = 0x80000000u;
          u32 self = current_task ? (u32)current_task->id : 0;
          u32 cur = 0;

          if (syscall_copyin(&cur, (void *)(usize)arg0, sizeof(u32)) < 0)
            return (u64)-EFAULT;

          if (base_op == 7) { /* UNLOCK_PI */
            if ((cur & ~waiters_bit) != self)
              return (u64)-EPERM;
            u32 zero = 0;

            if (syscall_copyout((void *)(usize)arg0, &zero, sizeof(zero)) < 0)
              return (u64)-EFAULT;
            (void)scheduler_futex(arg0, B1NIX_FUTEX_WAKE | futex_priv, 1, 0);
            return 0;
          }

          for (;;) {
            if ((cur & ~waiters_bit) == 0) { /* free — take it */
              u32 taken = self | (cur & waiters_bit);

              if (syscall_copyout((void *)(usize)arg0, &taken, sizeof(taken)) < 0)
                return (u64)-EFAULT;
              return 0;
            }
            if ((cur & ~waiters_bit) == self)
              return (u64)-EDEADLK;
            if (base_op == 8) /* TRYLOCK_PI: held by someone else */
              return (u64)-EAGAIN;

            /* Held: mark it contended and wait for the owner to release. */
            u32 contended = cur | waiters_bit;

            if (contended != cur &&
                syscall_copyout((void *)(usize)arg0, &contended, sizeof(contended)) < 0)
              return (u64)-EFAULT;
            isize rc = scheduler_futex(arg0, B1NIX_FUTEX_WAIT | futex_priv, (int)contended, 0);

            if (rc < 0 && rc != -EAGAIN)
              return (u64)rc;
            if (syscall_copyin(&cur, (void *)(usize)arg0, sizeof(cur)) < 0)
              return (u64)-EFAULT;
          }
        }
        /* Anything else is an operation this kernel does not implement, and
         * that is what it must say. The priority-inheritance family
         * (FUTEX_LOCK_PI and friends) is the one that matters: musl probes for
         * it with a real call and reads ENOSYS as "no PI here", turning it into
         * ENOTSUP for pthread_mutexattr_setprotocol. Any other error is passed
         * straight through to the caller instead — which is how pulseaudio,
         * pulled in by chromium, came to abort on
         * "Assertion 'r == 0 || r == 95' failed" and take the browser with it.
         *
         * The native handler knows WAIT and WAKE only, so passing an unknown
         * op down to it could never have done anything useful. */
        return (u64)-ENOSYS;
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

      /* termios needs no per-personality translation any more: every tty
       * driver reads and writes the Linux wire layout directly (see
       * <b1nix/termios_abi.h>). The translation that used to live here
       * repacked through the CALLER'S buffer, writing a 48-byte
       * struct b1nix_termios into what glibc passes to TCGETS — a 36-byte
       * struct __kernel_termios — and smashed 12 bytes of its stack. */
      /* ── M40 completion: the rest of the Linux syscall surface ────────────
       * Every call below needs an argument-shape or semantic translation, so it
       * cannot be a plain number-table entry. Grouped by area. */

      /* pread64(17)/pwrite64(18)/preadv(295)/pwritev(296)/preadv2(327)/
       * pwritev2(328): positional I/O. A b1nix fd carries a single shared
       * offset, so seek → transfer → seek back gives the "does not change the
       * file offset" guarantee callers depend on. */
      if (number == 17 || number == 18 || number == 295 || number == 296 ||
          number == 327 || number == 328) {
        int fd = (int)arg0;
        /* pread/pwrite carry the offset in arg3; the *v forms split it into
         * (pos_l, pos_h) — pos_h is 0 for a 64-bit caller. */
        i64 off = (number == 17 || number == 18)
                      ? (i64)arg3
                      : (i64)(arg3 | (arg4 << 32));
        if (off < 0)
          return (u64)-EINVAL;
        isize saved = vfs_lseek(fd, 0, B1NIX_SEEK_CUR);
        if (saved < 0)
          return (u64)saved;
        isize sk = vfs_lseek(fd, (isize)off, B1NIX_SEEK_SET);
        if (sk < 0)
          return (u64)sk;
        isize r;
        if (number == 17)
          r = sys_read(fd, (void *)(usize)arg1, (usize)arg2);
        else if (number == 18)
          r = sys_write(fd, (const void *)(usize)arg1, (usize)arg2);
        else if (number == 295 || number == 327)
          r = sys_readv(fd, (const struct b1nix_iovec *)(usize)arg1, (int)arg2);
        else
          r = sys_writev(fd, (const struct b1nix_iovec *)(usize)arg1, (int)arg2);
        vfs_lseek(fd, saved, B1NIX_SEEK_SET);
        return (u64)r;
      }

      /* truncate(76): b1nix truncates by descriptor, so open the path first.
       * O_WRONLY also runs the write-permission check truncate(2) requires. */
      if (number == 76) {
        char kpath[VFS_MAX_PATH], resolved[VFS_MAX_PATH];
        int cs = syscall_copyinstr(kpath, sizeof(kpath),
                                   (const char *)(usize)arg0);
        if (cs < 0)
          return (u64)(isize)cs; /* ENAMETOOLONG must not become EFAULT */
        vfs_resolve_path(kpath, resolved);
        int fd = vfs_open_flags(resolved, B1NIX_O_WRONLY);
        if (fd < 0)
          return (u64)fd;
        int rc = vfs_ftruncate(fd, arg1);
        vfs_close(fd);
        return (u64)rc;
      }

      /* creat(85) == open(path, O_CREAT|O_WRONLY|O_TRUNC, mode). */
      if (number == 85) {
        char kpath[VFS_MAX_PATH], resolved[VFS_MAX_PATH];
        int cs = syscall_copyinstr(kpath, sizeof(kpath),
                                   (const char *)(usize)arg0);
        if (cs < 0)
          return (u64)(isize)cs; /* ENAMETOOLONG must not become EFAULT */
        vfs_resolve_path(kpath, resolved);
        return (u64)vfs_open_flags_mode(
            resolved, B1NIX_O_WRONLY | B1NIX_O_CREAT | B1NIX_O_TRUNC,
            (u16)arg1);
      }

      /* lchown(94): change a symlink's own ownership. */
      if (number == 94) {
        char kpath[VFS_MAX_PATH], resolved[VFS_MAX_PATH];
        int cs = syscall_copyinstr(kpath, sizeof(kpath),
                                   (const char *)(usize)arg0);
        if (cs < 0)
          return (u64)(isize)cs; /* ENAMETOOLONG must not become EFAULT */
        vfs_resolve_path(kpath, resolved);
        return (u64)vfs_lchown(resolved, (u16)arg1, (u16)arg2);
      }

      /* pause(34): suspend with the CURRENT signal mask until a handler runs. */
      if (number == 34)
        return sigsuspend_with_mask(current_task->blocked_signals);

      /* rt_sigpending(127): the pending set, in Linux signal numbering. */
      if (number == 127) {
        if (!arg0)
          return (u64)-EFAULT;
        u64 pend = __atomic_load_n(&current_task->pending_signals,
                                   __ATOMIC_ACQUIRE);
        u64 lx = b1nix_sigset_to_linux(pend);
        return syscall_copyout((void *)(usize)arg0, &lx, sizeof(lx)) == 0
                   ? 0
                   : (u64)-EFAULT;
      }

      /* gettimeofday(96)(tv, tz) and time(201)(t*). Both halves come from the
       * one wall clock, for the reason rtc_now_unix_nanos explains. */
      if (number == 96 || number == 201) {
        u64 wall_ns = rtc_now_unix_nanos();
        u64 secs = wall_ns / 1000000000ull;
        if (number == 201) {
          if (arg0 &&
              syscall_copyout((void *)(usize)arg0, &secs, sizeof(secs)) != 0)
            return (u64)-EFAULT;
          return secs;
        }
        if (arg0) {
          struct timeval tv;
          tv.tv_sec = (i64)secs;
          tv.tv_usec = (i64)((wall_ns % 1000000000ull) / 1000ull);
          if (syscall_copyout((void *)(usize)arg0, &tv, sizeof(tv)) != 0)
            return (u64)-EFAULT;
        }
        if (arg1) {
          /* struct timezone: b1nix keeps the clock in UTC and does no DST
           * bookkeeping, which is exactly {0, 0}. */
          i32 tz[2] = {0, 0};
          if (syscall_copyout((void *)(usize)arg1, tz, sizeof(tz)) != 0)
            return (u64)-EFAULT;
        }
        return 0;
      }

      /* clock_settime(227)(clockid, timespec) — CLOCK_REALTIME only; the
       * monotonic clocks are uptime and cannot be stepped. */
      if (number == 227) {
        if ((int)arg0 != 0)
          return (u64)-EINVAL;
        struct cred *c = scheduler_get_current_cred();
        if (!c || (!cred_has_cap(c, CAP_SYS_TIME)))
          return (u64)-EPERM;
        struct timespec ts;
        if (!arg1 ||
            syscall_copyin(&ts, (const void *)(usize)arg1, sizeof(ts)) != 0)
          return (u64)-EFAULT;
        if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000)
          return (u64)-EINVAL;
        rtc_set_unix_time((u64)ts.tv_sec);
        return 0;
      }

      /* epoll_create(213)(size): the size hint has been ignored since 2.6.8. */
      if (number == 213)
        return (u64)vfs_epoll_create(0);

      /* sync_file_range(277)(fd, offset, nbytes, flags): b1nix writeback is
       * whole-file, so the honest superset is fsync. */
      if (number == 277)
        return (u64)vfs_fsync((int)arg0);

      /* readahead(187) / fadvise64(221): advisory only. The block layer already
       * reads ahead (kernel/dev/blk.c) and both calls are defined to be
       * droppable, so accepting them changes nothing a caller can observe. */
      if (number == 187 || number == 221)
        return 0;

      /* restart_syscall(219): b1nix restarts an interrupted call by rewinding
       * RIP in syscall_dispatch_impl, never by handing userspace a restart
       * block, so reaching this from ring 3 means the call was spurious. */
      if (number == 219)
        return (u64)-EINTR;

      /* personality(135): the ABI is a property of the image, fixed by the ELF
       * loader. Report PER_LINUX (0) and refuse to change it. */
      if (number == 135)
        return (arg0 == 0 || arg0 == 0xffffffffULL) ? 0 : (u64)-EINVAL;

      /* M109 — unshare(272) / setns(308). UTS and mount namespaces are real;
       * the kinds b1nix has only one of are refused rather than faked. */
      if (number == 272)
        return (u64)sys_unshare(arg0);
      if (number == 308)
        return (u64)sys_setns((int)arg0, (int)arg1);

      /* getresuid(118) / getresgid(120). */
      if (number == 118 || number == 120) {
        struct cred *c = scheduler_get_current_cred();
        if (!c)
          return (u64)-EINVAL;
        if (!arg0 || !arg1 || !arg2)
          return (u64)-EFAULT;
        u32 v[3];
        if (number == 118) {
          v[0] = c->uid; v[1] = c->euid; v[2] = c->suid;
        } else {
          v[0] = c->gid; v[1] = c->egid; v[2] = c->sgid;
        }
        if (syscall_copyout((void *)(usize)arg0, &v[0], sizeof(u32)) != 0 ||
            syscall_copyout((void *)(usize)arg1, &v[1], sizeof(u32)) != 0 ||
            syscall_copyout((void *)(usize)arg2, &v[2], sizeof(u32)) != 0)
          return (u64)-EFAULT;
        return 0;
      }

      /* setfsuid(122)/setfsgid(123): the filesystem ids the VFS checks against
       * (kernel/sched/uidgid.c cred_can_access). Both return the PREVIOUS
       * value whether or not the change was permitted, which is the documented
       * way for a caller to find out. */
      if (number == 122 || number == 123) {
        struct cred *c = scheduler_get_current_cred();
        if (!c)
          return (u64)-EINVAL;
        return (u64)(number == 122 ? cred_set_fsuid(c, (u16)arg0)
                                   : cred_set_fsgid(c, (u16)arg0));
      }

      /* capget(125)/capset(126): the task's real capability sets. A root task
       * starts with everything; capset can only give capabilities up, and a
       * dropped one stays dropped (it leaves the bounding set) even if the
       * task later returns to euid 0. */
      if (number == 125 || number == 126) {
        struct cred *c = scheduler_get_current_cred();
        if (!c)
          return (u64)-EINVAL;
        struct lx_cap_header { u32 version; int pid; } hdr;
        if (!arg0 ||
            syscall_copyin(&hdr, (const void *)(usize)arg0, sizeof(hdr)) != 0)
          return (u64)-EFAULT;
        /* The version negotiation, which is not optional.
         *
         * Linux answers a header whose version it does not recognise with
         * EINVAL *and* the version it prefers written back into the header —
         * that pair is how a caller discovers which layout to use. libcap-ng
         * opens with capget(&hdr, NULL) and hdr.version = 0 for exactly this,
         * and marks itself permanently broken if the version it reads back is
         * not one it knows. Returning 0 and leaving the caller's zero in place
         * therefore disabled the whole library: dbus-daemon's
         * capng_change_id() then failed before making a single syscall, with
         * errno untouched — "Failed to drop capabilities: Success" — and the
         * system bus never started. */
        {
          u32 v = (u32)hdr.version;
          if (v != 0x19980330u && v != 0x20071026u && v != 0x20080522u) {
            hdr.version = 0x20080522u; /* _LINUX_CAPABILITY_VERSION_3 */
            if (syscall_copyout((void *)(usize)arg0, &hdr, sizeof(hdr)) != 0)
              return (u64)-EFAULT;
            return (u64)-EINVAL;
          }
        }
        /* _LINUX_CAPABILITY_VERSION_1 carries one data struct, _2/_3 carry two
         * (the 64-bit capability set). Writing the wrong count would run off
         * the caller's buffer. */
        usize nwords =
            (hdr.version == 0x19980330u) ? 3 : 6; /* 3 u32 per data struct */
        if (hdr.pid != 0 && (usize)hdr.pid != scheduler_get_pid())
          return (u64)-EPERM;
        /* struct __user_cap_data_struct is {effective, permitted, inheritable}
         * of 32 bits each; the v3 header carries two of them, holding the low
         * and high halves of the 64-bit sets. */
        if (number == 125) {
          if (!arg1)
            return 0; /* version probe: the header is all the caller wanted */
          u32 data[6];
          data[0] = (u32)c->cap_effective;
          data[1] = (u32)c->cap_permitted;
          data[2] = (u32)c->cap_inheritable;
          data[3] = (u32)(c->cap_effective >> 32);
          data[4] = (u32)(c->cap_permitted >> 32);
          data[5] = (u32)(c->cap_inheritable >> 32);
          return syscall_copyout((void *)(usize)arg1, data,
                                 nwords * sizeof(u32)) == 0
                     ? 0
                     : (u64)-EFAULT;
        }
        if (!arg1)
          return (u64)-EFAULT;
        u32 want[6] = {0};
        if (syscall_copyin(want, (const void *)(usize)arg1,
                           nwords * sizeof(u32)) != 0)
          return (u64)-EFAULT;
        u64 eff = want[0], perm = want[1], inh = want[2];
        if (nwords == 6) {
          eff |= (u64)want[3] << 32;
          perm |= (u64)want[4] << 32;
          inh |= (u64)want[5] << 32;
        }
        return cred_capset(c, eff, perm, inh) == 0 ? 0 : (u64)-EPERM;
      }

      /* sethostname(170) / setdomainname(171). */
      if (number == 170 || number == 171) {
        struct cred *c = scheduler_get_current_cred();
        if (!c || (!cred_has_cap(c, CAP_SYS_ADMIN)))
          return (u64)-EPERM;
        if (!arg0)
          return (u64)-EFAULT;
        if (arg1 > 64)
          return (u64)-EINVAL;
        char name[65];
        usize n = (usize)arg1;
        if (n && syscall_copyin(name, (const void *)(usize)arg0, n) != 0)
          return (u64)-EFAULT;
        name[n] = '\0';
        return (u64)(number == 170 ? kernel_hostname_set(name)
                                   : kernel_domainname_set(name));
      }

      /* mlock(149)/mlock2(325)/munlock(150)/mlockall(151)/munlockall(152).
       * Two halves make this a real guarantee rather than a success return:
       *  1. the range is POPULATED here — every page is faulted in through the
       *     demand pager and verified present, so mlock returns only once the
       *     memory is actually resident (Linux semantics: no later major fault);
       *  2. the range is RECORDED in the eviction lock table, and the CLOCK swap
       *     scan (kernel/mm/eviction.c) skips locked pages, so nothing can push
       *     them back out. A mapped page's frame also carries a VMA reference
       *     (paging.c pmm_ref_frame), so page-cache reclaim cannot free it
       *     either — swap eviction was the only remaining way out.
       * A page that cannot be made resident fails the call with ENOMEM instead
       * of leaving the caller with an unenforced promise. */
      if (number == 149 || number == 150 || number == 325) {
        u64 start = arg0 & ~(u64)(PAGE_SIZE - 1);
        u64 end = arg0 + arg1;
        if (arg1 == 0)
          return 0;
        if (end < arg0)
          return (u64)-EINVAL;
        end = (end + PAGE_SIZE - 1) & ~(u64)(PAGE_SIZE - 1);
        if (number == 150) {
          eviction_unlock_range(current_task, start, end);
          return 0;
        }
        if (eviction_lock_range(current_task, start, end) != 0)
          return (u64)-ENOMEM;
        if (mlock_populate(start, end) != 0) {
          eviction_unlock_range(current_task, start, end);
          return (u64)-ENOMEM;
        }
        return 0;
      }
      if (number == 151) { /* mlockall(flags): MCL_CURRENT(1)|MCL_FUTURE(2) */
        if ((arg0 & 3) == 0)
          return (u64)-EINVAL;
        /* One lock record spans the whole user range, so MCL_FUTURE is covered
         * too: anything mapped later lands inside it and is never evicted. */
        if (eviction_lock_range(current_task, PAGE_SIZE, USER_STACK_TOP) != 0)
          return (u64)-ENOMEM;
        if (arg0 & 1) { /* MCL_CURRENT: populate what is mapped right now */
          for (struct vm_area *v = current_task->vma_list; v; v = v->next) {
            if (mlock_populate(v->start, v->end) != 0) {
              eviction_unlock_all(current_task);
              return (u64)-ENOMEM;
            }
          }
        }
        return 0;
      }
      if (number == 152) {
        eviction_unlock_all(current_task);
        return 0;
      }

      /* sched_*: b1nix runs one policy — SCHED_OTHER, stride scheduling with
       * nice weighting — so the policy calls report it truthfully and refuse to
       * switch to a policy that does not exist. Nice lives in get/setpriority. */
      if (number == 144) /* sched_setscheduler(pid, policy, param) */
        return (int)arg1 == 0 ? 0 : (u64)-EINVAL;
      if (number == 145) /* sched_getscheduler → SCHED_OTHER */
        return 0;
      if (number == 142 || number == 143) {
        /* sched_setparam / sched_getparam: struct sched_param {int priority;},
         * always 0 under SCHED_OTHER. */
        int prio = 0;
        if (!arg1)
          return (u64)-EINVAL;
        if (number == 143)
          return syscall_copyout((void *)(usize)arg1, &prio, sizeof(prio)) == 0
                     ? 0
                     : (u64)-EFAULT;
        if (syscall_copyin(&prio, (const void *)(usize)arg1, sizeof(prio)) != 0)
          return (u64)-EFAULT;
        return prio == 0 ? 0 : (u64)-EINVAL;
      }
      if (number == 146 || number == 147) /* sched_get_priority_max/min */
        return (int)arg0 == 0 ? 0 : (u64)-EINVAL;
      if (number == 148) { /* sched_rr_get_interval(pid, timespec) */
        if (!arg1)
          return (u64)-EFAULT;
        /* One scheduler tick — the real one, not a written-out 10 ms. */
        struct timespec ts = {0, (i64)(1000000000ULL / sc_tick_hz())};
        return syscall_copyout((void *)(usize)arg1, &ts, sizeof(ts)) == 0
                   ? 0
                   : (u64)-EFAULT;
      }
      /* sched_setaffinity(203)(pid, cpusetsize, mask): a real restriction. The
       * mask is stored per task and every path that can place a task on a CPU
       * — the global-runqueue pick, the scan, and work stealing — honours it
       * (kernel/sched/scheduler.c, runqueue.c). */
      if (number == 203) {
        usize sz = (usize)arg1;
        if (!arg2 || sz == 0)
          return (u64)-EINVAL;
        u8 kmask[128];
        usize n = sz < sizeof(kmask) ? sz : sizeof(kmask);
        memset(kmask, 0, sizeof(kmask));
        if (syscall_copyin(kmask, (const void *)(usize)arg2, n) != 0)
          return (u64)-EFAULT;
        u64 mask = 0;
        for (usize b = 0; b < n && b < 8; b++)
          mask |= (u64)kmask[b] << (b * 8);
        return (u64)(isize)scheduler_set_affinity((usize)arg0, mask);
      }

      /* fsetxattr(190)/fgetxattr(193)/flistxattr(196)/fremovexattr(199): the
       * fd-relative xattr forms. Resolve the descriptor's path and use the same
       * VFS helpers the path forms do. */
      if (number == 190 || number == 193 || number == 196 || number == 199) {
        char fdpath[VFS_MAX_PATH];
        int rc = vfs_fd_abspath((int)arg0, fdpath, sizeof(fdpath));
        if (rc < 0)
          return (u64)rc;
        if (number == 196) { /* flistxattr(fd, list, size) */
          char list[XATTR_VALUE_MAX];
          usize size = (usize)arg2;
          isize lr = vfs_listxattr(fdpath, list, sizeof(list), 0);
          if (lr < 0)
            return (u64)lr;
          if (size == 0)
            return (u64)lr; /* size query */
          if ((usize)lr > size)
            return (u64)-ERANGE;
          return syscall_copyout((void *)(usize)arg1, list, (usize)lr) == 0
                     ? (u64)lr
                     : (u64)-EFAULT;
        }
        char name[XATTR_NAME_MAX + 1];
        if (syscall_copyinstr(name, sizeof(name), (const char *)(usize)arg1) < 0)
          return (u64)-EFAULT;
        if (number == 199)
          return (u64)vfs_removexattr(fdpath, name, 0);
        if (number == 190) { /* fsetxattr(fd, name, value, size, flags) */
          usize size = (usize)arg3;
          if (size > XATTR_VALUE_MAX)
            return (u64)-E2BIG;
          static char value[XATTR_VALUE_MAX];
          if (size &&
              syscall_copyin(value, (const void *)(usize)arg2, size) != 0)
            return (u64)-EFAULT;
          return (u64)vfs_setxattr(fdpath, name, value, size, (int)arg4, 0);
        }
        /* fgetxattr(fd, name, value, size) */
        usize size = (usize)arg3;
        static char value[XATTR_VALUE_MAX];
        isize gr = vfs_getxattr(fdpath, name, value, sizeof(value), 0);
        if (gr < 0)
          return (u64)gr;
        if (size == 0)
          return (u64)gr; /* size query */
        if ((usize)gr > size)
          return (u64)-ERANGE;
        return syscall_copyout((void *)(usize)arg2, value, (usize)gr) == 0
                   ? (u64)gr
                   : (u64)-EFAULT;
      }

      /* utime(132)(path, struct utimbuf{actime, modtime}); a NULL buffer means
       * "now". The native handler takes the two stamps as arguments. */
      if (number == 132) {
        u64 times[2];
        if (arg1) {
          if (syscall_copyin(times, (const void *)(usize)arg1,
                             sizeof(times)) != 0)
            return (u64)-EFAULT;
        } else {
          times[0] = times[1] = vfs_get_unix_time();
        }
        return (u64)sys_utime((const char *)(usize)arg0, times[0], times[1]);
      }

      /* futimesat(261)(dirfd, path, struct timeval[2]). */
      if (number == 261)
        return (u64)sys_linux_utimensat((int)arg0, (const char *)(usize)arg1,
                                        arg2, 0);

      /* io_destroy(207)/io_cancel(210): b1nix keeps one AIO context per task and
       * its worker completes a submitted request without an intermediate
       * cancellable state, which is exactly the case Linux documents io_cancel
       * as returning EINVAL for. */
      if (number == 207) {
        aio_task_cleanup(current_task);
        return 0;
      }
      if (number == 210)
        return (u64)-EINVAL;

      /* recvmmsg(299)/sendmmsg(307): iterate the mmsghdr array over the
       * single-message handlers — b1nix has no batched socket path. struct
       * mmsghdr is { struct msghdr (56 bytes); unsigned msg_len; } padded to a
       * 64-byte stride on x86_64. */
      if (number == 299 || number == 307) {
        int fd = (int)arg0;
        u32 vlen = (u32)arg2;
        int flags = (int)arg3;
        u32 done = 0;
        for (u32 i = 0; i < vlen; i++) {
          u64 hdr = arg1 + (u64)i * 64;
          isize r = (number == 307)
                        ? sys_sendmsg(fd, (const struct syscall_msghdr *)(usize)hdr,
                                      flags)
                        : sys_recvmsg(fd, (struct syscall_msghdr *)(usize)hdr,
                                      flags);
          if (r < 0)
            return done ? (u64)done : (u64)r;
          u32 len32 = (u32)r;
          if (syscall_copyout((void *)(usize)(hdr + 56), &len32,
                              sizeof(len32)) != 0)
            return (u64)-EFAULT;
          done++;
        }
        return done;
      }

      /* rt_tgsigqueueinfo(297)(tgid, tid, sig, siginfo): only the signal number
       * needs remapping before the native handler. */
      if (number == 297) {
        int b = linux_signo_to_b1nix((int)arg2);
        if (b <= 0)
          return (u64)-EINVAL;
        arg2 = (u64)b;
      }

      /* getdents(78): the pre-64-bit record layout. */
      if (number == 78)
        return (u64)sys_linux_getdents_common((int)arg0, arg1, (usize)arg2, 1);

      /* chroot(161): the calling task's filesystem root. Path resolution
       * starts at this node and ".." clamps there (kernel/fs/vfs.c), so a
       * chrooted task cannot name anything outside it. The node keeps a
       * reference for as long as a task (or a fork child) names it. */
      if (number == 161) {
        struct cred *c = scheduler_get_current_cred();
        if (!c || (!cred_has_cap(c, CAP_SYS_CHROOT)))
          return (u64)-EPERM;
        char kpath[VFS_MAX_PATH], resolved[VFS_MAX_PATH];
        int cs = syscall_copyinstr(kpath, sizeof(kpath),
                                   (const char *)(usize)arg0);
        if (cs < 0)
          return (u64)(isize)cs; /* ENAMETOOLONG must not become EFAULT */
        vfs_resolve_path(kpath, resolved);
        struct vfs_node *n = vfs_find_node(resolved);
        if (!n || IS_ERR(n))
          return (u64)(n ? PTR_ERR(n) : -ENOENT);
        if (n->inode->type != VFS_DIRECTORY) {
          vfs_node_put(n);
          return (u64)-ENOTDIR;
        }
        /* The working directory is kept when it lies inside the new root —
         * rewritten to the root-relative form, because every later lookup
         * resolves from the new root. A cwd outside the new root can no longer
         * be named at all, so it moves to "/" (POSIX leaves that case
         * unspecified; silently resolving the old string under the new root
         * would point at a different directory). */
        const char *cwd = scheduler_get_cwd();
        char newcwd[VFS_MAX_PATH];
        usize rlen = strlen(resolved);
        if (rlen == 1 && resolved[0] == '/') {
          strncpy(newcwd, cwd, sizeof(newcwd) - 1);
          newcwd[sizeof(newcwd) - 1] = '\0';
        } else if (strncmp(cwd, resolved, rlen) == 0 &&
                   (cwd[rlen] == '/' || cwd[rlen] == '\0')) {
          if (cwd[rlen] == '\0') {
            newcwd[0] = '/';
            newcwd[1] = '\0';
          } else {
            strncpy(newcwd, cwd + rlen, sizeof(newcwd) - 1);
            newcwd[sizeof(newcwd) - 1] = '\0';
          }
        } else {
          newcwd[0] = '/';
          newcwd[1] = '\0';
        }
        /* The stored root path is relative to the PREVIOUS root, which is what
         * a nested chroot needs. */
        scheduler_set_root(n, resolved); /* takes the reference */
        scheduler_set_cwd(newcwd);
        return 0;
      }

      /* swapon(167)(path, flags) / swapoff(168)(path): attach or detach the
       * swap device at runtime. b1nix swaps to a whole block device, so the
       * path names one under /dev. swapoff pages every swapped page back in
       * first and refuses (EBUSY) if anything is still out there. */
      if (number == 167 || number == 168) {
        struct cred *c = scheduler_get_current_cred();
        if (!c || (!cred_has_cap(c, CAP_SYS_ADMIN)))
          return (u64)-EPERM;
        char kpath[VFS_MAX_PATH];
        if (syscall_copyinstr(kpath, sizeof(kpath), (const char *)(usize)arg0) <
            0)
          return (u64)-EFAULT;
        const char *devname = kpath;
        for (const char *p = kpath; *p; p++)
          if (*p == '/')
            devname = p + 1;
        struct block_device *dev = blk_get(devname);
        if (!dev)
          return (u64)-ENODEV;
        if (number == 167) {
          if (swap_active())
            return (u64)-EBUSY;
          vmm_set_swap_device(dev);
          return swap_active() ? 0 : (u64)-EINVAL;
        }
        if (!swap_active())
          return (u64)-EINVAL;
        scheduler_swapin_all_tasks();
        int dr = swap_detach();
        if (dr == -2)
          return (u64)-EBUSY;
        return dr == 0 ? 0 : (u64)-EINVAL;
      }

      /* tee(276)(fd_in, fd_out, len, flags): duplicate pipe data without
       * consuming it. vmsplice(278)(fd, iov, nr, flags): move user memory
       * to/from a pipe — b1nix pipes copy, so this is readv/writev on the
       * pipe end the descriptor names (SPLICE_F_GIFT only promises the kernel
       * MAY take the pages, so copying satisfies it). */
      if (number == 276) {
        struct vfs_handle *hin = scheduler_fd_get((int)arg0);
        struct vfs_handle *hout = scheduler_fd_get((int)arg1);
        if (!hin || !hout)
          return (u64)-EBADF;
        return (u64)vfs_pipe_tee(hin, hout, (usize)arg2);
      }
      if (number == 278) {
        struct vfs_handle *h = scheduler_fd_get((int)arg0);
        if (!h)
          return (u64)-EBADF;
        if (h->kind == VFS_HANDLE_PIPE_WRITE)
          return (u64)sys_writev((int)arg0,
                                 (const struct b1nix_iovec *)(usize)arg1,
                                 (int)arg2);
        if (h->kind == VFS_HANDLE_PIPE_READ)
          return (u64)sys_readv((int)arg0,
                                (const struct b1nix_iovec *)(usize)arg1,
                                (int)arg2);
        return (u64)-EBADF;
      }

      /* ioprio_set(251)/ioprio_get(252): per-task I/O class and level. b1nix
       * issues block requests synchronously (no reordering elevator), so the
       * value is stored and reported but does not reorder I/O — the same
       * observable behaviour Linux has with the `none` scheduler. */
      if (number == 251 || number == 252) {
        int which = (int)arg0;
        if (which != 1 /* IOPRIO_WHO_PROCESS */)
          return (u64)-EINVAL;
        usize who = (usize)arg1;
        if (number == 252)
          return (u64)(isize)scheduler_get_ioprio(who);
        int prio = (int)arg2;
        int class = (prio >> 13) & 0x7;
        if (class > 3) /* NONE/RT/BE/IDLE */
          return (u64)-EINVAL;
        if (class == 1 /* IOPRIO_CLASS_RT */ && c_euid_not_root())
          return (u64)-EPERM;
        return (u64)(isize)scheduler_set_ioprio(who, prio);
      }

      /* name_to_handle_at(303) / open_by_handle_at(304): opaque file handles.
       * The handle carries the resolved path in a kernel-side table, so a
       * handle stays valid for the lifetime of the boot (Linux only promises
       * validity while the filesystem is mounted). */
      if (number == 303)
        return (u64)sys_linux_name_to_handle_at((int)arg0,
                                                (const char *)(usize)arg1, arg2,
                                                arg3, (int)arg4);
      if (number == 304)
        return (u64)sys_linux_open_by_handle_at((int)arg0, arg1, (int)arg2);

      /* rseq(334)(rseq, len, flags, sig): restartable sequences. */
      if (number == 334)
        return (u64)rseq_register(current_task, arg0, (u32)arg1,
                                  (u32)arg3,
                                  ((u32)arg2 & 1) != 0 /* RSEQ_FLAG_UNREGISTER */);

      /* ── System V semaphores (64 semget / 65 semop / 66 semctl /
       *    220 semtimedop) and message queues (68 msgget / 69 msgsnd /
       *    70 msgrcv / 71 msgctl). The IPC_CREAT/IPC_EXCL bits differ between
       *    the ABIs exactly as they do for shmget, so they are remapped here;
       *    everything else is copied in/out around the kernel implementations
       *    in kernel/ipc/sysv_{sem,msg}.c. ── */
      if (number == 64 || number == 68) { /* semget / msgget */
        int lxflg = (int)((number == 64) ? arg2 : arg1);
        int f = lxflg & 0777;
        if (lxflg & 0x200) f |= IPC_CREAT;
        if (lxflg & 0x400) f |= IPC_EXCL;
        if (number == 64)
          return (u64)(isize)sysv_semget((u32)arg0, (int)arg1, f);
        return (u64)(isize)sysv_msgget((u32)arg0, f);
      }
      if (number == 65 || number == 220) { /* semop / semtimedop */
        usize nops = (usize)arg2;
        if (nops == 0 || nops > SEMMSL)
          return (u64)-EINVAL;
        struct sysv_sembuf ops[SEMMSL];
        if (syscall_copyin(ops, (const void *)(usize)arg1,
                           nops * sizeof(ops[0])) < 0)
          return (u64)-EFAULT;
        i64 timeout_ms = -1;
        if (number == 220 && arg3) {
          struct timespec ts;
          if (syscall_copyin(&ts, (const void *)(usize)arg3, sizeof(ts)) < 0)
            return (u64)-EFAULT;
          timeout_ms = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
          if (timeout_ms < 0)
            return (u64)-EINVAL;
        }
        return (u64)(isize)sysv_semop((int)arg0, ops, nops, timeout_ms);
      }
      if (number == 66) { /* semctl(semid, semnum, cmd, arg) */
        int semid = (int)arg0, semnum = (int)arg1, cmd = (int)arg2;
        switch (cmd) {
        case GETVAL:
          return (u64)(isize)sysv_semctl_getval(semid, semnum);
        case SETVAL:
          return (u64)(isize)sysv_semctl_setval(semid, semnum, (int)arg3);
        case GETPID:
          return (u64)(isize)sysv_semctl_getpid(semid, semnum);
        case GETNCNT:
          return (u64)(isize)sysv_semctl_getcnt(semid, semnum, 0);
        case GETZCNT:
          return (u64)(isize)sysv_semctl_getcnt(semid, semnum, 1);
        case IPC_RMID:
          return (u64)(isize)sysv_semctl_rmid(semid);
        case GETALL: {
          u16 vals[SEMMSL];
          int n = sysv_semctl_getall(semid, vals, SEMMSL);
          if (n < 0)
            return (u64)(isize)n;
          if (syscall_copyout((void *)(usize)arg3, vals,
                              (usize)n * sizeof(u16)) < 0)
            return (u64)-EFAULT;
          return 0;
        }
        case SETALL: {
          struct sysv_semid_info info;
          int rc = sysv_semctl_stat(semid, &info);
          if (rc < 0)
            return (u64)(isize)rc;
          u16 vals[SEMMSL];
          if (syscall_copyin(vals, (const void *)(usize)arg3,
                             (usize)info.sem_nsems * sizeof(u16)) < 0)
            return (u64)-EFAULT;
          return (u64)(isize)sysv_semctl_setall(semid, vals, SEMMSL);
        }
        case IPC_STAT:
        case IPC_SET: {
          /* Linux struct semid_ds: struct ipc_perm (key,uid,gid,cuid,cgid,
           * mode, __pad, seq at 4-byte granularity, 48 bytes total on x86_64)
           * then sem_otime, sem_ctime, sem_nsems. Build it field by field so
           * the b1nix ipc_perm layout stays private to the kernel. */
          struct lx_semid_ds {
            i32 key; u32 uid, gid, cuid, cgid; u16 mode, __pad1;
            u16 seq, __pad2; u64 __unused1, __unused2;
            i64 sem_otime; i64 sem_ctime; u64 sem_nsems;
            u64 __unused3, __unused4;
          } lds;
          if (cmd == IPC_STAT) {
            struct sysv_semid_info info;
            int rc = sysv_semctl_stat(semid, &info);
            if (rc < 0)
              return (u64)(isize)rc;
            memset(&lds, 0, sizeof(lds));
            lds.key = (i32)info.sem_perm.key;
            lds.uid = info.sem_perm.uid;
            lds.gid = info.sem_perm.gid;
            lds.cuid = info.sem_perm.cuid;
            lds.cgid = info.sem_perm.cgid;
            lds.mode = info.sem_perm.mode;
            lds.seq = info.sem_perm.seq;
            lds.sem_otime = (i64)info.sem_otime;
            lds.sem_ctime = (i64)info.sem_ctime;
            lds.sem_nsems = info.sem_nsems;
            if (syscall_copyout((void *)(usize)arg3, &lds, sizeof(lds)) < 0)
              return (u64)-EFAULT;
            return 0;
          }
          if (syscall_copyin(&lds, (const void *)(usize)arg3, sizeof(lds)) < 0)
            return (u64)-EFAULT;
          return (u64)(isize)sysv_semctl_set(semid, (u16)lds.uid, (u16)lds.gid,
                                             lds.mode);
        }
        default:
          return (u64)-EINVAL;
        }
      }
      if (number == 69 || number == 70) { /* msgsnd / msgrcv */
        /* struct msgbuf { long mtype; char mtext[]; } */
        usize size = (usize)arg2;
        if (size > MSGMAX)
          return (u64)-EINVAL;
        static char msgtmp[MSGMAX];
        if (number == 69) {
          i64 mtype = 0;
          if (syscall_copyin(&mtype, (const void *)(usize)arg1,
                             sizeof(mtype)) < 0)
            return (u64)-EFAULT;
          if (size &&
              syscall_copyin(msgtmp, (const void *)(usize)(arg1 + 8), size) < 0)
            return (u64)-EFAULT;
          return (u64)sysv_msgsnd((int)arg0, mtype, msgtmp, size, (int)arg3);
        }
        i64 got_type = 0;
        isize n = sysv_msgrcv((int)arg0, (i64)arg3, msgtmp, size, (int)arg4,
                              &got_type);
        if (n < 0)
          return (u64)n;
        if (syscall_copyout((void *)(usize)arg1, &got_type,
                            sizeof(got_type)) < 0)
          return (u64)-EFAULT;
        if (n && syscall_copyout((void *)(usize)(arg1 + 8), msgtmp, (usize)n) < 0)
          return (u64)-EFAULT;
        return (u64)n;
      }
      if (number == 71) { /* msgctl(msqid, cmd, buf) */
        int msqid = (int)arg0, cmd = (int)arg1;
        struct lx_msqid_ds {
          i32 key; u32 uid, gid, cuid, cgid; u16 mode, __pad1;
          u16 seq, __pad2; u64 __unused1, __unused2;
          i64 msg_stime, msg_rtime, msg_ctime;
          u64 msg_cbytes, msg_qnum, msg_qbytes;
          i32 msg_lspid, msg_lrpid; u64 __unused3, __unused4;
        } lds;
        if (cmd == IPC_RMID)
          return (u64)(isize)sysv_msgctl_rmid(msqid);
        if (cmd == IPC_STAT) {
          struct sysv_msqid_info info;
          int rc = sysv_msgctl_stat(msqid, &info);
          if (rc < 0)
            return (u64)(isize)rc;
          memset(&lds, 0, sizeof(lds));
          lds.key = (i32)info.msg_perm.key;
          lds.uid = info.msg_perm.uid;
          lds.gid = info.msg_perm.gid;
          lds.cuid = info.msg_perm.cuid;
          lds.cgid = info.msg_perm.cgid;
          lds.mode = info.msg_perm.mode;
          lds.seq = info.msg_perm.seq;
          lds.msg_stime = (i64)info.msg_stime;
          lds.msg_rtime = (i64)info.msg_rtime;
          lds.msg_ctime = (i64)info.msg_ctime;
          lds.msg_qnum = info.msg_qnum;
          lds.msg_qbytes = info.msg_qbytes;
          if (syscall_copyout((void *)(usize)arg2, &lds, sizeof(lds)) < 0)
            return (u64)-EFAULT;
          return 0;
        }
        if (cmd == IPC_SET) {
          if (syscall_copyin(&lds, (const void *)(usize)arg2, sizeof(lds)) < 0)
            return (u64)-EFAULT;
          return (u64)(isize)sysv_msgctl_set(msqid, (u16)lds.uid, (u16)lds.gid,
                                             lds.mode, lds.msg_qbytes);
        }
        return (u64)-EINVAL;
      }

      /* Calls b1nix knowingly does not implement. Each one names a subsystem
       * b1nix does not have; returning -ENOSYS is the documented Linux answer
       * for an unimplemented call and every libc has a fallback path. Listing
       * them here (instead of letting them fall through) keeps the boot log
       * free of "unmapped syscall" noise for calls that are not gaps to close. */
      /* ptrace(101)(request, pid, addr, data). A PEEK returns the word itself,
       * so it comes back through a separate out-parameter — the value is
       * indistinguishable from an error code otherwise. */
      /* process_vm_readv(310) / process_vm_writev(311): move bytes between two
       * processes' address spaces without stopping the target. A crash reporter
       * reaches for these before falling back to /proc/<pid>/mem, so the same
       * ptrace_may_access() check that guards attaching applies here. Partial
       * progress is reported honestly: the return value is the byte count that
       * actually moved. */
      if (number == 310 || number == 311) {
        int write = (number == 311);
        usize target_pid = (usize)arg0;
        const struct k_iovec_u {
          u64 base;
          u64 len;
        } *lvec = (const void *)(usize)arg1;
        usize liovcnt = (usize)arg2;
        const struct k_iovec_u *rvec = (const void *)(usize)arg3;
        usize riovcnt = (usize)arg4;
        if (arg5 != 0) /* flags must be 0 */
          return (u64)-EINVAL;
        if (liovcnt > 1024 || riovcnt > 1024)
          return (u64)-EINVAL;
        struct task *target = scheduler_task_by_pid(target_pid);
        if (!target)
          return (u64)-ESRCH;
        if (!ptrace_may_access(target))
          return (u64)-EPERM;

        usize moved = 0, li = 0, ri = 0;
        u64 loff = 0, roff = 0;
        struct k_iovec_u lcur = {0, 0}, rcur = {0, 0};
        u8 buf[512];
        while (li < liovcnt && ri < riovcnt) {
          if (lcur.len == loff) {
            if (syscall_copyin(&lcur, &lvec[li], sizeof(lcur)) < 0)
              return moved ? (u64)moved : (u64)-EFAULT;
            loff = 0;
            if (lcur.len == 0) { li++; continue; }
          }
          if (rcur.len == roff) {
            if (syscall_copyin(&rcur, &rvec[ri], sizeof(rcur)) < 0)
              return moved ? (u64)moved : (u64)-EFAULT;
            roff = 0;
            if (rcur.len == 0) { ri++; continue; }
          }
          usize chunk = sizeof(buf);
          if (chunk > lcur.len - loff) chunk = (usize)(lcur.len - loff);
          if (chunk > rcur.len - roff) chunk = (usize)(rcur.len - roff);

          isize rc;
          if (write) {
            if (syscall_copyin(buf, (const void *)(usize)(lcur.base + loff),
                               chunk) < 0)
              return moved ? (u64)moved : (u64)-EFAULT;
            rc = ptrace_copy_to_task(target, rcur.base + roff, buf, chunk);
          } else {
            rc = ptrace_copy_from_task(target, rcur.base + roff, buf, chunk);
            if (rc > 0 &&
                syscall_copyout((void *)(usize)(lcur.base + loff), buf,
                                (usize)rc) < 0)
              return moved ? (u64)moved : (u64)-EFAULT;
          }
          if (rc <= 0)
            return moved ? (u64)moved : (u64)rc;
          moved += (usize)rc;
          loff += (u64)rc;
          roff += (u64)rc;
          if (loff == lcur.len) { li++; loff = 0; lcur.len = 0; }
          if (roff == rcur.len) { ri++; roff = 0; rcur.len = 0; }
        }
        return (u64)moved;
      }

      if (number == 101) {
        u64 word = 0;
        isize rc = ptrace_request((long)arg0, (usize)arg1, arg2, arg3, &word);
        if (rc < 0)
          return (u64)rc;
        if ((long)arg0 == PTRACE_PEEKTEXT || (long)arg0 == PTRACE_PEEKDATA) {
          /* glibc/musl ptrace(2) wrappers pass a buffer in `data` for the PEEK
           * requests; the raw syscall returns the word in rax. Do both. */
          if (arg3)
            syscall_copyout((void *)(usize)arg3, &word, sizeof(word));
          return word;
        }
        return 0;
      }

      /* reboot(magic1, magic2, cmd, arg): the command Linux passes in arg2 is a
       * magic constant; SYS_REBOOT reads its own command from arg0. An
       * unrecognised command is EINVAL rather than a silent restart. */
      if (number == LINUX_NR_REBOOT) {
        switch ((u32)arg2) {
        case LINUX_REBOOT_CMD_POWER_OFF: arg0 = B1NIX_REBOOT_POWEROFF; break;
        case LINUX_REBOOT_CMD_HALT:      arg0 = B1NIX_REBOOT_HALT;     break;
        case LINUX_REBOOT_CMD_RESTART:   arg0 = B1NIX_REBOOT_RESTART;  break;
        /* CAD_OFF/CAD_ON only toggle the ctrl-alt-del action on Linux; b1nix
         * has none, so report success rather than failing PID 1's first call. */
        case LINUX_REBOOT_CMD_CAD_OFF:
        case LINUX_REBOOT_CMD_CAD_ON:    return 0;
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
        /* Report each missing call once, not once per call.
         *
         * A program that asks for something b1nix does not implement usually
         * asks repeatedly — Chromium calls get_mempolicy per allocation arena.
         * Printing every one turned a gap into a flood: several CPUs writing
         * the same line, the console lock hot enough for the spinlock watchdog
         * to call it a lockup, and a panic on a machine that was merely
         * missing a syscall. The first line is what a bring-up needs; the rest
         * is noise that changes the timing of what it is reporting on. */
        enum { UNMAPPED_SEEN_MAX = 512 };
        static u8 reported[UNMAPPED_SEEN_MAX];
        u32 slot = number < UNMAPPED_SEEN_MAX ? (u32)number : 0;
        if (!__atomic_exchange_n(&reported[slot], 1, __ATOMIC_RELAXED)) {
          console_write("linux-abi: unmapped syscall ");
          console_write(linux_syscall_name(number));
          console_write(" (nr=");
          console_write_dec(number);
          console_write(") -> -ENOSYS\n");
        }
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
    /* exit(2) ends this THREAD (Linux nr 60); exit_group(2) ends the process.
     * A group leader with live threads parks until they are done (M86). */
    scheduler_exit_thread((int)arg0);
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
  case SYS_MKNOD:
    return (u64)sys_mknod((const char *)(usize)arg0, (u32)arg1, arg2);
  case SYS_CLOCK_GETRES:
    return (u64)sys_clock_getres((int)arg0, (struct timespec *)(usize)arg1);
  case SYS_SIGTIMEDWAIT:
    return (u64)sys_sigtimedwait((const u64 *)(usize)arg0,
                                 (const struct timespec *)(usize)arg2);
  /* M95: loadable kernel modules. */
  case SYS_INIT_MODULE:
    return (u64)sys_init_module((const void *)(usize)arg0, arg1,
                                (const char *)(usize)arg2);
  case SYS_DELETE_MODULE:
    return (u64)sys_delete_module((const char *)(usize)arg0, (u32)arg1);
  case SYS_FINIT_MODULE:
    return (u64)sys_finit_module((int)arg0, (const char *)(usize)arg1,
                                 (u32)arg2);
  case SYS_FLOCK:
    /* Whole-file advisory lock. OpenRC's openrc-run takes one per service
     * before it runs any of its actions, so without this no service starts. */
    return (u64)filelock_flock((int)arg0, (int)arg1);
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
    return ns_pid_out((u64)scheduler_fork_current());
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
    /* The pid argument is an int, and must be read as one.
     *
     * glibc emits `mov $-1, %edi` for waitpid(-1, ...), so the register holds
     * 0x00000000ffffffff — the upper half is not sign-extended, because the
     * kernel is expected to truncate to int, as Linux's SYSCALL_DEFINE does.
     * Reading it as a 64-bit value turned "any child" into the pid
     * 4294967295, which no namespace could resolve: every wait() a Debian
     * shell made came back -ECHILD and every command it ran reported status
     * 255. (musl's wrapper widens to long, so this never showed up.) */
    usize wpid = (usize)(isize)(int)arg0;
    if ((isize)wpid > 0 && !(wpid = ns_pid_in(arg0)))
      return (u64)-ECHILD;
    u64 wr = (u64)scheduler_wait(wpid, &kstatus);
    if ((isize)wr >= 0 && current_task && current_task->user_image &&
        ((struct user_loaded_image *)current_task->user_image)->personality == PERSONALITY_LINUX)
      kstatus = wait_status_to_linux(kstatus);
    if ((isize)wr >= 0 && arg1 && syscall_copyout((void *)(usize)arg1, &kstatus, sizeof(kstatus)) != 0) {
      return (u64)-EFAULT;
    }
    return ns_pid_out(wr);
  }
  case SYS_WAITPID: {
    int kstatus = 0;
    /* Only a positive argument names a task; 0, -1 and the < -1 process-group
     * forms are relative to the caller and need no translation. */
    /* The pid argument is an int, and must be read as one.
     *
     * glibc emits `mov $-1, %edi` for waitpid(-1, ...), so the register holds
     * 0x00000000ffffffff — the upper half is not sign-extended, because the
     * kernel is expected to truncate to int, as Linux's SYSCALL_DEFINE does.
     * Reading it as a 64-bit value turned "any child" into the pid
     * 4294967295, which no namespace could resolve: every wait() a Debian
     * shell made came back -ECHILD and every command it ran reported status
     * 255. (musl's wrapper widens to long, so this never showed up.) */
    usize wpid = (usize)(isize)(int)arg0;
    if ((isize)wpid > 0 && !(wpid = ns_pid_in(arg0)))
      return (u64)-ECHILD;
    u64 wr = (u64)scheduler_waitpid(wpid, &kstatus, (int)arg2);
    if ((isize)wr >= 0 && current_task && current_task->user_image &&
        ((struct user_loaded_image *)current_task->user_image)->personality == PERSONALITY_LINUX)
      kstatus = wait_status_to_linux(kstatus);
    if ((isize)wr >= 0 && arg1 && syscall_copyout((void *)(usize)arg1, &kstatus, sizeof(kstatus)) != 0) {
      return (u64)-EFAULT;
    }
    /* ERESTARTSYS conversion and signal delivery handled by the wrapper. */
    return ns_pid_out(wr);
  }
  case SYS_GETPID:
    return ns_pid_out((u64)scheduler_get_pid());
  case SYS_GETPPID:
    /* A namespace's own pid 1 has a parent outside it. Linux reports 0 there,
     * because there is no number in this namespace that names it. */
    return (u64)(current_task
                     ? namespace_pid_to_user(current_task->parent_id)
                     : 0);
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
    {
      isize frc = vfs_ftruncate((int)arg0, arg1);
      /* Sizing an anonymous region is the step right after creating it, and a
       * refusal here reads to the caller exactly like being out of memory. */
      if (frc < 0 && bootinfo_has_flag("b1nix.trace-mmap")) {
        console_write("ftruncate(fd ");
        console_write_dec((u64)(int)arg0);
        console_write(", 0x");
        console_write_hex64(arg1);
        console_write(") -> error ");
        console_write_dec((u64)(-frc));
        console_write("\n");
      }
      return (u64)frc;
    }
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
    if (!c) return (u64)-EACCES;
    int rc = cred_set_uid(c, (u16)arg0);
    return rc == 0 ? 0 : (u64)-EPERM;
  }
  case SYS_SETGID: {
    klog_info("audit: setgid called");
    struct cred *c = scheduler_get_current_cred();
    if (!c) return (u64)-EACCES;
    int rc = cred_set_gid(c, (u16)arg0);
    return rc == 0 ? 0 : (u64)-EPERM;
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
  case SYS_WAITID: {
    usize wid = (usize)(isize)(int)arg1;
    if ((arg0 == P_PID || arg0 == P_PGID) && arg1 != 0 && !(wid = ns_pid_in(arg1)))
      return (u64)-ECHILD;
    return (u64)scheduler_waitid((idtype_t)arg0, wid,
                                 (siginfo_t *)(usize)arg2, (int)arg3);
  }
  case SYS_TIMES: {
    struct tms *user_tms = (struct tms *)(usize)arg0;
    if (user_tms) {
      struct tms k_tms;
      /* POSIX: times() reports the PROCESS, so every thread's CPU time counts
       * (M86 — this used to report the calling thread alone). */
      u64 gu = 0, gs = 0;
      task_group_cputime_ns(current_task, &gu, &gs);
      k_tms.tms_utime = (clock_t)(gu / NS_PER_USER_TICK);
      k_tms.tms_stime = (clock_t)(gs / NS_PER_USER_TICK);
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

    /* M86: nanosecond accounting, so the microsecond field is exact rather than
     * a 10 ms tick scaled up. RUSAGE_SELF is the whole thread group; only
     * RUSAGE_THREAD is the calling thread. */
    u64 uns = 0, sns = 0;
    if (who == RUSAGE_SELF) {
      task_group_cputime_ns(current_task, &uns, &sns);
    } else if (who == RUSAGE_CHILDREN) {
      uns = task_cutime_ns(current_task);
      sns = task_cstime_ns(current_task);
    } else if (who == RUSAGE_THREAD) {
      uns = task_utime_ns(current_task);
      sns = task_stime_ns(current_task);
    } else {
      return (u64)-EINVAL;
    }
    k_ru.ru_utime.tv_sec = (i64)(uns / 1000000000ULL);
    k_ru.ru_utime.tv_usec = (i64)((uns % 1000000000ULL) / 1000ULL);
    k_ru.ru_stime.tv_sec = (i64)(sns / 1000000000ULL);
    k_ru.ru_stime.tv_usec = (i64)((sns % 1000000000ULL) / 1000ULL);
    if (who != RUSAGE_CHILDREN) {
      k_ru.ru_nvcsw = (long)task_nvcsw(current_task);
      k_ru.ru_nivcsw = (long)task_nivcsw(current_task);
      /* Resident set high-water mark, in kilobytes, as Linux reports it: the
       * sampler measures the address space now and keeps the largest value it
       * has ever seen (samples are also taken just before every unmap, so a
       * peak that has already been released still counts). */
      u64 peak = task_rss_sample(current_task, 1);
      k_ru.ru_maxrss = (long)(peak * (PAGE_SIZE / 1024));
    }

    if (syscall_copyout(user_ru, &k_ru, sizeof(struct rusage)) < 0) {
      return (u64)-EFAULT;
    }
    return 0;
  }
  case SYS_GETPGID: {
    usize t = ns_pid_in(arg0);
    if (arg0 != 0 && !t)
      return (u64)-ESRCH;
    return ns_pid_out((u64)scheduler_getpgid(t));
  }
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
    if (!cred_has_cap(c, CAP_SETGID)) {
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
    u64 ticks = 0;
    u64 tick_ns = 1000000000ULL / (u64)sched_tick_hz();
    u64 slept_ticks = syscall_sleep_timespec(&ts, &ticks);
    u64 sleep_start = 0;

    (void)sleep_start;
    /* Interrupted, not finished. A sleep cut short by a signal must say so,
     * with the time left in `rem` — a caller told the sleep completed simply
     * carries on, and one told nothing about the remainder cannot resume it.
     * Programs park here with timeouts of years precisely because a signal is
     * what they expect to end the wait. */
    int interrupted = slept_ticks < ticks && scheduler_signal_pending_any();
    u64 slept = slept_ticks;
    if (arg1) {
      u64 left = interrupted ? (ticks - slept) : 0;
      struct timespec rem;
      rem.tv_sec = (i64)(left / (u64)sched_tick_hz());
      rem.tv_nsec = (i64)((left % (u64)sched_tick_hz()) * tick_ns);
      if (syscall_copyout((void *)(usize)arg1, &rem, sizeof(rem)) != 0)
        return (u64)-EFAULT;
    }
    return interrupted ? (u64)-EINTR : 0;
  }
  case SYS_KILL: {
    /* POSIX kill(2) pid decoding: 0 = caller's process group, -1 = every
     * process the caller may signal, < -1 = process group |pid|, > 0 = that
     * process. The old code sent pid 0 to a task lookup (always failing) and
     * pid -1 to process group 1. */
    /* Read as an int: a caller that loaded -1 into a 32-bit register leaves
     * the upper half zero (see the note in SYS_WAITPID). */
    isize target = (isize)(int)arg0;
    u64 kill_ret;
    /* Translate before decoding: a namespaced caller names both a process and
     * a process group by the numbers its namespace uses. */
    if (target > 0) {
      usize t = ns_pid_in((u64)target);
      if (!t)
        return (u64)-ESRCH;
      target = (isize)t;
    } else if (target < -1) {
      usize t = ns_pid_in((u64)(-target));
      if (!t)
        return (u64)-ESRCH;
      target = -(isize)t;
    }
    if (target == 0) {
      kill_ret = (u64)scheduler_kill_process_group_user(scheduler_getpgrp(),
                                                        (int)arg1);
    } else if (target == -1) {
      kill_ret = (u64)scheduler_kill_all_user((int)arg1);
    } else if (target < 0) {
      kill_ret =
          (u64)scheduler_kill_process_group_user((usize)(-target), (int)arg1);
    } else {
      /* M86: a positive pid names a PROCESS. The signal goes to a thread in
       * that group that does not block it, and stop/continue act on the whole
       * group — kill(pid) used to hit the leader alone. */
      kill_ret =
          (u64)scheduler_kill_thread_group_user((usize)target, (int)arg1);
    }
    /* Signal delivery handled by the wrapper. */
    return kill_ret;
  }
  case SYS_TKILL: {
    /* tkill(tid, sig): no thread-group check (that is what tgkill adds). */
    usize tid = ns_pid_in(arg0);
    if (!tid)
      return (u64)-ESRCH;
    return (u64)scheduler_tkill(0, tid, (int)arg1);
  }
  case SYS_TGKILL: {
    usize tgid = ns_pid_in(arg0);
    usize tid = ns_pid_in(arg1);
    if (!tgid || !tid)
      return (u64)-ESRCH;
    return (u64)scheduler_tkill(tgid, tid, (int)arg2);
  }
  case SYS_RT_TGSIGQUEUEINFO: {
    /* rt_tgsigqueueinfo(tgid, tid, sig, siginfo): send a signal to one thread
     * of a thread group. M86 targets the named tid for real (it used to
     * re-raise on the caller regardless of who was addressed, which is only
     * correct when a crash handler addresses itself). The siginfo payload
     * (arg3) is still dropped for non-RT signals, which have no queue to carry
     * it; an RT signal keeps its queued instance via scheduler_sigqueue. */
    int rsig = (int)arg2;
    if (rsig <= 0 || rsig >= 64)
      return (u64)-EINVAL;
    usize rtgid = ns_pid_in(arg0);
    usize rtid = ns_pid_in(arg1);
    if (!rtgid || !rtid)
      return (u64)-ESRCH;
    return (u64)scheduler_tkill(rtgid, rtid, rsig);
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
    usize qpid = ns_pid_in(arg0);
    if (!qpid)
      return (u64)-ESRCH;
    if (SIG_IS_RT(sig))
      return (u64)scheduler_sigqueue(qpid, sig, v, B1NIX_SI_QUEUE);
    return (u64)scheduler_kill(qpid, sig);
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
     * ticks at the rate the timer was programmed with (see sc_time_to_ticks --
     * a hardcoded 100 here made every POSIX timer fire ten times early);
     * it_value all-zero disarms. TIMER_ABSTIME (flags&1) is treated relative
     * (the smoke uses relative arming). */
    struct k_timespec { i64 tv_sec; i64 tv_nsec; };
    struct k_itimerspec { struct k_timespec it_interval; struct k_timespec it_value; } its;
    if (!arg2)
      return (u64)-EINVAL;
    if (syscall_copyin(&its, (void *)(usize)arg2, sizeof(its)) < 0)
      return (u64)-EFAULT;
    u64 first = sc_time_to_ticks((u64)its.it_value.tv_sec,
                                 (u64)its.it_value.tv_nsec, 1000000000ull);
    u64 interval = sc_time_to_ticks((u64)its.it_interval.tv_sec,
                                    (u64)its.it_interval.tv_nsec, 1000000000ull);
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
      u64 osec = 0, onsec = 0;
      sc_ticks_to_time(old_rem, &osec, &onsec, 1000000000ull);
      old.it_value.tv_sec = (i64)osec;
      old.it_value.tv_nsec = (i64)onsec;
      sc_ticks_to_time(old_int, &osec, &onsec, 1000000000ull);
      old.it_interval.tv_sec = (i64)osec;
      old.it_interval.tv_nsec = (i64)onsec;
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
    u64 gsec = 0, gnsec = 0;
    sc_ticks_to_time(rem, &gsec, &gnsec, 1000000000ull);
    its.it_value.tv_sec = (i64)gsec;
    its.it_value.tv_nsec = (i64)gnsec;
    sc_ticks_to_time(interval, &gsec, &gnsec, 1000000000ull);
    its.it_interval.tv_sec = (i64)gsec;
    its.it_interval.tv_nsec = (i64)gnsec;
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
  case SYS_GETSID: {
    usize t = ns_pid_in(arg0);
    if (arg0 != 0 && !t)
      return (u64)-ESRCH;
    return ns_pid_out((u64)scheduler_getsid(t));
  }
  case SYS_GETPGRP:
    return ns_pid_out((u64)scheduler_getpgrp());
  case SYS_SETPGRP: {
    usize who = ns_pid_in(arg0);
    usize grp = ns_pid_in(arg1);
    if ((arg0 != 0 && !who) || (arg1 != 0 && !grp))
      return (u64)-ESRCH;
    return (u64)scheduler_setpgrp(who, grp);
  }
  case SYS_SETPRIORITY: {
    /* Linux setpriority(int which, id_t who, int prio): args are
     * arg0=which, arg1=who, arg2=prio. Only PRIO_PROCESS is supported; who==0
     * means the calling process. The old code read arg0 as the pid and arg1 as
     * the value, so it stored `who` (0) as the nice value and ignored the real
     * prio in arg2 — every nice()/setpriority() collapsed to nice 0, breaking
     * nice biasing (M46 nice-biasing). */
    usize who = ns_pid_in(arg1);
    int prio = (int)arg2;
    if (arg1 != 0 && !who)
      return (u64)-ESRCH;
    usize pid = who == 0 ? scheduler_get_pid() : who;
    return (u64)scheduler_set_priority(pid, prio);
  }
  case SYS_GETPRIORITY: {
    /* Linux getpriority(int which, id_t who): arg0=which, arg1=who. */
    usize who = ns_pid_in(arg1);
    if (arg1 != 0 && !who)
      return (u64)-ESRCH;
    usize pid = who == 0 ? scheduler_get_pid() : who;
    /* scheduler_get_priority already returns the Linux 20-nice encoding. */
    return (u64)(isize)scheduler_get_priority(pid);
  }
  case SYS_BRK:
    /* A shrinking brk drops pages — same reason as munmap. */
    task_rss_sample(current_task, 0);
    return sys_brk(arg0);
  case SYS_MMAP: {
    unsigned vma_slot = vma_mutator_lock();
    u64 r = sys_mmap((void *)(usize)arg0, (usize)arg1, (int)arg2, (int)arg3,
                     (int)arg4, (isize)arg5);
    vma_mutator_unlock(vma_slot);
    if (!mmap_failed(r))
      vma_trace_record("mmap", r, r + ((arg1 + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1)));
    return r;
  }
  case SYS_MUNMAP: {
    /* Sample before the pages go: this is where the resident set falls, so it
     * is where an unrecorded peak would be lost (M86). */
    task_rss_sample(current_task, 0);
    unsigned vma_slot = vma_mutator_lock();
    u64 r = (u64)sys_munmap((void *)(usize)arg0, (usize)arg1);
    vma_mutator_unlock(vma_slot);
    return r;
  }
  case SYS_UNSHARE:
    return (u64)sys_unshare(arg0);
  case SYS_SETNS:
    return (u64)sys_setns((int)arg0, (int)arg1);
  case SYS_MREMAP: {
    task_rss_sample(current_task, 0);
    unsigned vma_slot = vma_mutator_lock();
    u64 r = sys_mremap((void *)(usize)arg0, (usize)arg1, (usize)arg2, (int)arg3,
                       (void *)(usize)arg4);
    vma_mutator_unlock(vma_slot);
    return r;
  }
  case SYS_MPROTECT: {
    unsigned vma_slot = vma_mutator_lock();
    u64 r = (u64)sys_mprotect((void *)(usize)arg0, (usize)arg1, (int)arg2);
    vma_mutator_unlock(vma_slot);
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
    /* PR_SET_PTRACER (Yama): nominate the process allowed to ptrace this one
     * when /proc/sys/kernel/yama/ptrace_scope is 1. arg1 == 0 withdraws the
     * declaration, PR_SET_PTRACER_ANY allows any process. This is how a crash
     * handler that is not the crashing process's parent gets permission to
     * attach after the fault. */
    if (option == PR_SET_PTRACER) {
      usize tracer = (arg1 == (u64)PR_SET_PTRACER_ANY) ? PTRACE_ANY_TRACER
                                                       : (usize)arg1;
      if (tracer != 0 && tracer != PTRACE_ANY_TRACER &&
          !scheduler_task_by_pid(tracer))
        return (u64)-EINVAL;
      return (u64)ptrace_set_declared_tracer(current_task, tracer);
    }
    /* PR_SET_PDEATHSIG (1) / PR_GET_PDEATHSIG (2): Chromium sets this on every
     * child it forks and treats the failure as fatal to that child, so
     * returning -EINVAL here killed every subprocess it started. */
    if (option == 1)
      return (u64)scheduler_set_pdeathsig(current_task->id, (int)arg1);
    /* PR_SET_NAME (15) / PR_GET_NAME (16): the comm name is the task's own
     * string, which we do not rewrite; accept and ignore rather than fail, as
     * callers use it purely for diagnostics. */
    /* PR_SET_NAME (15): really change comm. Accepting it and doing nothing
     * meant a process that renamed itself was still listed under the name of
     * the file it was exec'd from. */
    if (option == 15) {
      char nm[16];
      if (syscall_copyin(nm, (const void *)(usize)arg1, sizeof(nm)) != 0) {
        /* Linux copies up to 16 bytes and tolerates a shorter string as long
         * as the NUL is inside the buffer; fall back to a string copy so a
         * name at the very end of a mapping still works. */
        if (syscall_copyinstr(nm, sizeof(nm), (const char *)(usize)arg1) < 0)
          return (u64)-EFAULT;
      }
      nm[sizeof(nm) - 1] = '\0';
      return (u64)scheduler_set_comm(current_task->id, nm);
    }
    /* PR_GET_NAME (16): the caller's comm, 16 bytes including the NUL. */
    if (option == 16) {
      char comm[16];
      const char *nm = current_task && current_task->name ? current_task->name : "";
      usize i = 0;
      for (; nm[i] && i < sizeof(comm) - 1; i++)
        comm[i] = nm[i];
      comm[i] = '\0';
      if (syscall_copyout((void *)(usize)arg1, comm, sizeof(comm)) != 0)
        return (u64)-EFAULT;
      return 0;
    }
    /* PR_GET_PDEATHSIG (2). */
    if (option == 2) {
      int sig = scheduler_get_pdeathsig(current_task->id);
      if (syscall_copyout((void *)(usize)arg1, &sig, sizeof(sig)) != 0)
        return (u64)-EFAULT;
      return 0;
    }
    /* PR_GET_DUMPABLE (3) / PR_SET_DUMPABLE (4). Every task here is dumpable:
     * b1nix has no suid-binary core-dump suppression to switch off. */
    if (option == 3)
      return 1;
    if (option == 4)
      return (arg1 == 0 || arg1 == 1 || arg1 == 2) ? 0 : (u64)-EINVAL;
    /* PR_GET_KEEPCAPS (7) / PR_SET_KEEPCAPS (8): the securebits KEEP_CAPS flag
     * under its older name. Unlike PR_SET_SECUREBITS this needs no privilege. */
    if (option == 7) {
      struct cred *c = scheduler_get_current_cred();
      return (u64)((cred_get_securebits(c) & SECBIT(SECURE_KEEP_CAPS)) ? 1 : 0);
    }
    if (option == 8) {
      struct cred *c = scheduler_get_current_cred();
      if (!c)
        return (u64)-EINVAL;
      if (arg1 > 1)
        return (u64)-EINVAL;
      if (c->securebits & SECBIT(SECURE_KEEP_CAPS_LOCKED))
        return (u64)-EPERM;
      if (arg1)
        c->securebits |= SECBIT(SECURE_KEEP_CAPS);
      else
        c->securebits &= ~SECBIT(SECURE_KEEP_CAPS);
      return 0;
    }
    /* PR_CAPBSET_READ (23) / PR_CAPBSET_DROP (24). */
    if (option == 23) {
      struct cred *c = scheduler_get_current_cred();
      if (!c || (int)arg1 < 0 || (int)arg1 > CAP_LAST)
        return (u64)-EINVAL;
      return (u64)((c->cap_bounding >> (int)arg1) & 1);
    }
    if (option == 24) {
      struct cred *c = scheduler_get_current_cred();
      if (!c || (int)arg1 < 0 || (int)arg1 > CAP_LAST)
        return (u64)-EINVAL;
      if (!cred_has_cap(c, CAP_SETPCAP))
        return (u64)-EPERM;
      /* The bounding set is a CEILING on what may be gained, not a statement
       * about what is held: dropping a capability from it must not take that
       * capability away from the running process. Refreshing here did, so the
       * moment systemd's bounding-set loop reached CAP_SETPCAP it lost the
       * privilege the loop itself needs and every remaining drop returned
       * EPERM — "Failed to drop capabilities" for every service that names a
       * CapabilityBoundingSet, journald included. */
      c->cap_bounding &= ~(1ULL << (int)arg1);
      c->cap_inheritable &= c->cap_bounding;
      return 0;
    }
    /* PR_CAP_AMBIENT (47): the set a non-setuid execve carries across.
     *
     * systemd calls this before spawning EVERY process -- once to clear the
     * set, then once per capability the unit asks to keep -- and treats a
     * failure as fatal to the spawn ("Failed to apply the starting ambient
     * set"). Answering EINVAL therefore did not degrade the machine, it
     * stopped it: no unit could start at all.
     *
     * Linux's rule, kept here: a capability may be raised into the ambient set
     * only while it is both permitted and inheritable, and it leaves the set
     * as soon as it leaves either. */
    if (option == 47) {
      enum {
        CAP_AMBIENT_IS_SET = 1,
        CAP_AMBIENT_RAISE = 2,
        CAP_AMBIENT_LOWER = 3,
        CAP_AMBIENT_CLEAR_ALL = 4,
      };
      struct cred *c = scheduler_get_current_cred();
      if (!c)
        return (u64)-EINVAL;
      if ((u32)arg1 == CAP_AMBIENT_CLEAR_ALL) {
        /* The unused arguments must be zero; Linux checks and so do we,
         * because a caller passing something there means something else. */
        if (arg2 || arg3 || arg4)
          return (u64)-EINVAL;
        c->cap_ambient = 0;
        return 0;
      }
      if ((int)arg2 < 0 || (int)arg2 > CAP_LAST || arg3 || arg4)
        return (u64)-EINVAL;
      u64 bit = 1ULL << (int)arg2;
      switch ((u32)arg1) {
      case CAP_AMBIENT_IS_SET:
        return (u64)((c->cap_ambient & bit) ? 1 : 0);
      case CAP_AMBIENT_RAISE:
        if (!(c->cap_permitted & bit) || !(c->cap_inheritable & bit))
          return (u64)-EPERM;
        /* SECBIT_NOROOT-style locking: Linux refuses a raise once the ambient
         * set has been locked by securebits. b1nix models the lock bits, so
         * the same refusal applies. */
        if (c->securebits & SECBIT(SECURE_NO_CAP_AMBIENT_RAISE))
          return (u64)-EPERM;
        c->cap_ambient |= bit;
        return 0;
      case CAP_AMBIENT_LOWER:
        c->cap_ambient &= ~bit;
        return 0;
      default:
        return (u64)-EINVAL;
      }
    }
    /* PR_GET_SECUREBITS (27) / PR_SET_SECUREBITS (28). */
    if (option == 27)
      return (u64)cred_get_securebits(scheduler_get_current_cred());
    if (option == 28)
      return (u64)cred_set_securebits(scheduler_get_current_cred(), (u32)arg1);
    /* PR_SET_TIMERSLACK (29) / PR_GET_TIMERSLACK (30). The scheduler wakes on
     * the tick, so the slack a caller can choose is already the tick; report
     * that rather than a number nothing honours -- and report the tick this
     * kernel actually runs at, not the one it used to. */
    if (option == 29)
      return 0;
    if (option == 30)
      return (u64)(1000000000ULL / sc_tick_hz());
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
    int mrc = vfs_memfd_create(name, (u32)arg1);
    /* An anonymous shared region is how one process hands state to another,
     * and a caller that cannot get one has nowhere to put that state — the
     * browser treats the failure as fatal and aborts before it opens a
     * window, naming only the size it wanted. Say whether we gave it one. */
    if (bootinfo_has_flag("b1nix.trace-mmap") || mrc < 0) {
      console_write("memfd_create(\"");
      console_write(name);
      console_write("\", flags=0x");
      console_write_hex64((u64)(u32)arg1);
      console_write(") -> ");
      if (mrc < 0) {
        console_write("error ");
        console_write_dec((u64)(-mrc));
      } else {
        console_write("fd ");
        console_write_dec((u64)mrc);
      }
      console_write("\n");
    }
    return (u64)mrc;
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

  case SYS_READAHEAD:
    return (u64)sys_readahead((int)arg0, arg1, (usize)arg2);

  case SYS_PIVOT_ROOT:
    return (u64)sys_pivot_root((const char *)(usize)arg0,
                               (const char *)(usize)arg1);

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
    /* M86: the CPU-time clocks report real per-thread/per-process CPU time,
     * not uptime. CLOCK_PROCESS_CPUTIME_ID(2) is the thread group's
     * user+system time; CLOCK_THREAD_CPUTIME_ID(3) is the calling thread's.
     * Negative ids are the dynamic per-task clocks that
     * clock_getcpuclockid(3)/pthread_getcpuclockid(3) hand out. */
    if (clk_id < 0 || clk_id == 2 || clk_id == 3) {
      u64 ns = 0;
      if (sys_cpu_clock_ns(clk_id, &ns) < 0)
        return (u64)-EINVAL;
      ktp.tv_sec = (i64)(ns / 1000000000ULL);
      ktp.tv_nsec = (i64)(ns % 1000000000ULL);
      if (syscall_copyout((void *)(usize)arg1, &ktp, sizeof(struct timespec)) != 0)
        return (u64)-EFAULT;
      return 0;
    }
    /* Monotonic family: CLOCK_MONOTONIC(1), CLOCK_MONOTONIC_RAW(4),
     * CLOCK_MONOTONIC_COARSE(6), CLOCK_BOOTTIME(7) -> uptime monotonic clock.
     * b1nix has no separate boot/raw clocks. CLOCK_REALTIME(0) and
     * CLOCK_REALTIME_COARSE(5) -> wall clock. */
    /* Resolution: the cycle counter where the CPU guarantees it runs at a
     * constant rate, the 100 Hz tick otherwise. A tick-derived reading lands
     * on a 10 ms boundary, which is too coarse for a program that schedules
     * work in milliseconds — durations come back as 0 or 10 ms and nothing in
     * between. COARSE clocks keep the tick on purpose: their whole contract is
     * to be cheap and approximate. */
    u64 mono_ns = (clk_id == 6 || clk_id == 5) ? 0 : arch_tsc_monotonic_ns();

    if (clk_id == 1 || clk_id == 4 ||
        clk_id == 6 || clk_id == 7) {
      if (mono_ns) {
        ktp.tv_sec = (i64)(mono_ns / 1000000000ull);
        ktp.tv_nsec = (i64)(mono_ns % 1000000000ull);
      } else {
        u64 tsec = 0, tnsec = 0;
        sc_ticks_to_time(ticks, &tsec, &tnsec, 1000000000ull);
        ktp.tv_sec = (i64)tsec;
        ktp.tv_nsec = (i64)tnsec;
      }
    } else {
      /* CLOCK_REALTIME / CLOCK_REALTIME_COARSE: epoch-based wall clock, taken
       * whole from one source. Composing the seconds from the tick counter and
       * the nanoseconds from the TSC made the clock walk backwards by up to a
       * second (see rtc_now_unix_nanos). */
      u64 wall_ns = rtc_now_unix_nanos();
      ktp.tv_sec = (i64)(wall_ns / 1000000000ull);
      ktp.tv_nsec = (i64)(wall_ns % 1000000000ull);
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
    if (!c || (!cred_has_cap(c, CAP_SYS_TIME)))
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
    /* sched_getaffinity(pid, cpusetsize, mask): the task's actual mask — the
     * set sched_setaffinity installed, or every online CPU when it never did.
     * Returns the number of bytes written, matching the raw-syscall
     * convention. */
    usize cpusetsize = (usize)arg1;
    void *umask = (void *)(usize)arg2;
    if (!umask || cpusetsize == 0) return (u64)-EINVAL;
    u64 mask = scheduler_get_affinity((usize)arg0);
    u8 kmask[128];
    usize n = cpusetsize < sizeof(kmask) ? cpusetsize : sizeof(kmask);
    memset(kmask, 0, n);
    for (usize b = 0; b < n && b < 8; b++)
      kmask[b] = (u8)(mask >> (b * 8));
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
    return (u64)scheduler_clone_thread(arg0, arg1, arg2, arg3, arg4, arg5, 0, 0, 0, 0);
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
    /* The THREAD's id, which is not the process's.
     *
     * scheduler_get_pid answers with the thread-group id — correct for
     * getpid(), and wrong here. With every thread reporting the same number,
     * anything that identifies a thread by it sees one thread: libc++abi's
     * static-initialisation guard concluded that a second thread entering an
     * initialiser was the same thread re-entering it, called that recursive
     * initialisation, and aborted the process. Chromium died that way before
     * it could open a window. */
    return current_task ? ns_pid_out((u64)current_task->id) : 0;
  case SYS_EXIT_THREAD:
    /* SYS_EXIT_THREAD(code) — thread-only exit. For an is_thread task
     * scheduler_exit_current already handles the CLONE_CHILD_CLEARTID
     * futex wake. For a process leader this acts the same as SYS_EXIT. */
    scheduler_exit_thread((int)arg0);
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
        (timeout_ms == (u64)-1)
            ? (u64)-1
            : (timeout_ms + (1000ull / sched_tick_hz()) - 1) /
                  (1000ull / sched_tick_hz());

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

  case SYS_SCHED_GETSCHEDULER:
    /* One policy for every task here, so the honest answer is SCHED_OTHER (0)
     * rather than ENOSYS. A crash reporter asks this per thread while walking
     * a process and logs a failure for each one when it is missing. */
    ret = 0;
    break;
  case SYS_SCHED_SETSCHEDULER:
    /* Accept a request for the policy we already run, refuse the rest — a
     * silent "yes" to SCHED_FIFO would promise real-time scheduling that this
     * kernel does not provide. */
    ret = ((int)arg1 == 0) ? 0 : (u64)-EINVAL;
    break;
  case SYS_SCHED_GETPARAM: {
    /* sched_param is one int, and under SCHED_OTHER it is always zero. */
    int prio = 0;
    if (arg1 && syscall_copyout((void *)(usize)arg1, &prio, sizeof(prio)) != 0)
      return (u64)-EFAULT;
    ret = 0;
    break;
  }
  case SYS_SCHED_SETPARAM:
    ret = 0;
    break;
  case SYS_SCHED_GET_PRIORITY_MAX:
  case SYS_SCHED_GET_PRIORITY_MIN:
    /* SCHED_OTHER's range is [0, 0] on Linux too. */
    ret = ((int)arg0 == 0) ? 0 : (u64)-EINVAL;
    break;

  case SYS_SET_ROBUST_LIST:
    /* Remembered, not acted on. A robust mutex held by a thread that dies is
     * still not released — that needs the list walked at exit, which is not
     * implemented. Accepting the registration is what libc expects, and it
     * costs nothing; the previous mapping onto sync(2) cost a full filesystem
     * flush per thread created. */
    ret = 0;
    break;
  case SYS_GET_ROBUST_LIST:
    /* Nothing is stored, so report an empty registration rather than claim a
     * list exists. */
    ret = (u64)-ENOSYS;
    break;

  case SYS_SET_TID_ADDRESS:
    /* set_tid_address(tidptr): store the clear-child-tid pointer and return
     * the calling thread's TID.  musl calls this during __init_tls. */
    task_set_child_tid_clear(current_task, (u64)arg0);
    /* Returns the caller's THREAD id, like gettid — musl stores it as the
     * thread's own tid during __init_tls, and a process-wide value there makes
     * every thread believe it is the group leader. */
    ret = current_task ? (u64)current_task->id : 0;
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
