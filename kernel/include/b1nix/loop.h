#ifndef B1NIX_LOOP_H
#define B1NIX_LOOP_H

#include <b1nix/blk.h>

struct block_device *loop_register_file(const char *path, const char *name);

/* Pre-register 8 loop block devices + /dev/loop-control. Call once at boot
 * before blk_create_dev_nodes() so /dev/loop0../dev/loop7 nodes are made. */
void loop_init(void);

/* Handle LOOP_* / LOOP_CTL_* ioctls on /dev/loopN and /dev/loop-control. */
struct vfs_node;
int loop_ioctl(struct vfs_node *node, u64 request, void *arg);

#endif
