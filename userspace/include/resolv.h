#ifndef B1NIX_U_RESOLV_H
#define B1NIX_U_RESOLV_H

#include <netinet/in.h>

/* Minimal BSD/glibc resolver state. b1nix resolves names through the kernel
 * (getaddrinfo -> SYS_NET_DNS), so _res is only used by BusyBox nslookup to
 * *display* the default nameserver. res_init() fills nsaddr_list[0] from
 * /etc/resolv.conf. The field paths (nsaddr_list, nscount, _u._ext.nsaddrs,
 * _u._ext.nscount) match what nslookup touches; the binary layout need not
 * match glibc since nothing else in the b1nix userspace consumes _res. */

#define MAXNS 3
#define RES_INIT     0x00000001
#define RES_USE_INET6 0x00002000

struct __res_state {
  int retrans;
  int retry;
  unsigned long options;
  int nscount;
  struct sockaddr_in nsaddr_list[MAXNS];
  unsigned short id;
  char *dnsrch[7];
  char defdname[256];
  unsigned long pfcode;
  unsigned ndots : 4;
  unsigned nsort : 4;
  unsigned ipv6_unavail : 1;
  unsigned unused : 23;
  union {
    char pad[52];
    struct {
      unsigned short nscount;
      unsigned short nsmap[MAXNS];
      int nssocks[MAXNS];
      unsigned short nscount6;
      unsigned short nsinit;
      struct sockaddr_in6 *nsaddrs[MAXNS];
    } _ext;
  } _u;
};

extern struct __res_state _res;

int res_init(void);

#endif
