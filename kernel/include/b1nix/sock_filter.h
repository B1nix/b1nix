#ifndef B1NIX_SOCK_FILTER_H
#define B1NIX_SOCK_FILTER_H

#include <b1nix/types.h>

/*
 * Classic BPF socket filters (SO_ATTACH_FILTER / SO_DETACH_FILTER).
 *
 * A socket filter is a tiny program the kernel runs over every datagram before
 * it is queued: it returns the number of bytes to accept, and zero means drop.
 * This is not an optimisation a listener can do without — systemd's device
 * monitor installs one on its NETLINK_KOBJECT_UEVENT socket and treats the
 * setsockopt failing as fatal, so with no filter engine there is no device
 * monitor, and with no device monitor no `.device` unit ever activates.
 *
 * Only the classic (cBPF) instruction set is implemented, which is the whole
 * of what SO_ATTACH_FILTER accepts; eBPF programs arrive through bpf(2) and
 * are a different thing entirely.
 */

/* struct sock_filter, byte-for-byte as userspace passes it. */
struct sock_filter_insn {
  u16 code;
  u8 jt;
  u8 jf;
  u32 k;
};

/* struct sock_fprog as userspace passes it on x86_64. */
struct sock_fprog_user {
  u16 len;
  u16 pad[3];
  u64 filter; /* user pointer to struct sock_filter[len] */
};

/* Linux's own ceiling. A program longer than this is rejected, not truncated. */
#define BPF_MAXINSNS 4096

struct sock_filter_prog {
  u32 len;
  struct sock_filter_insn insns[];
};

/* Verify `insns` and return a kmalloc'd program, or NULL when the program is
 * malformed (out-of-range jump, bad opcode, division by a constant zero, empty
 * or over-long). The caller owns the result and frees it with kfree(). */
struct sock_filter_prog *sock_filter_compile(const struct sock_filter_insn *insns,
                                             u32 len);

/* Run `prog` over one packet. Returns the number of bytes the filter accepts,
 * 0 to drop. A NULL program accepts everything. */
u32 sock_filter_run(const struct sock_filter_prog *prog, const u8 *data,
                    u32 len);

#endif
