// b1nix shim for the linux-syscall-support (lss) subset that crashpad's client
// and util libraries use. lss issues RAW Linux-numbered syscalls, which are
// wrong on b1nix (it has its own syscall numbers), so forward each to the b1nix
// libc instead. crashpad does not actually function on b1nix (no ptrace /
// coredump / proc-task kernel ABI) — this only lets it COMPILE and LINK; the
// crash-capture paths are never correctly invoked. Copied into the crashpad
// tree by tools/patches/chromium/apply.sh and pulled in by lss.h for __b1nix__.
#ifndef CRASHPAD_THIRD_PARTY_LSS_LSS_B1NIX_H_
#define CRASHPAD_THIRD_PARTY_LSS_LSS_B1NIX_H_

#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <syscall.h>
#include <time.h>
#include <unistd.h>

#define KERNEL_NSIG 64

struct kernel_sigset_t {
  unsigned long sig[KERNEL_NSIG / (8 * sizeof(unsigned long))];
};
struct kernel_timespec {
  long long tv_sec;
  long long tv_nsec;
};

static inline void* sys_mmap(void* addr, size_t length, int prot, int flags,
                             int fd, int64_t offset) {
  return mmap(addr, length, prot, flags, fd, (long)offset);
}
static inline int sys_munmap(void* addr, size_t length) {
  return munmap(addr, length);
}
static inline int sys_mprotect(const void* addr, size_t len, int prot) {
  return mprotect((void*)addr, len, prot);
}
static inline int sys_getpid(void) { return (int)getpid(); }
static inline int sys_gettid(void) { return (int)syscall(SYS_GETTID); }
static inline int sys_prctl(int option, unsigned long a2, unsigned long a3,
                            unsigned long a4, unsigned long a5) {
  return prctl(option, a2, a3, a4, a5);
}
static inline int sys_futex(int* uaddr, int op, int val,
                            struct kernel_timespec* t, int* uaddr2, int val3) {
  (void)uaddr2;
  (void)val3;
  long ms = 0;
  if (t) ms = (long)(t->tv_sec * 1000 + t->tv_nsec / 1000000);
  return (int)syscall(SYS_FUTEX, uaddr, op, val, ms);
}
static inline int sys_fallocate(int fd, int mode, long long offset,
                                long long len) {
  // ponytail: b1nix files auto-extend on write, so preallocation is a no-op
  // success. Wire to a real SYS_FALLOCATE if a port ever needs reserved blocks.
  (void)fd;
  (void)mode;
  (void)offset;
  (void)len;
  return 0;
}
static inline int sys_sigemptyset(struct kernel_sigset_t* set) {
  memset(&set->sig, 0, sizeof(set->sig));
  return 0;
}
static inline int sys_sigaddset(struct kernel_sigset_t* set, int signum) {
  if (signum < 1 || (size_t)signum > 8 * sizeof(set->sig)) {
    errno = EINVAL;
    return -1;
  }
  set->sig[(size_t)(signum - 1) / (8 * sizeof(set->sig[0]))] |=
      1UL << ((size_t)(signum - 1) % (8 * sizeof(set->sig[0])));
  return 0;
}
static inline int sys_sigprocmask(int how, const struct kernel_sigset_t* set,
                                  struct kernel_sigset_t* oldset) {
  return sigprocmask(how, (const sigset_t*)set, (sigset_t*)oldset);
}
static inline int sys_sigtimedwait(const struct kernel_sigset_t* set,
                                   siginfo_t* info,
                                   const struct timespec* timeout) {
  // b1nix has no sigtimedwait; crashpad's exception handler never runs here.
  (void)set;
  (void)info;
  (void)timeout;
  errno = ENOSYS;
  return -1;
}

#endif  // CRASHPAD_THIRD_PARTY_LSS_LSS_B1NIX_H_
