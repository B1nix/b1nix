#ifndef B1NIX_U_NET_IF_ARP_H
#define B1NIX_U_NET_IF_ARP_H

#include <sys/socket.h>

/* ARP hardware identifiers (Linux linux/if_arp.h ABI). BusyBox ifconfig /
 * interface.c map a link's ARPHRD_ type to a printable name; b1nix interfaces
 * are always ARPHRD_ETHER. */
#define ARPHRD_NETROM     0
#define ARPHRD_ETHER      1
#define ARPHRD_EETHER     2
#define ARPHRD_AX25       3
#define ARPHRD_PRONET     4
#define ARPHRD_CHAOS      5
#define ARPHRD_IEEE802    6
#define ARPHRD_ARCNET     7
#define ARPHRD_DLCI       15
#define ARPHRD_ATM        19
#define ARPHRD_IEEE1394   24
#define ARPHRD_EUI64      27
#define ARPHRD_INFINIBAND 32
#define ARPHRD_SLIP       256
#define ARPHRD_CSLIP      257
#define ARPHRD_SLIP6      258
#define ARPHRD_CSLIP6     259
#define ARPHRD_RSRVD      260
#define ARPHRD_ADAPT      264
#define ARPHRD_ROSE       270
#define ARPHRD_X25        271
#define ARPHRD_PPP        512
#define ARPHRD_HDLC       513
#define ARPHRD_LAPB       516
#define ARPHRD_TUNNEL     768
#define ARPHRD_FRAD       770
#define ARPHRD_LOOPBACK   772
#define ARPHRD_SIT        776
#define ARPHRD_FDDI       774
#define ARPHRD_IRDA       783

/* ARP flags for struct arpreq (SIOCGARP/SIOCSARP). */
#define ATF_COM       0x02
#define ATF_PERM      0x04
#define ATF_PUBL      0x08
#define ATF_USETRAILERS 0x10
#define ATF_NETMASK   0x20
#define ATF_DONTPUB   0x40

struct arpreq {
  struct sockaddr arp_pa;      /* protocol address      */
  struct sockaddr arp_ha;      /* hardware address      */
  int arp_flags;               /* flags                 */
  struct sockaddr arp_netmask; /* netmask of protocol   */
  char arp_dev[16];
};

#endif
