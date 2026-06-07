#ifndef B1NIX_U_NET_IF_H
#define B1NIX_U_NET_IF_H

#include <sys/socket.h>

#define IFNAMSIZ 16
#define IF_NAMESIZE IFNAMSIZ

/* Interface flags (Linux net/if.h ABI). The b1nix socket ioctl reports
 * UP|BROADCAST|RUNNING|MULTICAST for the single modelled interface. */
#define IFF_UP          0x1
#define IFF_BROADCAST   0x2
#define IFF_DEBUG       0x4
#define IFF_LOOPBACK    0x8
#define IFF_POINTOPOINT 0x10
#define IFF_NOTRAILERS  0x20
#define IFF_RUNNING     0x40
#define IFF_NOARP       0x80
#define IFF_PROMISC     0x100
#define IFF_ALLMULTI    0x200
#define IFF_MASTER      0x400
#define IFF_SLAVE       0x800
#define IFF_MULTICAST   0x1000
#define IFF_PORTSEL     0x2000
#define IFF_AUTOMEDIA   0x4000
#define IFF_DYNAMIC     0x8000

struct ifmap {
  unsigned long mem_start;
  unsigned long mem_end;
  unsigned short base_addr;
  unsigned char irq;
  unsigned char dma;
  unsigned char port;
};

/* Standard Linux struct ifreq: a name union plus a value union, with the
 * familiar ifr_* accessor macros. The kernel only ever copies the first 32
 * bytes (16-byte name + the address/flags slot of the value union), which is
 * all the SIOCGIF* queries fill; the wider ifru_map tail is display-only. */
struct ifreq {
  union {
    char ifrn_name[IFNAMSIZ];
  } ifr_ifrn;
  union {
    struct sockaddr ifru_addr;
    struct sockaddr ifru_dstaddr;
    struct sockaddr ifru_broadaddr;
    struct sockaddr ifru_netmask;
    struct sockaddr ifru_hwaddr;
    short ifru_flags;
    int ifru_ivalue;
    int ifru_mtu;
    struct ifmap ifru_map;
    char ifru_slave[IFNAMSIZ];
    char ifru_newname[IFNAMSIZ];
    char *ifru_data;
  } ifr_ifru;
};

#define ifr_name      ifr_ifrn.ifrn_name
#define ifr_addr      ifr_ifru.ifru_addr
#define ifr_dstaddr   ifr_ifru.ifru_dstaddr
#define ifr_broadaddr ifr_ifru.ifru_broadaddr
#define ifr_netmask   ifr_ifru.ifru_netmask
#define ifr_hwaddr    ifr_ifru.ifru_hwaddr
#define ifr_flags     ifr_ifru.ifru_flags
#define ifr_metric    ifr_ifru.ifru_ivalue
#define ifr_mtu       ifr_ifru.ifru_mtu
#define ifr_ifindex   ifr_ifru.ifru_ivalue
#define ifr_ifqlen    ifr_ifru.ifru_ivalue
#define ifr_data      ifr_ifru.ifru_data
#define ifr_map       ifr_ifru.ifru_map
#define ifr_slave     ifr_ifru.ifru_slave
#define ifr_newname   ifr_ifru.ifru_newname

struct ifconf {
  int ifc_len;
  union {
    char *ifcu_buf;
    struct ifreq *ifcu_req;
  } ifc_ifcu;
};
#define ifc_buf ifc_ifcu.ifcu_buf
#define ifc_req ifc_ifcu.ifcu_req

unsigned int if_nametoindex(const char *ifname);

#endif
