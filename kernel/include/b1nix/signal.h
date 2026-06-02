#ifndef B1NIX_SIGNAL_H
#define B1NIX_SIGNAL_H

#include <b1nix/arch.h>
#include <b1nix/types.h>

#define B1NIX_SIGFRAME_MAGIC 0x5349474652414d45ULL /* "SIGFRAME" */

struct b1nix_sigframe {
  u64 magic;
  u64 old_blocked_signals;
  struct interrupt_frame saved_frame;
} __attribute__((packed));

#endif
