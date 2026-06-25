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
#define MAXDNSRCH 6          /* max domains in the search path (dnsrch[7]) */

/* Resolver option flags (res.options), standard glibc values. b1nix's _res is
 * display/parse-only — nothing acts on most of these — but ports (e.g. Chromium
 * dns_config_service_linux) read them, so define the full set. */
#define RES_INIT        0x00000001
#define RES_DEBUG       0x00000002
#define RES_AAONLY      0x00000004
#define RES_USEVC       0x00000008
#define RES_PRIMARY     0x00000010
#define RES_IGNTC       0x00000020
#define RES_RECURSE     0x00000040
#define RES_DEFNAMES    0x00000080
#define RES_STAYOPEN    0x00000100
#define RES_DNSRCH      0x00000200
#define RES_INSECURE1   0x00000400
#define RES_INSECURE2   0x00000800
#define RES_NOALIASES   0x00001000
#define RES_USE_INET6   0x00002000
#define RES_ROTATE      0x00004000
#define RES_NOCHECKNAME 0x00008000
#define RES_KEEPTSIG    0x00010000
#define RES_BLAST       0x00020000
#define RES_USE_EDNS0   0x00100000
#define RES_SNGLKUP     0x00200000
#define RES_SNGLKUPREOP 0x00400000
#define RES_USE_DNSSEC  0x00800000
#define RES_NOTLDQUERY  0x01000000
#define RES_DEFAULT     (RES_RECURSE | RES_DEFNAMES | RES_DNSRCH)

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

typedef struct __res_state *res_state;

int res_init(void);
int res_ninit(res_state statp);
void res_nclose(res_state statp);

#endif
