#ifndef B1NIX_GDBSTUB_H
#define B1NIX_GDBSTUB_H

#include <b1nix/arch_x86_64.h>
#include <b1nix/types.h>

/* GDB Remote Serial Protocol stub (M36). See kernel/arch/x86_64/gdbstub.c. */

struct gdb_transport {
  int (*getc)(void *ctx);         /* blocking; byte 0..255, or -1 on EOF */
  void (*putc)(void *ctx, char c);
  void *ctx;
};

/* Build the x86_64 'g' (read-all-registers) packet payload for `f` into `out`
 * (NUL-terminated). Returns the hex-character count. */
usize gdb_build_g_packet(struct interrupt_frame *f, char *out);

/* Process one decoded packet payload `in`; write the response payload into
 * `out` (capacity outcap). *resume is set for continue/step. Returns response
 * length. */
int gdb_handle_packet(const char *in, char *out, usize outcap,
                      struct interrupt_frame *f, int *resume);

/* Framing over a transport. */
void gdb_send_packet(struct gdb_transport *t, const char *payload);
int gdb_recv_packet(struct gdb_transport *t, char *buf, usize cap);

/* Interactive serial stub entry (only when booted with b1nix.gdb). */
void gdb_stub_enter(struct interrupt_frame *f);

#endif
