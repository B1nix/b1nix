/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_RELAY_H
#define LKPI_LINUX_RELAY_H
#include <linux/types.h>
/*
 * Relay channels: a per-CPU ring a driver streams bulk data through to a
 * debugfs file. i915 uses one for the GuC log. b1nix has no relay, and the GuC
 * log is diagnostics — so the type exists and a channel is never opened.
 */
struct rchan;
struct rchan_callbacks;
static inline struct rchan *relay_open(const char *base, struct dentry *parent,
                                       size_t subbuf_size, size_t n_subbufs,
                                       const struct rchan_callbacks *cb,
                                       void *private_data)
{ (void)base; (void)parent; (void)subbuf_size; (void)n_subbufs; (void)cb;
  (void)private_data; return 0; }
static inline void relay_close(struct rchan *chan) { (void)chan; }
static inline void relay_flush(struct rchan *chan) { (void)chan; }
static inline size_t relay_write(struct rchan *chan, const void *data, size_t length)
{ (void)chan; (void)data; return length; }

/*
 * relayfs: a per-CPU ring of buffers a driver fills and userspace mmaps. i915
 * uses it for the GuC firmware log.
 *
 * b1nix has no relayfs, so the channel here is never created — relay_open()
 * reports failure and the driver takes its no-log path. These are the operations
 * a caller would perform on a live channel; with no channel they are unreachable,
 * and they say so rather than pretending a buffer exists to reserve space in.
 */
struct rchan_buf { void *data; usize offset; usize subbufs_produced; };
struct rchan {
	usize subbuf_size;
	usize n_subbufs;
	void *private_data;
};
static inline int relay_buf_full(struct rchan_buf *buf) { (void)buf; return 1; }
void *relay_reserve(struct rchan *chan, usize length);
extern const struct file_operations relay_file_operations;


/* The callbacks a channel's owner supplies. No channel is created here — see
 * above — so none of them is ever called. */
struct dentry;
struct rchan_callbacks {
	int (*subbuf_start)(struct rchan_buf *buf, void *subbuf, void *prev_subbuf,
	                    usize prev_padding);
	struct dentry *(*create_buf_file)(const char *filename, struct dentry *parent,
	                                  umode_t mode, struct rchan_buf *buf,
	                                  int *is_global);
	int (*remove_buf_file)(struct dentry *dentry);
};
struct rchan *relay_open(const char *base_filename, struct dentry *parent,
                         usize subbuf_size, usize n_subbufs,
                         const struct rchan_callbacks *cb, void *private_data);
void relay_close(struct rchan *chan);
void relay_flush(struct rchan *chan);
usize relay_switch_subbuf(struct rchan_buf *buf, usize length);

#endif
