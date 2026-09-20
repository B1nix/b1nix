/*
 * ARCHIVED — not built. The native b1nix syscall ABI's own entry points.
 *
 * Removed from kernel/syscall/syscall.c when every user image became a Linux
 * image (see README.md). Kept verbatim so the native ABI can be restored if
 * b1nix grows its own userspace again: the handlers go back above
 * syscall_dispatch_impl_inner, the cases into its `switch (number)`.
 */

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

