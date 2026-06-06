#ifndef B1NIX_U_NET_IF_H
#define B1NIX_U_NET_IF_H

#include <sys/socket.h>

#define IFNAMSIZ 16

struct ifreq {
  char ifr_name[IFNAMSIZ];
  union {
    struct sockaddr ifr_addr;
    struct sockaddr ifr_dstaddr;
    struct sockaddr ifr_broadaddr;
    struct sockaddr ifr_netmask;
    struct sockaddr ifr_hwaddr;
    short           ifr_flags;
    int             ifr_ifindex;
    int             ifr_metric;
    int             ifr_mtu;
    char            ifr_slave[IFNAMSIZ];
    char            ifr_newname[IFNAMSIZ];
    void           *ifr_data;
  };
};

struct ifconf {
  int ifc_len;
  union {
    char         *ifc_buf;
    struct ifreq *ifc_req;
  };
};

#endif
