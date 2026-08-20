#ifndef B1NIX_NETLINK_H
#define B1NIX_NETLINK_H

#include <b1nix/types.h>

/*
 * AF_NETLINK / NETLINK_ROUTE (kernel/net/netlink.c).
 *
 * A netlink socket is a request/response channel onto the real interface,
 * address, route and neighbour tables. Every request written to the socket is
 * serviced synchronously and its reply (a multipart dump, or an NLMSG_ERROR
 * ack) is pushed into the socket's own datagram queue, so the recvmsg() that
 * follows returns it. Nothing is invented: a dump renders whatever the FIB,
 * the ARP/NDP caches and the netdev registry actually hold, and a NEW/DEL verb
 * calls the same route_add_table()/route_del_table()/arp_neigh_set() the rest
 * of the stack uses.
 */

struct vfs_socket_state;

/* Service one send() worth of netlink requests. `buf` is a kernel buffer that
 * may hold several concatenated nlmsghdrs. Returns `len` (netlink accepts the
 * whole write and reports failures in-band as NLMSG_ERROR), or -errno if the
 * buffer is malformed. */
isize netlink_socket_send(struct vfs_socket_state *s, const void *buf,
                          usize len);

/* The kernel-side ifindex the synthetic loopback interface is presented under.
 * Real NICs occupy 1..NET_MAX_NETDEVS through netdev_index_of(). */
#define NETLINK_LO_IFINDEX 32

/*
 * NETLINK_KOBJECT_UEVENT: the kernel's hotplug announcements.
 *
 * Unlike NETLINK_ROUTE this is a broadcast — no request precedes it. A socket
 * joins the group by binding with a non-zero nl_groups, and every uevent the
 * device model raises is then copied into its queue, in the "add@/devices/…"
 * form with NUL-separated key=value properties that mdev and udev parse.
 */
#define NETLINK_KOBJECT_UEVENT 15

void netlink_uevent_register(struct vfs_socket_state *s);
void netlink_uevent_unregister(struct vfs_socket_state *s);
void netlink_uevent_broadcast(const void *payload, usize len);

#endif
