/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef B1NIX_KPROBE_H
#define B1NIX_KPROBE_H

#include <b1nix/types.h>

/*
 * Dynamic probes: a breakpoint over the first byte of a kernel function.
 *
 * A probe is asked for by symbol name through
 * /sys/kernel/tracing/kprobe_events, becomes a tracepoint under the `kprobes`
 * group, and fires when the function is entered. The mechanism is the classic
 * one: the symbol's first byte is replaced with int3, the #BP handler reports
 * the hit, puts the original byte back, and single-steps the instruction with
 * TF so the function runs as written -- then the int3 goes in again on the #DB.
 *
 * What that buys and what it costs is in kernel/trace/kprobe.c.
 */

struct interrupt_frame;

/* Arm a probe on `symbol` for tracepoint `id`. A return probe (`r:`) is
 * refused: there is no return trampoline here. */
int kprobe_arm(const char *symbol, u16 id, int is_return);
int kprobe_disarm(u16 id);
/* Does this name a kernel symbol we can probe? */
int kprobe_symbol_ok(const char *symbol);
/* The symbol a probe was armed on, for kprobe_events to print back. */
const char *kprobe_symbol_of(u16 id);

/* The trap handlers. Each answers 1 when the trap was a probe's and the frame
 * has been fixed up, 0 when it belongs to somebody else. */
int kprobe_handle_bp(struct interrupt_frame *frame);
int kprobe_handle_db(struct interrupt_frame *frame);

#endif /* B1NIX_KPROBE_H */
