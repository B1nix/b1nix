#ifndef B1NIX_LOOP_H
#define B1NIX_LOOP_H

#include <b1nix/blk.h>

/* Attach `path` to the first free pre-registered loop device and return it.
 * The name of the device is the returned block_device's own name. */
struct block_device *loop_register_file(const char *path);

/* Register the 8 loop block devices that exist from boot, plus
 * /dev/loop-control. Call once at boot before blk_create_dev_nodes() so the
 * /dev/loop0../dev/loop7 nodes are made. Further loop devices are created on
 * demand by LOOP_CTL_ADD, and their nodes by whoever manages /dev. */
void loop_init(void);

/* Handle LOOP_* / LOOP_CTL_* ioctls on /dev/loopN and /dev/loop-control. */
struct vfs_node;
int loop_ioctl(struct vfs_node *node, u64 request, void *arg);

#endif
