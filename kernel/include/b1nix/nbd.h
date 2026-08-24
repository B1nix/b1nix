/*
 * Network block device: a remote NBD export attached as a local block device.
 * The wire protocol and the one-request-at-a-time policy are described in
 * kernel/dev/nbd.c.
 */
#ifndef B1NIX_NBD_H
#define B1NIX_NBD_H

#include <b1nix/net.h>
#include <b1nix/types.h>

/* Connect to `server`:`port`, complete the oldstyle handshake and register the
 * export as the next nbd* block device. 0, or a negative errno. */
void nbd_init(void);
int nbd_attach(struct ipv4_addr server, u16 port);

/* Disconnect a device by name ("nbd0"). Sends NBD_CMD_DISC first. */
int nbd_detach(const char *name);

/* The Linux ioctl interface, used by nbd-client: it connects and handshakes,
 * then hands the socket and the geometry over and parks in nbd_run(). */
struct nbd_device;
struct vfs_handle;
struct nbd_device *nbd_device_at(unsigned index);
int nbd_set_socket(struct nbd_device *nd, struct vfs_handle *sock);
int nbd_set_geometry(struct nbd_device *nd, u32 block_size, u64 blocks);
int nbd_run(struct nbd_device *nd);
int nbd_clear_socket(struct nbd_device *nd);
int nbd_disconnect(struct nbd_device *nd);
u32 nbd_block_size(struct nbd_device *nd);
u64 nbd_block_count(struct nbd_device *nd);

#endif /* B1NIX_NBD_H */
