/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_IO_URING_CMD_H
#define LKPI_LINUX_IO_URING_CMD_H
#include <linux/types.h>

/*
 * io_uring driver commands. b1nix has no io_uring, so no command is ever issued
 * to a file_operations::uring_cmd handler; what is here is enough for btrfs's
 * handler to compile, and the completion hooks are the upstream
 * !CONFIG_IO_URING no-ops.
 */
struct file;

/* The two submission-queue-entry fields a command handler reads. */
struct io_uring_sqe {
	__u64 addr;
	__u8  cmd[];
};

#define IORING_URING_CMD_CANCELABLE (1U << 30)
#define IORING_URING_CMD_REISSUE    (1U << 31)

enum {
	IO_URING_F_NONBLOCK = (int)0x80000000,
	IO_URING_F_COMPAT   = (1 << 12),
};

struct io_uring_cmd;
typedef void (*io_uring_cmd_tw_t)(struct io_uring_cmd *cmd, unsigned issue_flags);

struct io_uring_cmd {
	struct file *file;
	const struct io_uring_sqe *sqe;
	io_uring_cmd_tw_t task_work_cb;
	u32 cmd_op;
	u32 flags;
	u8 pdu[32];
};

#define io_uring_cmd_to_pdu(cmd, pdu_type) ((pdu_type *)&(cmd)->pdu)

static inline void io_uring_cmd_done(struct io_uring_cmd *cmd, s32 ret,
				     unsigned issue_flags)
{ (void)cmd; (void)ret; (void)issue_flags; }
static inline void io_uring_cmd_complete_in_task(struct io_uring_cmd *cmd,
						 io_uring_cmd_tw_t cb)
{ (void)cmd; (void)cb; }

#endif
