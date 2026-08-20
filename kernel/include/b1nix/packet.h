#ifndef B1NIX_PACKET_H
#define B1NIX_PACKET_H

#include <b1nix/types.h>
#include <b1nix/posix.h>

/* ── AF_PACKET: ethernet frames as they appear on the wire ──
 *
 * The family BusyBox udhcpc/udhcpd, arping and tcpdump reach for. A packet
 * socket sees complete frames (SOCK_RAW, header included) or their payload
 * with the header handed over separately (SOCK_DGRAM), and can put a frame on
 * a chosen interface without the IP stack having an opinion about it.
 *
 * Receive and poll come from the generic socket layer: a packet socket queues
 * into the same per-socket datagram ring UDP/netlink use, so recv/poll/select
 * need no packet-specific code. Only create, bind, send, close and the RX tap
 * live here.
 */

struct vfs_socket_state;
struct netdev;

/* Called from vfs_socket()/socket_teardown() for AF_PACKET sockets. */
void packet_sock_register(struct vfs_socket_state *s);
void packet_sock_unregister(struct vfs_socket_state *s);

/* bind(2) to a sockaddr_ll: pins the interface and/or the ethertype filter. */
int packet_bind(struct vfs_socket_state *s, const struct b1nix_sockaddr_ll *sll,
                usize addrlen);

/* send(2)/sendto(2). For SOCK_RAW `buf` is a complete frame starting at the
 * destination MAC; for SOCK_DGRAM it is the payload and the header is built
 * from the socket's (or sendto's) sockaddr_ll. */
isize packet_send(struct vfs_socket_state *s, const void *buf, usize len);

/* RX tap, called by ethernet_receive_flags() for every frame that arrives,
 * with the frame complete from the destination MAC onwards. */
void packet_socket_rx(struct netdev *rx, const void *frame, usize len);

/* TX tap: outgoing frames are part of what a packet socket sees (Linux calls
 * this dev_queue_xmit_nit), which is what lets tcpdump show both directions. */
void packet_socket_tx(struct netdev *dev, const u8 *hdr, const void *payload,
                      usize payload_len);

/* Fill a sockaddr_ll describing the frame recvfrom() just handed back, from
 * the per-datagram metadata the RX tap recorded. Returns bytes written. */
usize packet_fill_src(struct vfs_socket_state *s, void *addr, usize cap);

#endif
