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
#include <string.h>

int syscall_copyin(void *dst, const void *user_src, usize size) {
  if (size == 0)
    return 0;
  if (!dst || !user_src)
    return -1;
  memcpy(dst, user_src, size);
  return 0;
}

int syscall_copyout(void *user_dst, const void *src, usize size) {
  if (size == 0)
    return 0;
  if (!user_dst || !src)
    return -1;
  memcpy(user_dst, src, size);
  return 0;
}

int syscall_copyinstr(char *dst, usize dst_size, const char *user_src) {
  if (!dst || dst_size == 0 || !user_src)
    return -1;

  for (usize i = 0; i < dst_size; i++) {
    char c;
    if (syscall_copyin(&c, user_src + i, 1) != 0)
      return -1;
    dst[i] = c;
    if (c == '\0')
      return 0;
  }

  dst[dst_size - 1] = '\0';
  return -1;
}

static isize sys_read(int fd, void *buf, usize count) {
  char kbuf[4096];
  if (count > 4096)
    count = 4096;
  isize res = vfs_read(fd, kbuf, count);
  if (res > 0) {
    if (syscall_copyout(buf, kbuf, (usize)res) < 0)
      return -EFAULT;
  }
  return res;
}

static isize sys_write(int fd, const void *buf, usize count) {
  char kbuf[4096];
  if (count > 4096)
    count = 4096;
  if (syscall_copyin(kbuf, buf, count) < 0) {
    return -EFAULT;
  }
  isize res = vfs_write(fd, kbuf, count);
  return res;
}

static u64 sys_list(const char *dir_path) {
  const char *paths[64];
  usize count = vfs_list(dir_path, paths, 64);

  for (usize i = 0; i < count; i++) {
    console_write(paths[i]);
    console_write("\n");
  }

  return count;
}

static u64 sys_read_file(const char *path) {
  const struct initramfs_file *file = initramfs_find(path);

  if (file == 0) {
    return (u64)-ENOENT;
  }

  console_write(file->data);
  return file->size;
}

#ifndef __aarch64__
static u64 sys_read_kbd(void) {
  char c = 0;
  if (vfs_read(0, &c, 1) == 1)
    return (u64)c;
  return 0;
}
#endif

static u64 sys_readdir(const char *dir_path, struct dirent *buf,
                       usize max_entries) {
  const char *names[128];
  isize count = vfs_list(dir_path, names, 128);
  if (count < 0)
    return (u64)count;
  usize out_count = (usize)count;
  if (out_count > max_entries)
    out_count = max_entries;

  for (usize i = 0; i < out_count; i++) {
    usize len = strlen(names[i]);
    if (len > 63)
      len = 63;
    memcpy(buf[i].name, names[i], len);
    buf[i].name[len] = '\0';

    /* Try to get more info by resolving the path */
    char full_path[256];
    usize dirlen = strlen(dir_path);
    memcpy(full_path, dir_path, dirlen);
    if (dirlen > 0 && full_path[dirlen - 1] != '/') {
      full_path[dirlen++] = '/';
    }
    memcpy(full_path + dirlen, names[i], len + 1);

    struct vfs_node *node = vfs_find_node(full_path);
    if (!IS_ERR(node)) {
      buf[i].type = (u32)node->inode->type;
      buf[i].is_dir = (node->inode->type == VFS_DIRECTORY) ? 1 : 0;
      buf[i].is_exec = (node->inode->mode & 0111) ? 1 : 0;
      buf[i].size = node->inode->size;
    } else {
      buf[i].type = 0;
      buf[i].is_dir = 0;
      buf[i].is_exec = 0;
      buf[i].size = 0;
    }
  }

  return out_count;
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

static u64 sys_execve(const char *path, const char **argv, const char **envp) {
  char kernel_path[VFS_MAX_PATH];
  vfs_resolve_path(path, kernel_path);
  if (kernel_path[0] == '\0')
    return (u64)-ENOENT;
  return (u64)user_execve_current(kernel_path, argv, envp);
}

static u64 sys_ioctl(int fd, u64 request, void *arg) {
  return (u64)vfs_ioctl(fd, request, arg);
}

static u64 sys_selfhost_status(struct b1nix_selfhost_status *status) {
  if (!status)
    return (u64)-EFAULT;
  memset(status, 0, sizeof(*status));
  status->abi_version = 17;
  status->target_ready = 1;
  status->binutils_ready = 1;
  status->make_ready = 1;
  status->can_build_kernel_inside_b1nix = 0;
  copy_cstr(status->target_triple, sizeof(status->target_triple),
            "x86_64-b1nix");
  copy_cstr(status->compiler, sizeof(status->compiler), "gcc-port-manifest");
  copy_cstr(status->assembler, sizeof(status->assembler), "b1nix-as-abi");
  copy_cstr(status->linker, sizeof(status->linker), "b1nix-ld-abi");
  copy_cstr(status->make, sizeof(status->make), "nmake");
  return 0;
}

static isize sys_mkdir(const char *pathname, u32 mode) {
  char kpath[VFS_MAX_PATH];
  if (syscall_copyinstr(kpath, VFS_MAX_PATH, pathname) < 0)
    return -EFAULT;
  (void)mode;
  return vfs_mkdir(kpath);
}

static isize sys_unlink(const char *pathname) {
  char kpath[VFS_MAX_PATH];
  if (syscall_copyinstr(kpath, VFS_MAX_PATH, pathname) < 0)
    return -EFAULT;
  return vfs_unlink(kpath);
}

static isize sys_rmdir(const char *pathname) {
  char kpath[VFS_MAX_PATH];
  if (syscall_copyinstr(kpath, VFS_MAX_PATH, pathname) < 0)
    return -EFAULT;
  return vfs_rmdir(kpath);
}

static isize sys_rename(const char *oldpath, const char *newpath) {
  char kold[VFS_MAX_PATH];
  char knew[VFS_MAX_PATH];
  if (syscall_copyinstr(kold, VFS_MAX_PATH, oldpath) < 0)
    return -EFAULT;
  if (syscall_copyinstr(knew, VFS_MAX_PATH, newpath) < 0)
    return -EFAULT;
  return vfs_rename(kold, knew);
}

static isize sys_symlink(const char *target, const char *linkpath) {
  char ktarget[VFS_MAX_PATH];
  char klink[VFS_MAX_PATH];
  if (syscall_copyinstr(ktarget, VFS_MAX_PATH, target) < 0)
    return -EFAULT;
  if (syscall_copyinstr(klink, VFS_MAX_PATH, linkpath) < 0)
    return -EFAULT;
  return vfs_symlink(ktarget, klink);
}

static isize sys_readlink(const char *pathname, char *buf, usize bufsiz) {
  char kpath[VFS_MAX_PATH];
  if (syscall_copyinstr(kpath, VFS_MAX_PATH, pathname) < 0)
    return -EFAULT;
  char *kbuf = kmalloc(bufsiz);
  if (!kbuf)
    return -ENOMEM;
  isize result = vfs_readlink(kpath, kbuf, bufsiz);
  if (result > 0) {
    syscall_copyout(buf, kbuf, (usize)result);
  }
  kfree(kbuf);
  return result;
}

static isize sys_chmod(const char *pathname, u16 mode) {
  char kpath[VFS_MAX_PATH];
  if (syscall_copyinstr(kpath, VFS_MAX_PATH, pathname) < 0)
    return -EFAULT;
  return vfs_chmod(kpath, mode);
}

static isize sys_fchmod(int fd, u16 mode) { return vfs_fchmod(fd, mode); }

static isize sys_chown(const char *pathname, u16 uid, u16 gid) {
  char kpath[VFS_MAX_PATH];
  if (syscall_copyinstr(kpath, VFS_MAX_PATH, pathname) < 0)
    return -EFAULT;
  return vfs_chown(kpath, uid, gid);
}

static isize sys_fchown(int fd, u16 uid, u16 gid) {
  return vfs_fchown(fd, uid, gid);
}

static isize sys_fcntl(int fd, int cmd, u64 arg) {
  return vfs_fcntl(fd, cmd, arg);
}

static isize sys_statfs(const char *path, struct b1nix_statfs *buf) {
  char kpath[VFS_MAX_PATH];
  if (syscall_copyinstr(kpath, VFS_MAX_PATH, path) < 0)
    return -EFAULT;
  struct b1nix_statfs kbuf;
  int res = vfs_statfs(kpath, &kbuf);
  if (res == 0) {
    syscall_copyout(buf, &kbuf, sizeof(struct b1nix_statfs));
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

static isize sys_syncfs(int fd) { return vfs_syncfs(fd); }

static isize sys_umask(u16 mask) {
  struct cred *cred = scheduler_get_current_cred();
  if (!cred) return -EPERM;
  u16 old_mask = cred->umask;
  cred->umask = mask & 0777;
  return old_mask;
}

u64 syscall_dispatch(u64 number, u64 arg0, u64 arg1, u64 arg2, u64 arg3) {
  switch (number) {
  case SYS_WRITE:
    return (u64)sys_write((int)arg0, (const void *)(usize)arg1, (usize)arg2);
  case SYS_EXIT:
    scheduler_exit_current((int)arg0);
    return 0;
  case SYS_SPAWN: {
    char kpath[VFS_MAX_PATH], path[VFS_MAX_PATH];
    if (syscall_copyinstr(kpath, VFS_MAX_PATH, (const char *)(usize)arg0) != 0) return (u64)-EFAULT;
    vfs_resolve_path(kpath, path);
    return path[0] ? (u64)user_spawn(path, (int)arg1,
                                     (const char **)(usize)arg2)
                   : (u64)-ENOENT;
  }
  case SYS_LIST: {
    char kpath[VFS_MAX_PATH], path[VFS_MAX_PATH];
    if (syscall_copyinstr(kpath, VFS_MAX_PATH, (const char *)(usize)arg0) != 0) return (u64)-EFAULT;
    vfs_resolve_path(kpath, path);
    return sys_list(path);
  }
  case SYS_READ_FILE: {
    char kpath[VFS_MAX_PATH], path[VFS_MAX_PATH];
    if (syscall_copyinstr(kpath, VFS_MAX_PATH, (const char *)(usize)arg0) != 0) return (u64)-EFAULT;
    vfs_resolve_path(kpath, path);
    return sys_read_file(path);
  }
  case SYS_YIELD:
    scheduler_yield();
    return 0;
  case SYS_OPEN: {
    char kpath[VFS_MAX_PATH], path[VFS_MAX_PATH];
    if (syscall_copyinstr(kpath, VFS_MAX_PATH, (const char *)(usize)arg0) != 0) return (u64)-EFAULT;
    vfs_resolve_path(kpath, path);
    return (u64)vfs_open_flags(path, (int)arg1);
  }
  case SYS_READ:
    return (u64)sys_read((int)arg0, (void *)(usize)arg1, (usize)arg2);
  case SYS_CLOSE:
    vfs_close((int)arg0);
    return 0;
  case SYS_LSEEK:
    return (u64)vfs_lseek((int)arg0, (isize)arg1, (int)arg2);
  case SYS_STAT: {
    char kpath[VFS_MAX_PATH], path[VFS_MAX_PATH];
    if (syscall_copyinstr(kpath, VFS_MAX_PATH, (const char *)(usize)arg0) != 0) return (u64)-EFAULT;
    vfs_resolve_path(kpath, path);
    return (u64)vfs_stat(path, (struct b1nix_stat *)(usize)arg1);
  }
  case SYS_FSTAT:
    return (u64)vfs_fstat((int)arg0, (struct b1nix_stat *)(usize)arg1);
  case SYS_LSTAT: {
    char path[VFS_MAX_PATH];
    vfs_resolve_path((const char *)(usize)arg0, path);
    return (u64)vfs_lstat(path, (struct b1nix_stat *)(usize)arg1);
  }
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
  case SYS_CREATE: {
    char path[VFS_MAX_PATH];
    vfs_resolve_path((const char *)(usize)arg0, path);
    return (u64)vfs_create(path, (const char *)(usize)arg1);
  }
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
    return (u64)vfs_getdents((int)arg0, (struct dirent *)(usize)arg1,
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
    return (u64)sys_chmod((const char *)(usize)arg0, (u16)arg1);
  case SYS_FCHMOD:
    return (u64)sys_fchmod((int)arg0, (u16)arg1);
  case SYS_CHOWN:
    return (u64)sys_chown((const char *)(usize)arg0, (u16)arg1, (u16)arg2);
  case SYS_FCHOWN:
    return (u64)sys_fchown((int)arg0, (u16)arg1, (u16)arg2);
  case SYS_FORK:
    return (u64)scheduler_fork_current();
  case SYS_EXEC: {
    const char *empty_env[] = {0};
    return (u64)sys_execve((const char *)(usize)arg0,
                           (const char **)(usize)arg1, empty_env);
  }
  case SYS_EXECVE:
    return (u64)sys_execve((const char *)(usize)arg0,
                           (const char **)(usize)arg1,
                           (const char **)(usize)arg2);
  case SYS_WAIT:
    return (u64)scheduler_wait((usize)arg0, (int *)(usize)arg1);
  case SYS_WAITPID:
    return (u64)scheduler_waitpid((usize)arg0, (int *)(usize)arg1, (int)arg2);
  case SYS_GETPID:
    return (u64)scheduler_get_pid();
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
    struct cred *c = scheduler_get_current_cred();
    return c ? (u64)cred_set_uid(c, (u16)arg0) : (u64)-EACCES;
  }
  case SYS_SETGID: {
    struct cred *c = scheduler_get_current_cred();
    return c ? (u64)cred_set_gid(c, (u16)arg0) : (u64)-EACCES;
  }
  case SYS_SLEEP:
    scheduler_sleep_ticks(arg0);
    return 0;
  case SYS_KILL:
    return (u64)scheduler_kill((usize)arg0, (int)arg1);
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
    return (u64)scheduler_brk_set(arg0);
  case SYS_MMAP: {
    void *ptr = kmalloc((usize)arg0);
    return ptr ? (u64)(usize)ptr : (u64)-ENOMEM;
  }
  case SYS_MUNMAP:
    kfree((void *)(usize)arg0);
    return 0;
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
  case SYS_MQ_SEND:
    return (u64)mqueue_send((struct mqueue *)(usize)arg0,
                            (const void *)(usize)arg1, (u32)arg2);
  case SYS_MQ_RECEIVE:
    return (u64)mqueue_receive((struct mqueue *)(usize)arg0,
                               (void *)(usize)arg1, (u32 *)(usize)arg2);
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
  case SYS_SHMCTL:
    return (u64)shmctl((int)arg0, (int)arg1, (struct shmid_ds *)(usize)arg2);
  case SYS_SOCKET:
    return (u64)vfs_socket((int)arg0, (int)arg1, (int)arg2);
  case SYS_BIND:
    return (u64)vfs_bind((int)arg0, (const void *)(usize)arg1, (usize)arg2);
  case SYS_CONNECT:
    return (u64)vfs_connect((int)arg0, (const void *)(usize)arg1, (usize)arg2);
  case SYS_SEND:
    return (u64)vfs_socket_send((int)arg0, (const void *)(usize)arg1,
                                (usize)arg2, (int)arg3);
  case SYS_RECV:
    return (u64)vfs_socket_recv((int)arg0, (void *)(usize)arg1, (usize)arg2,
                                (int)arg3);
#ifndef __aarch64__
  case SYS_NET_INFO:
    net_dump_info();
    return 0;
  case SYS_NET_PING: {
    struct ipv4_addr dest;
    if (syscall_copyin(&dest, (void *)(usize)arg0, sizeof(dest)) != 0)
      return (u64)-EFAULT;
    for (int i = 0; i < 4; i++) {
      u8 echo[8] = {8, 0, 0, 0, 0, 0, 0, 0};
      u16 csum = 0;
      for (int j = 0; j < 8; j += 2)
        csum = (u16)(csum + (u16)((echo[j] << 8) | echo[j + 1]));
      csum = (u16)~csum;
      echo[2] = (u8)(csum >> 8);
      echo[3] = (u8)(csum & 0xff);
      ipv4_send(dest, 1, echo, sizeof(echo));
      console_write("ping: sent request seq=");
      console_write_dec(i + 1);
      console_write("\n");
      scheduler_sleep_ticks(100);
    }
    return 0;
  }
  case SYS_NET_DNS:
    dns_resolve((const char *)(usize)arg0);
    return 0;
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
    return scheduler_get_uptime_ticks() / 100;
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
    syscall_copyout((void *)(usize)arg0, cwd, len);
    ((char *)(usize)arg0)[len] = '\0';
    return (u64)len;
  }
  case SYS_CHDIR: {
    char path[VFS_MAX_PATH];
    vfs_resolve_path((const char *)(usize)arg0, path);
    struct vfs_node *node = vfs_find_node(path);
    if (IS_ERR(node))
      return (u64)PTR_ERR(node);
    if (node->inode->type != VFS_DIRECTORY)
      return (u64)-ENOTDIR;
    return (u64)scheduler_set_cwd(path);
  }
  case SYS_REBOOT:
    console_write("reboot requested\n");
    arch_halt();
  case SYS_DMESG:
    if (!arg0 || arg1 == 0)
      return (u64)-EINVAL;
    return (u64)klog_read((char *)(usize)arg0, (usize)arg1);
  case SYS_MOUNT:
    return (u64)vfs_mount((const char *)(usize)arg0, (const char *)(usize)arg1,
                          (const char *)(usize)arg2, arg3);
  case SYS_UMOUNT:
    return (u64)vfs_umount((const char *)(usize)arg0);
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
  default:
    console_write("syscall: unknown 0x");
    console_write_hex64(number);
    console_write("\n");
    return (u64)-ENOSYS;
  }
}
