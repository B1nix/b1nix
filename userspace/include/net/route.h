#ifndef B1NIX_U_NET_ROUTE_H
#define B1NIX_U_NET_ROUTE_H

#include <sys/socket.h>
#include <netinet/in.h>

/* Linux routing-table ioctl structures, enough for BusyBox `route`. b1nix
 * displays routes from /proc/net/route; the SIOCADDRT/SIOCDELRT mutate path
 * is accepted by the kernel socket ioctl handler (currently a no-op stub). */

struct rtentry {
  unsigned long rt_pad1;
  struct sockaddr rt_dst;     /* target address              */
  struct sockaddr rt_gateway; /* gateway addr (RTF_GATEWAY)  */
  struct sockaddr rt_genmask; /* target network mask (IP)    */
  unsigned short rt_flags;
  short rt_pad2;
  unsigned long rt_pad3;
  unsigned char rt_tos;
  unsigned char rt_class;
  short rt_pad4[3];
  short rt_metric; /* +1 for binary compatibility!         */
  char *rt_dev;    /* forcing the device at add            */
  unsigned long rt_mtu; /* per route MTU/Window                  */
  unsigned long rt_window; /* Window clamping                      */
  unsigned short rt_irtt;  /* Initial RTT                          */
};

/* glibc-compatible alias; BusyBox route uses rt_mss. RTF_* flag macros are
 * left to BusyBox route.c's own `#ifndef RTF_UP` block so the full set
 * (RTF_IRTT etc.) is defined consistently. */
#define rt_mss rt_mtu

struct in6_rtmsg {
  struct in6_addr rtmsg_dst;
  struct in6_addr rtmsg_src;
  struct in6_addr rtmsg_gateway;
  unsigned int rtmsg_type;
  unsigned short rtmsg_dst_len;
  unsigned short rtmsg_src_len;
  unsigned int rtmsg_metric;
  unsigned long rtmsg_info;
  unsigned int rtmsg_flags;
  int rtmsg_ifindex;
};

#endif
