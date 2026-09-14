/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_KMSG_DUMP_H
#define LKPI_LINUX_KMSG_DUMP_H
#include <linux/errno.h>
#include <linux/list.h>

/*
 * Crash-time log dumpers. b1nix's panic path draws its own screen and has no
 * dumper list, so registration is refused and there is no record to iterate —
 * a DRM panic handler built on this finds no log to print and draws without it.
 */
enum kmsg_dump_reason {
	KMSG_DUMP_UNDEF,
	KMSG_DUMP_PANIC,
	KMSG_DUMP_OOPS,
	KMSG_DUMP_EMERG,
	KMSG_DUMP_SHUTDOWN,
	KMSG_DUMP_MAX
};

struct kmsg_dump_iter {
	u64 cur_seq;
	u64 next_seq;
};

struct kmsg_dump_detail {
	enum kmsg_dump_reason reason;
	const char *description;
};

struct kmsg_dumper {
	struct list_head list;
	void (*dump)(struct kmsg_dumper *dumper, struct kmsg_dump_detail *detail);
	enum kmsg_dump_reason max_reason;
	bool registered;
};

static inline bool kmsg_dump_get_line(struct kmsg_dump_iter *iter, bool syslog,
                                      char *line, size_t size, size_t *len)
{ (void)iter; (void)syslog; (void)line; (void)size; (void)len; return false; }
static inline bool kmsg_dump_get_buffer(struct kmsg_dump_iter *iter, bool syslog,
                                        char *buf, size_t size, size_t *len)
{ (void)iter; (void)syslog; (void)buf; (void)size; (void)len; return false; }
static inline void kmsg_dump_rewind(struct kmsg_dump_iter *iter)
{ (void)iter; }
static inline int kmsg_dump_register(struct kmsg_dumper *dumper)
{ (void)dumper; return -EINVAL; }
static inline int kmsg_dump_unregister(struct kmsg_dumper *dumper)
{ (void)dumper; return -EINVAL; }

#endif
