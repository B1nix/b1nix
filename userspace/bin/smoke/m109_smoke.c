#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

/* M109 smoke: the kernel facilities the remaining Alpine applets were blocked
 * on. Every marker is emitted only after the operation ran AND its result was
 * checked against something this test knows independently.
 *
 *   packet-socket     socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL)) opens, and
 *                     getsockname() on it reports a 20-byte sockaddr_ll naming
 *                     the interface bind() was given.
 *   packet-tx-rx      a frame written to an AF_PACKET socket comes back on a
 *                     second one, byte for byte, with the source MAC, the
 *                     ethertype and the ifindex reported in the sockaddr_ll
 *                     recvfrom() fills in — a private ethertype nothing in the
 *                     IP stack handles, so only the packet path can carry it.
 *   packet-rx-inbound the receive tap, as opposed to the transmit one: a frame
 *                     that arrived off the wire (not PACKET_OUTGOING) while
 *                     the gateway was being pinged. Skipped, with the reason
 *                     stated, on an instance with no default route.
 *   packet-filter     a socket bound to one ethertype does NOT see a frame of
 *                     another, while an ETH_P_ALL socket sees both.
 *   packet-dgram      SOCK_DGRAM strips the header on receive and builds it
 *                     from the sockaddr_ll on send.
 *   gretap-loop       a frame transmitted on a gretap tunnel whose remote is
 *                     127.0.0.1 is encapsulated in GRE over IPv4, comes back
 *                     round the loopback datapath and is delivered on the
 *                     tunnel as a *received* frame, byte for byte.
 *   vlan-tag          a frame sent on vlan10 leaves the lower interface four
 *                     bytes longer, carrying 0x8100 and VID 10, with the
 *                     original ethertype moved inside the tag.
 *   vlan-strip        that tagged frame, arriving back on the lower
 *                     interface, is matched, stripped and delivered on vlan10
 *                     as what was originally written.
 *   vlan-vid-filter   a tag for a VID no device is configured for never
 *                     surfaces on vlan10, while a correctly tagged one still
 *                     does.
 *   bridge-learn      the address a frame arrived with is in /proc/net/bridge
 *                     against the port it arrived on, and the broadcast
 *                     reached the bridge device itself.
 *   bridge-flood      a broadcast received on one port is flooded out the
 *                     other one, unchanged.
 *   bridge-fdb-forward a unicast for a learned address leaves by that port
 *                     alone - the other port never sees it.
 *   bond-active-tx    a bond transmits through its active slave and not
 *                     through the backup.
 *   bond-failover     with the active slave taken down, the same bond sends
 *                     through the other slave, and a frame received on that
 *                     slave surfaces on the bond.
 *   vnet-link-lifecycle  RTM_NEWLINK creates, a duplicate name is EEXIST,
 *                     RTM_DELLINK removes, and a physical NIC cannot be
 *                     deleted at all.
 *   pivot-root        pivot_root(2) makes a mounted filesystem "/": a file
 *                     created on it before the pivot is at / afterwards, the
 *                     old root is reachable at put_old, and /proc/mounts says
 *                     so. Then the pivot is undone the same way.
 *   pivot-root-errno  pivot_root rejects a plain directory (EINVAL), a put_old
 *                     outside new_root (EINVAL), and "/" itself (EBUSY).
 *   pid-namespace     unshare(CLONE_NEWPID) numbers a task's CHILDREN from 1,
 *                     as Linux does: the first child reports getpid()==1 and
 *                     getppid()==0 itself, its own child gets 2, and waitpid
 *                     inside the namespace reports that same 2.
 *   pid-ns-isolation  a pid from outside the namespace names nothing inside
 *                     it - kill() on it is ESRCH.
 *   pid-ns-handles    /proc/<pid>/ns/pid differs for the task inside, and is
 *                     UNCHANGED for the task that unshared.
 *   veth-pair         `ip link add veth0 type veth peer name veth1` makes two
 *                     interfaces, both in /proc/net/dev, and a duplicate name
 *                     is EEXIST.
 *   veth-carries-frame  a frame sent on one end arrives on the other as a
 *                     RECEIVED frame, byte for byte.
 *   net-namespace     unshare(CLONE_NEWNET) leaves a task with no interfaces
 *                     at all - not the NIC, not the veth pair made before it.
 *   veth-crosses-namespace  one end moved into that namespace vanishes from
 *                     this one, and a frame sent here is received there.
 *   net-ns-routes     a route added inside the namespace is in ITS
 *                     /proc/net/route and not in this one's.
 *   unlink-enoent     unlink of a name that exists on neither the filesystem
 *                     nor the VFS still fails ENOENT, while an in-memory
 *                     device node on an on-disk directory really is removed.
 *   blkid-probe       `blkid <device>` reports a canonically shaped UUID and a
 *                     filesystem type for a real disk, read through the block
 *                     layer's own superblock probe.
 *   sysfs-ident       /sys/block/<dev>/{uuid,label,fstype} agree with what
 *                     blkid reports for the same device.
 */

#include <errno.h>
#include <signal.h>
#include <sys/sysmacros.h>
#include <fcntl.h>
#include <stdarg.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <dirent.h>
#include <sys/vfs.h>
#include <sys/statvfs.h>
#include <termios.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

/* musl's <netpacket/packet.h> and <net/ethernet.h> carry these, but spelling
 * out what the kernel is being asked for keeps the test readable. */
#ifndef AF_PACKET
#define AF_PACKET 17
#endif
#ifndef ETH_P_ALL
#define ETH_P_ALL 0x0003
#endif

struct sll {
  unsigned short sll_family;
  unsigned short sll_protocol;
  int sll_ifindex;
  unsigned short sll_hatype;
  unsigned char sll_pkttype;
  unsigned char sll_halen;
  unsigned char sll_addr[8];
};

/* Two ethertypes nothing in the IP stack handles, so a frame carrying one can
 * only have reached the reader through the packet path. */
#define ETH_P_TEST_A 0x88B5
#define ETH_P_TEST_B 0x88B6

#ifndef MS_MOVE
#define MS_MOVE 8192
#endif
#ifndef TMPFS_MAGIC
#define TMPFS_MAGIC 0x01021994
#endif

#define FS_IOC_GETFLAGS 0x80086601UL
#define FS_IOC_SETFLAGS 0x40086602UL
#define FS_SYNC_FL      0x00000008
#define FS_IMMUTABLE_FL 0x00000010
#define FS_APPEND_FL    0x00000020
#define FS_NODUMP_FL    0x00000040
#define FS_NOATIME_FL   0x00000080
/* A flag the ext2 on-disk byte has no room for (FS_PROJINHERIT_FL). */
#define FS_PROJINHERIT_FL 0x20000000

/* ── FITRIM (linux/fs.h) ────────────────────────────────────────────────── */
#define FITRIM 0xC0185879UL
struct fstrim_range_u {
  unsigned long long start;
  unsigned long long len;
  unsigned long long minlen;
};

/* ── block ioctls (linux/fs.h) ──────────────────────────────────────────── */
#define BLKDISCARD       0x1277UL
#define BLKDISCARDZEROES 0x127CUL
#define BLKROTATIONAL    0x127EUL
#define BLKZEROOUT       0x127FUL

/* ── ioprio (linux/ioprio.h) ────────────────────────────────────────────── */
#define IOPRIO_WHO_PROCESS 1
#define IOPRIO_CLASS_SHIFT 13
#define IOPRIO_PRIO(cls, lvl) (((cls) << IOPRIO_CLASS_SHIFT) | (lvl))

/* ── setserial (linux/serial.h) ─────────────────────────────────────────── */
#define TIOCGSERIAL 0x541E
#define TIOCSSERIAL 0x541F
struct serial_struct_u {
  int type;
  int line;
  unsigned int port;
  int irq;
  int flags;
  int xmit_fifo_size;
  int custom_divisor;
  int baud_base;
  unsigned short close_delay;
  char io_type;
  char reserved_char;
  int hub6;
  unsigned short closing_wait;
  unsigned short closing_wait2;
  unsigned char *iomem_base;
  unsigned short iomem_reg_shift;
  unsigned int port_high;
  unsigned long iomap_base;
};

#define VBLK_DEV "/dev/vda"
/* mount(2) names a b1nix block device by its bare name, not by its /dev path. */
#define NVME_NAME "nvme0n1"
#define MNT "/mnt/ext4nvme"

static int g_fail;

static void marker(const char *s) {
  write(1, s, strlen(s));
  write(1, "\n", 1);
}

static void ok(const char *name) {
  char line[128];
  snprintf(line, sizeof(line), "M109-SMOKE: ok %s", name);
  marker(line);
}

static void fail(const char *name, long v) {
  char line[192];
  snprintf(line, sizeof(line), "M109-SMOKE: FAIL %s (%ld, errno=%d)", name, v,
           errno);
  marker(line);
  g_fail = 1;
}

/* Two spellings of "this check did not pass": one that carries a number (an
 * errno or a byte count), one that carries a sentence. */
static void failm(const char *name, const char *why) {
  marker("M109-SMOKE: FAIL ");
  marker(name);
  marker(" ");
  marker(why);
  marker("\n");
  g_fail = 1;
}

/* Diagnostic detail printed beside a failure, so the log says what was
 * compared rather than only that it did not match. */
static void note(const char *fmt, ...) {
  char line[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  char out[300];
  snprintf(out, sizeof(out), "M109-SMOKE: note %s", line);
  marker(out);
}

static void check(const char *name, int cond, long v) {
  if (cond)
    ok(name);
  else
    fail(name, v);
}

/* memmem() is a GNU extension the freestanding build does not enable; the
 * search is three lines. */
static int contains(const unsigned char *hay, size_t hlen, const char *needle) {
  size_t nlen = strlen(needle);
  if (nlen > hlen)
    return 0;
  for (size_t i = 0; i + nlen <= hlen; i++)
    if (memcmp(hay + i, needle, nlen) == 0)
      return 1;
  return 0;
}

static unsigned short hton16(unsigned short v) {
  return (unsigned short)((v << 8) | (v >> 8));
}

/* Run a command and capture its first line of stdout (NUL-terminated, newline
 * stripped). Returns the exit status, or -1 if it could not be run. */
static int run_capture(const char *cmd, char *out, size_t cap) {
  out[0] = '\0';
  FILE *f = popen(cmd, "r");
  if (!f)
    return -1;
  if (fgets(out, (int)cap, f)) {
    size_t n = strlen(out);
    while (n && (out[n - 1] == '\n' || out[n - 1] == '\r'))
      out[--n] = '\0';
  }
  int st = pclose(f);
  return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* ── AF_PACKET ── */

static int packet_open(unsigned short proto_host, int type, int ifindex) {
  int fd = socket(AF_PACKET, type, (int)hton16(proto_host));
  if (fd < 0)
    return -1;
  struct sll a;
  memset(&a, 0, sizeof(a));
  a.sll_family = AF_PACKET;
  a.sll_protocol = hton16(proto_host);
  a.sll_ifindex = ifindex;
  if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

/* The index of a real (non-loopback) interface, asked for the same way
 * BusyBox asks: SIOCGIFINDEX on a name. */
static int first_ifindex(void) {
  int s = socket(AF_INET, SOCK_DGRAM, 0);
  if (s < 0)
    return 0;
  int idx = 0;
  for (int i = 0; i < 4 && !idx; i++) {
    struct ifreq r;
    memset(&r, 0, sizeof(r));
    snprintf(r.ifr_name, sizeof(r.ifr_name), "eth%d", i);
    if (ioctl(s, SIOCGIFINDEX, &r) == 0)
      idx = r.ifr_ifindex;
  }
  close(s);
  return idx;
}

static int g_ifindex;

static void test_packet_socket(void) {
  g_ifindex = first_ifindex();
  if (!g_ifindex) {
    fail("packet-socket", -1);
    return;
  }
  int fd = packet_open(ETH_P_ALL, SOCK_RAW, g_ifindex);
  if (fd < 0) {
    fail("packet-socket", -1);
    return;
  }
  struct sll got;
  socklen_t len = sizeof(got);
  memset(&got, 0, sizeof(got));
  int rc = getsockname(fd, (struct sockaddr *)&got, &len);
  /* 20 bytes, the family, and the interface bind() named: a family that
   * reported sizeof(sockaddr_un) here is one libc will not believe. */
  check("packet-socket",
        rc == 0 && len == sizeof(struct sll) && got.sll_family == AF_PACKET &&
            got.sll_ifindex == g_ifindex && got.sll_halen == 6,
        rc == 0 ? (long)len : -1);
  close(fd);
}

/* Build a frame with our own header. dst is broadcast so nothing has to know
 * a real neighbour's address. */
static size_t build_frame(unsigned char *f, size_t cap, unsigned short proto,
                          const char *payload) {
  size_t plen = strlen(payload);
  if (14 + plen > cap)
    return 0;
  memset(f, 0xFF, 6);            /* dst: broadcast */
  memset(f + 6, 0, 6);           /* src: filled by nothing — ours to choose */
  f[6] = 0x02;                   /* locally administered, so it is obviously
                                  * a frame this test wrote */
  f[11] = 0x09;
  f[12] = (unsigned char)(proto >> 8);
  f[13] = (unsigned char)(proto & 0xFF);
  memcpy(f + 14, payload, plen);
  return 14 + plen;
}

/* Read frames until one carries `payload`, or the socket runs dry. */
static int recv_until(int fd, const char *payload, unsigned char *buf,
                      size_t cap, struct sll *from) {
  for (int tries = 0; tries < 64; tries++) {
    socklen_t alen = sizeof(*from);
    memset(from, 0, sizeof(*from));
    ssize_t n = recvfrom(fd, buf, cap, MSG_DONTWAIT, (struct sockaddr *)from,
                         &alen);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        usleep(10000);
        continue;
      }
      return -1;
    }
    if (contains(buf, (size_t)n, payload))
      return (int)n;
  }
  return -1;
}

static void test_packet_tx_rx(void) {
  if (!g_ifindex) {
    fail("packet-tx-rx", -1);
    return;
  }
  int rx = packet_open(ETH_P_TEST_A, SOCK_RAW, g_ifindex);
  int tx = packet_open(ETH_P_TEST_A, SOCK_RAW, g_ifindex);
  if (rx < 0 || tx < 0) {
    fail("packet-tx-rx", -1);
    if (rx >= 0) close(rx);
    if (tx >= 0) close(tx);
    return;
  }

  unsigned char frame[64];
  size_t flen = build_frame(frame, sizeof(frame), ETH_P_TEST_A, "M109-PKT-A");
  ssize_t sent = send(tx, frame, flen, 0);

  unsigned char buf[256];
  struct sll from;
  int got = sent == (ssize_t)flen
                ? recv_until(rx, "M109-PKT-A", buf, sizeof(buf), &from)
                : -1;

  /* The whole frame, unchanged, plus the link-layer facts the kernel is
   * supposed to report about it. A locally sent frame is PACKET_OUTGOING —
   * seeing one's own transmissions is what makes tcpdump show both
   * directions. */
  check("packet-tx-rx",
        got == (int)flen && memcmp(buf, frame, flen) == 0 &&
            from.sll_family == AF_PACKET && from.sll_ifindex == g_ifindex &&
            from.sll_protocol == hton16(ETH_P_TEST_A) &&
            from.sll_pkttype == 4 /* PACKET_OUTGOING */ &&
            memcmp(from.sll_addr, frame + 6, 6) == 0,
        got);
  close(rx);
  close(tx);
}

/* The default gateway, from /proc/net/route (little-endian hex), in dotted
 * form. Returns 0 when there is no default route. */
static int default_gateway(char *out, size_t cap) {
  FILE *f = fopen("/proc/net/route", "r");
  if (!f)
    return 0;
  char line[256];
  int found = 0;
  /* Skip the header. */
  if (fgets(line, sizeof(line), f)) {
    while (!found && fgets(line, sizeof(line), f)) {
      char iface[32], dest[32], gw[32];
      if (sscanf(line, "%31s %31s %31s", iface, dest, gw) != 3)
        continue;
      if (strcmp(dest, "00000000") != 0)
        continue;
      unsigned long v = strtoul(gw, NULL, 16);
      if (!v)
        continue;
      snprintf(out, cap, "%lu.%lu.%lu.%lu", v & 0xFF, (v >> 8) & 0xFF,
               (v >> 16) & 0xFF, (v >> 24) & 0xFF);
      found = 1;
    }
  }
  fclose(f);
  return found;
}

/* The RX tap, as opposed to the TX one the test above exercises: a frame the
 * machine did not send. Traffic is provoked by pinging the gateway in the
 * background while the socket is drained, because the queue is short and a
 * burst of our own outgoing frames would otherwise fill it. */
static void test_packet_rx_inbound(void) {
  char gw[32];
  if (!g_ifindex || !default_gateway(gw, sizeof(gw))) {
    marker("M109-SMOKE: skip packet-rx-inbound (no default route on this "
           "instance)");
    return;
  }
  int fd = packet_open(ETH_P_ALL, SOCK_RAW, g_ifindex);
  if (fd < 0) {
    fail("packet-rx-inbound", -1);
    return;
  }

  char cmd[96];
  snprintf(cmd, sizeof(cmd), "ping -c 3 -W 1 %s >/dev/null 2>&1 &", gw);
  system(cmd);

  unsigned char buf[2048];
  struct sll from;
  int inbound = 0;
  for (int tries = 0; tries < 600 && !inbound; tries++) {
    socklen_t alen = sizeof(from);
    memset(&from, 0, sizeof(from));
    ssize_t n = recvfrom(fd, buf, sizeof(buf), MSG_DONTWAIT,
                         (struct sockaddr *)&from, &alen);
    if (n < 0) {
      usleep(10000);
      continue;
    }
    /* Something that arrived, addressed to this machine or broadcast, whose
     * source is not our own MAC — i.e. it came off the wire. */
    if (n >= 14 && from.sll_pkttype != 4 /* not PACKET_OUTGOING */ &&
        from.sll_ifindex == g_ifindex)
      inbound = 1;
  }
  if (inbound)
    ok("packet-rx-inbound");
  else
    marker("M109-SMOKE: skip packet-rx-inbound (no reply from the gateway)");
  close(fd);
}

static void test_packet_filter(void) {
  if (!g_ifindex) {
    fail("packet-filter", -1);
    return;
  }
  int narrow = packet_open(ETH_P_TEST_A, SOCK_RAW, g_ifindex);
  int wide = packet_open(ETH_P_ALL, SOCK_RAW, g_ifindex);
  int tx = packet_open(ETH_P_TEST_B, SOCK_RAW, g_ifindex);
  if (narrow < 0 || wide < 0 || tx < 0) {
    fail("packet-filter", -1);
    if (narrow >= 0) close(narrow);
    if (wide >= 0) close(wide);
    if (tx >= 0) close(tx);
    return;
  }

  unsigned char frame[64];
  size_t flen = build_frame(frame, sizeof(frame), ETH_P_TEST_B, "M109-PKT-B");
  ssize_t sent = send(tx, frame, flen, 0);

  unsigned char buf[256];
  struct sll from;
  int wide_got = sent == (ssize_t)flen
                     ? recv_until(wide, "M109-PKT-B", buf, sizeof(buf), &from)
                     : -1;

  /* The bound socket must not have it: one non-blocking read is enough,
   * because the wide socket already saw the frame go past. */
  socklen_t alen = sizeof(from);
  ssize_t narrow_got =
      recvfrom(narrow, buf, sizeof(buf), MSG_DONTWAIT, (struct sockaddr *)&from,
               &alen);

  check("packet-filter",
        wide_got == (int)flen && narrow_got < 0 &&
            (errno == EAGAIN || errno == EWOULDBLOCK),
        (long)narrow_got);
  close(narrow);
  close(wide);
  close(tx);
}

static void test_packet_dgram(void) {
  if (!g_ifindex) {
    fail("packet-dgram", -1);
    return;
  }
  int rx = packet_open(ETH_P_TEST_A, SOCK_DGRAM, g_ifindex);
  int tx = socket(AF_PACKET, SOCK_DGRAM, (int)hton16(ETH_P_TEST_A));
  if (rx < 0 || tx < 0) {
    fail("packet-dgram", -1);
    if (rx >= 0) close(rx);
    if (tx >= 0) close(tx);
    return;
  }

  const char *payload = "M109-PKT-DGRAM";
  struct sll to;
  memset(&to, 0, sizeof(to));
  to.sll_family = AF_PACKET;
  to.sll_protocol = hton16(ETH_P_TEST_A);
  to.sll_ifindex = g_ifindex;
  to.sll_halen = 6;
  memset(to.sll_addr, 0xFF, 6);
  ssize_t sent = sendto(tx, payload, strlen(payload), 0,
                        (struct sockaddr *)&to, sizeof(to));

  unsigned char buf[256];
  struct sll from;
  int got = sent == (ssize_t)strlen(payload)
                ? recv_until(rx, payload, buf, sizeof(buf), &from)
                : -1;

  /* SOCK_DGRAM: the payload alone, with the header reported in the address. */
  check("packet-dgram",
        got == (int)strlen(payload) &&
            memcmp(buf, payload, strlen(payload)) == 0 &&
            from.sll_protocol == hton16(ETH_P_TEST_A) &&
            from.sll_pkttype == 4 /* PACKET_OUTGOING */,
        got);
  close(rx);
  close(tx);
}

/* ── virtual network devices: gretap, VLAN, bridge, bond ──
 *
 * All four are created the way `ip link add` creates them — rtnetlink
 * RTM_NEWLINK with NLM_F_CREATE and an IFLA_LINKINFO naming the kind — and
 * every claim below is checked by watching real frames with AF_PACKET.
 *
 * The gretap tunnel comes first because it is what gives the other three a
 * wire: with local and remote both 127.0.0.1, a frame transmitted on the
 * tunnel is encapsulated, goes round the IPv4 loopback datapath, is
 * decapsulated and arrives back as a *received* frame. That is a real receive
 * event on a virtual device, which is what a VLAN has to strip a tag from and
 * what a bridge has to learn from.
 */

#ifndef AF_NETLINK
#define AF_NETLINK 16
#endif
#define NETLINK_ROUTE_PROTO 0

#define NL_RTM_NEWLINK 16
#define NL_RTM_DELLINK 17
#define NL_F_REQUEST 0x001
#define NL_F_ACK 0x004
#define NL_F_CREATE 0x400
#define NLMSG_ERROR_TYPE 2

#define K_IFLA_IFNAME 3
#define K_IFLA_LINK 5
#define K_IFLA_MASTER 10
#define K_IFLA_LINKINFO 18
#define K_IFLA_INFO_KIND 1
#define K_IFLA_INFO_DATA 2
#define K_IFLA_VLAN_ID 1
#define K_IFLA_GRE_IKEY 4
#define K_IFLA_GRE_OKEY 5
#define K_IFLA_GRE_LOCAL 6
#define K_IFLA_GRE_REMOTE 7

#define PACKET_OUTGOING 4

struct snl {
  unsigned short nl_family;
  unsigned short nl_pad;
  unsigned int nl_pid;
  unsigned int nl_groups;
};

struct nlreq {
  unsigned char b[512];
  size_t len;
};

static int g_nl = -1;
static unsigned g_nl_seq;

static void nl_put_u32(unsigned char *p, unsigned v) {
  p[0] = (unsigned char)(v & 0xFF);
  p[1] = (unsigned char)((v >> 8) & 0xFF);
  p[2] = (unsigned char)((v >> 16) & 0xFF);
  p[3] = (unsigned char)((v >> 24) & 0xFF);
}

static unsigned nl_get_u32(const unsigned char *p) {
  return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) |
         ((unsigned)p[3] << 24);
}

static void nl_put_u16(unsigned char *p, unsigned v) {
  p[0] = (unsigned char)(v & 0xFF);
  p[1] = (unsigned char)((v >> 8) & 0xFF);
}

/* nlmsghdr + ifinfomsg, the fixed part of every link message. */
static void nlr_init(struct nlreq *r, unsigned type, unsigned flags,
                     int ifindex) {
  memset(r, 0, sizeof(*r));
  nl_put_u16(r->b + 4, type);
  nl_put_u16(r->b + 6, flags | NL_F_REQUEST | NL_F_ACK);
  nl_put_u32(r->b + 8, ++g_nl_seq);
  nl_put_u32(r->b + 12, (unsigned)getpid());
  nl_put_u32(r->b + 16 + 4, (unsigned)ifindex); /* ifi_index */
  r->len = 32;                                  /* 16 + sizeof(ifinfomsg) */
}

static void nlr_attr(struct nlreq *r, unsigned type, const void *data,
                     size_t len) {
  size_t total = 4 + len;
  if (r->len + ((total + 3) & ~(size_t)3) > sizeof(r->b))
    return;
  nl_put_u16(r->b + r->len, (unsigned)total);
  nl_put_u16(r->b + r->len + 2, type);
  if (len)
    memcpy(r->b + r->len + 4, data, len);
  r->len += (total + 3) & ~(size_t)3;
}

static void nlr_attr_u32(struct nlreq *r, unsigned type, unsigned v) {
  unsigned char buf[4];
  nl_put_u32(buf, v);
  nlr_attr(r, type, buf, 4);
}

static void nlr_attr_u16(struct nlreq *r, unsigned type, unsigned v) {
  unsigned char buf[2];
  nl_put_u16(buf, v);
  nlr_attr(r, type, buf, 2);
}

static size_t nlr_nest_begin(struct nlreq *r, unsigned type) {
  size_t off = r->len;
  nlr_attr(r, type, NULL, 0);
  return off;
}

static void nlr_nest_end(struct nlreq *r, size_t off) {
  nl_put_u16(r->b + off, (unsigned)(r->len - off));
}

static int nl_connect(void) {
  if (g_nl >= 0)
    return g_nl;
  int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE_PROTO);
  if (fd < 0)
    return -1;
  struct snl a;
  memset(&a, 0, sizeof(a));
  a.nl_family = AF_NETLINK;
  a.nl_pid = (unsigned)getpid();
  if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
    close(fd);
    return -1;
  }
  g_nl = fd;
  return fd;
}

/* Send one request and return the kernel's verdict: 0 for the ack, or the
 * negative errno the NLMSG_ERROR carried. */
static int nl_do(struct nlreq *r) {
  int fd = nl_connect();
  if (fd < 0)
    return -1;
  nl_put_u32(r->b, (unsigned)r->len);
  if (send(fd, r->b, r->len, 0) < 0)
    return -1;
  unsigned char buf[1024];
  for (int tries = 0; tries < 64; tries++) {
    ssize_t n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        usleep(5000);
        continue;
      }
      return -1;
    }
    size_t pos = 0;
    while (pos + 16 <= (size_t)n) {
      unsigned mlen = nl_get_u32(buf + pos);
      unsigned type = (unsigned)buf[pos + 4] | ((unsigned)buf[pos + 5] << 8);
      if (mlen < 16 || pos + mlen > (size_t)n)
        break;
      if (type == NLMSG_ERROR_TYPE)
        return (int)nl_get_u32(buf + pos + 16);
      pos += (mlen + 3) & ~3u;
    }
  }
  return -1;
}

static int link_add_bridge(const char *name) {
  struct nlreq r;
  nlr_init(&r, NL_RTM_NEWLINK, NL_F_CREATE, 0);
  nlr_attr(&r, K_IFLA_IFNAME, name, strlen(name) + 1);
  size_t li = nlr_nest_begin(&r, K_IFLA_LINKINFO);
  nlr_attr(&r, K_IFLA_INFO_KIND, "bridge", 7);
  nlr_nest_end(&r, li);
  return nl_do(&r);
}

static int link_add_bond(const char *name) {
  struct nlreq r;
  nlr_init(&r, NL_RTM_NEWLINK, NL_F_CREATE, 0);
  nlr_attr(&r, K_IFLA_IFNAME, name, strlen(name) + 1);
  size_t li = nlr_nest_begin(&r, K_IFLA_LINKINFO);
  nlr_attr(&r, K_IFLA_INFO_KIND, "bond", 5);
  nlr_nest_end(&r, li);
  return nl_do(&r);
}

static int link_add_vlan(const char *name, int lower_ifindex, int vid) {
  struct nlreq r;
  nlr_init(&r, NL_RTM_NEWLINK, NL_F_CREATE, 0);
  nlr_attr(&r, K_IFLA_IFNAME, name, strlen(name) + 1);
  nlr_attr_u32(&r, K_IFLA_LINK, (unsigned)lower_ifindex);
  size_t li = nlr_nest_begin(&r, K_IFLA_LINKINFO);
  nlr_attr(&r, K_IFLA_INFO_KIND, "vlan", 5);
  size_t id = nlr_nest_begin(&r, K_IFLA_INFO_DATA);
  nlr_attr_u16(&r, K_IFLA_VLAN_ID, (unsigned)vid);
  nlr_nest_end(&r, id);
  nlr_nest_end(&r, li);
  return nl_do(&r);
}

/* local/remote are given in network order, as the attribute carries them. */
static int link_add_gretap(const char *name, const unsigned char local[4],
                           const unsigned char remote[4], unsigned key) {
  struct nlreq r;
  nlr_init(&r, NL_RTM_NEWLINK, NL_F_CREATE, 0);
  nlr_attr(&r, K_IFLA_IFNAME, name, strlen(name) + 1);
  size_t li = nlr_nest_begin(&r, K_IFLA_LINKINFO);
  nlr_attr(&r, K_IFLA_INFO_KIND, "gretap", 7);
  size_t id = nlr_nest_begin(&r, K_IFLA_INFO_DATA);
  nlr_attr(&r, K_IFLA_GRE_LOCAL, local, 4);
  nlr_attr(&r, K_IFLA_GRE_REMOTE, remote, 4);
  if (key) {
    nlr_attr_u32(&r, K_IFLA_GRE_IKEY, key);
    nlr_attr_u32(&r, K_IFLA_GRE_OKEY, key);
  }
  nlr_nest_end(&r, id);
  nlr_nest_end(&r, li);
  return nl_do(&r);
}

static int link_set_master(int ifindex, int master_ifindex) {
  struct nlreq r;
  nlr_init(&r, NL_RTM_NEWLINK, 0, ifindex);
  nlr_attr_u32(&r, K_IFLA_MASTER, (unsigned)master_ifindex);
  return nl_do(&r);
}

static int link_del(int ifindex) {
  struct nlreq r;
  nlr_init(&r, NL_RTM_DELLINK, 0, ifindex);
  return nl_do(&r);
}

/* SIOCGIFINDEX by name — the same question `ip link show <name>` asks. */
static int if_index(const char *name) {
  int s = socket(AF_INET, SOCK_DGRAM, 0);
  if (s < 0)
    return 0;
  struct ifreq r;
  memset(&r, 0, sizeof(r));
  snprintf(r.ifr_name, sizeof(r.ifr_name), "%s", name);
  int idx = ioctl(s, SIOCGIFINDEX, &r) == 0 ? r.ifr_ifindex : 0;
  close(s);
  return idx;
}

/* `ifconfig <name> up|down` — how a bond is made to fail over for real. */
static int if_set_up(const char *name, int up) {
  int s = socket(AF_INET, SOCK_DGRAM, 0);
  if (s < 0)
    return -1;
  struct ifreq r;
  memset(&r, 0, sizeof(r));
  snprintf(r.ifr_name, sizeof(r.ifr_name), "%s", name);
  int rc = ioctl(s, SIOCGIFFLAGS, &r);
  if (rc == 0) {
    if (up)
      r.ifr_flags |= IFF_UP;
    else
      r.ifr_flags = (short)(r.ifr_flags & ~IFF_UP);
    rc = ioctl(s, SIOCSIFFLAGS, &r);
  }
  close(s);
  return rc;
}

/* A frame built with a chosen source and destination address, so a bridge has
 * something to learn and something to look up. */
static size_t build_frame_ex(unsigned char *f, size_t cap,
                             const unsigned char *dst,
                             const unsigned char *src, unsigned short proto,
                             const char *payload) {
  size_t plen = strlen(payload);
  if (14 + plen > cap)
    return 0;
  memcpy(f, dst, 6);
  memcpy(f + 6, src, 6);
  f[12] = (unsigned char)(proto >> 8);
  f[13] = (unsigned char)(proto & 0xFF);
  memcpy(f + 14, payload, plen);
  return 14 + plen;
}

/* Read until a frame carrying `payload` shows up with the wanted direction:
 * want_inbound != 0 insists it was *received* (anything but PACKET_OUTGOING),
 * which is what separates a real decapsulation from the transmit tap's copy
 * of what this test just sent. */
static int recv_dir(int fd, const char *payload, int want_inbound,
                    unsigned char *buf, size_t cap, struct sll *from) {
  for (int tries = 0; tries < 120; tries++) {
    socklen_t alen = sizeof(*from);
    memset(from, 0, sizeof(*from));
    ssize_t n =
        recvfrom(fd, buf, cap, MSG_DONTWAIT, (struct sockaddr *)from, &alen);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        usleep(10000);
        continue;
      }
      return -1;
    }
    if (!contains(buf, (size_t)n, payload))
      continue;
    int inbound = from->sll_pkttype != PACKET_OUTGOING;
    if (want_inbound && !inbound)
      continue;
    if (!want_inbound && inbound)
      continue;
    return (int)n;
  }
  return -1;
}

/* Drain whatever is queued, so an absence test cannot pass on a stale frame
 * and cannot fail on one either. */
static void drain(int fd) {
  unsigned char buf[2048];
  for (int i = 0; i < 64; i++) {
    ssize_t n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
    if (n < 0)
      return;
  }
}

/* Does /proc/net/bridge list `mac` learned on `port` of `bridge`? The file is
 * the kernel's own view of the forwarding database, so agreeing with it is a
 * second, independent witness to what the forwarding decisions showed. */
static int bridge_fdb_has(const char *bridge, const char *port,
                          const char *mac) {
  char cmd[256], out[256];
  snprintf(cmd, sizeof(cmd),
           "grep -c \"^%s.*%s.*%s\" /proc/net/bridge 2>/dev/null", bridge,
           port, mac);
  run_capture(cmd, out, sizeof(out));
  return out[0] && out[0] != '0';
}

static const unsigned char MAC_A[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0xaa};
static const unsigned char MAC_B[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0xbb};
static const unsigned char MAC_BCAST[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

static int g_gre_idx;

/* The tunnel: a frame sent on it comes back as a received frame, having been
 * wrapped in GRE, carried over IPv4 and unwrapped. */
static void test_gretap(void) {
  static const unsigned char lo[4] = {127, 0, 0, 1};
  int rc = link_add_gretap("gre0", lo, lo, 0x4d31);
  g_gre_idx = if_index("gre0");
  if (rc != 0 || g_gre_idx <= 0) {
    note("gretap create rc=%d idx=%d", rc, g_gre_idx);
    fail("gretap-loop", rc);
    return;
  }
  int fd = packet_open(ETH_P_ALL, SOCK_RAW, g_gre_idx);
  if (fd < 0) {
    fail("gretap-loop", -1);
    return;
  }
  unsigned char frame[128];
  size_t flen = build_frame_ex(frame, sizeof(frame), MAC_BCAST, MAC_A,
                               ETH_P_TEST_A, "M109-GRE-LOOP");
  ssize_t sent = send(fd, frame, flen, 0);
  unsigned char buf[2048];
  struct sll from;
  int got = sent == (ssize_t)flen
                ? recv_dir(fd, "M109-GRE-LOOP", 1, buf, sizeof(buf), &from)
                : -1;
  /* Byte for byte what was sent, received on the tunnel rather than merely
   * echoed by the transmit tap. */
  check("gretap-loop",
        got == (int)flen && memcmp(buf, frame, flen) == 0 &&
            from.sll_ifindex == g_gre_idx,
        got);
  close(fd);
}

static void test_vlan(void) {
  if (g_gre_idx <= 0) {
    fail("vlan-tag", -1);
    fail("vlan-strip", -1);
    fail("vlan-vid-filter", -1);
    return;
  }
  int rc = link_add_vlan("vlan10", g_gre_idx, 10);
  int vidx = if_index("vlan10");
  if (rc != 0 || vidx <= 0) {
    note("vlan create rc=%d idx=%d", rc, vidx);
    fail("vlan-tag", rc);
    fail("vlan-strip", rc);
    fail("vlan-vid-filter", rc);
    return;
  }
  int lower = packet_open(ETH_P_ALL, SOCK_RAW, g_gre_idx);
  int upper = packet_open(ETH_P_ALL, SOCK_RAW, vidx);
  if (lower < 0 || upper < 0) {
    fail("vlan-tag", -1);
    fail("vlan-strip", -1);
    fail("vlan-vid-filter", -1);
    if (lower >= 0) close(lower);
    if (upper >= 0) close(upper);
    return;
  }
  drain(lower);
  drain(upper);

  unsigned char frame[128];
  size_t flen = build_frame_ex(frame, sizeof(frame), MAC_BCAST, MAC_A,
                               ETH_P_TEST_A, "M109-VLAN-A");
  ssize_t sent = send(upper, frame, flen, 0);

  /* On the lower interface the same frame is 4 bytes longer, carries 0x8100
   * and the VID, and the original ethertype has moved inside the tag. */
  unsigned char buf[2048];
  struct sll from;
  int got = sent == (ssize_t)flen
                ? recv_dir(lower, "M109-VLAN-A", 0, buf, sizeof(buf), &from)
                : -1;
  int vid = got >= 18 ? (((buf[14] & 0x0F) << 8) | buf[15]) : -1;
  check("vlan-tag",
        got == (int)flen + 4 && buf[12] == 0x81 && buf[13] == 0x00 &&
            vid == 10 && buf[16] == (ETH_P_TEST_A >> 8) &&
            buf[17] == (ETH_P_TEST_A & 0xFF) &&
            memcmp(buf + 18, frame + 14, flen - 14) == 0,
        got);

  /* The tunnel brings the tagged frame back; the VLAN device must match the
   * VID, strip the tag and deliver exactly what was written. */
  int back = recv_dir(upper, "M109-VLAN-A", 1, buf, sizeof(buf), &from);
  check("vlan-strip",
        back == (int)flen && memcmp(buf, frame, flen) == 0 &&
            from.sll_ifindex == vidx,
        back);

  /* A tag for a VID no device is configured for is not this device's frame.
   * Written by hand onto the lower interface, it must never surface on
   * vlan10 — while a second correctly tagged frame still does, so the
   * absence above is a filter and not a dead receive path. */
  drain(upper);
  unsigned char tagged[128];
  memcpy(tagged, frame, 12);
  tagged[12] = 0x81;
  tagged[13] = 0x00;
  tagged[14] = 0x00;
  tagged[15] = 20; /* VID 20 */
  tagged[16] = (unsigned char)(ETH_P_TEST_A >> 8);
  tagged[17] = (unsigned char)(ETH_P_TEST_A & 0xFF);
  memcpy(tagged + 18, "M109-VLAN-WRONGVID", 18);
  send(lower, tagged, 36, 0);

  unsigned char frame2[128];
  size_t f2len = build_frame_ex(frame2, sizeof(frame2), MAC_BCAST, MAC_A,
                                ETH_P_TEST_A, "M109-VLAN-B");
  send(upper, frame2, f2len, 0);
  int second = recv_dir(upper, "M109-VLAN-B", 1, buf, sizeof(buf), &from);
  /* Anything still queued on vlan10 that mentions the wrong-VID payload would
   * have arrived before this one, since it was sent first. */
  int leaked = 0;
  unsigned char scan[2048];
  for (int i = 0; i < 8; i++) {
    ssize_t n = recv(upper, scan, sizeof(scan), MSG_DONTWAIT);
    if (n < 0)
      break;
    if (contains(scan, (size_t)n, "M109-VLAN-WRONGVID"))
      leaked = 1;
  }
  check("vlan-vid-filter", second > 0 && !leaked, second);

  close(lower);
  close(upper);
  if (link_del(vidx) != 0 || if_index("vlan10") != 0)
    note("vlan10 outlived its RTM_DELLINK");
}

static void test_bridge(void) {
  int eth1 = if_index("eth1");
  if (g_gre_idx <= 0 || eth1 <= 0) {
    /* Flooding needs somewhere to flood to: a second port that does not echo
     * what it is given. Stated rather than skipped silently. */
    note("bridge test needs gre0 and a second interface (gre0=%d eth1=%d)",
         g_gre_idx, eth1);
    fail("bridge-learn", -1);
    fail("bridge-flood", -1);
    fail("bridge-fdb-forward", -1);
    return;
  }
  int rc = link_add_bridge("br0");
  int bidx = if_index("br0");
  int r1 = bidx > 0 ? link_set_master(g_gre_idx, bidx) : -1;
  int r2 = bidx > 0 ? link_set_master(eth1, bidx) : -1;
  if (rc != 0 || bidx <= 0 || r1 != 0 || r2 != 0) {
    note("bridge create rc=%d idx=%d port1=%d port2=%d", rc, bidx, r1, r2);
    fail("bridge-learn", rc);
    fail("bridge-flood", r1);
    fail("bridge-fdb-forward", r2);
    return;
  }

  int p_gre = packet_open(ETH_P_ALL, SOCK_RAW, g_gre_idx);
  int p_eth = packet_open(ETH_P_ALL, SOCK_RAW, eth1);
  int p_br = packet_open(ETH_P_ALL, SOCK_RAW, bidx);
  if (p_gre < 0 || p_eth < 0 || p_br < 0) {
    fail("bridge-learn", -1);
    fail("bridge-flood", -1);
    fail("bridge-fdb-forward", -1);
    goto out;
  }
  drain(p_gre);
  drain(p_eth);
  drain(p_br);

  /* A broadcast from station A, injected on the tunnel port: it arrives on
   * gre0 for real, so the bridge learns A there, floods it to eth1 and hands
   * it up to br0. */
  unsigned char frame[128];
  size_t flen = build_frame_ex(frame, sizeof(frame), MAC_BCAST, MAC_A,
                               ETH_P_TEST_A, "M109-BR-FLOOD");
  send(p_gre, frame, flen, 0);

  unsigned char buf[2048];
  struct sll from;
  int flooded = recv_dir(p_eth, "M109-BR-FLOOD", 0, buf, sizeof(buf), &from);
  check("bridge-flood",
        flooded == (int)flen && memcmp(buf, frame, flen) == 0, flooded);

  int local = recv_dir(p_br, "M109-BR-FLOOD", 1, buf, sizeof(buf), &from);
  if (local <= 0)
    note("the bridge did not deliver the broadcast to itself");

  check("bridge-learn", bridge_fdb_has("br0", "gre0", "02:00:00:00:00:aa") &&
                            local == (int)flen,
        local);

  /* Now a unicast for A, sent by the bridge itself. A was learned on gre0, so
   * it must leave by gre0 alone — the tunnel brings it back, and eth1 never
   * sees it. */
  drain(p_eth);
  drain(p_gre);
  unsigned char uni[128];
  size_t ulen = build_frame_ex(uni, sizeof(uni), MAC_A, MAC_B, ETH_P_TEST_A,
                               "M109-BR-UNICAST");
  send(p_br, uni, ulen, 0);
  int on_gre = recv_dir(p_gre, "M109-BR-UNICAST", 1, buf, sizeof(buf), &from);
  int on_eth = recv_dir(p_eth, "M109-BR-UNICAST", 0, buf, sizeof(buf), &from);
  check("bridge-fdb-forward", on_gre == (int)ulen && on_eth < 0, on_eth);

out:
  if (p_gre >= 0) close(p_gre);
  if (p_eth >= 0) close(p_eth);
  if (p_br >= 0) close(p_br);
  /* Hand the interfaces back before anything else in the suite wants them. */
  link_set_master(g_gre_idx, 0);
  link_set_master(eth1, 0);
  if (bidx > 0)
    link_del(bidx);
}

static void test_bond(void) {
  int eth1 = if_index("eth1");
  if (g_gre_idx <= 0 || eth1 <= 0) {
    note("bond test needs two interfaces (gre0=%d eth1=%d)", g_gre_idx, eth1);
    fail("bond-active-tx", -1);
    fail("bond-failover", -1);
    return;
  }
  int rc = link_add_bond("bond0");
  int bidx = if_index("bond0");
  /* eth1 first, so it is the active slave and the bond wears its address. */
  int r1 = bidx > 0 ? link_set_master(eth1, bidx) : -1;
  int r2 = bidx > 0 ? link_set_master(g_gre_idx, bidx) : -1;
  if (rc != 0 || bidx <= 0 || r1 != 0 || r2 != 0) {
    note("bond create rc=%d idx=%d slave1=%d slave2=%d", rc, bidx, r1, r2);
    fail("bond-active-tx", rc);
    fail("bond-failover", r1);
    return;
  }

  int p_eth = packet_open(ETH_P_ALL, SOCK_RAW, eth1);
  int p_gre = packet_open(ETH_P_ALL, SOCK_RAW, g_gre_idx);
  int p_bond = packet_open(ETH_P_ALL, SOCK_RAW, bidx);
  if (p_eth < 0 || p_gre < 0 || p_bond < 0) {
    fail("bond-active-tx", -1);
    fail("bond-failover", -1);
    goto out;
  }
  drain(p_eth);
  drain(p_gre);
  drain(p_bond);

  unsigned char frame[128];
  size_t flen = build_frame_ex(frame, sizeof(frame), MAC_BCAST, MAC_B,
                               ETH_P_TEST_A, "M109-BOND-A");
  send(p_bond, frame, flen, 0);
  unsigned char buf[2048];
  struct sll from;
  int on_eth = recv_dir(p_eth, "M109-BOND-A", 0, buf, sizeof(buf), &from);
  int eth_match = on_eth == (int)flen && memcmp(buf, frame, flen) == 0;
  int on_gre = recv_dir(p_gre, "M109-BOND-A", 0, buf, sizeof(buf), &from);
  /* The active slave carries it and the backup carries nothing: that is what
   * active-backup means. */
  check("bond-active-tx", eth_match && on_gre < 0, on_eth);

  /* Take the active slave down and the bond has to move to the other one,
   * with nothing above it reconfigured. The frame goes out the tunnel, comes
   * back on it, and surfaces on the bond — which also proves a slave's
   * received frames are delivered as the master's. */
  drain(p_gre);
  drain(p_bond);
  if (if_set_up("eth1", 0) != 0)
    note("could not take eth1 down");
  size_t f2len = build_frame_ex(frame, sizeof(frame), MAC_BCAST, MAC_B,
                                ETH_P_TEST_A, "M109-BOND-B");
  send(p_bond, frame, f2len, 0);
  int backup_tx = recv_dir(p_gre, "M109-BOND-B", 0, buf, sizeof(buf), &from);
  int up_on_bond = recv_dir(p_bond, "M109-BOND-B", 1, buf, sizeof(buf), &from);
  check("bond-failover",
        backup_tx == (int)f2len && up_on_bond == (int)f2len &&
            from.sll_ifindex == bidx,
        backup_tx);
  if (if_set_up("eth1", 1) != 0)
    note("could not bring eth1 back up");

out:
  if (p_eth >= 0) close(p_eth);
  if (p_gre >= 0) close(p_gre);
  if (p_bond >= 0) close(p_bond);
  link_set_master(eth1, 0);
  link_set_master(g_gre_idx, 0);
  if (bidx > 0)
    link_del(bidx);
}

/* The devices are created and destroyed by the same rtnetlink messages `ip`
 * sends, and a deleted one is really gone. */
static void test_vnet_lifecycle(void) {
  int rc = link_add_bridge("br9");
  int idx = if_index("br9");
  int dup = link_add_bridge("br9");
  int del = idx > 0 ? link_del(idx) : -1;
  int after = if_index("br9");
  /* A name already in use is refused with EEXIST rather than quietly making a
   * second device with the same name. */
  check("vnet-link-lifecycle",
        rc == 0 && idx > 0 && dup == -EEXIST && del == 0 && after == 0,
        dup);
  if (g_gre_idx > 0) {
    /* A driver's NIC is not something a link message may delete. */
    int eth0 = if_index("eth0");
    if (eth0 > 0 && link_del(eth0) == 0)
      note("RTM_DELLINK removed a physical interface");
    link_del(g_gre_idx);
    g_gre_idx = 0;
  }
}

/* ── pivot_root ── */

/* A tmpfs mounted at `dir`, with `dir/old` inside it to receive the old root. */
static int make_pivot_target(const char *dir) {
  mkdir(dir, 0755);
  if (mount("none", dir, "tmpfs", 0, NULL) != 0)
    return -1;
  char sub[128];
  snprintf(sub, sizeof(sub), "%s/old", dir);
  if (mkdir(sub, 0755) != 0 && errno != EEXIST)
    return -1;
  return 0;
}

static int file_exists(const char *p) {
  struct stat st;
  return stat(p, &st) == 0;
}

static void test_pivot_root(void) {
  const char *newroot = "/tmp/m109newroot";
  if (make_pivot_target(newroot) != 0) {
    fail("pivot-root", -1);
    return;
  }

  /* A file that exists only on the new root, so "am I on it" is answerable
   * without trusting any mount bookkeeping. */
  char witness[160];
  snprintf(witness, sizeof(witness), "%s/M109-ON-NEW-ROOT", newroot);
  int wfd = open(witness, O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (wfd < 0) {
    fail("pivot-root", -1);
    return;
  }
  close(wfd);

  char putold[160];
  snprintf(putold, sizeof(putold), "%s/old", newroot);

  if (syscall(SYS_pivot_root, newroot, putold) != 0) {
    fail("pivot-root", -1);
    umount(newroot);
    return;
  }

  /* On the new root: the witness is at "/", and the old root — which carries
   * /tmp — is where it was parked. Nothing else may run here: /proc and /bin
   * live on the old root and are now under /old, which is exactly the point. */
  int on_new = file_exists("/M109-ON-NEW-ROOT");
  int old_there = file_exists("/old/tmp");
  int old_proc = file_exists("/old/proc/mounts");

  /* Undo it: the old root goes back to "/" the same way it left. */
  int back = syscall(SYS_pivot_root, "/old", "/old/tmp/m109newroot") == 0;
  int restored = back && file_exists("/tmp/m109newroot/M109-ON-NEW-ROOT") &&
                 file_exists("/proc/mounts");

  /* The mount table has to have moved with the tree, not just the lookups:
   * a stale target string is what umount() would match on. */
  int mounts_ok = 0;
  FILE *mf = fopen("/proc/mounts", "r");
  if (mf) {
    char line[256];
    while (fgets(line, sizeof(line), mf))
      if (strstr(line, " /tmp/m109newroot ") && strstr(line, "tmpfs"))
        mounts_ok = 1;
    fclose(mf);
  }

  check("pivot-root",
        on_new && old_there && old_proc && back && restored && mounts_ok,
        (long)(on_new | (old_there << 1) | (old_proc << 2) | (back << 3) |
               (restored << 4) | (mounts_ok << 5)));

  unlink("/tmp/m109newroot/M109-ON-NEW-ROOT");
  umount("/tmp/m109newroot");
  rmdir("/tmp/m109newroot");
}

static void test_pivot_root_errno(void) {
  mkdir("/tmp/m109plain", 0755);
  mkdir("/tmp/m109plain/old", 0755);

  /* Not a mount point. */
  errno = 0;
  long a = syscall(SYS_pivot_root, "/tmp/m109plain", "/tmp/m109plain/old");
  int a_ok = a < 0 && errno == EINVAL;

  /* put_old outside new_root. */
  errno = 0;
  long b = syscall(SYS_pivot_root, "/tmp/m109plain", "/tmp");
  int b_ok = b < 0 && errno == EINVAL;

  /* new_root is already the root. */
  errno = 0;
  long c = syscall(SYS_pivot_root, "/", "/tmp");
  int c_ok = c < 0 && errno == EBUSY;

  check("pivot-root-errno", a_ok && b_ok && c_ok,
        (long)(a_ok | (b_ok << 1) | (c_ok << 2)));

  rmdir("/tmp/m109plain/old");
  rmdir("/tmp/m109plain");
}

/* ── findfs / volume identity ── */

/* blkid's line for one device: "/dev/sda: UUID="…" LABEL="…" TYPE="ext4"".
 * Pulls out one quoted field. */
static int blkid_field(const char *dev, const char *key, char *out,
                       size_t cap) {
  char cmd[192];
  char line[512];
  out[0] = '\0';
  snprintf(cmd, sizeof(cmd), "blkid %s 2>/dev/null", dev);
  if (run_capture(cmd, line, sizeof(line)) != 0 || !line[0])
    return -1;
  char pat[32];
  snprintf(pat, sizeof(pat), "%s=\"", key);
  char *p = strstr(line, pat);
  if (!p)
    return -1;
  p += strlen(pat);
  char *end = strchr(p, '"');
  if (!end || (size_t)(end - p) >= cap)
    return -1;
  memcpy(out, p, (size_t)(end - p));
  out[end - p] = '\0';
  return 0;
}

/* The first /dev/sd* or /dev/vd* blkid can identify. */
static int pick_device(char *dev, size_t cap) {
  static const char *const names[] = {"/dev/sda1", "/dev/sda", "/dev/vda1",
                                      "/dev/vda", "/dev/nvme0n1"};
  char uuid[64];
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    if (blkid_field(names[i], "UUID", uuid, sizeof(uuid)) == 0 && uuid[0]) {
      snprintf(dev, cap, "%s", names[i]);
      return 0;
    }
  }
  return -1;
}

static char g_dev[64];

/* `blkid <device>` names the same volume the kernel's own probe does. This is
 * the half of "UUID and label probing" that is finished: one implementation in
 * the block layer, reached both by userspace (through the raw device) and by
 * root=UUID=/root=LABEL= at boot.
 *
 * `findfs UUID=…` is NOT checked here, and deliberately so. BusyBox resolves a
 * UUID by scanning /dev, and after the root switch /dev enumerates only the
 * four entries the root image itself carries: the block-device nodes are
 * attached to that directory in memory, but the directory's readdir comes from
 * the underlying filesystem and reports only what is on disk. Nothing about
 * UUID probing is missing — the scan finds no devices to probe. That gap is
 * its own roadmap item; asserting a pass here would claim otherwise. */
static void test_blkid_probe(void) {
  if (pick_device(g_dev, sizeof(g_dev)) != 0) {
    fail("blkid-probe", -1);
    return;
  }
  char uuid[64], type[64];
  int ok_uuid = blkid_field(g_dev, "UUID", uuid, sizeof(uuid)) == 0;
  int ok_type = blkid_field(g_dev, "TYPE", type, sizeof(type)) == 0;
  /* A UUID is 36 characters in the canonical 8-4-4-4-12 shape; anything else
   * means the field was read from the wrong offset. */
  int shaped = ok_uuid && strlen(uuid) == 36 && uuid[8] == '-' &&
               uuid[13] == '-' && uuid[18] == '-' && uuid[23] == '-';
  if (!(shaped && ok_type))
    note("blkid dev=%s uuid='%s' type='%s'", g_dev, uuid, type);
  check("blkid-probe", shaped && ok_type, (long)strlen(uuid));
}

static void test_sysfs_ident(void) {
  if (!g_dev[0]) {
    fail("sysfs-ident", -1);
    return;
  }
  const char *base = strrchr(g_dev, '/');
  base = base ? base + 1 : g_dev;

  char uuid_blkid[64], type_blkid[64];
  if (blkid_field(g_dev, "UUID", uuid_blkid, sizeof(uuid_blkid)) != 0 ||
      blkid_field(g_dev, "TYPE", type_blkid, sizeof(type_blkid)) != 0) {
    fail("sysfs-ident", -1);
    return;
  }

  /* The disk directory holds the partition's, so try both shapes. */
  char cmd[256], uuid_sys[128], type_sys[128];
  snprintf(cmd, sizeof(cmd),
           "cat /sys/block/%s/uuid /sys/block/*/%s/uuid 2>/dev/null | head -1",
           base, base);
  run_capture(cmd, uuid_sys, sizeof(uuid_sys));
  snprintf(
      cmd, sizeof(cmd),
      "cat /sys/block/%s/fstype /sys/block/*/%s/fstype 2>/dev/null | head -1",
      base, base);
  run_capture(cmd, type_sys, sizeof(type_sys));

  int same = strcmp(uuid_sys, uuid_blkid) == 0 && strcmp(type_sys, type_blkid) == 0;
  if (!same)
    note("sysfs dev=%s uuid sys='%s' blkid='%s' type sys='%s' blkid='%s'",
         g_dev, uuid_sys, uuid_blkid, type_sys, type_blkid);
  check("sysfs-ident", same, (long)strlen(uuid_sys));
}

/* The namespace half of M109: UTS and mount namespaces, unshare(2),
 * setns(2) and /proc/<pid>/ns. Each effect is checked from OUTSIDE the
 * namespace that made it. */
static int slurp(const char *path, char *buf, size_t len) {
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return -1;
  ssize_t n = read(fd, buf, len - 1);
  close(fd);
  if (n < 0)
    return -1;
  buf[n] = '\0';
  return (int)n;
}

static void chomp(char *s) {
  size_t n = strlen(s);
  while (n && (s[n - 1] == '\n' || s[n - 1] == '\r'))
    s[--n] = '\0';
}

#define NEW_HOST "m109ns"
#define MNT_DIR "/tmp/m109mnt"
#define MNT_FILE MNT_DIR "/inside"

/* ── UTS namespace, its /proc handles, and setns(2) ───────────────────────
 * One child carries all three: it has to stay alive while the parent inspects
 * its handles and joins its namespace. */
static void test_uts(void) {
  char before[128] = {0};
  if (gethostname(before, sizeof(before) - 1) != 0) {
    failm("uts-namespace", "gethostname");
    return;
  }

  int rep[2], go[2];
  if (pipe(rep) != 0 || pipe(go) != 0) {
    failm("uts-namespace", "pipe");
    return;
  }

  pid_t pid = fork();
  if (pid < 0) {
    failm("uts-namespace", "fork");
    return;
  }
  if (pid == 0) {
    close(rep[0]);
    close(go[1]);
    char msg[160];
    if (unshare(CLONE_NEWUTS) != 0) {
      snprintf(msg, sizeof(msg), "E unshare %d", errno);
    } else if (sethostname(NEW_HOST, strlen(NEW_HOST)) != 0) {
      snprintf(msg, sizeof(msg), "E sethostname %d", errno);
    } else {
      char mine[128] = {0};
      if (gethostname(mine, sizeof(mine) - 1) != 0)
        snprintf(msg, sizeof(msg), "E gethostname %d", errno);
      else
        snprintf(msg, sizeof(msg), "H %s", mine);
    }
    write(rep[1], msg, strlen(msg) + 1);
    close(rep[1]);
    /* Stay in this namespace until the parent is done looking at it. */
    char c;
    read(go[0], &c, 1);
    _exit(0);
  }

  close(rep[1]);
  close(go[0]);

  char msg[160] = {0};
  ssize_t n = read(rep[0], msg, sizeof(msg) - 1);
  close(rep[0]);
  if (n <= 0 || msg[0] != 'H') {
    failm("uts-namespace", n > 0 ? msg : "no report");
  } else if (strcmp(msg + 2, NEW_HOST) != 0) {
    failm("uts-namespace", "child hostname wrong");
  } else {
    char after[128] = {0};
    if (gethostname(after, sizeof(after) - 1) != 0)
      failm("uts-namespace", "gethostname after");
    else if (strcmp(after, before) != 0)
      failm("uts-namespace", "parent hostname changed");
    else
      ok("uts-namespace");
  }

  /* ── the /proc handles ── */
  char p[64], mine_uts[128] = {0}, kid_uts[128] = {0};
  char mine_mnt[128] = {0}, kid_mnt[128] = {0};
  snprintf(p, sizeof(p), "/proc/%d/ns/uts", (int)pid);
  int have = (slurp("/proc/self/ns/uts", mine_uts, sizeof(mine_uts)) > 0 &&
              slurp(p, kid_uts, sizeof(kid_uts)) > 0);
  snprintf(p, sizeof(p), "/proc/%d/ns/mnt", (int)pid);
  have = have && (slurp("/proc/self/ns/mnt", mine_mnt, sizeof(mine_mnt)) > 0 &&
                  slurp(p, kid_mnt, sizeof(kid_mnt)) > 0);
  chomp(mine_uts);
  chomp(kid_uts);
  chomp(mine_mnt);
  chomp(kid_mnt);
  if (!have)
    failm("ns-handles", "cannot read /proc/<pid>/ns");
  else if (strncmp(kid_uts, "uts:[", 5) != 0)
    failm("ns-handles", kid_uts);
  else if (strcmp(mine_uts, kid_uts) == 0)
    failm("ns-handles", kid_uts);
  else if (strcmp(mine_mnt, kid_mnt) != 0)
    failm("ns-handles", "mnt handles differ without unshare");
  else
    ok("ns-handles");

  /* ── setns(2) into the child's UTS namespace and back ── */
  int own = open("/proc/self/ns/uts", O_RDONLY);
  snprintf(p, sizeof(p), "/proc/%d/ns/uts", (int)pid);
  int kid = open(p, O_RDONLY);
  if (own < 0 || kid < 0) {
    failm("setns-uts", "open ns handle");
  } else if (setns(kid, CLONE_NEWUTS) != 0) {
    failm("setns-uts", "setns into child");
  } else {
    char joined[128] = {0};
    int bad = (gethostname(joined, sizeof(joined) - 1) != 0) ||
              strcmp(joined, NEW_HOST) != 0;
    /* Back to our own namespace through our own handle — a setns that could
     * only go one way would not be a namespace, it would be a hostname
     * change. */
    int back = setns(own, CLONE_NEWUTS);
    char again[128] = {0};
    gethostname(again, sizeof(again) - 1);
    if (bad)
      failm("setns-uts", "hostname in joined namespace wrong");
    else if (back != 0)
      failm("setns-uts", "setns back");
    else if (strcmp(again, before) != 0)
      failm("setns-uts", "did not return to the original namespace");
    else
      ok("setns-uts");
  }
  if (own >= 0)
    close(own);
  if (kid >= 0)
    close(kid);

  write(go[1], "g", 1);
  close(go[1]);
  int status = 0;
  waitpid(pid, &status, 0);
}

/* ── mount namespace ─────────────────────────────────────────────────────── */
static int mounts_mention(const char *needle) {
  char buf[8192];
  if (slurp("/proc/mounts", buf, sizeof(buf)) <= 0)
    return -1;
  return strstr(buf, needle) != NULL;
}

static void test_mount_ns(void) {
  mkdir(MNT_DIR, 0755);

  int rep[2], go[2];
  if (pipe(rep) != 0 || pipe(go) != 0) {
    failm("mount-namespace", "pipe");
    return;
  }

  pid_t pid = fork();
  if (pid < 0) {
    failm("mount-namespace", "fork");
    return;
  }
  if (pid == 0) {
    close(rep[0]);
    close(go[1]);
    char msg[160];
    int mounted = 0;
    if (unshare(CLONE_NEWNS) != 0) {
      snprintf(msg, sizeof(msg), "E unshare %d", errno);
    } else if (mount("tmpfs", MNT_DIR, "tmpfs", 0, NULL) != 0) {
      snprintf(msg, sizeof(msg), "E mount %d", errno);
    } else {
      mounted = 1;
      int fd = open(MNT_FILE, O_CREAT | O_WRONLY, 0644);
      if (fd < 0) {
        snprintf(msg, sizeof(msg), "E create %d", errno);
      } else {
        write(fd, "m109", 4);
        close(fd);
        if (mounts_mention(MNT_DIR) != 1)
          snprintf(msg, sizeof(msg), "E own-mounts-missing");
        else
          snprintf(msg, sizeof(msg), "M ok");
      }
    }
    write(rep[1], msg, strlen(msg) + 1);
    close(rep[1]);
    char c;
    read(go[0], &c, 1);
    if (mounted)
      umount(MNT_DIR);
    _exit(0);
  }

  close(rep[1]);
  close(go[0]);

  char msg[160] = {0};
  ssize_t n = read(rep[0], msg, sizeof(msg) - 1);
  close(rep[0]);
  if (n <= 0 || msg[0] != 'M') {
    failm("mount-namespace", n > 0 ? msg : "no report");
  } else {
    int seen = mounts_mention(MNT_DIR);
    int fd = open(MNT_FILE, O_RDONLY);
    if (fd >= 0) {
      close(fd);
      failm("mount-namespace", "child's file visible outside its namespace");
    } else if (seen != 0) {
      failm("mount-namespace", "child's mount listed in the parent's /proc/mounts");
    } else {
      ok("mount-namespace");
    }
  }

  write(go[1], "g", 1);
  close(go[1]);
  int status = 0;
  waitpid(pid, &status, 0);
  rmdir(MNT_DIR);
}

/* ── the kinds b1nix has only one of ─────────────────────────────────────── */
/* ── PID namespace ───────────────────────────────────────────────────────
 *
 * unshare(CLONE_NEWPID) is done in a forked child, never in this process: it
 * chooses the namespace the CALLER'S CHILDREN are born into, so doing it here
 * would drop every later test's fork into a private numbering.
 *
 * Every number below is reported by the process it belongs to, out of its own
 * getpid()/getppid()/fork()/waitpid() — this process never tells a child what
 * its pid ought to be. */
static void test_pid_ns(void) {
  int inner[2], outer[2];
  if (pipe(inner) != 0 || pipe(outer) != 0) {
    failm("pid-namespace", "pipe");
    return;
  }

  pid_t top = fork();
  if (top < 0) {
    failm("pid-namespace", "fork");
    return;
  }
  if (top == 0) {
    close(inner[0]);
    close(outer[0]);
    char msg[256];
    if (unshare(CLONE_NEWPID) != 0) {
      snprintf(msg, sizeof(msg), "E unshare %d", errno);
      write(outer[1], msg, strlen(msg) + 1);
      _exit(0);
    }
    /* Linux does not move the caller: this process keeps the number it had. */
    pid_t mine = getpid();
    char myns[64] = {0};
    slurp("/proc/self/ns/pid", myns, sizeof(myns));
    chomp(myns);

    pid_t kid = fork();
    if (kid == 0) {
      /* First task in the new namespace: it must be pid 1, and its parent
       * lives outside, so it has no number here. */
      close(inner[0]);
      char m[256];
      pid_t p = getpid();
      pid_t pp = getppid();
      char kidns[64] = {0};
      slurp("/proc/self/ns/pid", kidns, sizeof(kidns));
      chomp(kidns);

      pid_t g = fork();
      if (g == 0)
        _exit(7);
      int st = 0;
      pid_t w = g > 0 ? waitpid(g, &st, 0) : -1;

      /* A pid from outside cannot be named from inside. `mine` is the number
       * the OUTER process reported for itself, which this namespace has never
       * heard of. */
      errno = 0;
      int outside = kill((int)mine, 0);
      int oerr = errno;
      snprintf(m, sizeof(m), "C %d %d %d %d %d %d %d %s", (int)p, (int)pp,
               (int)g, (int)w, outside, oerr,
               (g > 0 && w == g) ? (WIFEXITED(st) ? WEXITSTATUS(st) : -1) : -1,
               kidns);
      write(inner[1], m, strlen(m) + 1);
      close(inner[1]);
      _exit(0);
    }
    int st = 0;
    if (kid > 0)
      waitpid(kid, &st, 0);
    snprintf(msg, sizeof(msg), "O %d %d %s", (int)mine, (int)kid, myns);
    write(outer[1], msg, strlen(msg) + 1);
    close(outer[1]);
    _exit(0);
  }

  close(inner[1]);
  close(outer[1]);
  char cbuf[256] = {0}, obuf[256] = {0};
  ssize_t cn = read(inner[0], cbuf, sizeof(cbuf) - 1);
  ssize_t on = read(outer[0], obuf, sizeof(obuf) - 1);
  close(inner[0]);
  close(outer[0]);
  int wst = 0;
  waitpid(top, &wst, 0);

  if (on <= 0 || obuf[0] != 'O') {
    failm("pid-namespace", on > 0 ? obuf : "no report from the unsharing task");
    return;
  }
  int outer_pid = 0, outer_kid = 0;
  char outer_ns[64] = {0};
  if (sscanf(obuf + 2, "%d %d %63s", &outer_pid, &outer_kid, outer_ns) != 3) {
    failm("pid-namespace", "unparsable outer report");
    return;
  }
  if (cn <= 0 || cbuf[0] != 'C') {
    failm("pid-namespace", cn > 0 ? cbuf : "no report from inside the namespace");
    return;
  }
  int p = 0, pp = 0, g = 0, w = 0, outside = 0, oerr = 0, gstatus = 0;
  char kid_ns[64] = {0};
  if (sscanf(cbuf + 2, "%d %d %d %d %d %d %d %63s", &p, &pp, &g, &w, &outside,
             &oerr, &gstatus, kid_ns) != 8) {
    failm("pid-namespace", "unparsable inner report");
    return;
  }

  /* The numbering itself. */
  if (p != 1 || pp != 0 || g != 2 || w != 2 || gstatus != 7 || outer_kid <= 2)
    note("pid-namespace: pid=%d ppid=%d child=%d waited=%d status=%d "
         "outer-sees=%d",
         p, pp, g, w, gstatus, outer_kid);
  check("pid-namespace",
        p == 1 && pp == 0 && g == 2 && w == 2 && gstatus == 7 &&
            outer_kid > 2 && outer_kid != 1,
        (long)p);

  /* The isolation: a number from outside names nothing inside. */
  if (outer_pid <= 2)
    note("pid-ns-isolation: the outer pid was %d — too small to distinguish",
         outer_pid);
  check("pid-ns-isolation",
        outer_pid > 2 && outside == -1 && oerr == ESRCH, (long)oerr);

  /* The handles: the task inside is in a different pid namespace, while the
   * one that unshared is still in ours — that is what "affects children only"
   * means, and it is visible in /proc. */
  char mine_ns[64] = {0};
  slurp("/proc/self/ns/pid", mine_ns, sizeof(mine_ns));
  chomp(mine_ns);
  check("pid-ns-handles",
        strncmp(kid_ns, "pid:[", 5) == 0 && strcmp(kid_ns, mine_ns) != 0 &&
            strcmp(outer_ns, mine_ns) == 0,
        0);
}

/* ── veth pairs and the network namespace ─────────────────────────────────
 *
 * The two go together on purpose: a network namespace with no interface can
 * prove only absences, and a veth pair whose ends are in the same namespace
 * proves only that a frame can be copied. Together they show the one thing
 * that matters — the boundary is real, and the cable is the only way over it.
 */
#define K_IFLA_NET_NS_PID 19
#define K_VETH_INFO_PEER 1

static int link_add_veth(const char *name, const char *peer) {
  struct nlreq r;
  nlr_init(&r, NL_RTM_NEWLINK, NL_F_CREATE, 0);
  nlr_attr(&r, K_IFLA_IFNAME, name, strlen(name) + 1);
  size_t li = nlr_nest_begin(&r, K_IFLA_LINKINFO);
  nlr_attr(&r, K_IFLA_INFO_KIND, "veth", 5);
  size_t id = nlr_nest_begin(&r, K_IFLA_INFO_DATA);
  size_t pe = nlr_nest_begin(&r, K_VETH_INFO_PEER);
  /* VETH_INFO_PEER carries a whole ifinfomsg before its attributes, exactly
   * as iproute2 builds it. */
  unsigned char zero_ifi[16];
  memset(zero_ifi, 0, sizeof(zero_ifi));
  if (r.len + sizeof(zero_ifi) <= sizeof(r.b)) {
    memcpy(r.b + r.len, zero_ifi, sizeof(zero_ifi));
    r.len += sizeof(zero_ifi);
  }
  nlr_attr(&r, K_IFLA_IFNAME, peer, strlen(peer) + 1);
  nlr_nest_end(&r, pe);
  nlr_nest_end(&r, id);
  nlr_nest_end(&r, li);
  return nl_do(&r);
}

/* `ip link set <dev> netns <pid>`. */
static int link_set_netns_pid(int ifindex, pid_t pid) {
  struct nlreq r;
  nlr_init(&r, NL_RTM_NEWLINK, 0, ifindex);
  nlr_attr_u32(&r, K_IFLA_NET_NS_PID, (unsigned)pid);
  return nl_do(&r);
}

/* Does /proc/net/dev list this interface? The kernel's own listing, so it is
 * a second witness beside SIOCGIFINDEX. */
static int procnetdev_has(const char *name) {
  char buf[4096];
  if (slurp("/proc/net/dev", buf, sizeof(buf)) <= 0)
    return 0;
  char needle[24];
  snprintf(needle, sizeof(needle), "%s:", name);
  return strstr(buf, needle) != NULL;
}

static void test_veth_pair(void) {
  int rc = link_add_veth("veth0", "veth1");
  if (rc != 0) {
    fail("veth-pair", rc);
    return;
  }
  int a = if_index("veth0");
  int b = if_index("veth1");
  /* A duplicate name must be refused, and refused by name — not by silently
   * making a third device. */
  int dup = link_add_veth("veth0", "vethX");
  check("veth-pair",
        a > 0 && b > 0 && a != b && procnetdev_has("veth0") &&
            procnetdev_has("veth1") && dup == -EEXIST,
        (long)dup);

  /* A frame put on one end arrives on the other as a RECEIVED frame — not as
   * the transmit tap's copy of what was sent. */
  int rx = packet_open(ETH_P_TEST_A, SOCK_RAW, b);
  int tx = packet_open(ETH_P_TEST_A, SOCK_RAW, a);
  if (rx < 0 || tx < 0) {
    failm("veth-carries-frame", "packet socket");
  } else {
    unsigned char frame[64];
    size_t flen =
        build_frame_ex(frame, sizeof(frame), MAC_B, MAC_A, ETH_P_TEST_A,
                       "M109-VETH-X");
    ssize_t sent = send(tx, frame, flen, 0);
    unsigned char buf[256];
    struct sll from;
    int got = sent == (ssize_t)flen
                  ? recv_dir(rx, "M109-VETH-X", 1, buf, sizeof(buf), &from)
                  : -1;
    check("veth-carries-frame",
          got == (int)flen && memcmp(buf, frame, flen) == 0 &&
              from.sll_ifindex == b,
          got);
  }
  if (rx >= 0)
    close(rx);
  if (tx >= 0)
    close(tx);
}

static void test_net_ns_inner(void) {
  int rep[2], go[2];
  if (pipe(rep) != 0 || pipe(go) != 0) {
    failm("net-namespace", "pipe");
    return;
  }
  int host_eth = if_index("eth0");
  int veth1_idx = if_index("veth1");
  if (veth1_idx <= 0) {
    failm("net-namespace", "no veth1 to move (veth-pair must run first)");
    close(rep[0]); close(rep[1]); close(go[0]); close(go[1]);
    return;
  }

  pid_t pid = fork();
  if (pid < 0) {
    failm("net-namespace", "fork");
    return;
  }
  if (pid == 0) {
    close(rep[0]);
    close(go[1]);
    char msg[256];
    if (unshare(CLONE_NEWNET) != 0) {
      snprintf(msg, sizeof(msg), "E unshare %d", errno);
      write(rep[1], msg, strlen(msg) + 1);
      _exit(0);
    }
    /* A fresh namespace has no interfaces at all — not the NIC, not the veth
     * pair that was created before the unshare. */
    int sees_eth = if_index("eth0");
    int sees_veth0 = if_index("veth0");
    int sees_veth1 = if_index("veth1");
    snprintf(msg, sizeof(msg), "A %d %d %d", sees_eth, sees_veth0, sees_veth1);
    write(rep[1], msg, strlen(msg) + 1);

    /* Wait for the parent to hand veth1 over. */
    char c;
    if (read(go[0], &c, 1) != 1)
      _exit(0);

    int moved = if_index("veth1");
    int rx = moved > 0 ? packet_open(ETH_P_TEST_B, SOCK_RAW, moved) : -1;
    /* A route added here must be ours alone. */
    int radd = system("ip route add 192.0.2.0/24 dev veth1 "
                      ">/tmp/m109-ip.out 2>&1");
    char iperr[96] = {0};
    slurp("/tmp/m109-ip.out", iperr, sizeof(iperr));
    for (char *q = iperr; *q; q++)
      if (*q == '\n' || *q == ' ')
        *q = '_';
    char rbuf[4096] = {0};
    slurp("/proc/net/route", rbuf, sizeof(rbuf));
    /* 192.0.2.0 is host-order 0xC0000200; /proc/net/route prints it least
     * significant byte first, i.e. "000200C0". */
    unlink("/tmp/m109-ip.out");
    int route_here = strstr(rbuf, "000200C0") != NULL;
    int nroutes = 0;
    for (const char *q = rbuf; *q; q++)
      if (*q == '\n')
        nroutes++;
    snprintf(msg, sizeof(msg), "B %d %d %d %d %d %s", moved, rx >= 0, radd,
             route_here, nroutes, iperr[0] ? iperr : "-");
    write(rep[1], msg, strlen(msg) + 1);

    /* Listen for what the parent sends on the other end. No second handshake:
     * recv_dir polls for over a second, and the parent sends the moment it has
     * read the report above. */
    int got = -1;
    if (rx >= 0) {
      unsigned char buf[256];
      struct sll from;
      got = recv_dir(rx, "M109-NETNS-X", 1, buf, sizeof(buf), &from);
      close(rx);
    }
    snprintf(msg, sizeof(msg), "R %d", got);
    write(rep[1], msg, strlen(msg) + 1);
    close(rep[1]);
    _exit(0);
  }

  close(rep[1]);
  close(go[0]);

  char m[256] = {0};
  ssize_t n = read(rep[0], m, sizeof(m) - 1);
  int sees_eth = -1, sees_v0 = -1, sees_v1 = -1;
  if (n <= 0 || m[0] != 'A' ||
      sscanf(m + 2, "%d %d %d", &sees_eth, &sees_v0, &sees_v1) != 3) {
    failm("net-namespace", n > 0 ? m : "no report");
    close(rep[0]);
    close(go[1]);
    waitpid(pid, NULL, 0);
    return;
  }
  check("net-namespace",
        host_eth > 0 && sees_eth == 0 && sees_v0 == 0 && sees_v1 == 0, 0);

  /* Hand one end of the cable over and require the parent to lose sight of it
   * in the same move — an interface is in exactly one namespace. */
  int mv = link_set_netns_pid(veth1_idx, pid);
  int parent_still_sees = if_index("veth1");
  write(go[1], "g", 1);

  memset(m, 0, sizeof(m));
  n = read(rep[0], m, sizeof(m) - 1);
  int moved = -1, rxok = -1, radd = -1, route_here = -1, nroutes = -1;
  char iperr[96] = {0};
  if (n <= 0 || m[0] != 'B' ||
      sscanf(m + 2, "%d %d %d %d %d %95s", &moved, &rxok, &radd, &route_here,
             &nroutes, iperr) != 6) {
    failm("veth-crosses-namespace", n > 0 ? m : "no report after the move");
    close(rep[0]);
    close(go[1]);
    waitpid(pid, NULL, 0);
    return;
  }

  /* The frame: sent here on veth0, received there on veth1, in the other
   * namespace, by a packet socket that cannot see this one's interfaces. */
  int tx = packet_open(ETH_P_TEST_B, SOCK_RAW, if_index("veth0"));
  unsigned char frame[64];
  size_t flen = build_frame_ex(frame, sizeof(frame), MAC_B, MAC_A,
                               ETH_P_TEST_B, "M109-NETNS-X");
  ssize_t sent = tx >= 0 ? send(tx, frame, flen, 0) : -1;
  if (tx >= 0)
    close(tx);

  memset(m, 0, sizeof(m));
  n = read(rep[0], m, sizeof(m) - 1);
  int got = -1;
  if (n > 0 && m[0] == 'R')
    sscanf(m + 2, "%d", &got);

  if (mv != 0 || moved <= 0 || parent_still_sees != 0 || rxok != 1 ||
      sent != (ssize_t)flen || got != (int)flen)
    note("veth-crosses-namespace: mv=%d moved=%d parent_sees=%d rx=%d sent=%ld "
         "got=%d want=%ld",
         mv, moved, parent_still_sees, rxok, (long)sent, got, (long)flen);
  check("veth-crosses-namespace",
        mv == 0 && moved > 0 && parent_still_sees == 0 && rxok == 1 &&
            sent == (ssize_t)flen && got == (int)flen,
        (long)got);

  /* The route the child added is the child's. This process shares no
   * forwarding entry with it. */
  char rbuf[8192] = {0};
  slurp("/proc/net/route", rbuf, sizeof(rbuf));
  if (radd != 0 || route_here != 1 || strstr(rbuf, "000200C0") != NULL)
    note("net-ns-routes: ip-exit=%d in-ns=%d leaked-out=%d ns-routes=%d ip=%s",
         radd, route_here, strstr(rbuf, "000200C0") != NULL, nroutes, iperr);
  check("net-ns-routes",
        radd == 0 && route_here == 1 && strstr(rbuf, "000200C0") == NULL, 0);

  close(rep[0]);
  close(go[1]);
  waitpid(pid, NULL, 0);

  /* Give the slots back: deleting either end removes both, wherever they are. */
  int a = if_index("veth0");
  if (a > 0)
    link_del(a);
}

/* Every write in the test above goes to a pipe whose reader is a process whose
 * exit timing this test does not fully control, and the default action for
 * SIGPIPE is to kill — which ended the whole binary with no marker and no
 * failure. Ignore it only for the duration: an IGNORED disposition survives
 * execve, so leaving it set would change how every later child behaves. */
static void test_net_ns(void) {
  void (*prev)(int) = signal(SIGPIPE, SIG_IGN);
  test_net_ns_inner();
  signal(SIGPIPE, prev == SIG_ERR ? SIG_DFL : prev);
}

/* ── a network namespace with an address of its own ───────────────────────
 *
 * The veth test above proves a namespaced interface carries FRAMES. This one
 * proves it carries an ADDRESS: the IPv4 configuration is per namespace, so
 * two namespaces joined by one veth pair each hold their own address, talk IP
 * to each other over the cable, and neither address exists in the namespace
 * that made the pair.
 *
 * Both ways of assigning an address are exercised, because both had to become
 * namespace-aware: the ioctl pair `ifconfig` uses (SIOCSIFADDR /
 * SIOCSIFNETMASK) on one side, and rtnetlink's RTM_NEWADDR — `ip addr add` —
 * on the other.
 */
#define NSIP_PORT 7797
#define NSIP_A "10.99.0.1"
#define NSIP_B "10.99.0.2"
#define NSIP_MSG "M109-NETNS-IPV4"
/* 10.99.0.0 is host-order 0x0A630000; /proc/net/route prints it least
 * significant byte first. */
#define NSIP_ROUTE_HEX "0000630A"

#define K_RTM_NEWADDR 20
#define K_IFA_LOCAL 2
#define K_IFA_ADDRESS 1

/* nlmsghdr + ifaddrmsg. nlr_init lays down an ifinfomsg instead, which is a
 * different fixed part and a different length. */
static void nlr_init_addr(struct nlreq *r, unsigned type, unsigned flags,
                          int ifindex, int plen) {
  memset(r, 0, sizeof(*r));
  nl_put_u16(r->b + 4, type);
  nl_put_u16(r->b + 6, flags | NL_F_REQUEST | NL_F_ACK);
  nl_put_u32(r->b + 8, ++g_nl_seq);
  nl_put_u32(r->b + 12, (unsigned)getpid());
  r->b[16] = AF_INET;          /* ifa_family */
  r->b[17] = (unsigned char)plen; /* ifa_prefixlen */
  nl_put_u32(r->b + 20, (unsigned)ifindex);
  r->len = 24;                 /* 16 + sizeof(ifaddrmsg) */
}

/* `ip addr add <ip>/<plen> dev <ifindex>`. */
static int addr_add_nl(int ifindex, const char *ip, int plen) {
  struct in_addr a;
  if (inet_aton(ip, &a) == 0)
    return -1;
  struct nlreq r;
  nlr_init_addr(&r, K_RTM_NEWADDR, NL_F_CREATE, ifindex, plen);
  nlr_attr(&r, K_IFA_LOCAL, &a.s_addr, 4);
  nlr_attr(&r, K_IFA_ADDRESS, &a.s_addr, 4);
  return nl_do(&r);
}

/* `ifconfig <if> <ip> netmask <mask>`. */
static int addr_set_ioctl(const char *ifname, const char *ip,
                          const char *mask) {
  int s = socket(AF_INET, SOCK_DGRAM, 0);
  if (s < 0)
    return -errno;
  struct ifreq r;
  struct sockaddr_in *sin = (struct sockaddr_in *)&r.ifr_addr;
  int rc = 0;

  memset(&r, 0, sizeof(r));
  snprintf(r.ifr_name, sizeof(r.ifr_name), "%s", ifname);
  sin->sin_family = AF_INET;
  if (inet_aton(ip, &sin->sin_addr) == 0)
    rc = -1;
  else if (ioctl(s, SIOCSIFADDR, &r) != 0)
    rc = -errno;

  if (rc == 0) {
    memset(&r, 0, sizeof(r));
    snprintf(r.ifr_name, sizeof(r.ifr_name), "%s", ifname);
    sin = (struct sockaddr_in *)&r.ifr_netmask;
    sin->sin_family = AF_INET;
    if (inet_aton(mask, &sin->sin_addr) == 0)
      rc = -1;
    else if (ioctl(s, SIOCSIFNETMASK, &r) != 0)
      rc = -errno;
  }
  close(s);
  return rc;
}

/* SIOCGIFADDR read-back, as dotted quad. "" when the interface has none. */
static void addr_get(const char *ifname, char *out, size_t cap) {
  out[0] = '\0';
  int s = socket(AF_INET, SOCK_DGRAM, 0);
  if (s < 0)
    return;
  struct ifreq r;
  memset(&r, 0, sizeof(r));
  snprintf(r.ifr_name, sizeof(r.ifr_name), "%s", ifname);
  if (ioctl(s, SIOCGIFADDR, &r) == 0) {
    struct sockaddr_in *sin = (struct sockaddr_in *)&r.ifr_addr;
    snprintf(out, cap, "%s", inet_ntoa(sin->sin_addr));
  }
  close(s);
}

/* Poll a datagram socket for up to ~4 s. Returns the length, or -1. */
static ssize_t udp_wait(int fd, void *buf, size_t cap,
                        struct sockaddr_in *from) {
  for (int i = 0; i < 800; i++) {
    socklen_t fl = sizeof(*from);
    ssize_t n = recvfrom(fd, buf, cap, MSG_DONTWAIT, (struct sockaddr *)from,
                         &fl);
    if (n >= 0)
      return n;
    if (errno != EAGAIN && errno != EWOULDBLOCK)
      return -1;
    usleep(5000);
  }
  return -1;
}

static int nsip_route_present(void) {
  char rbuf[8192] = {0};
  slurp("/proc/net/route", rbuf, sizeof(rbuf));
  return strstr(rbuf, NSIP_ROUTE_HEX) != NULL;
}

/* The receiving side: address by rtnetlink, then echo one datagram back. */
static void nsip_child_b(int rep, int go) {
  char msg[256];
  if (unshare(CLONE_NEWNET) != 0) {
    snprintf(msg, sizeof(msg), "E unshare %d", errno);
    write(rep, msg, strlen(msg) + 1);
    _exit(0);
  }
  /* The inherited netlink socket was opened in the initial namespace. */
  if (g_nl >= 0) {
    close(g_nl);
    g_nl = -1;
  }
  write(rep, "R", 2);
  char c;
  if (read(go, &c, 1) != 1)
    _exit(0);

  int idx = if_index("vethB");
  int up = idx > 0 ? if_set_up("vethB", 1) : -1;
  int aa = idx > 0 ? addr_add_nl(idx, NSIP_B, 24) : -1;
  char mine[32];
  addr_get("vethB", mine, sizeof(mine));

  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  struct sockaddr_in me;
  memset(&me, 0, sizeof(me));
  me.sin_family = AF_INET;
  me.sin_port = htons(NSIP_PORT);
  me.sin_addr.s_addr = htonl(INADDR_ANY);
  int bnd = fd >= 0 ? bind(fd, (struct sockaddr *)&me, sizeof(me)) : -1;

  snprintf(msg, sizeof(msg), "B %d %d %d %d %d %s", idx, up, aa, bnd,
           nsip_route_present(), mine[0] ? mine : "-");
  write(rep, msg, strlen(msg) + 1);

  /* One datagram in, the same payload back to wherever it came from. The
   * source address the kernel reports is the sender NAMESPACE's address —
   * the thing ipv4_send_tx now stamps from the interface's namespace. */
  int txp = idx > 0 ? packet_open(0x0800, SOCK_RAW, idx) : -1;
  char buf[128];
  struct sockaddr_in from;
  memset(&from, 0, sizeof(from));
  ssize_t n = fd >= 0 ? udp_wait(fd, buf, sizeof(buf) - 1, &from) : -1;
  ssize_t back = -1;
  if (n > 0) {
    buf[n] = '\0';
    back = sendto(fd, buf, (size_t)n, 0, (struct sockaddr *)&from,
                  sizeof(from));
  }
  int wout = -1;
  if (txp >= 0) {
    unsigned char fbuf[512];
    struct sll fsll;
    wout = recv_dir(txp, NSIP_MSG, 0, fbuf, sizeof(fbuf), &fsll);
    close(txp);
  }
  char mine2[32];
  addr_get("vethB", mine2, sizeof(mine2));
  snprintf(msg, sizeof(msg), "X %ld %s %s %ld %d %s", (long)n,
           n > 0 ? inet_ntoa(from.sin_addr) : "-", n > 0 ? buf : "-",
           (long)back, wout, mine2[0] ? mine2 : "-");
  write(rep, msg, strlen(msg) + 1);
  if (fd >= 0)
    close(fd);
  _exit(0);
}

/* The sending side: address by ioctl, then one request and its echo. */
static void nsip_child_a(int rep, int go) {
  char msg[256];
  if (unshare(CLONE_NEWNET) != 0) {
    snprintf(msg, sizeof(msg), "E unshare %d", errno);
    write(rep, msg, strlen(msg) + 1);
    _exit(0);
  }
  if (g_nl >= 0) {
    close(g_nl);
    g_nl = -1;
  }
  write(rep, "R", 2);
  char c;
  if (read(go, &c, 1) != 1)
    _exit(0);

  int idx = if_index("vethA");
  int up = idx > 0 ? if_set_up("vethA", 1) : -1;
  int aa = idx > 0 ? addr_set_ioctl("vethA", NSIP_A, "255.255.255.0") : -1;
  char mine[32];
  addr_get("vethA", mine, sizeof(mine));

  int rxp = idx > 0 ? packet_open(0x0800, SOCK_RAW, idx) : -1;
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  struct sockaddr_in me;
  memset(&me, 0, sizeof(me));
  me.sin_family = AF_INET;
  me.sin_port = htons(NSIP_PORT + 1);
  me.sin_addr.s_addr = htonl(INADDR_ANY);
  int bnd = fd >= 0 ? bind(fd, (struct sockaddr *)&me, sizeof(me)) : -1;
  struct sockaddr_in peer;
  memset(&peer, 0, sizeof(peer));
  peer.sin_family = AF_INET;
  peer.sin_port = htons(NSIP_PORT);
  inet_aton(NSIP_B, &peer.sin_addr);
  ssize_t sent = fd >= 0 && bnd == 0
                     ? sendto(fd, NSIP_MSG, strlen(NSIP_MSG), 0,
                              (struct sockaddr *)&peer, sizeof(peer))
                     : -1;
  char buf[128];
  struct sockaddr_in from;
  memset(&from, 0, sizeof(from));
  ssize_t got = fd >= 0 && sent > 0 ? udp_wait(fd, buf, sizeof(buf) - 1, &from)
                                    : -1;
  int rerr = got < 0 ? errno : 0;
  if (got > 0)
    buf[got] = '\0';
  /* Did the reply reach the interface at all? A packet socket on this end
   * separates "the peer never sent it" from "the stack did not deliver it". */
  int wire = -1;
  if (rxp >= 0) {
    unsigned char fbuf[512];
    struct sll fsll;
    wire = recv_dir(rxp, NSIP_MSG, 1, fbuf, sizeof(fbuf), &fsll);
    close(rxp);
  }
  snprintf(msg, sizeof(msg), "A %d %d %d %d %s %ld %ld %s %s %d %d %d", idx, up,
           aa, nsip_route_present(), mine[0] ? mine : "-", (long)sent,
           (long)got, got > 0 ? buf : "-",
           got > 0 ? inet_ntoa(from.sin_addr) : "-", bnd, rerr, wire);
  write(rep, msg, strlen(msg) + 1);
  if (fd >= 0)
    close(fd);
  _exit(0);
}

static void test_net_ns_ipv4_inner(void) {
  char host_eth[32], host_now[32], m[256];
  int repa[2], gopa[2], repb[2], gopb[2];
  int idx_a, idx_b, mv_a = -1, mv_b = -1;
  int b_idx = -1, b_up = -1, b_aa = -1, b_bnd = -1, b_route = -1;
  int a_idx = -1, a_up = -1, a_aa = -1, a_route = -1, a_bnd = -1;
  int a_rerr = -1, a_wire = -1;
  long a_sent = -1, a_got = -1, b_n = -1, b_back = -1;
  char a_addr[32] = {0}, a_echo[64] = {0}, a_from[32] = {0};
  char b_addr[32] = {0}, b_from[32] = {0}, b_payload[64] = {0};
  char b_addr2[32] = {0};
  int b_wout = -1;
  int addr_ok, xchg_ok, leaked;
  pid_t pa, pb;

  addr_get("eth0", host_eth, sizeof(host_eth));

  if (link_add_veth("vethA", "vethB") != 0) {
    failm("netns-ipv4-address", "veth pair");
    return;
  }
  idx_a = if_index("vethA");
  idx_b = if_index("vethB");
  if (pipe(repa) || pipe(gopa) || pipe(repb) || pipe(gopb)) {
    failm("netns-ipv4-address", "pipe");
    return;
  }

  pb = fork();
  if (pb == 0) {
    close(repa[0]); close(repa[1]); close(gopa[0]); close(gopa[1]);
    close(repb[0]); close(gopb[1]);
    nsip_child_b(repb[1], gopb[0]);
  }
  pa = fork();
  if (pa == 0) {
    close(repb[0]); close(repb[1]); close(gopb[0]); close(gopb[1]);
    close(repa[0]); close(gopa[1]);
    nsip_child_a(repa[1], gopa[0]);
  }
  close(repa[1]); close(gopa[0]); close(repb[1]); close(gopb[0]);
  if (pa < 0 || pb < 0) {
    failm("netns-ipv4-address", "fork");
    close(repa[0]); close(gopa[1]); close(repb[0]); close(gopb[1]);
    return;
  }

  memset(m, 0, sizeof(m));
  if (read(repb[0], m, sizeof(m) - 1) <= 0 || m[0] != 'R') {
    failm("netns-ipv4-address", m[0] ? m : "no report from B");
    goto reap;
  }
  memset(m, 0, sizeof(m));
  if (read(repa[0], m, sizeof(m) - 1) <= 0 || m[0] != 'R') {
    failm("netns-ipv4-address", m[0] ? m : "no report from A");
    goto reap;
  }

  /* One end of the cable into each namespace. Both indexes were taken before
   * the first move: an interface that has left is not nameable here. */
  mv_a = link_set_netns_pid(idx_a, pa);
  mv_b = link_set_netns_pid(idx_b, pb);

  /* B first: it must be listening before A sends. */
  write(gopb[1], "g", 1);
  memset(m, 0, sizeof(m));
  if (read(repb[0], m, sizeof(m) - 1) <= 0 || m[0] != 'B' ||
      sscanf(m + 2, "%d %d %d %d %d %31s", &b_idx, &b_up, &b_aa, &b_bnd,
             &b_route, b_addr) != 6) {
    failm("netns-ipv4-address", m[0] ? m : "B never configured");
    goto reap;
  }

  write(gopa[1], "g", 1);
  memset(m, 0, sizeof(m));
  if (read(repa[0], m, sizeof(m) - 1) <= 0 || m[0] != 'A' ||
      sscanf(m + 2, "%d %d %d %d %31s %ld %ld %63s %31s %d %d %d", &a_idx,
             &a_up, &a_aa, &a_route, a_addr, &a_sent, &a_got, a_echo, a_from,
             &a_bnd, &a_rerr, &a_wire) != 12) {
    failm("netns-ipv4-exchange", m[0] ? m : "A never reported");
    goto reap;
  }

  memset(m, 0, sizeof(m));
  if (read(repb[0], m, sizeof(m) - 1) <= 0 || m[0] != 'X' ||
      sscanf(m + 2, "%ld %31s %63s %ld %d %31s", &b_n, b_from, b_payload,
             &b_back, &b_wout, b_addr2) != 6)
    b_n = -1;

  /* 1. The address took effect inside the namespace - by ioctl on one side,
   *    by rtnetlink on the other - and each namespace got the on-link route
   *    that comes with it. */
  addr_ok = mv_a == 0 && mv_b == 0 && a_idx > 0 && b_idx > 0 && a_up == 0 &&
            b_up == 0 && a_aa == 0 && b_aa == 0 &&
            strcmp(a_addr, NSIP_A) == 0 && strcmp(b_addr, NSIP_B) == 0 &&
            a_route == 1 && b_route == 1;
  if (!addr_ok)
    note("netns-ipv4-address: mv=%d/%d idx=%d/%d up=%d/%d set=%d/%d "
         "addr=%s/%s route=%d/%d",
         mv_a, mv_b, a_idx, b_idx, a_up, b_up, a_aa, b_aa, a_addr, b_addr,
         a_route, b_route);
  check("netns-ipv4-address", addr_ok, 0);

  /* 2. A real IPv4 exchange over the cable: the datagram arrives with the
   *    SENDING namespace's address as its source, and the echo comes back. */
  xchg_ok = b_bnd == 0 && a_bnd == 0 && a_sent == (long)strlen(NSIP_MSG) &&
            b_n == (long)strlen(NSIP_MSG) &&
            strcmp(b_payload, NSIP_MSG) == 0 && strcmp(b_from, NSIP_A) == 0 &&
            b_back == b_n && a_got == (long)strlen(NSIP_MSG) &&
            strcmp(a_echo, NSIP_MSG) == 0 && strcmp(a_from, NSIP_B) == 0;
  if (!xchg_ok)
    note("netns-ipv4-exchange: bind=%d/%d sent=%ld rx=%ld from=%s payload=%s "
         "back=%ld echo=%ld/%s from=%s rerr=%d wire=%d b-wire-out=%d "
         "b-addr-after=%s",
         b_bnd, a_bnd, a_sent, b_n, b_from, b_payload, b_back, a_got, a_echo,
         a_from, a_rerr, a_wire, b_wout, b_addr2[0] ? b_addr2 : "-");
  check("netns-ipv4-exchange", xchg_ok, 0);

  /* 3. None of it exists out here: not the interfaces, not the addresses, not
   *    the prefix they installed - and this namespace's own lease is
   *    untouched. */
  addr_get("eth0", host_now, sizeof(host_now));
  leaked = nsip_route_present() || if_index("vethA") || if_index("vethB") ||
           strcmp(host_now, NSIP_A) == 0 || strcmp(host_now, NSIP_B) == 0;
  if (leaked || strcmp(host_now, host_eth) != 0)
    note("netns-ipv4-isolated: route=%d vethA=%d vethB=%d eth0=%s was=%s",
         nsip_route_present(), if_index("vethA"), if_index("vethB"), host_now,
         host_eth[0] ? host_eth : "-");
  check("netns-ipv4-isolated", !leaked && strcmp(host_now, host_eth) == 0, 0);

reap:
  close(repa[0]); close(gopa[1]); close(repb[0]); close(gopb[1]);
  waitpid(pa, NULL, 0);
  waitpid(pb, NULL, 0);
  /* Both namespaces are gone with their last task, and the veth pair with
   * them; nothing is left to delete here. */
}

static void test_net_ns_ipv4(void) {
  void (*prev)(int) = signal(SIGPIPE, SIG_IGN);
  test_net_ns_ipv4_inner();
  signal(SIGPIPE, prev == SIG_ERR ? SIG_DFL : prev);
}

/* ── unlink of an in-memory node on an on-disk filesystem ─────────────────
 *
 * mknod(2) keeps a device node in memory even when its directory is on ext4 —
 * a device number is a property of the running kernel, not of the image. The
 * filesystem therefore answers ENOENT when asked to remove the directory entry
 * it never had, and the VFS treats that one error as "already gone" rather
 * than as a veto, which is what lets `rm /dev/loop0` and mdev's own removals
 * work at all.
 *
 * That relaxation sits in the path every unlink takes, so this checks the
 * thing it must NOT have broken: a name that exists on neither side still
 * fails with ENOENT, before and after the node's lifetime. */
#define NODE_PATH "/m109-devnode-test"
#define NODE_ABSENT "/m109-no-such-name"

static void test_unlink_enoent(void) {
  unlink(NODE_PATH);
  unlink(NODE_ABSENT);

  /* A name on neither side. */
  errno = 0;
  int gone_before = (unlink(NODE_ABSENT) == -1) && errno == ENOENT;

  /* A character node: in memory, on an on-disk directory. */
  if (mknod(NODE_PATH, S_IFCHR | 0600, makedev(1, 3)) != 0) {
    failm("unlink-enoent", "mknod");
    return;
  }
  struct stat st;
  int made = stat(NODE_PATH, &st) == 0 && S_ISCHR(st.st_mode) &&
             st.st_rdev == (dev_t)makedev(1, 3);

  int removed = unlink(NODE_PATH) == 0;
  errno = 0;
  int gone_after = (stat(NODE_PATH, &st) == -1) && errno == ENOENT;
  errno = 0;
  int twice = (unlink(NODE_PATH) == -1) && errno == ENOENT;

  if (!gone_before || !made || !removed || !gone_after || !twice)
    note("unlink-enoent: absent=%d made=%d removed=%d gone=%d twice=%d",
         gone_before, made, removed, gone_after, twice);
  check("unlink-enoent",
        gone_before && made && removed && gone_after && twice, 0);
}

/* The device-node and mount half of M109: /dev listings merged over the
 * on-disk directory, findfs/blkid resolving a disk, and MS_MOVE. */
static int run_capture_argv(const char *const argv[], char *out, size_t cap) {
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

#define MAX_NAMES 1024
static char g_names[MAX_NAMES][64];
static int g_name_count;

static int list_dir(const char *path) {
  DIR *d = opendir(path);
  if (!d)
    return -1;
  g_name_count = 0;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
      continue;
    if (g_name_count < MAX_NAMES)
      snprintf(g_names[g_name_count++], 64, "%s", e->d_name);
  }
  closedir(d);
  return 0;
}

static int names_have(const char *name) {
  for (int i = 0; i < g_name_count; i++)
    if (strcmp(g_names[i], name) == 0)
      return 1;
  return 0;
}

/* Every disk /sys/block knows about must have a node in /dev that a readdir
 * finds — that is exactly what BusyBox's blkid and findfs scan for. */
static void test_dev_nodes_listed(void) {
  static char disks[32][64];
  int ndisks = 0;

  if (list_dir("/sys/block") != 0) {
    fail("dev-nodes-listed", -1);
    return;
  }
  for (int i = 0; i < g_name_count && ndisks < 32; i++)
    snprintf(disks[ndisks++], 64, "%s", g_names[i]);
  if (ndisks == 0) {
    fail("dev-nodes-listed", -2);
    return;
  }

  if (list_dir("/dev") != 0) {
    fail("dev-nodes-listed", -3);
    return;
  }
  int listed = 0;
  for (int i = 0; i < ndisks; i++) {
    if (!names_have(disks[i])) {
      fail("dev-nodes-listed", -100 - i);
      return;
    }
    char p[128];
    struct stat st;
    snprintf(p, sizeof(p), "/dev/%s", disks[i]);
    if (stat(p, &st) != 0 || !S_ISBLK(st.st_mode)) {
      fail("dev-nodes-listed", -200 - i);
      return;
    }
    listed++;
  }
  /* The character devices the kernel publishes there live in the same list. */
  if (!names_have("null") || !names_have("zero")) {
    fail("dev-nodes-listed", -4);
    return;
  }
  check("dev-nodes-listed", listed == ndisks, listed);
}

/* /bin holds the image's own binaries and the applet symlinks the kernel
 * attaches in memory. Both halves must be listed, each name once. */
static void test_dir_merge_no_dups(void) {
  if (list_dir("/bin") != 0) {
    fail("dir-merge-no-dups", -1);
    return;
  }
  int count = g_name_count;
  if (count < 100) {
    fail("dir-merge-no-dups", count);
    return;
  }
  if (!names_have("sh")) {
    fail("dir-merge-no-dups", -2);
    return;
  }
  for (int i = 0; i < count; i++)
    for (int j = i + 1; j < count; j++)
      if (strcmp(g_names[i], g_names[j]) == 0) {
        fail("dir-merge-no-dups", -300 - i);
        return;
      }
  ok("dir-merge-no-dups");
}

/* ════════════════════ findfs / blkid ════════════════════ */

/* Read the ext2/3/4 superblock of a block device and report its UUID and
 * volume label. This is a second, independent implementation of what BusyBox's
 * volume_id does, so a findfs answer can be checked against the bytes on the
 * disk rather than against BusyBox itself. */
static int probe_super(const char *dev, char *uuid, size_t uuidcap, char *label,
                       size_t labelcap) {
  char path[128];
  unsigned char sb[1024];

  snprintf(path, sizeof(path), "/dev/%s", dev);
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return -1;
  if (lseek(fd, 1024, SEEK_SET) != 1024 || read(fd, sb, sizeof(sb)) != 1024) {
    close(fd);
    return -1;
  }
  close(fd);
  if (sb[0x38] != 0x53 || sb[0x39] != 0xEF)
    return -1; /* not ext2/3/4 */

  const unsigned char *u = sb + 0x68;
  snprintf(uuid, uuidcap,
           "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
           "%02x%02x%02x%02x%02x%02x",
           u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10],
           u[11], u[12], u[13], u[14], u[15]);
  char l[17];
  memcpy(l, sb + 0x78, 16);
  l[16] = '\0';
  snprintf(label, labelcap, "%s", l);
  return 0;
}

static int list_disks(char disks[][64], int max) {
  if (list_dir("/sys/block") != 0)
    return 0;
  int n = 0;
  for (int i = 0; i < g_name_count && n < max; i++)
    snprintf(disks[n++], 64, "%s", g_names[i]);
  return n;
}

/* The disk `probe_disk()` settled on, and what its superblock says. Named
 * apart from the g_dev of the blkid/sysfs-ident pair above: both were file
 * scope `g_dev[64]`, which C merges into ONE object, so whichever test ran
 * first decided what the other saw — a path there, a bare name here. */
static char g_fs_dev[64], g_fs_uuid[128], g_fs_label[128];

/* Find a disk carrying an ext filesystem, and what its superblock says. */
static int probe_disk(void) {
  static char disks[32][64];
  int ndisks = list_disks(disks, 32);

  for (int i = 0; i < ndisks; i++) {
    char u[128] = "", l[128] = "";
    if (probe_super(disks[i], u, sizeof(u), l, sizeof(l)) != 0)
      continue;
    snprintf(g_fs_dev, sizeof(g_fs_dev), "%s", disks[i]);
    snprintf(g_fs_uuid, sizeof(g_fs_uuid), "%s", u);
    snprintf(g_fs_label, sizeof(g_fs_label), "%s", l);
    return 0;
  }
  return -1;
}

static void test_findfs_uuid(void) {
  if (access("/bin/findfs", X_OK) != 0) {
    fail("findfs-uuid", -1);
    return;
  }
  if (g_fs_dev[0] == '\0' && probe_disk() != 0) {
    fail("findfs-uuid", -2);
    return;
  }
  char spec[160];
  snprintf(spec, sizeof(spec), "UUID=%s", g_fs_uuid);
  const char *argv[] = {"/bin/findfs", spec, NULL};
  static char out[256];
  int rc = run_capture_argv(argv, out, sizeof(out));
  char *nl = strchr(out, '\n');
  if (nl)
    *nl = '\0';
  char want[128];
  snprintf(want, sizeof(want), "/dev/%s", g_fs_dev);
  if (rc != 0 || strcmp(out, want) != 0)
    note("findfs-uuid: dev=%s uuid=%s got=%s rc=%d", g_fs_dev, g_fs_uuid, out, rc);
  check("findfs-uuid",
        rc == 0 && g_fs_uuid[0] != '\0' && strcmp(out, want) == 0, rc);
}

static void test_findfs_label(void) {
  if (access("/bin/findfs", X_OK) != 0) {
    fail("findfs-label", -1);
    return;
  }
  /* The disk the smoke run gave a volume label to. */
  static char disks[32][64];
  int ndisks = list_disks(disks, 32);
  char dev[64] = "", label[128] = "";

  for (int i = 0; i < ndisks; i++) {
    char u[128] = "", l[128] = "";
    if (probe_super(disks[i], u, sizeof(u), l, sizeof(l)) != 0 || l[0] == '\0')
      continue;
    snprintf(dev, sizeof(dev), "%s", disks[i]);
    snprintf(label, sizeof(label), "%s", l);
    break;
  }
  if (dev[0] == '\0') {
    fail("findfs-label", -3);
    return;
  }
  char spec[160];
  snprintf(spec, sizeof(spec), "LABEL=%s", label);
  const char *argv[] = {"/bin/findfs", spec, NULL};
  static char out[256];
  int rc = run_capture_argv(argv, out, sizeof(out));
  char *nl = strchr(out, '\n');
  if (nl)
    *nl = '\0';
  char want[128];
  snprintf(want, sizeof(want), "/dev/%s", dev);
  check("findfs-label", rc == 0 && strcmp(out, want) == 0, rc);
}

static void test_blkid(void) {
  if (access("/bin/blkid", X_OK) != 0) {
    fail("blkid-lists-disks", -1);
    return;
  }
  if (g_fs_dev[0] == '\0' && probe_disk() != 0) {
    fail("blkid-lists-disks", -2);
    return;
  }
  const char *argv[] = {"/bin/blkid", NULL};
  static char out[8192];
  int rc = run_capture_argv(argv, out, sizeof(out));
  char want[128];
  snprintf(want, sizeof(want), "/dev/%s", g_fs_dev);
  if (rc != 0 || !strstr(out, want) || !strstr(out, g_fs_uuid))
    note("blkid-lists-disks: dev=%s uuid=%s rc=%d listing=%.120s", g_fs_dev,
         g_fs_uuid, rc, out);
  /* An empty uuid would make the strstr below trivially true — the check has
   * to fail when the probe found nothing, not pass vacuously. */
  check("blkid-lists-disks",
        rc == 0 && g_fs_uuid[0] != '\0' && strstr(out, want) != NULL &&
            strstr(out, g_fs_uuid) != NULL,
        rc);
}

/* ════════════════════ MS_MOVE ════════════════════ */

static int write_file(const char *path, const char *text) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0)
    return -1;
  ssize_t n = write(fd, text, strlen(text));
  close(fd);
  return n == (ssize_t)strlen(text) ? 0 : -1;
}

static int file_says(const char *path, const char *text) {
  char buf[128] = "";
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return 0;
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0)
    return 0;
  buf[n] = '\0';
  return strcmp(buf, text) == 0;
}


static void test_mount_move(void) {
  const char *a = "/mnt/m109a", *b = "/mnt/m109b";
  mkdir("/mnt", 0755);
  mkdir(a, 0755);
  mkdir(b, 0755);

  if (mount("none", a, "tmpfs", 0, NULL) != 0) {
    fail("mount-move", -1);
    return;
  }
  if (write_file("/mnt/m109a/marker", "moved") != 0) {
    umount(a);
    fail("mount-move", -2);
    return;
  }
  mkdir("/mnt/m109a/sub", 0755);
  int nested = mount("none", "/mnt/m109a/sub", "tmpfs", 0, NULL) == 0;
  if (nested && write_file("/mnt/m109a/sub/nested", "deep") != 0)
    nested = 0;
  if (!nested) {
    umount(a);
    fail("mount-move", -3);
    return;
  }

  if (mount(a, b, NULL, MS_MOVE, NULL) != 0) {
    umount("/mnt/m109a/sub");
    umount(a);
    fail("mount-move", -4);
    return;
  }

  int moved = file_says("/mnt/m109b/marker", "moved");
  int nested_moved = file_says("/mnt/m109b/sub/nested", "deep");
  int old_gone = access("/mnt/m109a/marker", F_OK) != 0;
  int listed = mounts_mention(b) && mounts_mention("/mnt/m109b/sub") &&
               !mounts_mention(a);

  struct statfs sf;
  int type_ok = statfs(b, &sf) == 0 && (unsigned)sf.f_type == TMPFS_MAGIC;

  check("mount-move", moved && nested_moved && old_gone && listed,
        (moved ? 0 : 1) | (nested_moved ? 0 : 2) | (old_gone ? 0 : 4) |
            (listed ? 0 : 8));
  check("mount-move-statfs", type_ok, (long)sf.f_type);

  umount("/mnt/m109b/sub");
  umount(b);
  rmdir(a);
  rmdir(b);
}

/* The block-device half of M109: inode attribute flags, discard/TRIM,
  * I/O priorities and serial line configuration. */
static void note2(const char *fmt, long a, long b) {
  char line[192];
  snprintf(line, sizeof(line), fmt, a, b);
  marker(line);
}
static int getflags(int fd, int *out) {
  int f = 0;
  if (ioctl(fd, FS_IOC_GETFLAGS, &f) != 0)
    return -1;
  *out = f;
  return 0;
}
static int setflags(int fd, int f) { return ioctl(fd, FS_IOC_SETFLAGS, &f); }

/* chattr through a path, so the caller does not have to keep an fd open across
 * the operations the flag is supposed to forbid. */
static int chattr_path(const char *path, int f) {
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return -1;
  int rc = setflags(fd, f);
  close(fd);
  return rc;
}

/* ── 1. inode attribute flags ───────────────────────────────────────────── */

static void test_attr_roundtrip(void) {
  const char *path = "/tmp/m109_attr";
  unlink(path);
  int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    fail("attr-roundtrip", -1);
    return;
  }

  int f = 0;
  if (getflags(fd, &f) != 0) {
    fail("attr-roundtrip", -2);
    close(fd);
    return;
  }
  if (f != 0) { /* a fresh file carries no attributes */
    fail("attr-roundtrip", f);
    close(fd);
    return;
  }

  int want = FS_NODUMP_FL | FS_NOATIME_FL | FS_SYNC_FL;
  if (setflags(fd, want) != 0) {
    fail("attr-roundtrip", -3);
    close(fd);
    return;
  }
  if (getflags(fd, &f) != 0 || f != want) {
    fail("attr-roundtrip", f);
    close(fd);
    return;
  }

  /* A flag the format cannot store must be refused, not quietly dropped: a
   * chattr that reports success and changes nothing is worse than an error. */
  errno = 0;
  if (setflags(fd, want | FS_PROJINHERIT_FL) == 0 || errno != EOPNOTSUPP) {
    fail("attr-roundtrip", -4);
    close(fd);
    return;
  }
  if (getflags(fd, &f) != 0 || f != want) {
    fail("attr-roundtrip", f);
    close(fd);
    return;
  }

  setflags(fd, 0);
  close(fd);
  unlink(path);
  ok("attr-roundtrip");
}

static void test_attr_immutable(void) {
  const char *path = "/tmp/m109_imm";
  const char *other = "/tmp/m109_imm2";
  unlink(path);
  unlink(other);
  int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (fd < 0 || write(fd, "before", 6) != 6) {
    fail("attr-immutable", -1);
    if (fd >= 0)
      close(fd);
    return;
  }
  close(fd);

  if (chattr_path(path, FS_IMMUTABLE_FL) != 0) {
    fail("attr-immutable", -2);
    return;
  }

  int step = 0;
  /* write */
  fd = open(path, O_WRONLY);
  if (fd >= 0) {
    errno = 0;
    if (write(fd, "x", 1) != -1 || errno != EPERM)
      step = 1;
    close(fd);
  }
  /* truncate */
  fd = open(path, O_RDWR);
  if (fd >= 0) {
    errno = 0;
    if (ftruncate(fd, 0) != -1 || errno != EPERM)
      step = 2;
    close(fd);
  }
  /* rename */
  errno = 0;
  if (rename(path, other) != -1 || errno != EPERM)
    step = 3;
  /* unlink */
  errno = 0;
  if (unlink(path) != -1 || errno != EPERM)
    step = 4;

  if (step) {
    fail("attr-immutable", step);
    chattr_path(path, 0);
    unlink(path);
    return;
  }

  /* The content is still what it was before all of that. */
  char buf[16];
  fd = open(path, O_RDONLY);
  ssize_t n = fd >= 0 ? read(fd, buf, sizeof(buf)) : -1;
  if (fd >= 0)
    close(fd);
  if (n != 6 || memcmp(buf, "before", 6) != 0) {
    fail("attr-immutable", (long)n);
    chattr_path(path, 0);
    unlink(path);
    return;
  }

  /* Clearing the flag gives the file back. */
  if (chattr_path(path, 0) != 0 || unlink(path) != 0) {
    fail("attr-immutable", -5);
    return;
  }
  ok("attr-immutable");
}

static void test_attr_append(void) {
  const char *path = "/tmp/m109_app";
  unlink(path);
  int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (fd < 0 || write(fd, "head", 4) != 4) {
    fail("attr-append", -1);
    if (fd >= 0)
      close(fd);
    return;
  }
  close(fd);

  if (chattr_path(path, FS_APPEND_FL) != 0) {
    fail("attr-append", -2);
    return;
  }

  int step = 0;
  /* A plain write is not an append, even at the end of the file. */
  fd = open(path, O_WRONLY);
  if (fd >= 0) {
    errno = 0;
    if (write(fd, "x", 1) != -1 || errno != EPERM)
      step = 1;
    close(fd);
  }
  /* O_APPEND is. */
  fd = open(path, O_WRONLY | O_APPEND);
  if (fd < 0 || write(fd, "tail", 4) != 4)
    step = 2;
  if (fd >= 0)
    close(fd);
  /* Truncation would shorten it, so it is still refused. */
  fd = open(path, O_RDWR);
  if (fd >= 0) {
    errno = 0;
    if (ftruncate(fd, 0) != -1 || errno != EPERM)
      step = 3;
    close(fd);
  }
  /* So would removing it. */
  errno = 0;
  if (unlink(path) != -1 || errno != EPERM)
    step = 4;

  char buf[32];
  fd = open(path, O_RDONLY);
  ssize_t n = fd >= 0 ? read(fd, buf, sizeof(buf)) : -1;
  if (fd >= 0)
    close(fd);
  if (n != 8 || memcmp(buf, "headtail", 8) != 0)
    step = 5;

  chattr_path(path, 0);
  unlink(path);
  if (step) {
    fail("attr-append", step);
    return;
  }
  ok("attr-append");
}

/* The flags are only real if they are on the disk. Mount a scratch ext4
 * filesystem, set them, unmount it — which drops every in-memory inode — and
 * mount it again. */
static void test_attr_persist(void) {
  if (mount(NVME_NAME, MNT, "ext4", 0, NULL) != 0) {
    fail("attr-persist", -1);
    return;
  }
  const char *path = MNT "/m109_persist";
  unlink(path);
  int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    fail("attr-persist", -2);
    umount(MNT);
    return;
  }
  int want = FS_NODUMP_FL | FS_APPEND_FL;
  int rc = setflags(fd, want);
  close(fd);
  if (rc != 0) {
    fail("attr-persist", -3);
    umount(MNT);
    return;
  }

  if (umount(MNT) != 0 || mount(NVME_NAME, MNT, "ext4", 0, NULL) != 0) {
    fail("attr-persist", -4);
    return;
  }

  int f = -1;
  fd = open(path, O_RDONLY);
  if (fd < 0 || getflags(fd, &f) != 0) {
    fail("attr-persist", -5);
    if (fd >= 0)
      close(fd);
    umount(MNT);
    return;
  }
  close(fd);
  if (f != want) {
    fail("attr-persist", f);
    umount(MNT);
    return;
  }
  chattr_path(path, 0);
  unlink(path);
  umount(MNT);
  ok("attr-persist");
}

static void test_attr_applets(void) {
  const char *path = "/tmp/m109_applet";
  unlink(path);
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    fail("attr-applets", -1);
    return;
  }
  close(fd);

  if (system("chattr +ad /tmp/m109_applet >/dev/null 2>&1") != 0) {
    fail("attr-applets", -2);
    unlink(path);
    return;
  }
  /* lsattr prints one line: the flag letters, then the path. */
  if (system("lsattr /tmp/m109_applet > /tmp/m109_lsattr 2>&1") != 0) {
    fail("attr-applets", -3);
    system("chattr -ad /tmp/m109_applet >/dev/null 2>&1");
    unlink(path);
    return;
  }
  char buf[256];
  memset(buf, 0, sizeof(buf));
  fd = open("/tmp/m109_lsattr", O_RDONLY);
  ssize_t n = fd >= 0 ? read(fd, buf, sizeof(buf) - 1) : -1;
  if (fd >= 0)
    close(fd);
  unlink("/tmp/m109_lsattr");

  /* The letters lsattr uses for append-only and no-dump. Look only at the
   * flag field, which ends at the first space. */
  char *sp = strchr(buf, ' ');
  int has_a = 0, has_d = 0;
  if (n > 0 && sp) {
    *sp = '\0';
    has_a = strchr(buf, 'a') != NULL;
    has_d = strchr(buf, 'd') != NULL;
  }
  system("chattr -ad /tmp/m109_applet >/dev/null 2>&1");

  int f = -1;
  fd = open(path, O_RDONLY);
  if (fd >= 0) {
    getflags(fd, &f);
    close(fd);
  }
  unlink(path);

  if (!has_a || !has_d) {
    fail("attr-applets", (long)n);
    return;
  }
  if (f != 0) { /* chattr -ad really took them off again */
    fail("attr-applets", f);
    return;
  }
  ok("attr-applets");
}

/* ── 2. discard ─────────────────────────────────────────────────────────── */

/* One place that decides what a device should do, so the two markers below
 * cannot disagree about it. */
static int discard_probe(const char *dev, int *supported) {
  int fd = open(dev, O_RDWR);
  if (fd < 0)
    return -1;
  unsigned long long range[2] = {1024ULL * 1024ULL, 512ULL * 1024ULL};
  errno = 0;
  int rc = ioctl(fd, BLKDISCARD, range);
  int err = errno;
  close(fd);
  if (rc == 0) {
    *supported = 1;
    return 0;
  }
  if (err == EOPNOTSUPP) {
    *supported = 0;
    return 0;
  }
  errno = err;
  return -1;
}

static void test_discard_support(void) {
  /* Two devices, one of each kind, and the answer must be the honest one for
   * both. Only scratch devices are probed: a discard destroys what it covers,
   * so the disks this test later mounts are left alone.
   *
   *   vda   virtio-blk, which negotiated VIRTIO_BLK_F_DISCARD -> accepted
   *   ram0  a RAM disk, which has no such command at all      -> EOPNOTSUPP
   *
   * The second half is the one that matters: a kernel that quietly emulated
   * discard by writing zeroes would report success here, and would have spent
   * exactly the I/O the caller asked it to avoid. */
  int sup = 0;
  if (discard_probe(VBLK_DEV, &sup) != 0 || !sup) {
    fail("discard-support", 1);
    return;
  }
  /* And a device that has no discard command at all, whichever one that is.
   *
   * It used to be /dev/ram0, which existed only because the root filesystem
   * travelled inside the boot image as a RAM disk. The point of the check is
   * not that device in particular: it is that a device without the command
   * answers EOPNOTSUPP rather than pretending, so any device that reports no
   * discard support serves — and the run is only inconclusive if every device
   * on the machine happens to have it. */
  static const char *const plain[] = { "/dev/ram0", "/dev/sata1", "/dev/sata0",
                                       "/dev/vdb" };
  int checked = 0;

  for (unsigned i = 0; i < sizeof(plain) / sizeof(plain[0]) && !checked; i++) {
    if (access(plain[i], F_OK) != 0)
      continue;
    if (discard_probe(plain[i], &sup) != 0)
      continue;
    if (sup)
      continue; /* this one has discard; it is not the example we need */
    checked = 1;
  }
  if (!checked) {
    fail("discard-support", 3);
    return;
  }
  ok("discard-support");
}

static void test_discard_virtio(void) {
  int fd = open(VBLK_DEV, O_RDWR);
  if (fd < 0) {
    fail("discard-virtio", -1);
    return;
  }
  unsigned long long size = 0;
  if (ioctl(fd, BLKGETSIZE64, &size) != 0 || size == 0) {
    fail("discard-virtio", -2);
    close(fd);
    return;
  }

  unsigned long long range[2] = {0, 0};
  int step = 0;

  /* A range the device has is accepted. */
  range[0] = 1024 * 1024;
  range[1] = 512 * 1024;
  if (ioctl(fd, BLKDISCARD, range) != 0)
    step = 1;

  /* A range that runs off the end of the device is not. */
  range[0] = size - 512;
  range[1] = 4096;
  errno = 0;
  if (ioctl(fd, BLKDISCARD, range) != -1 || errno != EINVAL)
    step = 2;

  /* Neither is one that is not a whole number of sectors. */
  range[0] = 511;
  range[1] = 512;
  errno = 0;
  if (ioctl(fd, BLKDISCARD, range) != -1 || errno != EINVAL)
    step = 3;

  /* Nothing here promises a discarded range reads back as zeroes, and the
   * kernel must say so rather than claim a guarantee it cannot keep. */
  int dz = -1;
  if (ioctl(fd, BLKDISCARDZEROES, &dz) != 0 || dz != 0)
    step = 4;

  /* virtio-blk is not a spinning disk, and BLKROTATIONAL should know. */
  int rot = -1;
  if (ioctl(fd, BLKROTATIONAL, &rot) != 0 || rot != 0)
    step = 5;

  close(fd);
  if (step) {
    fail("discard-virtio", step);
    return;
  }
  ok("discard-virtio");
}

static void test_discard_zeroout(void) {
  int fd = open(VBLK_DEV, O_RDWR);
  if (fd < 0) {
    fail("discard-zeroout", -1);
    return;
  }
  char pattern[4096], back[4096];
  memset(pattern, 0x5a, sizeof(pattern));
  const off_t at = 2 * 1024 * 1024;

  if (pwrite(fd, pattern, sizeof(pattern), at) != (ssize_t)sizeof(pattern)) {
    fail("discard-zeroout", -2);
    close(fd);
    return;
  }
  /* Read it back first, so a failure below cannot be the write's fault. */
  if (pread(fd, back, sizeof(back), at) != (ssize_t)sizeof(back) ||
      memcmp(back, pattern, sizeof(back)) != 0) {
    fail("discard-zeroout", -3);
    close(fd);
    return;
  }

  unsigned long long range[2] = {(unsigned long long)at, sizeof(pattern)};
  if (ioctl(fd, BLKZEROOUT, range) != 0) {
    fail("discard-zeroout", -4);
    close(fd);
    return;
  }
  if (pread(fd, back, sizeof(back), at) != (ssize_t)sizeof(back)) {
    fail("discard-zeroout", -5);
    close(fd);
    return;
  }
  close(fd);
  for (size_t i = 0; i < sizeof(back); i++) {
    if (back[i] != 0) {
      fail("discard-zeroout", (long)i);
      return;
    }
  }
  ok("discard-zeroout");
}

static void test_discard_applet(void) {
  if (system("blkdiscard -o 1048576 -l 524288 " VBLK_DEV
             " >/dev/null 2>&1") != 0) {
    fail("discard-applet", -1);
    return;
  }
  ok("discard-applet");
}

/* ── 3. fstrim ──────────────────────────────────────────────────────────── */

static void test_fstrim(void) {
  if (mount(NVME_NAME, MNT, "ext4", 0, NULL) != 0) {
    fail("fstrim-ioctl", -1);
    return;
  }

  struct statvfs vfs;
  if (statvfs(MNT, &vfs) != 0) {
    fail("fstrim-ioctl", -2);
    umount(MNT);
    return;
  }
  unsigned long long free_bytes =
      (unsigned long long)vfs.f_bfree * (unsigned long long)vfs.f_bsize;

  int fd = open(MNT, O_RDONLY);
  if (fd < 0) {
    fail("fstrim-ioctl", -3);
    umount(MNT);
    return;
  }
  struct fstrim_range_u r;
  memset(&r, 0, sizeof(r));
  r.len = ~0ULL;
  int rc = ioctl(fd, FITRIM, &r);
  int err = errno;
  close(fd);

  if (rc != 0) {
    fail("fstrim-ioctl", err);
    umount(MNT);
    return;
  }
  /* It must have found something, and it must not claim to have trimmed more
   * than the filesystem has free — which would mean it walked over blocks
   * that hold data. */
  if (r.len == 0 || r.len > free_bytes) {
    note2("M109-SMOKE: note fstrim trimmed=%ld free=%ld", (long)r.len,
         (long)free_bytes);
    fail("fstrim-ioctl", (long)r.len);
    umount(MNT);
    return;
  }
  note2("M109-SMOKE: note fstrim trimmed=%ld free=%ld", (long)r.len,
       (long)free_bytes);
  ok("fstrim-ioctl");

  /* The file this test wrote before the trim must still be readable: trimming
   * free space is only correct if it left the used space alone. */
  const char *path = MNT "/m109_trim";
  unlink(path);
  fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
  int intact = 0;
  if (fd >= 0 && write(fd, "keepme", 6) == 6) {
    close(fd);
    fd = open(MNT, O_RDONLY);
    memset(&r, 0, sizeof(r));
    r.len = ~0ULL;
    ioctl(fd, FITRIM, &r);
    close(fd);
    char buf[8];
    fd = open(path, O_RDONLY);
    if (fd >= 0 && read(fd, buf, 6) == 6 && memcmp(buf, "keepme", 6) == 0)
      intact = 1;
  }
  if (fd >= 0)
    close(fd);
  unlink(path);

  if (!intact) {
    fail("fstrim-keeps-data", -1);
    umount(MNT);
    return;
  }
  ok("fstrim-keeps-data");

  if (system("fstrim " MNT " >/dev/null 2>&1") != 0)
    fail("fstrim-applet", -1);
  else
    ok("fstrim-applet");
  umount(MNT);
}

/* ── 4. I/O priorities ──────────────────────────────────────────────────── */

static void test_ioprio(void) {
  int step = 0;

  /* best-effort, level 7 */
  int want = IOPRIO_PRIO(2, 7);
  if (syscall(SYS_ioprio_set, IOPRIO_WHO_PROCESS, 0, want) != 0)
    step = 1;
  long got = syscall(SYS_ioprio_get, IOPRIO_WHO_PROCESS, 0);
  if (got != want)
    step = 2;

  /* idle */
  want = IOPRIO_PRIO(3, 0);
  if (syscall(SYS_ioprio_set, IOPRIO_WHO_PROCESS, 0, want) != 0)
    step = 3;
  got = syscall(SYS_ioprio_get, IOPRIO_WHO_PROCESS, 0);
  if (got != want)
    step = 4;

  /* There are four classes; a fifth is a mistake, not a preference. */
  errno = 0;
  if (syscall(SYS_ioprio_set, IOPRIO_WHO_PROCESS, 0, IOPRIO_PRIO(5, 0)) != -1 ||
      errno != EINVAL)
    step = 5;

  /* Only WHO_PROCESS is implemented, and asking about a group must not be
   * answered with a process's value. */
  errno = 0;
  if (syscall(SYS_ioprio_get, 2 /* WHO_PGRP */, 0) != -1 || errno != EINVAL)
    step = 6;

  /* Back to the default so the rest of the suite runs at normal priority. */
  syscall(SYS_ioprio_set, IOPRIO_WHO_PROCESS, 0, IOPRIO_PRIO(2, 4));

  if (step) {
    fail("ioprio-roundtrip", step);
    return;
  }
  ok("ioprio-roundtrip");
}

static void test_ioprio_applet(void) {
  /* ionice with no class argument prints the class of the process it is
   * given; run it on itself through a shell so the class it sets is the one
   * it then reports. */
  if (system("ionice -c 3 sh -c 'ionice -p $$' > /tmp/m109_ionice 2>&1") !=
      0) {
    fail("ioprio-applet", -1);
    return;
  }
  char buf[128];
  memset(buf, 0, sizeof(buf));
  int fd = open("/tmp/m109_ionice", O_RDONLY);
  ssize_t n = fd >= 0 ? read(fd, buf, sizeof(buf) - 1) : -1;
  if (fd >= 0)
    close(fd);
  unlink("/tmp/m109_ionice");
  if (n <= 0 || strstr(buf, "idle") == NULL) {
    fail("ioprio-applet", (long)n);
    return;
  }
  ok("ioprio-applet");
}

/* ── 5. serial line configuration ───────────────────────────────────────── */

/* COM2. The boot console is COM1, and reprogramming the line the log travels
 * over would corrupt the log rather than test anything. */
#define TTY "/dev/ttyS1"

static void test_serial_line(void) {
  int fd = open(TTY, O_RDWR | O_NOCTTY);
  if (fd < 0) {
    fail("serial-line", -1);
    return;
  }
  struct termios saved, t;
  if (tcgetattr(fd, &saved) != 0) {
    fail("serial-line", -2);
    close(fd);
    return;
  }

  /* The line comes up at what the boot code programmed, and says so. */
  if ((saved.c_cflag & CBAUD) != B38400 || (saved.c_cflag & CSIZE) != CS8 ||
      (saved.c_cflag & (PARENB | CSTOPB)) != 0) {
    fail("serial-line", (long)saved.c_cflag);
    close(fd);
    return;
  }

  t = saved;
  t.c_cflag &= ~(CBAUD | CSIZE);
  t.c_cflag |= B115200 | CS7 | PARENB | PARODD | CSTOPB;
  if (tcsetattr(fd, TCSANOW, &t) != 0) {
    fail("serial-line", -3);
    tcsetattr(fd, TCSANOW, &saved);
    close(fd);
    return;
  }

  struct termios back;
  if (tcgetattr(fd, &back) != 0) {
    fail("serial-line", -4);
    tcsetattr(fd, TCSANOW, &saved);
    close(fd);
    return;
  }
  /* Read back from the chip's own divisor latch and line-control register,
   * not from a copy of what was asked for. */
  int good = (back.c_cflag & CBAUD) == B115200 &&
             (back.c_cflag & CSIZE) == CS7 && (back.c_cflag & CSTOPB) != 0 &&
             (back.c_cflag & PARENB) != 0 && (back.c_cflag & PARODD) != 0;

  tcsetattr(fd, TCSANOW, &saved);
  if (tcgetattr(fd, &back) == 0 && (back.c_cflag & CBAUD) != B38400)
    good = 0;
  close(fd);

  if (!good) {
    fail("serial-line", (long)back.c_cflag);
    return;
  }
  ok("serial-line");
}

static void test_serial_badbaud(void) {
  int fd = open(TTY, O_RDWR | O_NOCTTY);
  if (fd < 0) {
    fail("serial-badbaud", -1);
    return;
  }
  struct termios saved, t;
  if (tcgetattr(fd, &saved) != 0) {
    fail("serial-badbaud", -2);
    close(fd);
    return;
  }
  t = saved;
  t.c_cflag &= ~CBAUD;
  t.c_cflag |= B230400; /* 115200 / 230400 is not a whole divisor */
  errno = 0;
  int rc = tcsetattr(fd, TCSANOW, &t);
  int err = errno;

  struct termios back;
  int unchanged = tcgetattr(fd, &back) == 0 &&
                  (back.c_cflag & CBAUD) == (saved.c_cflag & CBAUD);
  tcsetattr(fd, TCSANOW, &saved);
  close(fd);

  if (rc != -1 || err != EINVAL || !unchanged) {
    fail("serial-badbaud", (long)rc);
    return;
  }
  ok("serial-badbaud");
}

static void test_serial_modem(void) {
  int fd = open(TTY, O_RDWR | O_NOCTTY);
  if (fd < 0) {
    fail("serial-modem", -1);
    return;
  }
  int bits = 0;
  if (ioctl(fd, TIOCMGET, &bits) != 0) {
    fail("serial-modem", -2);
    close(fd);
    return;
  }
  /* The boot code raises DTR and RTS on every detected line. */
  if (!(bits & TIOCM_DTR) || !(bits & TIOCM_RTS)) {
    fail("serial-modem", (long)bits);
    close(fd);
    return;
  }

  int drop = TIOCM_RTS;
  if (ioctl(fd, TIOCMBIC, &drop) != 0 || ioctl(fd, TIOCMGET, &bits) != 0 ||
      (bits & TIOCM_RTS) || !(bits & TIOCM_DTR)) {
    fail("serial-modem", (long)bits);
    ioctl(fd, TIOCMBIS, &drop);
    close(fd);
    return;
  }
  if (ioctl(fd, TIOCMBIS, &drop) != 0 || ioctl(fd, TIOCMGET, &bits) != 0 ||
      !(bits & TIOCM_RTS)) {
    fail("serial-modem", (long)bits);
    close(fd);
    return;
  }
  close(fd);
  ok("serial-modem");
}

static void test_serial_setserial(void) {
  int fd = open(TTY, O_RDWR | O_NOCTTY);
  if (fd < 0) {
    fail("serial-setserial", -1);
    return;
  }
  struct serial_struct_u ss;
  memset(&ss, 0, sizeof(ss));
  if (ioctl(fd, TIOCGSERIAL, &ss) != 0) {
    fail("serial-setserial", -2);
    close(fd);
    return;
  }
  /* COM2 on a PC: 0x2f8 on IRQ 3, off a 115200 clock. */
  if (ss.port != 0x2f8 || ss.irq != 3 || ss.baud_base != 115200 ||
      ss.line != 1) {
    fail("serial-setserial", (long)ss.port);
    close(fd);
    return;
  }
  /* The divisor it reports has to agree with the baud rate termios reports. */
  struct termios t;
  if (tcgetattr(fd, &t) != 0 || (t.c_cflag & CBAUD) != B38400 ||
      ss.custom_divisor != 3) {
    fail("serial-setserial", (long)ss.custom_divisor);
    close(fd);
    return;
  }
  /* None of it is changeable on a legacy COM port, and saying otherwise would
   * be a lie the caller acts on. */
  errno = 0;
  if (ioctl(fd, TIOCSSERIAL, &ss) != -1 || errno != EPERM) {
    fail("serial-setserial", -3);
    close(fd);
    return;
  }
  close(fd);
  ok("serial-setserial");
}

/* ── derived limits ─────────────────────────────────────────────────────────
 * These two checks exist to show that a ceiling which used to be compiled in is
 * gone. Each deliberately goes PAST the old constant: passing them is only
 * possible on a kernel that derives the limit at runtime. */

/* The mount table held 64 entries globally, and a boot already spends a third
 * of them, so this mounts well past 64 tmpfs instances at once. On the old
 * kernel the 65th mount(2) returned ENOMEM. */
#define MOUNTS_WANTED 96

static void test_mounts_past_64(void) {
  char dir[64];
  int made = 0;
  int i;

  if (mkdir("/tmp/mntcap", 0755) != 0 && errno != EEXIST) {
    fail("mounts-past-64", -1);
    return;
  }

  for (i = 0; i < MOUNTS_WANTED; i++) {
    snprintf(dir, sizeof(dir), "/tmp/mntcap/%d", i);
    if (mkdir(dir, 0755) != 0 && errno != EEXIST)
      break;
    if (mount("none", dir, "tmpfs", 0, NULL) != 0)
      break;
    made++;
  }

  /* A mount is only real if the filesystem underneath it works, so write into
   * the deepest one rather than trusting the return value alone. */
  int usable = 0;

  if (made == MOUNTS_WANTED) {
    char probe[80];
    snprintf(probe, sizeof(probe), "/tmp/mntcap/%d/f", made - 1);
    int fd = open(probe, O_CREAT | O_RDWR, 0644);
    if (fd >= 0) {
      usable = (write(fd, "x", 1) == 1);
      close(fd);
    }
  }

  for (i = made - 1; i >= 0; i--) {
    snprintf(dir, sizeof(dir), "/tmp/mntcap/%d", i);
    umount(dir);
    rmdir(dir);
  }
  rmdir("/tmp/mntcap");

  if (made == MOUNTS_WANTED && usable)
    ok("mounts-past-64");
  else {
    note("mounted %d of %d, usable=%d", made, MOUNTS_WANTED, usable);
    fail("mounts-past-64", made);
  }
}

/* The filesystem folded at most 64 adjacent blocks into one device request —
 * 256 KiB at a 4 KiB block size. This writes a file well past that, reads it
 * back in ONE read(2), and checks every byte: the run length is now derived
 * from what a single command to the device can carry, so the read is served by
 * fewer, larger requests, and the data must still be exactly right. */
/* 512 KiB. The scratch ext4 is 4 MiB, so this is as large as fits comfortably
 * beside the other checks' files — and it is still many times the old 64-block
 * bound at any block size mke2fs picks for a volume this small. */
#define RUN_BYTES (512 * 1024)

static void test_fs_run_past_64(void) {
  /* On the real ext4 volume, not /tmp: run coalescing is a property of the
   * disk filesystem, and a tmpfs would prove nothing about it. */
  if (mount(NVME_NAME, MNT, "ext4", 0, NULL) != 0) {
    fail("fs-run-past-64", -1);
    return;
  }

  const char *path = MNT "/m109_runcap";
  unsigned char *w = malloc(RUN_BYTES);
  unsigned char *r = malloc(RUN_BYTES);

  if (!w || !r) {
    free(w);
    free(r);
    umount(MNT);
    fail("fs-run-past-64", -1);
    return;
  }
  /* A pattern that changes every byte AND every block, so a run stitched
   * together out of order, or short by a block, cannot compare equal. */
  for (int i = 0; i < RUN_BYTES; i++)
    w[i] = (unsigned char)((i * 31) ^ (i >> 12));

  int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
  if (fd < 0) {
    free(w);
    free(r);
    umount(MNT);
    fail("fs-run-past-64", -1);
    return;
  }

  ssize_t wrote = write(fd, w, RUN_BYTES);
  close(fd);

  ssize_t got = -1;
  int same = 0;

  /* Unmount and remount between the write and the read so the read cannot be
   * served out of the cache the write left behind — it has to come off the
   * disk, through the coalescing path this check is about. */
  if (wrote == RUN_BYTES && umount(MNT) == 0 &&
      mount(NVME_NAME, MNT, "ext4", 0, NULL) == 0) {
    fd = open(path, O_RDONLY);
    if (fd >= 0) {
      got = read(fd, r, RUN_BYTES);
      close(fd);
      same = (got == RUN_BYTES) && (memcmp(w, r, RUN_BYTES) == 0);
    }
  }

  unlink(path);
  umount(MNT);
  free(w);
  free(r);

  if (same)
    ok("fs-run-past-64");
  else {
    note("wrote %ld read %ld of %d", (long)wrote, (long)got, RUN_BYTES);
    fail("fs-run-past-64", (long)got);
  }
}

int main(void) {
  marker("M109-SMOKE: start");

  test_packet_socket();
  test_packet_tx_rx();
  test_packet_rx_inbound();
  test_packet_filter();
  test_packet_dgram();

  test_gretap();
  test_vlan();
  test_bridge();
  test_bond();
  test_vnet_lifecycle();

  test_pivot_root();
  test_pivot_root_errno();

  test_blkid_probe();
  test_sysfs_ident();

  test_uts();
  test_mount_ns();
  test_pid_ns();
  test_veth_pair();
  test_net_ns();
  test_net_ns_ipv4();
  test_unlink_enoent();

  test_dev_nodes_listed();
  test_dir_merge_no_dups();
  test_findfs_uuid();
  test_findfs_label();
  test_blkid();
  test_mount_move();

  test_attr_roundtrip();
  test_attr_immutable();
  test_attr_append();
  test_attr_persist();
  test_attr_applets();

  test_discard_support();
  test_discard_virtio();
  test_discard_zeroout();
  test_discard_applet();

  test_fstrim();

  test_ioprio();
  test_ioprio_applet();

  test_serial_line();
  test_serial_badbaud();
  test_serial_modem();
  test_serial_setserial();

  test_mounts_past_64();
  test_fs_run_past_64();

  marker(g_fail ? "M109-SMOKE: done with failures" : "M109-SMOKE: done");
  return g_fail ? 1 : 0;
}
