/*
 * ARCHIVED — not built. The native b1nix syscall ABI's dispatch cases.
 *
 * Removed from kernel/syscall/syscall.c when every user image became a Linux
 * image (see README.md). Kept verbatim so the native ABI can be restored if
 * b1nix grows its own userspace again: the handlers go back above
 * syscall_dispatch_impl_inner, the cases into its `switch (number)`.
 */

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

  case SYS_OPEN: {
    return (u64)sys_open((const char *)(usize)arg0, (int)arg1);
  }

  case SYS_STAT: {
    return (u64)sys_stat((const char *)(usize)arg0,
                         (struct b1nix_stat *)(usize)arg1);
  }

  case SYS_LSTAT:
    return (u64)sys_lstat((const char *)(usize)arg0,
                          (struct b1nix_stat *)(usize)arg1);

  case SYS_CREATE:
    return (u64)sys_create((const char *)(usize)arg0, (u32)arg1);

  case SYS_SIGTIMEDWAIT:
    return (u64)sys_sigtimedwait((const u64 *)(usize)arg0,
                                 (const struct timespec *)(usize)arg2);

  case SYS_GETDENTS:
    return (u64)sys_getdents((int)arg0, (struct dirent *)(usize)arg1,
                             (usize)arg2);

  case SYS_READDIR:
    return (u64)sys_readdir((const char *)(usize)arg0,
                            (struct dirent *)(usize)arg1, (usize)arg2);

  case SYS_SIGSUSPEND: {
    /* Signal delivery handled by the wrapper after we return. */
    u64 r = sys_sigsuspend((const u64 *)(usize)arg0);
    return r;
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

  case SYS_SEND:
    return sys_send((int)arg0, (const void *)(usize)arg1, (usize)arg2,
                    (int)arg3);

  case SYS_RECV:
    return sys_recv((int)arg0, (void *)(usize)arg1, (usize)arg2, (int)arg3);

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

  case SYS_READAHEAD:
    return (u64)sys_readahead((int)arg0, arg1, (usize)arg2);

  case SYS_CLEAR:
    return sys_clear();

  case SYS_SET_STDOUT:
    scheduler_set_stdout((int)arg0);
    return 0;

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

