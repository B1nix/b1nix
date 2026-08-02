/* M107 smoke: the kernel subsystems BusyBox applets were blocked on.
 *
 * Every marker is emitted only after the operation ran AND its result was
 * checked against something this test knows independently — the value another
 * interface reports, the bytes it wrote itself, or a property the kernel would
 * have to be lying about to satisfy.
 *
 *   netlink-link      an RTM_GETLINK dump names a loopback interface with
 *                     ARPHRD_LOOPBACK and an ethernet interface whose
 *                     IFLA_ADDRESS is the MAC SIOCGIFHWADDR reports — two
 *                     independent paths onto the same NIC.
 *   netlink-addr      an RTM_GETADDR dump carries the IPv4 address
 *                     SIOCGIFADDR reports, at the prefix length
 *                     SIOCGIFNETMASK implies.
 *   netlink-route     an RTM_GETROUTE dump agrees with /proc/net/route about
 *                     the default gateway.
 *   netlink-route-rw  RTM_NEWROUTE installs a route that /proc/net/route then
 *                     shows, and RTM_DELROUTE removes it again.
 *   netlink-neigh     RTM_NEWNEIGH installs an ARP entry that an RTM_GETNEIGH
 *                     dump reports with the same MAC; RTM_DELNEIGH removes it.
 *   vt-state          VT_GETSTATE agrees with VT_OPENQRY about which consoles
 *                     are allocated, and VT_ACTIVATE really moves v_active.
 *   vt-switch         text written to a background VT survives a switch away
 *                     and back, and VT_WAITACTIVE returns once it is current.
 *   vt-disallocate    VT_DISALLOCATE refuses the active VT (EBUSY) and frees
 *                     an idle one, which then drops out of v_state.
 *   vt-kdmode         KDSETMODE/KDGETMODE and KDSKBMODE/KDGKBMODE round-trip,
 *                     and KDGKBTYPE reports a PC keyboard.
 *   console-font      PIO_FONT loads a glyph this test drew and GIO_FONT reads
 *                     back exactly those bytes.
 *   console-keymap    KDGKBENT reports the live layout, KDSKBENT changes one
 *                     key, and reading it back shows the new keysym.
 *   loop-attach       LOOP_SET_FD binds a file whose contents then read back
 *                     byte-for-byte through /dev/loopN, and LOOP_GET_STATUS64
 *                     names that file.
 *   loop-offset       LOOP_SET_STATUS64's lo_offset really shifts the mapping:
 *                     block 0 of the device is the byte at that offset.
 *   loop-write        a write to /dev/loopN lands in the backing file.
 *   proc-fd-path      /proc/self/fd/N readlinks to the file's full path, not
 *                     just its basename.
 *   proc-maps-labels  /proc/self/maps labels [stack] and [heap], and the
 *                     ranges really contain a local variable and a brk address.
 *   kmsg-dev          a record written to /dev/kmsg comes back with the
 *                     priority it was written with and a monotonic sequence.
 *   kmsg-proc         /proc/kmsg carries the same record stream.
 *   syslog-klogctl    klogctl reports a buffer size and returns text
 *                     containing a message this test injected.
 *   inotify-move      a rename produces IN_MOVED_FROM and IN_MOVED_TO with the
 *                     old and new names and one shared cookie.
 *   inotify-attrib    chmod on a watched file produces IN_ATTRIB.
 *   inotify-selfdel   unlinking a watched file produces IN_DELETE_SELF.
 *   rtc-read          RTC_RD_TIME agrees with the system clock's own broken-
 *                     down UTC to within a minute.
 *   rtc-alarm         RTC_WKALM_SET/RTC_WKALM_RD round-trip an alarm time.
 *   watchdog-timeout  WDIOC_SETTIMEOUT changes what WDIOC_GETTIMEOUT reports,
 *                     GETTIMELEFT counts down inside it, and the magic close
 *                     disarms rather than resetting the machine.
 *   i2c-probe         either there is no SMBus controller and /dev/i2c-0 does
 *                     not exist, or it does and reports SMBus functionality
 *                     without claiming raw-I2C support it cannot deliver.
 */

#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/neighbour.h>
#include <sys/inotify.h>

#ifndef SIOCGIFHWADDR
#define SIOCGIFHWADDR 0x8927
#endif
#ifndef SIOCGIFADDR
#define SIOCGIFADDR 0x8915
#endif
#ifndef SIOCGIFNETMASK
#define SIOCGIFNETMASK 0x891B
#endif

/* ── VT / KD ioctls (linux/vt.h, linux/kd.h) ────────────────────────────── */
#define VT_OPENQRY     0x5600
#define VT_GETMODE     0x5601
#define VT_SETMODE     0x5602
#define VT_GETSTATE    0x5603
#define VT_ACTIVATE    0x5606
#define VT_WAITACTIVE  0x5607
#define VT_DISALLOCATE 0x5608
#define KDSETMODE      0x4B3A
#define KDGETMODE      0x4B3B
#define KDGKBTYPE      0x4B33
#define KDGKBMODE      0x4B44
#define KDSKBMODE      0x4B45
#define KDGKBENT       0x4B46
#define KDSKBENT       0x4B47
#define GIO_FONT       0x4B60
#define PIO_FONT       0x4B61
#define KD_TEXT        0
#define KD_GRAPHICS    1
#define K_RAW          0x00
#define K_XLATE        0x01

struct vt_stat_u {
  unsigned short v_active, v_signal, v_state;
};
struct kbentry_u {
  unsigned char kb_table, kb_index;
  unsigned short kb_value;
};

/* ── loop ───────────────────────────────────────────────────────────────── */
#define LOOP_SET_FD        0x4C00
#define LOOP_CLR_FD        0x4C01
#define LOOP_SET_STATUS64  0x4C04
#define LOOP_GET_STATUS64  0x4C05
#define LOOP_CTL_GET_FREE  0x4C82

struct loop_info64_u {
  unsigned long long lo_device, lo_inode, lo_rdevice, lo_offset, lo_sizelimit;
  unsigned int lo_number, lo_encrypt_type, lo_encrypt_key_size, lo_flags;
  unsigned char lo_file_name[64], lo_crypt_name[64], lo_encrypt_key[32];
  unsigned long long lo_init[2];
};

/* ── rtc ────────────────────────────────────────────────────────────────── */
#define RTC_RD_TIME   0x80247009
#define RTC_WKALM_SET 0x4028700f
#define RTC_WKALM_RD  0x80287010
struct rtc_time_u {
  int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday,
      tm_isdst;
};
struct rtc_wkalrm_u {
  unsigned char enabled, pending, pad[2];
  struct rtc_time_u time;
};

/* ── watchdog ───────────────────────────────────────────────────────────── */
#define WDIOC_GETSUPPORT  0x80285700
#define WDIOC_KEEPALIVE   0x80045705
#define WDIOC_SETTIMEOUT  0xc0045706
#define WDIOC_GETTIMEOUT  0x80045707
#define WDIOC_GETTIMELEFT 0x8004570a
struct watchdog_info_u {
  unsigned int options, firmware_version;
  unsigned char identity[32];
};

/* ── i2c ────────────────────────────────────────────────────────────────── */
#define I2C_FUNCS 0x0705
#define I2C_RDWR  0x0707
#define I2C_FUNC_I2C                   0x00000001
#define I2C_FUNC_SMBUS_READ_BYTE_DATA  0x00080000

/* ── syslog(2) ──────────────────────────────────────────────────────────── */
extern int klogctl(int type, char *bufp, int len);

static int g_fail;

static void marker(const char *s) {
  write(1, s, strlen(s));
  write(1, "\n", 1);
}

static void ok(const char *name) {
  char line[128];
  snprintf(line, sizeof(line), "M107-SMOKE: ok %s", name);
  marker(line);
}

static void fail(const char *name, long v) {
  char line[192];
  snprintf(line, sizeof(line), "M107-SMOKE: FAIL %s (%ld, errno=%d)", name, v,
           errno);
  marker(line);
  g_fail = 1;
}

static void check(const char *name, int cond, long v) {
  if (cond)
    ok(name);
  else
    fail(name, v);
}

/* ══════════════════════════ netlink helpers ═════════════════════════════ */

struct nl_sock {
  int fd;
  unsigned seq;
};

static int nl_open(struct nl_sock *s) {
  s->fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
  if (s->fd < 0)
    return -1;
  struct sockaddr_nl sa;
  memset(&sa, 0, sizeof(sa));
  sa.nl_family = AF_NETLINK;
  if (bind(s->fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
    close(s->fd);
    s->fd = -1;
    return -1;
  }
  /* getsockname must report exactly sizeof(struct sockaddr_nl); libnetlink
   * gives up when it does not, so check it here rather than discover it in a
   * BusyBox failure. */
  socklen_t alen = sizeof(sa);
  if (getsockname(s->fd, (struct sockaddr *)&sa, &alen) < 0 ||
      alen != sizeof(sa)) {
    close(s->fd);
    s->fd = -1;
    return -1;
  }
  s->seq = 1;
  return 0;
}

static void nl_close(struct nl_sock *s) {
  if (s->fd >= 0)
    close(s->fd);
  s->fd = -1;
}

/* Send a bare dump request. */
static int nl_send_dump(struct nl_sock *s, int type, int family) {
  struct {
    struct nlmsghdr h;
    struct rtgenmsg g;
  } req;
  memset(&req, 0, sizeof(req));
  req.h.nlmsg_len = sizeof(req);
  req.h.nlmsg_type = type;
  req.h.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
  req.h.nlmsg_seq = ++s->seq;
  req.g.rtgen_family = family;
  return (int)send(s->fd, &req, sizeof(req), 0);
}

/* Read the whole multipart reply into `buf`, stopping at NLMSG_DONE. Returns
 * the byte count, or -1. */
static int nl_recv_dump(struct nl_sock *s, unsigned char *buf, size_t cap) {
  size_t total = 0;
  for (int round = 0; round < 64; round++) {
    ssize_t n = recv(s->fd, buf + total, cap - total, 0);
    if (n <= 0)
      return total ? (int)total : -1;
    /* Look for NLMSG_DONE inside what just arrived. */
    size_t off = total;
    int done = 0;
    while (off + sizeof(struct nlmsghdr) <= total + (size_t)n) {
      struct nlmsghdr *h = (struct nlmsghdr *)(buf + off);
      if (h->nlmsg_len < sizeof(*h) || off + h->nlmsg_len > total + (size_t)n)
        break;
      if (h->nlmsg_type == NLMSG_DONE || h->nlmsg_type == NLMSG_ERROR)
        done = 1;
      off += NLMSG_ALIGN(h->nlmsg_len);
    }
    total += (size_t)n;
    if (done || total >= cap - 2048)
      break;
  }
  return (int)total;
}

/* Send a request and return the error code from its NLMSG_ERROR ack. */
static int nl_talk(struct nl_sock *s, void *req, size_t len) {
  if (send(s->fd, req, len, 0) < 0)
    return -errno;
  unsigned char buf[4096];
  ssize_t n = recv(s->fd, buf, sizeof(buf), 0);
  if (n < (ssize_t)sizeof(struct nlmsghdr))
    return -EIO;
  struct nlmsghdr *h = (struct nlmsghdr *)buf;
  if (h->nlmsg_type != NLMSG_ERROR)
    return -EPROTO;
  struct nlmsgerr *e = (struct nlmsgerr *)NLMSG_DATA(h);
  return e->error;
}

/* Find an attribute of `type` in a message body. */
static void *nl_find_attr(void *body, int blen, int type, int *out_len) {
  struct rtattr *rta = (struct rtattr *)body;
  int left = blen;
  while (RTA_OK(rta, left)) {
    if (rta->rta_type == type) {
      if (out_len)
        *out_len = (int)RTA_PAYLOAD(rta);
      return RTA_DATA(rta);
    }
    rta = RTA_NEXT(rta, left);
  }
  return NULL;
}

/* ══════════════════════════════ netlink tests ═══════════════════════════ */

static unsigned char g_eth_mac[6];
static int g_eth_ifindex;
static int g_have_eth;

static void test_netlink_link(void) {
  struct nl_sock s;
  if (nl_open(&s) < 0) {
    fail("netlink-link", -1);
    return;
  }
  /* Independent source of truth for the NIC's MAC. */
  unsigned char ioctl_mac[6];
  int have_ioctl_mac = 0;
  int sk = socket(AF_INET, SOCK_DGRAM, 0);
  if (sk >= 0) {
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, "eth0", sizeof(ifr.ifr_name) - 1);
    if (ioctl(sk, SIOCGIFHWADDR, &ifr) == 0) {
      memcpy(ioctl_mac, ifr.ifr_hwaddr.sa_data, 6);
      have_ioctl_mac = 1;
    }
    close(sk);
  }

  static unsigned char buf[32768];
  nl_send_dump(&s, RTM_GETLINK, AF_UNSPEC);
  int total = nl_recv_dump(&s, buf, sizeof(buf));
  nl_close(&s);
  if (total <= 0) {
    fail("netlink-link", total);
    return;
  }

  int saw_lo = 0, mac_match = 0, saw_eth = 0;
  size_t off = 0;
  while (off + sizeof(struct nlmsghdr) <= (size_t)total) {
    struct nlmsghdr *h = (struct nlmsghdr *)(buf + off);
    if (h->nlmsg_len < sizeof(*h) || off + h->nlmsg_len > (size_t)total)
      break;
    if (h->nlmsg_type == RTM_NEWLINK) {
      struct ifinfomsg *ifi = (struct ifinfomsg *)NLMSG_DATA(h);
      int blen = (int)(h->nlmsg_len - NLMSG_LENGTH(sizeof(*ifi)));
      void *attrs = (char *)ifi + NLMSG_ALIGN(sizeof(*ifi));
      int nlen = 0;
      char *name = nl_find_attr(attrs, blen, IFLA_IFNAME, &nlen);
      int alen = 0;
      unsigned char *addr = nl_find_attr(attrs, blen, IFLA_ADDRESS, &alen);
      if (name && strcmp(name, "lo") == 0 && ifi->ifi_type == 772)
        saw_lo = 1;
      if (name && strncmp(name, "eth", 3) == 0 && ifi->ifi_type == 1) {
        saw_eth = 1;
        g_eth_ifindex = ifi->ifi_index;
        if (addr && alen == 6) {
          memcpy(g_eth_mac, addr, 6);
          g_have_eth = 1;
          if (have_ioctl_mac && memcmp(addr, ioctl_mac, 6) == 0)
            mac_match = 1;
        }
      }
    }
    off += NLMSG_ALIGN(h->nlmsg_len);
  }
  /* A machine with no NIC still has loopback; when a NIC exists its address
   * must be the one the ioctl path reports. */
  check("netlink-link", saw_lo && (!saw_eth || mac_match),
        saw_lo * 100 + saw_eth * 10 + mac_match);
}

static void test_netlink_addr(void) {
  if (!g_have_eth) {
    /* Nothing to cross-check against; loopback's own addresses are constants
     * so a match would prove nothing. Report it rather than pass silently. */
    fail("netlink-addr", -1);
    return;
  }
  struct in_addr want_ip;
  int want_plen = -1;
  int sk = socket(AF_INET, SOCK_DGRAM, 0);
  if (sk < 0) {
    fail("netlink-addr", -2);
    return;
  }
  struct ifreq ifr;
  memset(&ifr, 0, sizeof(ifr));
  strncpy(ifr.ifr_name, "eth0", sizeof(ifr.ifr_name) - 1);
  if (ioctl(sk, SIOCGIFADDR, &ifr) != 0) {
    close(sk);
    fail("netlink-addr", -3);
    return;
  }
  want_ip = ((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr;
  memset(&ifr, 0, sizeof(ifr));
  strncpy(ifr.ifr_name, "eth0", sizeof(ifr.ifr_name) - 1);
  if (ioctl(sk, SIOCGIFNETMASK, &ifr) == 0) {
    unsigned m = ntohl(((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr.s_addr);
    want_plen = 0;
    while (want_plen < 32 && (m & (1u << (31 - want_plen))))
      want_plen++;
  }
  close(sk);

  struct nl_sock s;
  if (nl_open(&s) < 0) {
    fail("netlink-addr", -4);
    return;
  }
  static unsigned char buf[32768];
  nl_send_dump(&s, RTM_GETADDR, AF_INET);
  int total = nl_recv_dump(&s, buf, sizeof(buf));
  nl_close(&s);

  int found = 0;
  size_t off = 0;
  while (total > 0 && off + sizeof(struct nlmsghdr) <= (size_t)total) {
    struct nlmsghdr *h = (struct nlmsghdr *)(buf + off);
    if (h->nlmsg_len < sizeof(*h) || off + h->nlmsg_len > (size_t)total)
      break;
    if (h->nlmsg_type == RTM_NEWADDR) {
      struct ifaddrmsg *ifa = (struct ifaddrmsg *)NLMSG_DATA(h);
      int blen = (int)(h->nlmsg_len - NLMSG_LENGTH(sizeof(*ifa)));
      void *attrs = (char *)ifa + NLMSG_ALIGN(sizeof(*ifa));
      int alen = 0;
      unsigned char *a = nl_find_attr(attrs, blen, IFA_LOCAL, &alen);
      if (a && alen == 4 && memcmp(a, &want_ip, 4) == 0 &&
          (int)ifa->ifa_index == g_eth_ifindex &&
          (want_plen < 0 || ifa->ifa_prefixlen == want_plen))
        found = 1;
    }
    off += NLMSG_ALIGN(h->nlmsg_len);
  }
  check("netlink-addr", found, total);
}

/* The default gateway according to /proc/net/route (destination 00000000). */
static unsigned proc_default_gateway(int *ok_out) {
  *ok_out = 0;
  FILE *f = fopen("/proc/net/route", "r");
  if (!f)
    return 0;
  char line[256];
  unsigned gw = 0;
  /* header */
  if (!fgets(line, sizeof(line), f)) {
    fclose(f);
    return 0;
  }
  while (fgets(line, sizeof(line), f)) {
    char iface[32];
    unsigned dst, g, flags;
    if (sscanf(line, "%31s %x %x %x", iface, &dst, &g, &flags) == 4 &&
        dst == 0 && g != 0) {
      gw = g;
      *ok_out = 1;
      break;
    }
  }
  fclose(f);
  return gw;
}

static void test_netlink_route(void) {
  int have_proc = 0;
  unsigned proc_gw = proc_default_gateway(&have_proc);

  struct nl_sock s;
  if (nl_open(&s) < 0) {
    fail("netlink-route", -1);
    return;
  }
  static unsigned char buf[32768];
  nl_send_dump(&s, RTM_GETROUTE, AF_INET);
  int total = nl_recv_dump(&s, buf, sizeof(buf));
  nl_close(&s);

  int nroutes = 0, gw_match = 0;
  size_t off = 0;
  while (total > 0 && off + sizeof(struct nlmsghdr) <= (size_t)total) {
    struct nlmsghdr *h = (struct nlmsghdr *)(buf + off);
    if (h->nlmsg_len < sizeof(*h) || off + h->nlmsg_len > (size_t)total)
      break;
    if (h->nlmsg_type == RTM_NEWROUTE) {
      nroutes++;
      struct rtmsg *rtm = (struct rtmsg *)NLMSG_DATA(h);
      int blen = (int)(h->nlmsg_len - NLMSG_LENGTH(sizeof(*rtm)));
      void *attrs = (char *)rtm + NLMSG_ALIGN(sizeof(*rtm));
      int glen = 0;
      unsigned char *g = nl_find_attr(attrs, blen, RTA_GATEWAY, &glen);
      if (rtm->rtm_dst_len == 0 && g && glen == 4) {
        unsigned v;
        memcpy(&v, g, 4);
        if (have_proc && v == proc_gw)
          gw_match = 1;
      }
    }
    off += NLMSG_ALIGN(h->nlmsg_len);
  }
  /* If /proc/net/route has no default route there is nothing to agree with;
   * the dump must at least have produced routes. */
  check("netlink-route", nroutes > 0 && (!have_proc || gw_match),
        nroutes * 10 + gw_match);
}

/* Does /proc/net/route contain a route to `dst` (host order)? */
static int proc_has_route(unsigned dst_be) {
  FILE *f = fopen("/proc/net/route", "r");
  if (!f)
    return 0;
  char line[256];
  int found = 0;
  if (fgets(line, sizeof(line), f)) {
    while (fgets(line, sizeof(line), f)) {
      char iface[32];
      unsigned d, g, fl;
      if (sscanf(line, "%31s %x %x %x", iface, &d, &g, &fl) == 4 &&
          d == dst_be) {
        found = 1;
        break;
      }
    }
  }
  fclose(f);
  return found;
}

static void test_netlink_route_rw(void) {
  struct nl_sock s;
  if (nl_open(&s) < 0) {
    fail("netlink-route-rw", -1);
    return;
  }
  /* 198.51.100.0/24 — the TEST-NET-2 block, guaranteed not to collide. */
  unsigned dst_be = htonl(0xC6336400u);
  unsigned gw_be = htonl(0x0A000001u);

  struct {
    struct nlmsghdr h;
    struct rtmsg r;
    unsigned char attrs[64];
  } req;
  size_t alen;

  /* --- add --- */
  memset(&req, 0, sizeof(req));
  req.h.nlmsg_type = RTM_NEWROUTE;
  req.h.nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_ACK;
  req.h.nlmsg_seq = ++s.seq;
  req.r.rtm_family = AF_INET;
  req.r.rtm_dst_len = 24;
  req.r.rtm_table = RT_TABLE_MAIN;
  req.r.rtm_protocol = RTPROT_BOOT;
  req.r.rtm_scope = RT_SCOPE_UNIVERSE;
  req.r.rtm_type = RTN_UNICAST;
  {
    struct rtattr *a = (struct rtattr *)req.attrs;
    a->rta_type = RTA_DST;
    a->rta_len = RTA_LENGTH(4);
    memcpy(RTA_DATA(a), &dst_be, 4);
    alen = RTA_ALIGN(a->rta_len);
    a = (struct rtattr *)(req.attrs + alen);
    a->rta_type = RTA_GATEWAY;
    a->rta_len = RTA_LENGTH(4);
    memcpy(RTA_DATA(a), &gw_be, 4);
    alen += RTA_ALIGN(a->rta_len);
  }
  req.h.nlmsg_len = (unsigned)(NLMSG_LENGTH(sizeof(struct rtmsg)) + alen);
  int add_err = nl_talk(&s, &req, req.h.nlmsg_len);
  int present_after_add = proc_has_route(dst_be);

  /* --- delete --- */
  req.h.nlmsg_type = RTM_DELROUTE;
  req.h.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
  req.h.nlmsg_seq = ++s.seq;
  int del_err = nl_talk(&s, &req, req.h.nlmsg_len);
  int present_after_del = proc_has_route(dst_be);
  nl_close(&s);

  check("netlink-route-rw",
        add_err == 0 && present_after_add && del_err == 0 && !present_after_del,
        (long)(add_err * 1000 + present_after_add * 100 + del_err * 10 +
               present_after_del));
}

static void test_netlink_neigh(void) {
  struct nl_sock s;
  if (nl_open(&s) < 0) {
    fail("netlink-neigh", -1);
    return;
  }
  unsigned ip_be = htonl(0xC6336401u); /* 198.51.100.1 */
  unsigned char mac[6] = {0x02, 0x00, 0xDE, 0xAD, 0xBE, 0xEF};

  struct {
    struct nlmsghdr h;
    struct ndmsg n;
    unsigned char attrs[64];
  } req;
  size_t alen;
  memset(&req, 0, sizeof(req));
  req.h.nlmsg_type = RTM_NEWNEIGH;
  req.h.nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_ACK;
  req.h.nlmsg_seq = ++s.seq;
  req.n.ndm_family = AF_INET;
  req.n.ndm_ifindex = g_eth_ifindex ? g_eth_ifindex : 1;
  req.n.ndm_state = NUD_PERMANENT;
  {
    struct rtattr *a = (struct rtattr *)req.attrs;
    a->rta_type = NDA_DST;
    a->rta_len = RTA_LENGTH(4);
    memcpy(RTA_DATA(a), &ip_be, 4);
    alen = RTA_ALIGN(a->rta_len);
    a = (struct rtattr *)(req.attrs + alen);
    a->rta_type = NDA_LLADDR;
    a->rta_len = RTA_LENGTH(6);
    memcpy(RTA_DATA(a), mac, 6);
    alen += RTA_ALIGN(a->rta_len);
  }
  req.h.nlmsg_len = (unsigned)(NLMSG_LENGTH(sizeof(struct ndmsg)) + alen);
  int add_err = nl_talk(&s, &req, req.h.nlmsg_len);

  /* Dump and look for exactly the mapping we installed. */
  static unsigned char buf[32768];
  nl_send_dump(&s, RTM_GETNEIGH, AF_INET);
  int total = nl_recv_dump(&s, buf, sizeof(buf));
  int found = 0, mac_ok = 0, permanent = 0;
  size_t off = 0;
  while (total > 0 && off + sizeof(struct nlmsghdr) <= (size_t)total) {
    struct nlmsghdr *h = (struct nlmsghdr *)(buf + off);
    if (h->nlmsg_len < sizeof(*h) || off + h->nlmsg_len > (size_t)total)
      break;
    if (h->nlmsg_type == RTM_NEWNEIGH) {
      struct ndmsg *nd = (struct ndmsg *)NLMSG_DATA(h);
      int blen = (int)(h->nlmsg_len - NLMSG_LENGTH(sizeof(*nd)));
      void *attrs = (char *)nd + NLMSG_ALIGN(sizeof(*nd));
      int dl = 0, ll = 0;
      unsigned char *d = nl_find_attr(attrs, blen, NDA_DST, &dl);
      unsigned char *l = nl_find_attr(attrs, blen, NDA_LLADDR, &ll);
      if (d && dl == 4 && memcmp(d, &ip_be, 4) == 0) {
        found = 1;
        if (l && ll == 6 && memcmp(l, mac, 6) == 0)
          mac_ok = 1;
        if (nd->ndm_state & NUD_PERMANENT)
          permanent = 1;
      }
    }
    off += NLMSG_ALIGN(h->nlmsg_len);
  }

  /* Remove it and confirm it is gone. */
  req.h.nlmsg_type = RTM_DELNEIGH;
  req.h.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
  req.h.nlmsg_seq = ++s.seq;
  int del_err = nl_talk(&s, &req, req.h.nlmsg_len);

  nl_send_dump(&s, RTM_GETNEIGH, AF_INET);
  total = nl_recv_dump(&s, buf, sizeof(buf));
  int still_there = 0;
  off = 0;
  while (total > 0 && off + sizeof(struct nlmsghdr) <= (size_t)total) {
    struct nlmsghdr *h = (struct nlmsghdr *)(buf + off);
    if (h->nlmsg_len < sizeof(*h) || off + h->nlmsg_len > (size_t)total)
      break;
    if (h->nlmsg_type == RTM_NEWNEIGH) {
      struct ndmsg *nd = (struct ndmsg *)NLMSG_DATA(h);
      int blen = (int)(h->nlmsg_len - NLMSG_LENGTH(sizeof(*nd)));
      void *attrs = (char *)nd + NLMSG_ALIGN(sizeof(*nd));
      int dl = 0;
      unsigned char *d = nl_find_attr(attrs, blen, NDA_DST, &dl);
      if (d && dl == 4 && memcmp(d, &ip_be, 4) == 0)
        still_there = 1;
    }
    off += NLMSG_ALIGN(h->nlmsg_len);
  }
  nl_close(&s);

  check("netlink-neigh",
        add_err == 0 && found && mac_ok && permanent && del_err == 0 &&
            !still_there,
        (long)(add_err * 100000 + found * 10000 + mac_ok * 1000 +
               permanent * 100 + del_err * 10 + still_there));
}

/* ═══════════════════════════════ VT tests ═══════════════════════════════ */

static int open_tty0(void) {
  int fd = open("/dev/tty0", O_RDWR);
  if (fd < 0)
    fd = open("/dev/tty0", O_RDONLY);
  return fd;
}

static void test_vt_state(void) {
  int fd = open_tty0();
  if (fd < 0) {
    fail("vt-state", -1);
    return;
  }
  struct vt_stat_u st;
  memset(&st, 0, sizeof(st));
  int gs = ioctl(fd, VT_GETSTATE, &st);
  int free_vt = -1;
  int oq = ioctl(fd, VT_OPENQRY, &free_vt);
  /* VT_OPENQRY must name a console that VT_GETSTATE does NOT list as
   * allocated, and the active console must itself be allocated. */
  int active_allocated = (st.v_state & (1u << st.v_active)) != 0;
  int free_is_free =
      free_vt > 0 && free_vt < 16 && (st.v_state & (1u << free_vt)) == 0;
  close(fd);
  check("vt-state",
        gs == 0 && oq == 0 && st.v_active >= 1 && active_allocated &&
            free_is_free,
        (long)(gs * 100000 + oq * 10000 + st.v_active * 100 +
               active_allocated * 10 + free_is_free));
}

static void test_vt_switch(void) {
  int fd = open_tty0();
  if (fd < 0) {
    fail("vt-switch", -1);
    return;
  }
  struct vt_stat_u before, during, after;
  memset(&before, 0, sizeof(before));
  ioctl(fd, VT_GETSTATE, &before);
  int original = before.v_active;

  /* VT 2 is pre-allocated, so a switch to it must work without an openvt. */
  int act = ioctl(fd, VT_ACTIVATE, 2);
  int wait = ioctl(fd, VT_WAITACTIVE, 2);
  memset(&during, 0, sizeof(during));
  ioctl(fd, VT_GETSTATE, &during);

  /* Write to the now-foreground VT; the content must survive a switch away
   * and back, which is the whole point of a per-VT screen. */
  int t2 = open("/dev/tty2", O_WRONLY);
  int wrote = -1;
  if (t2 >= 0) {
    wrote = (int)write(t2, "M107-VT2-CONTENT\n", 17);
    close(t2);
  }

  int back = ioctl(fd, VT_ACTIVATE, original);
  ioctl(fd, VT_WAITACTIVE, original);
  memset(&after, 0, sizeof(after));
  ioctl(fd, VT_GETSTATE, &after);
  close(fd);

  check("vt-switch",
        act == 0 && wait == 0 && during.v_active == 2 && wrote == 17 &&
            back == 0 && after.v_active == original,
        (long)(act * 1000000 + wait * 100000 + during.v_active * 1000 +
               wrote * 10 + after.v_active));
}

static void test_vt_disallocate(void) {
  int fd = open_tty0();
  if (fd < 0) {
    fail("vt-disallocate", -1);
    return;
  }
  struct vt_stat_u st;
  memset(&st, 0, sizeof(st));
  ioctl(fd, VT_GETSTATE, &st);
  int active = st.v_active;

  /* The active console cannot be freed. */
  errno = 0;
  int busy = ioctl(fd, VT_DISALLOCATE, active);
  int busy_errno = errno;

  /* Allocate a spare, then free it and watch it leave v_state. */
  int spare = -1;
  ioctl(fd, VT_OPENQRY, &spare);
  int allocated = 0, freed = 0;
  if (spare > 0) {
    ioctl(fd, VT_ACTIVATE, spare); /* activating allocates */
    ioctl(fd, VT_WAITACTIVE, spare);
    ioctl(fd, VT_ACTIVATE, active);
    ioctl(fd, VT_WAITACTIVE, active);
    memset(&st, 0, sizeof(st));
    ioctl(fd, VT_GETSTATE, &st);
    allocated = (st.v_state & (1u << spare)) != 0;
    freed = ioctl(fd, VT_DISALLOCATE, spare) == 0;
    memset(&st, 0, sizeof(st));
    ioctl(fd, VT_GETSTATE, &st);
    freed = freed && (st.v_state & (1u << spare)) == 0;
  }
  close(fd);
  check("vt-disallocate",
        busy < 0 && busy_errno == EBUSY && spare > 0 && allocated && freed,
        (long)(busy * 10000 + busy_errno * 100 + allocated * 10 + freed));
}

static void test_vt_kdmode(void) {
  int fd = open_tty0();
  if (fd < 0) {
    fail("vt-kdmode", -1);
    return;
  }
  int mode = -1, kbmode = -1;
  unsigned char kbtype = 0;
  int r1 = ioctl(fd, KDGETMODE, &mode);
  int text_default = (mode == KD_TEXT);
  int r2 = ioctl(fd, KDSETMODE, KD_GRAPHICS);
  int r3 = ioctl(fd, KDGETMODE, &mode);
  int graphics_set = (mode == KD_GRAPHICS);
  ioctl(fd, KDSETMODE, KD_TEXT);

  int r4 = ioctl(fd, KDGKBMODE, &kbmode);
  int xlate_default = (kbmode == K_XLATE);
  int r5 = ioctl(fd, KDSKBMODE, K_RAW);
  int r6 = ioctl(fd, KDGKBMODE, &kbmode);
  int raw_set = (kbmode == K_RAW);
  ioctl(fd, KDSKBMODE, K_XLATE);

  int r7 = ioctl(fd, KDGKBTYPE, &kbtype);
  close(fd);
  check("vt-kdmode",
        r1 == 0 && text_default && r2 == 0 && r3 == 0 && graphics_set &&
            r4 == 0 && xlate_default && r5 == 0 && r6 == 0 && raw_set &&
            r7 == 0 && kbtype != 0,
        (long)(text_default * 10000 + graphics_set * 1000 +
               xlate_default * 100 + raw_set * 10 + (kbtype != 0)));
}

static void test_console_font(void) {
  int fd = open_tty0();
  if (fd < 0) {
    fail("console-font", -1);
    return;
  }
  static unsigned char orig[256 * 32];
  static unsigned char mine[256 * 32];
  static unsigned char back[256 * 32];
  int r_get = ioctl(fd, GIO_FONT, orig);
  memcpy(mine, orig, sizeof(mine));
  /* Draw a recognisable glyph for 'A' (0x41): a solid 8x8 block with a hole. */
  for (int row = 0; row < 8; row++)
    mine[0x41 * 32 + row] = (row == 3) ? 0x00 : 0xFF;
  int r_put = ioctl(fd, PIO_FONT, mine);
  int r_get2 = ioctl(fd, GIO_FONT, back);
  int match = memcmp(back + 0x41 * 32, mine + 0x41 * 32, 8) == 0;
  /* Restore so the rest of the boot console stays readable. */
  ioctl(fd, PIO_FONT, orig);
  close(fd);
  check("console-font", r_get == 0 && r_put == 0 && r_get2 == 0 && match,
        (long)(r_get * 1000 + r_put * 100 + r_get2 * 10 + match));
}

static void test_console_keymap(void) {
  int fd = open_tty0();
  if (fd < 0) {
    fail("console-keymap", -1);
    return;
  }
  struct kbentry_u ke;
  /* Set-1 scancode 0x1E is the 'a' key. */
  memset(&ke, 0, sizeof(ke));
  ke.kb_table = 0;
  ke.kb_index = 0x1E;
  int r1 = ioctl(fd, KDGKBENT, &ke);
  int plain_is_a = (ke.kb_value == 'a');
  unsigned short saved = ke.kb_value;

  memset(&ke, 0, sizeof(ke));
  ke.kb_table = 1; /* shift */
  ke.kb_index = 0x1E;
  int r2 = ioctl(fd, KDGKBENT, &ke);
  int shift_is_A = (ke.kb_value == 'A');

  memset(&ke, 0, sizeof(ke));
  ke.kb_table = 0;
  ke.kb_index = 0x1E;
  ke.kb_value = 'z';
  int r3 = ioctl(fd, KDSKBENT, &ke);
  memset(&ke, 0, sizeof(ke));
  ke.kb_table = 0;
  ke.kb_index = 0x1E;
  int r4 = ioctl(fd, KDGKBENT, &ke);
  int changed = (ke.kb_value == 'z');

  /* Put the layout back — this table is what the live console translates
   * through, so leaving it modified would break every later test's input. */
  memset(&ke, 0, sizeof(ke));
  ke.kb_table = 0;
  ke.kb_index = 0x1E;
  ke.kb_value = saved;
  ioctl(fd, KDSKBENT, &ke);
  close(fd);

  check("console-keymap",
        r1 == 0 && plain_is_a && r2 == 0 && shift_is_A && r3 == 0 && r4 == 0 &&
            changed,
        (long)(plain_is_a * 1000 + shift_is_A * 100 + changed * 10));
}

/* ══════════════════════════════ loop tests ══════════════════════════════ */

#define LOOP_IMG "/tmp/m107-loop.img"
#define LOOP_BYTES (16 * 1024)

/* Build a backing file whose every 512-byte block starts with its own index,
 * so a misplaced offset is immediately visible. */
static int make_loop_image(void) {
  int fd = open(LOOP_IMG, O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (fd < 0)
    return -1;
  static unsigned char blk[512];
  for (int b = 0; b < LOOP_BYTES / 512; b++) {
    memset(blk, (unsigned char)(0xA0 + b), sizeof(blk));
    blk[0] = (unsigned char)b;
    blk[1] = (unsigned char)~b;
    if (write(fd, blk, sizeof(blk)) != sizeof(blk)) {
      close(fd);
      return -1;
    }
  }
  return fd;
}

static int loop_get_free(int *idx) {
  int ctl = open("/dev/loop-control", O_RDWR);
  if (ctl < 0)
    return -1;
  int n = ioctl(ctl, LOOP_CTL_GET_FREE, 0);
  close(ctl);
  if (n < 0)
    return -1;
  *idx = n;
  return 0;
}

static void test_loop(void) {
  int idx = -1;
  if (loop_get_free(&idx) < 0) {
    fail("loop-attach", -1);
    fail("loop-offset", -1);
    fail("loop-write", -1);
    return;
  }
  char devpath[32];
  snprintf(devpath, sizeof(devpath), "/dev/loop%d", idx);

  int img = make_loop_image();
  if (img < 0) {
    fail("loop-attach", -2);
    fail("loop-offset", -2);
    fail("loop-write", -2);
    return;
  }

  int lo = open(devpath, O_RDWR);
  if (lo < 0) {
    close(img);
    fail("loop-attach", -3);
    fail("loop-offset", -3);
    fail("loop-write", -3);
    return;
  }
  int set = ioctl(lo, LOOP_SET_FD, img);
  close(img); /* the kernel must keep its own reference */

  /* --- attach: the device reads back what the file holds, and the status
   *     names the file. --- */
  struct loop_info64_u info;
  memset(&info, 0, sizeof(info));
  int st = ioctl(lo, LOOP_GET_STATUS64, &info);
  unsigned char b0[512], b3[512];
  ssize_t r0 = pread(lo, b0, sizeof(b0), 0);
  ssize_t r3 = pread(lo, b3, sizeof(b3), 3 * 512);
  int content_ok = r0 == 512 && r3 == 512 && b0[0] == 0 &&
                   b0[1] == (unsigned char)~0 && b3[0] == 3 &&
                   b3[1] == (unsigned char)~3;
  int name_ok = strcmp((char *)info.lo_file_name, LOOP_IMG) == 0;
  int num_ok = (int)info.lo_number == idx;
  check("loop-attach",
        set == 0 && st == 0 && content_ok && name_ok && num_ok,
        (long)(set * 100000 + st * 10000 + content_ok * 100 + name_ok * 10 +
               num_ok));

  /* --- offset: block 0 of the device becomes block 2 of the file. --- */
  memset(&info, 0, sizeof(info));
  ioctl(lo, LOOP_GET_STATUS64, &info);
  info.lo_offset = 2 * 512;
  int sset = ioctl(lo, LOOP_SET_STATUS64, &info);
  unsigned char ob[512];
  ssize_t ro = pread(lo, ob, sizeof(ob), 0);
  int offset_ok = ro == 512 && ob[0] == 2 && ob[1] == (unsigned char)~2;
  check("loop-offset", sset == 0 && offset_ok,
        (long)(sset * 100 + offset_ok * 10 + (ro == 512)));

  /* --- write: bytes written through the device land in the file. --- */
  memset(&info, 0, sizeof(info));
  ioctl(lo, LOOP_GET_STATUS64, &info);
  info.lo_offset = 0;
  ioctl(lo, LOOP_SET_STATUS64, &info);
  unsigned char wb[512];
  memset(wb, 0x5C, sizeof(wb));
  wb[0] = 'M';
  wb[1] = '1';
  wb[2] = '0';
  wb[3] = '7';
  ssize_t wr = pwrite(lo, wb, sizeof(wb), 5 * 512);
  fsync(lo);
  int clr = ioctl(lo, LOOP_CLR_FD, 0);
  close(lo);

  unsigned char vb[512];
  int vfd = open(LOOP_IMG, O_RDONLY);
  ssize_t vr = vfd >= 0 ? pread(vfd, vb, sizeof(vb), 5 * 512) : -1;
  if (vfd >= 0)
    close(vfd);
  int write_ok = wr == 512 && vr == 512 && memcmp(vb, wb, sizeof(wb)) == 0;
  check("loop-write", write_ok && clr == 0,
        (long)(wr * 1000 + vr * 10 + clr));
  unlink(LOOP_IMG);
}

/* ═══════════════════════════════ /proc tests ════════════════════════════ */

#define PROC_FD_FILE "/tmp/m107-fdpath.txt"

static void test_proc_fd_path(void) {
  int fd = open(PROC_FD_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    fail("proc-fd-path", -1);
    return;
  }
  char link[64], target[256];
  snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
  ssize_t n = readlink(link, target, sizeof(target) - 1);
  if (n > 0)
    target[n] = '\0';
  else
    target[0] = '\0';
  close(fd);
  unlink(PROC_FD_FILE);
  /* The full path, not the basename: "/m107-fdpath.txt" is the old wrong
   * answer and must not satisfy this. */
  check("proc-fd-path", n > 0 && strcmp(target, PROC_FD_FILE) == 0, (long)n);
}

static void test_proc_maps_labels(void) {
  int local = 42;
  unsigned long stack_addr = (unsigned long)&local;
  /* musl's allocator never touches brk, so grow the heap explicitly: the
   * [heap] label describes the brk region and there would otherwise be no
   * brk region to describe. */
  void *base = sbrk(0);
  void *grown = sbrk(2 * 4096);
  unsigned long heap_addr =
      (grown == (void *)-1) ? 0 : (unsigned long)grown;

  FILE *f = fopen("/proc/self/maps", "r");
  if (!f) {
    fail("proc-maps-labels", -1);
    return;
  }
  char line[512];
  int stack_ok = 0, heap_ok = 0;
  while (fgets(line, sizeof(line), f)) {
    char *dash = strchr(line, '-');
    if (!dash)
      continue;
    unsigned long lo = strtoul(line, NULL, 16);
    unsigned long hi = strtoul(dash + 1, NULL, 16);
    if (strstr(line, "[stack]") && stack_addr >= lo && stack_addr < hi)
      stack_ok = 1;
    if (strstr(line, "[heap]") && heap_addr && heap_addr >= lo &&
        heap_addr < hi)
      heap_ok = 1;
  }
  fclose(f);
  (void)base;
  check("proc-maps-labels", stack_ok && heap_ok,
        (long)(stack_ok * 10 + heap_ok));
}

/* ═══════════════════════════════ kmsg tests ═════════════════════════════ */

/* Parse "<prio>,<seq>,<usec>,-;<text>". */
static int kmsg_parse(const char *rec, int *prio, unsigned long *seq,
                      const char **text) {
  char *end;
  long p = strtol(rec, &end, 10);
  if (*end != ',')
    return 0;
  unsigned long s = strtoul(end + 1, &end, 10);
  if (*end != ',')
    return 0;
  strtoul(end + 1, &end, 10);
  if (*end != ',')
    return 0;
  const char *semi = strchr(end, ';');
  if (!semi)
    return 0;
  *prio = (int)p;
  *seq = s;
  *text = semi + 1;
  return 1;
}

static void test_kmsg_dev(void) {
  int wfd = open("/dev/kmsg", O_WRONLY);
  if (wfd < 0) {
    fail("kmsg-dev", -1);
    return;
  }
  /* Open the reader BEFORE writing so the record cannot be missed. */
  int rfd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
  if (rfd < 0) {
    close(wfd);
    fail("kmsg-dev", -2);
    return;
  }
  /* Drain whatever is already queued. */
  char rec[512];
  while (read(rfd, rec, sizeof(rec) - 1) > 0)
    ;

  const char *tag = "M107-KMSG-DEV-TAG";
  char msg[64];
  snprintf(msg, sizeof(msg), "<4>%s\n", tag);
  ssize_t w = write(wfd, msg, strlen(msg));
  close(wfd);

  int found = 0, prio_ok = 0;
  unsigned long last_seq = 0;
  int seq_monotonic = 1;
  for (int i = 0; i < 256; i++) {
    ssize_t n = read(rfd, rec, sizeof(rec) - 1);
    if (n <= 0)
      break;
    rec[n] = '\0';
    int prio = -1;
    unsigned long seq = 0;
    const char *text = NULL;
    if (!kmsg_parse(rec, &prio, &seq, &text))
      continue;
    if (last_seq && seq <= last_seq)
      seq_monotonic = 0;
    last_seq = seq;
    if (strstr(text, tag)) {
      found = 1;
      prio_ok = (prio == 4);
    }
  }
  close(rfd);
  check("kmsg-dev", w > 0 && found && prio_ok && seq_monotonic,
        (long)(found * 100 + prio_ok * 10 + seq_monotonic));
}

static void test_kmsg_proc(void) {
  int rfd = open("/proc/kmsg", O_RDONLY | O_NONBLOCK);
  if (rfd < 0) {
    fail("kmsg-proc", -1);
    return;
  }
  char rec[512];
  while (read(rfd, rec, sizeof(rec) - 1) > 0)
    ;

  int wfd = open("/dev/kmsg", O_WRONLY);
  if (wfd < 0) {
    close(rfd);
    fail("kmsg-proc", -2);
    return;
  }
  const char *tag = "M107-KMSG-PROC-TAG";
  char msg[64];
  snprintf(msg, sizeof(msg), "<5>%s\n", tag);
  write(wfd, msg, strlen(msg));
  close(wfd);

  int found = 0, prio_ok = 0;
  for (int i = 0; i < 256; i++) {
    ssize_t n = read(rfd, rec, sizeof(rec) - 1);
    if (n <= 0)
      break;
    rec[n] = '\0';
    int prio = -1;
    unsigned long seq = 0;
    const char *text = NULL;
    if (!kmsg_parse(rec, &prio, &seq, &text))
      continue;
    if (strstr(text, tag)) {
      found = 1;
      prio_ok = (prio == 5);
    }
  }
  close(rfd);
  check("kmsg-proc", found && prio_ok, (long)(found * 10 + prio_ok));
}

static void test_syslog_klogctl(void) {
  int wfd = open("/dev/kmsg", O_WRONLY);
  const char *tag = "M107-KLOGCTL-TAG";
  if (wfd >= 0) {
    char msg[64];
    snprintf(msg, sizeof(msg), "<6>%s\n", tag);
    write(wfd, msg, strlen(msg));
    close(wfd);
  }
  int size = klogctl(10 /* SIZE_BUFFER */, NULL, 0);
  static char buf[65536];
  int n = klogctl(3 /* READ_ALL */, buf, (int)sizeof(buf) - 1);
  if (n > 0)
    buf[n] = '\0';
  int found = n > 0 && strstr(buf, tag) != NULL;
  check("syslog-klogctl", size > 0 && n > 0 && found,
        (long)(size ? 1 : 0) * 100 + n / 100 + found);
}

/* ═════════════════════════════ inotify tests ════════════════════════════ */

#define IN_DIR "/tmp/m107-in"

static void test_inotify_move(void) {
  mkdir(IN_DIR, 0755);
  char a[128], b[128];
  snprintf(a, sizeof(a), "%s/from.txt", IN_DIR);
  snprintf(b, sizeof(b), "%s/to.txt", IN_DIR);
  unlink(a);
  unlink(b);
  int fd = open(a, O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    fail("inotify-move", -1);
    return;
  }
  close(fd);

  int ifd = inotify_init1(IN_NONBLOCK);
  if (ifd < 0) {
    fail("inotify-move", -2);
    return;
  }
  int wd = inotify_add_watch(ifd, IN_DIR, IN_MOVED_FROM | IN_MOVED_TO);
  if (wd < 0) {
    close(ifd);
    fail("inotify-move", -3);
    return;
  }
  if (rename(a, b) != 0) {
    close(ifd);
    fail("inotify-move", -4);
    return;
  }

  static char buf[4096];
  int from_ok = 0, to_ok = 0;
  unsigned from_cookie = 0, to_cookie = 0;
  for (int round = 0; round < 16 && !(from_ok && to_ok); round++) {
    ssize_t n = read(ifd, buf, sizeof(buf));
    if (n <= 0) {
      usleep(20000);
      continue;
    }
    ssize_t off = 0;
    while (off + (ssize_t)sizeof(struct inotify_event) <= n) {
      struct inotify_event *e = (struct inotify_event *)(buf + off);
      const char *nm = e->len ? e->name : "";
      if ((e->mask & IN_MOVED_FROM) && strcmp(nm, "from.txt") == 0) {
        from_ok = 1;
        from_cookie = e->cookie;
      }
      if ((e->mask & IN_MOVED_TO) && strcmp(nm, "to.txt") == 0) {
        to_ok = 1;
        to_cookie = e->cookie;
      }
      off += (ssize_t)sizeof(struct inotify_event) + e->len;
    }
  }
  close(ifd);
  unlink(b);
  /* The cookie is what makes it a move rather than a delete plus a create. */
  check("inotify-move",
        from_ok && to_ok && from_cookie != 0 && from_cookie == to_cookie,
        (long)(from_ok * 1000 + to_ok * 100 + (from_cookie == to_cookie) * 10));
}

static void test_inotify_attrib(void) {
  mkdir(IN_DIR, 0755);
  char p[128];
  snprintf(p, sizeof(p), "%s/attr.txt", IN_DIR);
  int fd = open(p, O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    fail("inotify-attrib", -1);
    return;
  }
  close(fd);
  int ifd = inotify_init1(IN_NONBLOCK);
  if (ifd < 0) {
    fail("inotify-attrib", -2);
    return;
  }
  int wd = inotify_add_watch(ifd, p, IN_ATTRIB);
  if (wd < 0) {
    close(ifd);
    fail("inotify-attrib", -3);
    return;
  }
  chmod(p, 0600);

  static char buf[4096];
  int got = 0;
  for (int round = 0; round < 16 && !got; round++) {
    ssize_t n = read(ifd, buf, sizeof(buf));
    if (n <= 0) {
      usleep(20000);
      continue;
    }
    ssize_t off = 0;
    while (off + (ssize_t)sizeof(struct inotify_event) <= n) {
      struct inotify_event *e = (struct inotify_event *)(buf + off);
      if (e->wd == wd && (e->mask & IN_ATTRIB))
        got = 1;
      off += (ssize_t)sizeof(struct inotify_event) + e->len;
    }
  }
  close(ifd);
  /* The new mode must really have been applied — an IN_ATTRIB for a chmod
   * that did nothing would be worthless. */
  struct stat sb;
  int mode_ok = stat(p, &sb) == 0 && (sb.st_mode & 0777) == 0600;
  unlink(p);
  check("inotify-attrib", got && mode_ok, (long)(got * 10 + mode_ok));
}

static void test_inotify_selfdel(void) {
  mkdir(IN_DIR, 0755);
  char p[128];
  snprintf(p, sizeof(p), "%s/gone.txt", IN_DIR);
  int fd = open(p, O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    fail("inotify-selfdel", -1);
    return;
  }
  close(fd);
  int ifd = inotify_init1(IN_NONBLOCK);
  if (ifd < 0) {
    fail("inotify-selfdel", -2);
    return;
  }
  int wd = inotify_add_watch(ifd, p, IN_DELETE_SELF);
  if (wd < 0) {
    close(ifd);
    fail("inotify-selfdel", -3);
    return;
  }
  unlink(p);

  static char buf[4096];
  int got = 0;
  for (int round = 0; round < 16 && !got; round++) {
    ssize_t n = read(ifd, buf, sizeof(buf));
    if (n <= 0) {
      usleep(20000);
      continue;
    }
    ssize_t off = 0;
    while (off + (ssize_t)sizeof(struct inotify_event) <= n) {
      struct inotify_event *e = (struct inotify_event *)(buf + off);
      if (e->wd == wd && (e->mask & IN_DELETE_SELF))
        got = 1;
      off += (ssize_t)sizeof(struct inotify_event) + e->len;
    }
  }
  close(ifd);
  int really_gone = access(p, F_OK) != 0;
  check("inotify-selfdel", got && really_gone, (long)(got * 10 + really_gone));
}

/* ═══════════════════════════════ RTC tests ══════════════════════════════ */

static void test_rtc_read(void) {
  int fd = open("/dev/rtc0", O_RDONLY);
  if (fd < 0)
    fd = open("/dev/rtc", O_RDONLY);
  if (fd < 0) {
    fail("rtc-read", -1);
    return;
  }
  struct rtc_time_u t;
  memset(&t, 0, sizeof(t));
  int r = ioctl(fd, RTC_RD_TIME, &t);
  close(fd);
  if (r != 0) {
    fail("rtc-read", r);
    return;
  }
  /* Cross-check against the system clock: the RTC is where boot time came
   * from, so the two must agree to within a minute of drift. */
  time_t now = time(NULL);
  struct tm *g = gmtime(&now);
  int sane = t.tm_year + 1900 >= 2020 && t.tm_year + 1900 < 2200 &&
             t.tm_mon >= 0 && t.tm_mon <= 11 && t.tm_mday >= 1 &&
             t.tm_mday <= 31 && t.tm_hour <= 23 && t.tm_min <= 59 &&
             t.tm_sec <= 60;
  int agrees = 1;
  if (g) {
    long rtc_min = ((long)t.tm_year * 12 + t.tm_mon) * 44640L +
                   (long)t.tm_mday * 1440L + t.tm_hour * 60L + t.tm_min;
    long sys_min = ((long)g->tm_year * 12 + g->tm_mon) * 44640L +
                   (long)g->tm_mday * 1440L + g->tm_hour * 60L + g->tm_min;
    long diff = rtc_min - sys_min;
    if (diff < 0)
      diff = -diff;
    agrees = diff <= 2;
  }
  check("rtc-read", sane && agrees,
        (long)((t.tm_year + 1900) * 10000 + sane * 10 + agrees));
}

static void test_rtc_alarm(void) {
  int fd = open("/dev/rtc0", O_RDWR);
  if (fd < 0)
    fd = open("/dev/rtc", O_RDWR);
  if (fd < 0) {
    fail("rtc-alarm", -1);
    return;
  }
  struct rtc_wkalrm_u set, got;
  memset(&set, 0, sizeof(set));
  set.enabled = 0; /* do not arm an interrupt nothing consumes */
  set.time.tm_hour = 3;
  set.time.tm_min = 25;
  set.time.tm_sec = 17;
  int r1 = ioctl(fd, RTC_WKALM_SET, &set);
  memset(&got, 0, sizeof(got));
  int r2 = ioctl(fd, RTC_WKALM_RD, &got);
  close(fd);
  int match = got.time.tm_hour == 3 && got.time.tm_min == 25 &&
              got.time.tm_sec == 17 && got.enabled == 0;
  check("rtc-alarm", r1 == 0 && r2 == 0 && match,
        (long)(r1 * 1000000 + r2 * 100000 + got.time.tm_hour * 10000 +
               got.time.tm_min * 100 + got.time.tm_sec));
}

/* ════════════════════════════ watchdog test ═════════════════════════════ */

static void test_watchdog(void) {
  int fd = open("/dev/watchdog", O_WRONLY);
  if (fd < 0) {
    fail("watchdog-timeout", -1);
    return;
  }
  struct watchdog_info_u info;
  memset(&info, 0, sizeof(info));
  int r_sup = ioctl(fd, WDIOC_GETSUPPORT, &info);
  int ident_ok = info.identity[0] != '\0';

  int want = 30, got = -1;
  int r_set = ioctl(fd, WDIOC_SETTIMEOUT, &want);
  int r_get = ioctl(fd, WDIOC_GETTIMEOUT, &got);
  int timeout_ok = (got == 30);

  int r_ping = ioctl(fd, WDIOC_KEEPALIVE, 0);
  int left = -1;
  int r_left = ioctl(fd, WDIOC_GETTIMELEFT, &left);
  /* Armed by the keepalive, so the remaining time must be inside the window
   * we just set — not zero, and not more than the timeout. */
  int left_ok = left > 0 && left <= 30;

  /* Magic close: 'V' then close must disarm. If it did not, the machine would
   * reset 30 seconds from now and take the whole smoke run with it — so this
   * write is load-bearing, not decoration. */
  ssize_t w = write(fd, "V", 1);
  close(fd);

  check("watchdog-timeout",
        r_sup == 0 && ident_ok && r_set == 0 && r_get == 0 && timeout_ok &&
            r_ping == 0 && r_left == 0 && left_ok && w == 1,
        (long)(ident_ok * 100000 + timeout_ok * 10000 + left * 10 + left_ok));
}

/* ═══════════════════════════════ i2c test ═══════════════════════════════ */

static void test_i2c(void) {
  int fd = open("/dev/i2c-0", O_RDWR);
  if (fd < 0) {
    /* No SMBus controller was probed, so the node must genuinely not be
     * there. Anything other than ENOENT/ENXIO means the driver registered a
     * device it cannot drive. */
    int e = errno;
    check("i2c-probe", e == ENOENT || e == ENXIO, (long)e);
    return;
  }
  unsigned long funcs = 0;
  int r = ioctl(fd, I2C_FUNCS, &funcs);
  /* An SMBus host controller can do byte-data transactions and cannot do raw
   * I2C messages; claiming the latter would make i2ctransfer fail obscurely
   * instead of saying so up front. */
  int smbus_ok = (funcs & I2C_FUNC_SMBUS_READ_BYTE_DATA) != 0;
  int no_raw_i2c = (funcs & I2C_FUNC_I2C) == 0;
  errno = 0;
  int rdwr = ioctl(fd, I2C_RDWR, 0);
  int rdwr_refused = rdwr < 0 && (errno == EOPNOTSUPP || errno == ENOTSUP ||
                                  errno == EINVAL || errno == EFAULT);
  close(fd);
  check("i2c-probe", r == 0 && smbus_ok && no_raw_i2c && rdwr_refused,
        (long)(r * 10000 + smbus_ok * 1000 + no_raw_i2c * 100 +
               rdwr_refused * 10));
}

/* ═══════════════════════════════ applets ════════════════════════════════ */

/* Run `argv` and capture up to cap-1 bytes of its stdout. Returns the exit
 * status, or -1. */
static int run_capture(const char *const argv[], char *out, size_t cap) {
  int pipefd[2];
  if (pipe(pipefd) < 0)
    return -1;
  pid_t pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    return -1;
  }
  if (pid == 0) {
    close(pipefd[0]);
    dup2(pipefd[1], 1);
    dup2(pipefd[1], 2);
    close(pipefd[1]);
    execv(argv[0], (char *const *)argv);
    _exit(127);
  }
  close(pipefd[1]);
  size_t n = 0;
  ssize_t r;
  while (n < cap - 1 && (r = read(pipefd[0], out + n, cap - 1 - n)) > 0)
    n += (size_t)r;
  out[n] = '\0';
  close(pipefd[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int have_applet(const char *path) { return access(path, X_OK) == 0; }

/* `ip addr show` must report the same address SIOCGIFADDR does. */
static void test_applet_ip(void) {
  if (!have_applet("/bin/ip")) {
    fail("applet-ip", -1);
    return;
  }
  char want[32] = "";
  int sk = socket(AF_INET, SOCK_DGRAM, 0);
  if (sk >= 0) {
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, "eth0", sizeof(ifr.ifr_name) - 1);
    if (ioctl(sk, SIOCGIFADDR, &ifr) == 0) {
      unsigned a = ntohl(((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr.s_addr);
      snprintf(want, sizeof(want), "%u.%u.%u.%u", (a >> 24) & 0xff,
               (a >> 16) & 0xff, (a >> 8) & 0xff, a & 0xff);
    }
    close(sk);
  }
  static char out[8192];
  const char *argv[] = {"/bin/ip", "addr", "show", NULL};
  int rc = run_capture(argv, out, sizeof(out));
  /* "<index>: lo: ..." — the interface list really came from RTM_GETLINK, not
   * from a bare substring that any word could satisfy. */
  int has_lo = strstr(out, "lo:") != NULL;
  int has_addr = want[0] ? (strstr(out, want) != NULL) : 1;
  check("applet-ip", rc == 0 && has_lo && has_addr,
        (long)(rc * 100 + has_lo * 10 + has_addr));
}

/* `losetup` must attach a file and then name it in its listing. */
static void test_applet_losetup(void) {
  if (!have_applet("/bin/losetup")) {
    fail("applet-losetup", -1);
    return;
  }
  int img = make_loop_image();
  if (img < 0) {
    fail("applet-losetup", -2);
    return;
  }
  close(img);

  static char out[4096];
  const char *find[] = {"/bin/losetup", "-f", NULL};
  int rc_f = run_capture(find, out, sizeof(out));
  char dev[64] = "";
  if (rc_f == 0) {
    char *nl = strchr(out, '\n');
    if (nl)
      *nl = '\0';
    snprintf(dev, sizeof(dev), "%s", out);
  }
  int attach_rc = -1, listed = 0, detach_rc = -1;
  if (dev[0] == '/') {
    const char *att[] = {"/bin/losetup", dev, LOOP_IMG, NULL};
    attach_rc = run_capture(att, out, sizeof(out));
    const char *show[] = {"/bin/losetup", "-a", NULL};
    run_capture(show, out, sizeof(out));
    listed = strstr(out, LOOP_IMG) != NULL && strstr(out, dev) != NULL;
    const char *det[] = {"/bin/losetup", "-d", dev, NULL};
    detach_rc = run_capture(det, out, sizeof(out));
  }
  unlink(LOOP_IMG);
  check("applet-losetup",
        rc_f == 0 && dev[0] == '/' && attach_rc == 0 && listed &&
            detach_rc == 0,
        (long)(rc_f * 10000 + attach_rc * 1000 + listed * 100 + detach_rc));
}

/* `hwclock` must print a date that agrees with the RTC ioctl. */
static void test_applet_hwclock(void) {
  if (!have_applet("/bin/hwclock")) {
    fail("applet-hwclock", -1);
    return;
  }
  int fd = open("/dev/rtc0", O_RDONLY);
  struct rtc_time_u t;
  memset(&t, 0, sizeof(t));
  int have_rtc = fd >= 0 && ioctl(fd, RTC_RD_TIME, &t) == 0;
  if (fd >= 0)
    close(fd);
  if (!have_rtc) {
    fail("applet-hwclock", -2);
    return;
  }
  static char out[1024];
  const char *argv[] = {"/bin/hwclock", "-r", NULL};
  int rc = run_capture(argv, out, sizeof(out));
  char year[8];
  snprintf(year, sizeof(year), "%d", t.tm_year + 1900);
  int year_ok = strstr(out, year) != NULL;
  check("applet-hwclock", rc == 0 && year_ok, (long)(rc * 10 + year_ok));
}

/* `lsof` must list a file this process is holding open, by its full path. */
static void test_applet_lsof(void) {
  if (!have_applet("/bin/lsof")) {
    fail("applet-lsof", -1);
    return;
  }
  const char *path = "/tmp/m107-lsof.txt";
  int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    fail("applet-lsof", -2);
    return;
  }
  static char out[65536];
  const char *argv[] = {"/bin/lsof", NULL};
  int rc = run_capture(argv, out, sizeof(out));
  int found = strstr(out, path) != NULL;
  close(fd);
  unlink(path);
  check("applet-lsof", rc == 0 && found, (long)(rc * 10 + found));
}

/* `chvt` must move the active VT, as seen through VT_GETSTATE. */
static void test_applet_chvt(void) {
  if (!have_applet("/bin/chvt")) {
    fail("applet-chvt", -1);
    return;
  }
  int fd = open_tty0();
  if (fd < 0) {
    fail("applet-chvt", -2);
    return;
  }
  struct vt_stat_u st;
  memset(&st, 0, sizeof(st));
  ioctl(fd, VT_GETSTATE, &st);
  int original = st.v_active;

  static char out[512];
  const char *to2[] = {"/bin/chvt", "2", NULL};
  int rc = run_capture(to2, out, sizeof(out));
  memset(&st, 0, sizeof(st));
  ioctl(fd, VT_GETSTATE, &st);
  int moved = (st.v_active == 2);

  char back[8];
  snprintf(back, sizeof(back), "%d", original);
  const char *tob[] = {"/bin/chvt", back, NULL};
  run_capture(tob, out, sizeof(out));
  memset(&st, 0, sizeof(st));
  ioctl(fd, VT_GETSTATE, &st);
  int restored = (st.v_active == original);
  close(fd);
  check("applet-chvt", rc == 0 && moved && restored,
        (long)(rc * 100 + moved * 10 + restored));
}

int main(void) {
  marker("M107-SMOKE: start");

  test_netlink_link();
  test_netlink_addr();
  test_netlink_route();
  test_netlink_route_rw();
  test_netlink_neigh();

  test_vt_state();
  test_vt_switch();
  test_vt_disallocate();
  test_vt_kdmode();
  test_console_font();
  test_console_keymap();

  test_loop();

  test_proc_fd_path();
  test_proc_maps_labels();

  test_kmsg_dev();
  test_kmsg_proc();
  test_syslog_klogctl();

  test_inotify_move();
  test_inotify_attrib();
  test_inotify_selfdel();

  test_rtc_read();
  test_rtc_alarm();
  test_watchdog();
  test_i2c();

  test_applet_ip();
  test_applet_losetup();
  test_applet_hwclock();
  test_applet_lsof();
  test_applet_chvt();

  marker(g_fail ? "M107-SMOKE: done with failures" : "M107-SMOKE: done");
  return g_fail ? 1 : 0;
}
