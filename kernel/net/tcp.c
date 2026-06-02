#include <b1nix/console.h>
#include <b1nix/mm.h>
#include <b1nix/net.h>
#include <b1nix/sched.h>
#include <b1nix/posix.h>
#include <string.h>

/* TCP protocol number in IPv4 */
#define IP_PROTO_TCP 6

/* TCP flags */
#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10
#define TCP_URG 0x20

/* TCP states */
#define TCP_CLOSED 0
#define TCP_LISTEN 1
#define TCP_SYN_SENT 2
#define TCP_SYN_RECEIVED 3
#define TCP_ESTABLISHED 4
#define TCP_FIN_WAIT1 5
#define TCP_FIN_WAIT2 6
#define TCP_CLOSE_WAIT 7
#define TCP_CLOSING 8
#define TCP_LAST_ACK 9
#define TCP_TIME_WAIT 10

struct tcp_header {
  u16 src_port;
  u16 dst_port;
  u32 seq_num;
  u32 ack_num;
  u8 data_offset; /* upper 4 bits = header length in 32-bit words */
  u8 flags;
  u16 window;
  u16 checksum;
  u16 urgent;
} __attribute__((packed));

/* TCP pseudo-header for checksum calculation */
struct tcp_pseudo {
  struct ipv4_addr src;
  struct ipv4_addr dst;
  u8 zero;
  u8 protocol;
  u16 tcp_length;
} __attribute__((packed));

/* 32, not 16: b1nix now runs a fork-per-connection SSH daemon. A single SSH
 * session occupies several slots at once (client conn + server listener +
 * accepted child conn), and a SIGKILLed client/server leaves connections that
 * sit in TIME_WAIT for ~2s, so the M32b SSH smoke's three back-to-back logins
 * plus the white-box kernel TCP tests that run right after would otherwise
 * exhaust a 16-slot table and fail to allocate (tcp_accept -> NULL). */
#define MAX_TCP_CONNS 32
#define TCP_RECV_BUF_SIZE 4096
#define TCP_SEND_BUF_SIZE 4096
#define TCP_MSS 1460
#define TCP_TIME_WAIT_TICKS 200

struct tcp_retransmit_pkt {
  u8 *data;
  usize len;
  u32 seq;
  u64 timestamp;
  int retries;
  struct tcp_retransmit_pkt *next;
};

struct tcp_conn {
  int used;
  int state;
  u8 family; /* B1NIX_AF_INET or B1NIX_AF_INET6 */
  struct ipv4_addr remote_ip;
  struct in6_addr_k remote_ip6;
  u16 remote_port;
  u16 local_port;
  u32 snd_una; /* oldest unacked sequence number */
  u32 snd_nxt; /* next sequence number to send */
  u32 rcv_nxt; /* next expected receive sequence number */
  u32 iss;     /* initial send sequence number */
  u32 irs;     /* initial receive sequence number */
  /* M32: sliding-window flow control + Reno-shaped congestion control.
   *
   *   snd_wnd  — peer's advertised receive window from the incoming
   *              TCP header (bytes peer is willing to buffer). Caps
   *              outbound sends from this side.
   *   cwnd     — congestion window. Slow-start ramps it by 1 MSS per
   *              ACK; on retransmit timeout / 3 dup-acks it halves.
   *   ssthresh — slow-start threshold. Below ssthresh we're in slow
   *              start (exponential); at/above we're in congestion
   *              avoidance (additive). Initialised to 64 KB.
   *   dup_acks — count of consecutive identical ACKs from the peer.
   *              Used to trigger fast retransmit at 3.
   *
   * The actual send path honours min(cwnd, snd_wnd); the framework is
   * load-bearing for any future TCP work even though the current smoke
   * doesn't drive enough traffic to exercise window throttling.
   */
  u32 snd_wnd;
  u32 cwnd;
  u32 ssthresh;
  int dup_acks;
  u8 recv_buf[TCP_RECV_BUF_SIZE];
  u32 recv_len;
  u32 recv_read;
  int handed_to_user;
  u64 time_wait_since;
  struct tcp_retransmit_pkt *retransmit_queue;
};

static struct tcp_conn tcp_conns[MAX_TCP_CONNS];
static u16 next_local_port = 1025;
static u32 tcp_iss_counter = 0;
static volatile int tcp_queue_lock;

static u64 irq_save(void) {
  u64 flags;
  __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
  return flags;
}

static void irq_restore(u64 flags) {
  __asm__ volatile("pushq %0; popfq" : : "r"(flags) : "memory", "cc");
}

/* Defined in kernel/arch/x86/tlb.c. tcp_lock() is always taken with IRQs
 * disabled (every caller wraps it in irq_save()/irq_restore()), so a CPU
 * spinning here cannot take the cross-CPU TLB-shootdown IPI. Without draining
 * shootdowns explicitly the initiator (also waiting IRQs-off) deadlocks —
 * the same failure that bit spinlock.h/rwlock.h/bkl.c/page_cache.c. */
void tlb_shootdown_poll(void);

static void tcp_lock(void) {
  while (__atomic_test_and_set(&tcp_queue_lock, __ATOMIC_ACQUIRE)) {
    __asm__ volatile("pause");
    tlb_shootdown_poll();
  }
}

static void tcp_unlock(void) {
  __atomic_clear(&tcp_queue_lock, __ATOMIC_RELEASE);
}

static void tcp_free_retransmit_list(struct tcp_retransmit_pkt *head) {
  while (head) {
    struct tcp_retransmit_pkt *next = head->next;
    kfree(head->data);
    kfree(head);
    head = next;
  }
}

static void tcp_clear_retransmit_queue(struct tcp_conn *conn) {
  struct tcp_retransmit_pkt *detached = 0;
  u64 irq = irq_save();
  tcp_lock();
  detached = conn->retransmit_queue;
  conn->retransmit_queue = 0;
  tcp_unlock();
  irq_restore(irq);
  tcp_free_retransmit_list(detached);
}

static void tcp_enter_time_wait(struct tcp_conn *conn) {
  conn->state = TCP_TIME_WAIT;
  conn->time_wait_since = scheduler_get_uptime_ticks();
}

static u16 bswap16(u16 v) { return (u16)((v << 8) | (v >> 8)); }
static u32 bswap32(u32 v) {
  return (u32)(((v & 0xFF) << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) |
               ((v >> 24) & 0xFF));
}

static u16 tcp_checksum(struct ipv4_addr src, struct ipv4_addr dst,
                        const void *tcp_data, usize tcp_len) {
  struct tcp_pseudo pseudo;
  pseudo.src = src;
  pseudo.dst = dst;
  pseudo.zero = 0;
  pseudo.protocol = 6; /* TCP */
  pseudo.tcp_length = bswap16((u16)tcp_len);

  u32 sum = 0;
  const u8 *p = (const u8 *)&pseudo;
  for (usize i = 0; i + 1 < sizeof(pseudo); i += 2)
    sum += ((u16)p[i] << 8) | p[i + 1];

  p = (const u8 *)tcp_data;
  for (usize i = 0; i + 1 < tcp_len; i += 2)
    sum += ((u16)p[i] << 8) | p[i + 1];

  if ((tcp_len & 1) != 0)
    sum += (u16)p[tcp_len - 1] << 8;

  while ((sum >> 16) != 0)
    sum = (sum & 0xffff) + (sum >> 16);

  return (u16)~sum;
}

/* TCP-over-IPv6 checksum: ones'-complement sum over the IPv6 pseudo-header
 * (src, dst, 32-bit TCP length, next header 6) and the segment. */
static u16 tcp6_checksum(struct in6_addr_k src, struct in6_addr_k dst,
                         const void *tcp_data, usize tcp_len) {
  u32 sum = 0;
  for (int i = 0; i < 16; i += 2)
    sum += ((u16)src.bytes[i] << 8) | src.bytes[i + 1];
  for (int i = 0; i < 16; i += 2)
    sum += ((u16)dst.bytes[i] << 8) | dst.bytes[i + 1];
  sum += (u16)(tcp_len >> 16);
  sum += (u16)(tcp_len & 0xffff);
  sum += 6; /* next header = TCP */
  const u8 *p = (const u8 *)tcp_data;
  for (usize i = 0; i + 1 < tcp_len; i += 2)
    sum += ((u16)p[i] << 8) | p[i + 1];
  if ((tcp_len & 1) != 0)
    sum += (u16)p[tcp_len - 1] << 8;
  while ((sum >> 16) != 0)
    sum = (sum & 0xffff) + (sum >> 16);
  return (u16)~sum;
}

/* Raw L3 transmit of an already-formed TCP segment, choosing IPv4 or IPv6
 * by address family. Does not touch the checksum (used for retransmits). */
static void tcp_l3_send(u8 family, struct ipv4_addr v4,
                        const struct in6_addr_k *v6, const void *pkt,
                        usize len) {
  if (family == B1NIX_AF_INET6)
    ipv6_send(*v6, IP_PROTO_TCP, pkt, len);
  else
    ipv4_send(v4, IP_PROTO_TCP, pkt, len);
}

/* Fill in the TCP checksum for the given address family. For the IPv6 ::1
 * loopback path the source address equals the destination. */
static void tcp_set_checksum(u8 family, struct ipv4_addr v4,
                             const struct in6_addr_k *v6, u8 *pkt, usize len) {
  struct tcp_header *t = (struct tcp_header *)pkt;
  t->checksum = 0;
  /* The checksum field is network byte order, like every other on-wire field.
   * Loopback RX does not verify it, but QEMU slirp/NAT and real peers drop a
   * segment with a wrong checksum — which is why external TCP timed out while
   * UDP (which already byte-swaps) worked. */
  if (family == B1NIX_AF_INET6)
    t->checksum = bswap16(tcp6_checksum(*v6, *v6, pkt, len));
  else
    t->checksum = bswap16(tcp_checksum(net_get_ip(), v4, pkt, len));
}

/* Checksum + transmit a segment to a connection's peer. */
static void tcp_conn_emit(struct tcp_conn *conn, u8 *pkt, usize len) {
  tcp_set_checksum(conn->family, conn->remote_ip, &conn->remote_ip6, pkt, len);
  tcp_l3_send(conn->family, conn->remote_ip, &conn->remote_ip6, pkt, len);
}

/* ── Allocate local port ── */
static u16 tcp_alloc_port(void) {
  u16 port = next_local_port++;
  if (next_local_port < 1025)
    next_local_port = 1025;
  return port;
}

/* ── Find connection by remote (family-aware) ── */
static struct tcp_conn *tcp_find_conn_af(u8 family, struct ipv4_addr v4,
                                         const struct in6_addr_k *v6,
                                         u16 remote_port, u16 local_port) {
  for (int i = 0; i < MAX_TCP_CONNS; i++) {
    if (!tcp_conns[i].used)
      continue;
    if (tcp_conns[i].remote_port != remote_port ||
        tcp_conns[i].local_port != local_port ||
        tcp_conns[i].family != family)
      continue;
    if (family == B1NIX_AF_INET6) {
      if (memcmp(&tcp_conns[i].remote_ip6, v6, 16) == 0)
        return &tcp_conns[i];
    } else if (memcmp(&tcp_conns[i].remote_ip, &v4, 4) == 0) {
      return &tcp_conns[i];
    }
  }
  return 0;
}

static struct tcp_conn *tcp_find_conn(struct ipv4_addr remote_ip,
                                      u16 remote_port, u16 local_port) {
  return tcp_find_conn_af(B1NIX_AF_INET, remote_ip, 0, remote_port, local_port);
}

static void tcp_queue_retransmit(struct tcp_conn *conn, const void *packet,
                                 usize len, u32 seq) {
  struct tcp_retransmit_pkt *rp = kmalloc(sizeof(struct tcp_retransmit_pkt));
  if (!rp)
    return;
  rp->data = kmalloc(len);
  if (!rp->data) {
    kfree(rp);
    return;
  }
  memcpy(rp->data, packet, len);
  rp->len = len;
  rp->seq = seq;
  rp->timestamp = scheduler_get_uptime_ticks();
  rp->retries = 0;
  rp->next = 0;

  u64 irq = irq_save();
  tcp_lock();
  struct tcp_retransmit_pkt **prev = &conn->retransmit_queue;
  while (*prev)
    prev = &(*prev)->next;
  *prev = rp;
  tcp_unlock();
  irq_restore(irq);
}

static struct tcp_conn *tcp_connect_start_af(u8 family, struct ipv4_addr v4,
                                             struct in6_addr_k v6,
                                             u16 dst_port) {
  struct tcp_conn *conn = 0;
  for (int i = 0; i < MAX_TCP_CONNS; i++) {
    if (!tcp_conns[i].used) {
      conn = &tcp_conns[i];
      break;
    }
  }
  if (!conn) {
    console_write("tcp: no free connection slots\n");
    return 0;
  }

  memset(conn, 0, sizeof(*conn));
  conn->used = 1;
  conn->state = TCP_CLOSED;
  conn->family = family;
  conn->remote_ip = v4;
  conn->remote_ip6 = v6;
  conn->remote_port = dst_port;
  conn->local_port = tcp_alloc_port();

  tcp_iss_counter += 1000;
  conn->iss = tcp_iss_counter;
  conn->snd_una = conn->iss;
  conn->snd_nxt = conn->iss;
  /* M32: initial flow/congestion-control state. */
  conn->snd_wnd = TCP_RECV_BUF_SIZE;  /* assume peer advertises >=1 segment */
  conn->cwnd = TCP_MSS;                /* slow start: 1 MSS */
  conn->ssthresh = 65535;
  conn->dup_acks = 0;

  u8 packet[sizeof(struct tcp_header)];
  memset(packet, 0, sizeof(packet));
  struct tcp_header *tcp = (struct tcp_header *)packet;
  tcp->src_port = bswap16(conn->local_port);
  tcp->dst_port = bswap16(dst_port);
  tcp->seq_num = bswap32(conn->iss);
  tcp->ack_num = 0;
  tcp->data_offset = (5 << 4);
  tcp->flags = TCP_SYN;
  tcp->window = bswap16(TCP_RECV_BUF_SIZE);

  conn->state = TCP_SYN_SENT;
  tcp_conn_emit(conn, packet, sizeof(packet));
  tcp_queue_retransmit(conn, packet, sizeof(packet), conn->iss);
  return conn;
}

/* Drive a freshly-started connection to ESTABLISHED by polling. For a
 * loopback peer the SYN/SYN-ACK/ACK all complete synchronously inside
 * tcp_connect_start_af (so this loop exits on the first iteration); for an
 * off-link peer we must actually wait out the round-trip, hence the per-poll
 * sleep — without it the loop spins to exhaustion in microseconds. */
static struct tcp_conn *tcp_connect_wait(struct tcp_conn *conn) {
  if (!conn)
    return 0;
  for (int tries = 0; tries < 400 && conn->state == TCP_SYN_SENT; tries++) {
    net_poll();
    if (conn->state != TCP_SYN_SENT)
      break;
    scheduler_sleep_ticks(1);
  }
  if (conn->state != TCP_ESTABLISHED) {
    console_write("tcp: connect failed\n");
    conn->used = 0;
    return 0;
  }
  console_write("tcp: connected\n");
  return conn;
}

/* ── Create new TCP connection (active open) ── */
struct tcp_conn *tcp_connect(struct ipv4_addr dst_ip, u16 dst_port) {
  struct in6_addr_k z;
  memset(&z, 0, sizeof(z));
  return tcp_connect_wait(
      tcp_connect_start_af(B1NIX_AF_INET, dst_ip, z, dst_port));
}

struct tcp_conn *tcp_connect_async(struct ipv4_addr dst_ip, u16 dst_port) {
  struct in6_addr_k z;
  memset(&z, 0, sizeof(z));
  return tcp_connect_start_af(B1NIX_AF_INET, dst_ip, z, dst_port);
}

struct tcp_conn *tcp_connect6(struct in6_addr_k dst_ip6, u16 dst_port) {
  struct ipv4_addr z4;
  memset(&z4, 0, sizeof(z4));
  return tcp_connect_wait(
      tcp_connect_start_af(B1NIX_AF_INET6, z4, dst_ip6, dst_port));
}

int tcp_is_established(struct tcp_conn *conn) {
  if (!conn || !conn->used)
    return 0;
  return conn->state == TCP_ESTABLISHED;
}

int tcp_is_readable(struct tcp_conn *conn) {
  if (!conn || !conn->used)
    return 1;
  if (conn->recv_len > conn->recv_read)
    return 1;
  if (conn->state == TCP_CLOSE_WAIT || conn->state == TCP_CLOSED ||
      conn->state == TCP_TIME_WAIT || conn->state == TCP_LAST_ACK ||
      conn->state == TCP_CLOSING) {
    return 1;
  }
  return 0;
}

int tcp_is_close_wait(struct tcp_conn *conn) {
  if (!conn || !conn->used)
    return 0;
  return conn->state == TCP_CLOSE_WAIT;
}

int tcp_is_closed(struct tcp_conn *conn) {
  if (!conn || !conn->used)
    return 1;
  return conn->state != TCP_ESTABLISHED && conn->state != TCP_CLOSE_WAIT;
}


/* ── TCP Listen ── */
int tcp_listen(u16 local_port, int backlog) {
  (void)backlog;
  for (int i = 0; i < MAX_TCP_CONNS; i++) {
    if (!tcp_conns[i].used) {
      memset(&tcp_conns[i], 0, sizeof(struct tcp_conn));
      tcp_conns[i].used = 1;
      tcp_conns[i].state = TCP_LISTEN;
      tcp_conns[i].local_port = local_port;
      return 0;
    }
  }
  return -1;
}

/* ── Check for pending connections (for poll) ── */
int tcp_pending_connections(u16 local_port) {
  for (int i = 0; i < MAX_TCP_CONNS; i++) {
    if (tcp_conns[i].used && tcp_conns[i].state == TCP_ESTABLISHED &&
        tcp_conns[i].local_port == local_port && !tcp_conns[i].handed_to_user) {
      return 1;
    }
  }
  return 0;
}

/* ── TCP Accept ── */
struct tcp_conn *tcp_accept(u16 local_port, struct ipv4_addr *client_ip,
                            u16 *client_port) {
  for (int i = 0; i < MAX_TCP_CONNS; i++) {
    if (tcp_conns[i].used && tcp_conns[i].state == TCP_ESTABLISHED &&
        tcp_conns[i].local_port == local_port &&
        tcp_conns[i].family == B1NIX_AF_INET &&
        !tcp_conns[i].handed_to_user) {
      tcp_conns[i].handed_to_user = 1;
      if (client_ip)
        *client_ip = tcp_conns[i].remote_ip;
      if (client_port)
        *client_port = tcp_conns[i].remote_port;
      return &tcp_conns[i];
    }
  }
  return 0;
}

struct tcp_conn *tcp_accept6(u16 local_port, struct in6_addr_k *client_ip6,
                             u16 *client_port) {
  for (int i = 0; i < MAX_TCP_CONNS; i++) {
    if (tcp_conns[i].used && tcp_conns[i].state == TCP_ESTABLISHED &&
        tcp_conns[i].local_port == local_port &&
        tcp_conns[i].family == B1NIX_AF_INET6 &&
        !tcp_conns[i].handed_to_user) {
      tcp_conns[i].handed_to_user = 1;
      if (client_ip6)
        *client_ip6 = tcp_conns[i].remote_ip6;
      if (client_port)
        *client_port = tcp_conns[i].remote_port;
      return &tcp_conns[i];
    }
  }
  return 0;
}

/* ── TCP send data ── */
int tcp_send(struct tcp_conn *conn, const void *data, usize len) {
  if (!conn || conn->state != TCP_ESTABLISHED)
    return -1;
  if (len == 0)
    return 0;
  u32 seq_start = conn->snd_nxt;

  /* M32 sliding-window flow control: never put more bytes in flight than the
   * smaller of the peer's advertised receive window (snd_wnd) and our own
   * congestion window (cwnd). bytes-in-flight is snd_nxt - snd_una. When the
   * window is full we send nothing and return 0 so the caller retries once an
   * incoming ACK advances snd_una (and refreshes snd_wnd). */
  u32 window = conn->snd_wnd < conn->cwnd ? conn->snd_wnd : conn->cwnd;
  u32 inflight = conn->snd_nxt - conn->snd_una;
  if (inflight >= window)
    return 0;
  u32 usable = window - inflight;

  usize to_send = len;
  if (to_send > TCP_MSS)
    to_send = TCP_MSS;
  if (to_send > usable)
    to_send = usable;
  if (to_send == 0)
    return 0;

  usize packet_len = sizeof(struct tcp_header) + to_send;
  u8 *packet = kzalloc(packet_len);
  if (!packet)
    return -1;

  struct tcp_header *tcp = (struct tcp_header *)packet;
  tcp->src_port = bswap16(conn->local_port);
  tcp->dst_port = bswap16(conn->remote_port);
  tcp->seq_num = bswap32(conn->snd_nxt);
  tcp->ack_num = bswap32(conn->rcv_nxt);
  tcp->data_offset = (5 << 4);
  tcp->flags = TCP_PSH | TCP_ACK;
  tcp->window = bswap16(TCP_RECV_BUF_SIZE - conn->recv_len);

  memcpy(packet + sizeof(struct tcp_header), data, to_send);

  conn->snd_nxt += (u32)to_send;

  tcp_conn_emit(conn, packet, packet_len);
  tcp_queue_retransmit(conn, packet, packet_len, seq_start);
  kfree(packet);

  return (int)to_send;
}

/* ── TCP receive data (non-blocking) ── */
int tcp_recv(struct tcp_conn *conn, void *buf, usize max_len, int flags) {
  if (!conn)
    return -1;
  if (conn->recv_read >= conn->recv_len) {
    /* Poll for new data */
    net_poll();
  }
  if (conn->recv_read >= conn->recv_len)
    return 0;

  usize avail = conn->recv_len - conn->recv_read;
  if (avail > max_len)
    avail = max_len;

  memcpy(buf, conn->recv_buf + conn->recv_read, avail);
  if (!(flags & B1NIX_MSG_PEEK)) {
    conn->recv_read += (u32)avail;

    /* Compact buffer if all read */
    if (conn->recv_read >= conn->recv_len) {
      conn->recv_len = 0;
      conn->recv_read = 0;
    }
  }

  return (int)avail;
}

/* ── TCP close ── */
int tcp_close(struct tcp_conn *conn) {
  if (!conn || !conn->used)
    return -1;

  if (conn->state == TCP_CLOSE_WAIT) {
    conn->state = TCP_LAST_ACK;
  }

  /* Send FIN */
  u8 packet[sizeof(struct tcp_header)];
  memset(packet, 0, sizeof(packet));
  struct tcp_header *tcp = (struct tcp_header *)packet;
  tcp->src_port = bswap16(conn->local_port);
  tcp->dst_port = bswap16(conn->remote_port);
  tcp->seq_num = bswap32(conn->snd_nxt);
  tcp->ack_num = bswap32(conn->rcv_nxt);
  tcp->data_offset = (5 << 4);
  tcp->flags = TCP_FIN | TCP_ACK;
  tcp->window = bswap16(TCP_RECV_BUF_SIZE);

  u32 seq_start = conn->snd_nxt;
  conn->state = TCP_FIN_WAIT1;
  conn->snd_nxt++;

  tcp_conn_emit(conn, packet, sizeof(packet));
  tcp_queue_retransmit(conn, packet, sizeof(packet), seq_start);

  /* Wait for FIN-ACK (poll a bit) */
  for (int tries = 0; tries < 50 && conn->state != TCP_CLOSED &&
                          conn->state != TCP_TIME_WAIT; tries++) {
    net_poll();
  }

  if (conn->state == TCP_CLOSED) {
    tcp_clear_retransmit_queue(conn);
    conn->used = 0;
  } else if (conn->state != TCP_TIME_WAIT) {
    tcp_enter_time_wait(conn);
  }
  return 0;
}

/* ── Receive TCP segment from IP layer (family-aware core) ── */
static void tcp_input(u8 family, struct ipv4_addr v4src,
                      struct in6_addr_k v6src, const void *data, usize size) {
  if (size < sizeof(struct tcp_header))
    return;

  const struct tcp_header *tcp = (const struct tcp_header *)data;
  u16 src_port = bswap16(tcp->src_port);
  u16 dst_port = bswap16(tcp->dst_port);
  u8 flags = tcp->flags;
  u32 seq = bswap32(tcp->seq_num);
  u32 ack = bswap32(tcp->ack_num);

  usize data_offset = (tcp->data_offset >> 4) * 4;
  if (data_offset < sizeof(struct tcp_header))
    return;

  const u8 *payload = (const u8 *)data + data_offset;
  usize payload_size = (size > data_offset) ? size - data_offset : 0;

  /* Find connection */
  struct tcp_conn *conn =
      tcp_find_conn_af(family, v4src, &v6src, src_port, dst_port);

  /* M32: refresh the peer's advertised window (snd_wnd) on every
   * segment seen on this connection. Sliding-window flow control
   * uses this to throttle sends; without the update we'd keep
   * sending against a stale (often initial) advertisement.
   *
   * Also drive Reno-shaped congestion control:
   *   - new ACK acknowledging fresh data → cwnd grows (slow start
   *     under ssthresh, additive afterwards), dup_acks resets.
   *   - duplicate ACK (same ack, no new data) → dup_acks++.
   *   - third dup ACK → ssthresh = cwnd/2; cwnd = ssthresh
   *     (entered fast recovery; the existing retransmit-on-timeout
   *     path also handles the retransmission). */
  if (conn && (flags & TCP_ACK)) {
    u32 ack_new = bswap32(tcp->ack_num);
    u16 wnd_new = bswap16(tcp->window);
    conn->snd_wnd = wnd_new;
    if (ack_new == conn->snd_una && payload_size == 0) {
      conn->dup_acks++;
      if (conn->dup_acks == 3) {
        /* Reno fast retransmit: cut ssthresh, drop cwnd, and resend
         * the oldest unacked segment immediately (don't wait for the
         * RTO). The retransmit queue is FIFO; the head is snd_una. */
        conn->ssthresh = conn->cwnd / 2;
        if (conn->ssthresh < TCP_MSS) conn->ssthresh = TCP_MSS;
        conn->cwnd = conn->ssthresh + 3 * TCP_MSS;  /* inflate per RFC 5681 */
        u64 irq = irq_save();
        tcp_lock();
        struct tcp_retransmit_pkt *head = conn->retransmit_queue;
        u8 *resend_data = 0;
        usize resend_len = 0;
        if (head) {
          resend_data = head->data;
          resend_len = head->len;
          head->timestamp = scheduler_get_uptime_ticks();
          head->retries++;
        }
        tcp_unlock();
        irq_restore(irq);
        if (resend_data && resend_len) {
          tcp_l3_send(conn->family, conn->remote_ip, &conn->remote_ip6,
                      resend_data, resend_len);
        }
      } else if (conn->dup_acks > 3) {
        /* Each additional dup ACK inflates cwnd by 1 MSS during fast
         * recovery (RFC 5681 section 3.2). */
        conn->cwnd += TCP_MSS;
      }
    } else if (ack_new > conn->snd_una) {
      conn->dup_acks = 0;
      if (conn->cwnd < conn->ssthresh) {
        /* Slow start: exponential — +MSS per new ACK. */
        conn->cwnd += TCP_MSS;
      } else {
        /* Congestion avoidance: additive — +MSS²/cwnd per RTT
         * (approximated per-ACK as MSS/cwnd-segments). */
        u32 inc = (TCP_MSS * TCP_MSS) / (conn->cwnd ? conn->cwnd : 1);
        if (inc < 1) inc = 1;
        conn->cwnd += inc;
      }
      if (conn->cwnd > 65535) conn->cwnd = 65535;
    }
  }

  if (!conn && (flags & TCP_SYN)) {
    /* Check for listener */
    for (int i = 0; i < MAX_TCP_CONNS; i++) {
      if (tcp_conns[i].used && tcp_conns[i].state == TCP_LISTEN &&
          tcp_conns[i].local_port == dst_port) {
        /* Found a listener, create a new connection for the client */
        struct tcp_conn *new_conn = 0;
        for (int j = 0; j < MAX_TCP_CONNS; j++) {
          if (!tcp_conns[j].used) {
            new_conn = &tcp_conns[j];
            break;
          }
        }
        if (new_conn) {
          memset(new_conn, 0, sizeof(*new_conn));
          new_conn->used = 1;
          new_conn->state = TCP_SYN_RECEIVED;
          new_conn->family = family;
          new_conn->remote_ip = v4src;
          new_conn->remote_ip6 = v6src;
          new_conn->remote_port = src_port;
          new_conn->local_port = dst_port;

          tcp_iss_counter += 1000;
          new_conn->iss = tcp_iss_counter;
          new_conn->snd_una = new_conn->iss;
          new_conn->snd_nxt = new_conn->iss;
          new_conn->rcv_nxt = seq + 1;
          new_conn->irs = seq;
          /* M32: initialise flow/congestion state on the accepted side too. */
          new_conn->snd_wnd = bswap16(tcp->window);
          new_conn->cwnd = TCP_MSS;
          new_conn->ssthresh = 65535;
          new_conn->dup_acks = 0;

          /* Send SYN-ACK */
          u8 packet[sizeof(struct tcp_header)];
          memset(packet, 0, sizeof(packet));
          struct tcp_header *tcp_hdr = (struct tcp_header *)packet;
          tcp_hdr->src_port = bswap16(new_conn->local_port);
          tcp_hdr->dst_port = bswap16(new_conn->remote_port);
          tcp_hdr->seq_num = bswap32(new_conn->iss);
          tcp_hdr->ack_num = bswap32(new_conn->rcv_nxt);
          tcp_hdr->data_offset = (5 << 4);
          tcp_hdr->flags = TCP_SYN | TCP_ACK;
          tcp_hdr->window = bswap16(TCP_RECV_BUF_SIZE);

          tcp_conn_emit(new_conn, packet, sizeof(packet));
          tcp_queue_retransmit(new_conn, packet, sizeof(packet), new_conn->iss);
        }
        return;
      }
    }
  }

  if (!conn) {
    /* No connection — send RST unless it's a RST itself */
    if (!(flags & TCP_RST)) {
      u8 rst[sizeof(struct tcp_header)];
      memset(rst, 0, sizeof(rst));
      struct tcp_header *r = (struct tcp_header *)rst;
      r->src_port = tcp->dst_port;
      r->dst_port = tcp->src_port;
      r->seq_num = 0;
      r->ack_num = (flags & TCP_ACK)
                       ? ack
                       : bswap32(seq + (u32)payload_size +
                                 ((flags & (TCP_SYN | TCP_FIN)) ? 1 : 0));
      r->data_offset = (5 << 4);
      r->flags = TCP_RST | (flags & TCP_ACK ? TCP_ACK : 0);
      r->window = 0;
      tcp_set_checksum(family, v4src, &v6src, rst, sizeof(rst));
      tcp_l3_send(family, v4src, &v6src, rst, sizeof(rst));
    }
    return;
  }

  switch (conn->state) {
  case TCP_SYN_RECEIVED:
    if (flags & TCP_ACK) {
      if (ack == conn->snd_nxt + 1) {
        conn->snd_una = ack;
        conn->snd_nxt = ack;
        conn->state = TCP_ESTABLISHED;
        extern void *vfs_poll_chan;
        scheduler_wake_all(vfs_poll_chan);
      }
    }
    break;

  case TCP_SYN_SENT:
    /* Expect SYN-ACK */
    if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
      if (ack == conn->snd_nxt + 1 || ack == conn->snd_una + 1 ||
          ack >= conn->iss) {
        conn->irs = seq;
        conn->rcv_nxt = seq + 1;
        conn->snd_una = ack;
        conn->snd_nxt = ack;

        /* Send ACK */
        u8 ack_pkt[sizeof(struct tcp_header)];
        memset(ack_pkt, 0, sizeof(ack_pkt));
        struct tcp_header *a = (struct tcp_header *)ack_pkt;
        a->src_port = bswap16(conn->local_port);
        a->dst_port = bswap16(conn->remote_port);
        a->seq_num = bswap32(conn->snd_nxt);
        a->ack_num = bswap32(conn->rcv_nxt);
        a->data_offset = (5 << 4);
        a->flags = TCP_ACK;
        a->window = bswap16(TCP_RECV_BUF_SIZE);
        tcp_conn_emit(conn, ack_pkt, sizeof(ack_pkt));

        conn->state = TCP_ESTABLISHED;
        extern void *vfs_poll_chan;
        scheduler_wake_all(vfs_poll_chan);
      }
    } else if (flags & TCP_RST) {
      conn->state = TCP_CLOSED;
    }
    break;

  case TCP_ESTABLISHED:
  case TCP_FIN_WAIT1:
  case TCP_FIN_WAIT2:
    /* Check ACK */
    if (flags & TCP_ACK) {
      if (ack > conn->snd_una || ack == conn->snd_una) {
        conn->snd_una = ack;
        struct tcp_retransmit_pkt *detached = 0;
        u64 irq = irq_save();
        tcp_lock();
        while (conn->retransmit_queue &&
               (isize)(ack - conn->retransmit_queue->seq) > 0) {
          struct tcp_retransmit_pkt *rp = conn->retransmit_queue;
          conn->retransmit_queue = rp->next;
          rp->next = detached;
          detached = rp;
        }
        tcp_unlock();
        irq_restore(irq);
        tcp_free_retransmit_list(detached);
      }
    }

    /* Receive data */
    if (payload_size > 0 && (flags & (TCP_PSH | TCP_ACK))) {
      if (seq == conn->rcv_nxt) {
        u32 space = TCP_RECV_BUF_SIZE - conn->recv_len;
        if (payload_size > space)
          payload_size = space;
        memcpy(conn->recv_buf + conn->recv_len, payload, payload_size);
        conn->recv_len += (u32)payload_size;
        conn->rcv_nxt += (u32)payload_size;

        /* Send ACK for data */
        u8 ack_pkt[sizeof(struct tcp_header)];
        memset(ack_pkt, 0, sizeof(ack_pkt));
        struct tcp_header *a = (struct tcp_header *)ack_pkt;
        a->src_port = bswap16(conn->local_port);
        a->dst_port = bswap16(conn->remote_port);
        a->seq_num = bswap32(conn->snd_nxt);
        a->ack_num = bswap32(conn->rcv_nxt);
        a->data_offset = (5 << 4);
        a->flags = TCP_ACK;
        a->window = bswap16(TCP_RECV_BUF_SIZE - conn->recv_len);
        tcp_conn_emit(conn, ack_pkt, sizeof(ack_pkt));
        extern void *vfs_poll_chan;
        scheduler_wake_all(vfs_poll_chan);
      }
    }

    /* Handle FIN */
    if (flags & TCP_FIN) {
      conn->rcv_nxt++;

      /* Send ACK for FIN */
      u8 ack_pkt[sizeof(struct tcp_header)];
      memset(ack_pkt, 0, sizeof(ack_pkt));
      struct tcp_header *a = (struct tcp_header *)ack_pkt;
      a->src_port = bswap16(conn->local_port);
      a->dst_port = bswap16(conn->remote_port);
      a->seq_num = bswap32(conn->snd_nxt);
      a->ack_num = bswap32(conn->rcv_nxt);
      a->data_offset = (5 << 4);
      a->flags = TCP_ACK;
      a->window = 0;
      tcp_conn_emit(conn, ack_pkt, sizeof(ack_pkt));

      if (conn->state == TCP_ESTABLISHED) {
        conn->state = TCP_CLOSE_WAIT;
      } else if (conn->state == TCP_FIN_WAIT1) {
        tcp_enter_time_wait(conn);
      } else if (conn->state == TCP_FIN_WAIT2) {
        tcp_enter_time_wait(conn);
      }
      extern void *vfs_poll_chan;
      scheduler_wake_all(vfs_poll_chan);
    }

    /* Handle RST */
    if (flags & TCP_RST) {
      conn->state = TCP_CLOSED;
      extern void *vfs_poll_chan;
      scheduler_wake_all(vfs_poll_chan);
    }
    break;

  case TCP_CLOSE_WAIT:
    /* Already received FIN from peer, waiting for app to close */
    if (flags & TCP_ACK)
      conn->snd_una = ack;
    break;

  case TCP_LAST_ACK:
    if (flags & TCP_ACK) {
      conn->state = TCP_CLOSED;
    }
    break;

  case TCP_TIME_WAIT:
    if (flags & TCP_FIN) {
      tcp_enter_time_wait(conn);
    }
    break;

  default:
    break;
  }
}

void tcp_receive(struct ipv4_addr src, const void *data, usize size) {
  struct in6_addr_k z;
  memset(&z, 0, sizeof(z));
  tcp_input(B1NIX_AF_INET, src, z, data, size);
}

void tcp6_receive(struct in6_addr_k src, const void *data, usize size) {
  struct ipv4_addr z4;
  memset(&z4, 0, sizeof(z4));
  tcp_input(B1NIX_AF_INET6, z4, src, data, size);
}

/* White-box test hook: return the ISS the kernel chose for the connection
 * matching (remote_ip, remote_port, local_port), or 0 if none. Lets the TCP
 * smoke craft a valid handshake ACK without observing the SYN-ACK on the wire
 * (the in-process equivalent of a peer echoing seq+1). */
u32 tcp_debug_peek_iss(struct ipv4_addr remote_ip, u16 remote_port,
                       u16 local_port) {
  struct tcp_conn *c = tcp_find_conn(remote_ip, remote_port, local_port);
  return c ? c->iss : 0;
}

/* ── Check if network is available ── */
int tcp_network_ready(void) {
  struct ipv4_addr ip = net_get_ip();
  return (ip.bytes[0] != 0 || ip.bytes[1] != 0 || ip.bytes[2] != 0 ||
          ip.bytes[3] != 0);
}

void tcp_timer_tick(void) {
  u64 now = scheduler_get_uptime_ticks();
  for (int i = 0; i < MAX_TCP_CONNS; i++) {
    struct tcp_conn *conn = &tcp_conns[i];
    if (!conn->used)
      continue;

    if (conn->state == TCP_TIME_WAIT) {
      if (now - conn->time_wait_since >= TCP_TIME_WAIT_TICKS) {
        tcp_clear_retransmit_queue(conn);
        conn->used = 0;
      }
      continue;
    }

    u64 irq = irq_save();
    tcp_lock();
    struct tcp_retransmit_pkt *rp = conn->retransmit_queue;
    while (rp) {
      if (now - rp->timestamp >= 50) { // 500ms
        if (rp->retries >= 5) {
          conn->state = TCP_CLOSED;
          conn->used = 0;
          tcp_unlock();
          irq_restore(irq);
          tcp_clear_retransmit_queue(conn);
          irq = irq_save();
          tcp_lock();
          break;
        }
        tcp_l3_send(conn->family, conn->remote_ip, &conn->remote_ip6, rp->data,
                    rp->len);
        rp->timestamp = now;
        rp->retries++;
      }
      rp = rp->next;
    }
    tcp_unlock();
    irq_restore(irq);
  }
}
