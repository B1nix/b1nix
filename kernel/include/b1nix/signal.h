#ifndef B1NIX_SIGNAL_H
#define B1NIX_SIGNAL_H

#include <b1nix/arch.h>
#include <b1nix/types.h>

#define B1NIX_SIGFRAME_MAGIC 0x5349474652414d45ULL /* "SIGFRAME" */

struct b1nix_sigframe {
  u64 magic;
  u64 old_blocked_signals;
  struct interrupt_frame saved_frame;
  /* The interrupted thread's protection-key rights (x86 PKRU), put back by
   * sigreturn; the handler itself runs with the initial rights. */
  u64 pkru;
  /* The interrupted thread's FPU/vector state, saved on the user stack just
   * above this frame (a 64-byte aligned XSAVE image of fpu_size bytes, or the
   * 512-byte FXSAVE image where XSAVE is not enabled) and loaded back by
   * sigreturn. Without it a handler's own use of the vector registers -- the
   * ABI treats them as caller-saved -- leaks into the interrupted code, which
   * is how a toolkit computing layout in AVX ended up with sizes of 1e-8. */
  u64 fpu_addr;
  u64 fpu_size;
} __attribute__((packed));

/* M74: native siginfo_t handed to an SA_SIGINFO handler. Layout MUST match the
 * userspace siginfo_t (third_party/b1cc-sysroot/signal.h) exactly — the kernel copies
 * this onto the user stack and the handler reads it as siginfo_t*. */
struct b1nix_native_siginfo {
  int si_signo;  /* 0 */
  int si_code;   /* 4 */
  int si_errno;  /* 8 */
  int si_pid;    /* 12 */
  int si_uid;    /* 16 */
  int si_status; /* 20 */
  void *si_addr; /* 24 */
  union {        /* 32: union sigval payload (RT signals / timers) */
    int sival_int;
    void *sival_ptr;
  } si_value;
};

#endif
