#include <b1nix/kprintf.h>
#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/mm.h>
#include <b1nix/net.h>
#include <b1nix/netproto.h>
#include <b1nix/netdev.h>
#include <b1nix/resource_caps.h>
#include <b1nix/sched.h>
#include <b1nix/posix.h>
#include <b1nix/arch.h>
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

/* Compile-time ceiling of the tcp_conns[] array. The number of slots actually
 * used is the runtime cap g_resource_caps.tcp_max_conns (M77), sized from RAM
 * and adjustable via /proc/sys/kernel/tcp-max-conns. The array keeps the
 * ceiling so a sysctl write can raise the cap without reallocation.
 *
 * 32, not 16: b1nix now runs a fork-per-connection SSH daemon. A single SSH
 * session occupies several slots at once (client conn + server listener +
 * accepted child conn), and a SIGKILLed client/server leaves connections that
 * sit in TIME_WAIT for ~2s, so the M32b SSH smoke's three back-to-back logins
 * plus the white-box kernel TCP tests that run right after would otherwise
 * exhaust a 16-slot table and fail to allocate (tcp_accept -> NULL). */
#define MAX_TCP_CONNS_CEIL 256
/* Receive buffer / advertised window. Sized to hold several TLS records so
 * HTTPS handshakes don't have to be drained in small chunks. Combined with the
 * window-update ACK in tcp_recv() (see recv_window_update), this keeps the peer
 * from ever parking on its zero-window persist timer — which previously cost
 * ~5 s per fresh HTTPS connection (zero-window stall on multi-KiB cert
 * flights).
 *
 * M84: the buffer is allocated per connection instead of being an array inside
 * struct tcp_conn. A 64 KiB inline buffer times the 256-slot connection table
 * would be 16 MiB of permanently resident BSS; heap-allocating it costs memory
 * only for live connections, and it is what makes a receive window larger than
 * the unscaled 16-bit maximum — and therefore a non-zero advertised window
 * scale — affordable at all. */
/*
 * A window that grows, the way Linux's does.
 *
 * The window is the ceiling on throughput over any link with latency: a
 * receiver that advertises 64 KiB cannot have more than that in flight, so at
 * 25 ms to a mirror the transfer stops at about 2.5 MB/s however wide the pipe
 * is. Downloading a few hundred megabytes of packages into the guest is now a
 * normal thing to do here, and it was taking half an hour.
 *
 * The window is the ceiling on throughput over a link with latency: a receiver
 * advertising 64 KiB cannot have more than that in flight, which at 25 ms to a
 * mirror stops the transfer at about 2.5 MB/s however wide the pipe is. But a
 * fixed large buffer is paid by every connection that exists, and a browser
 * opens them by the dozen.
 *
 * So the buffer starts at 64 KiB and doubles, up to a megabyte, each time it
 * is filled — a connection that is actually moving bulk data earns its window,
 * an idle one keeps the small buffer. The window *scale* cannot follow: it is
 * negotiated once in the SYN and never changes, so it is chosen for the
 * ceiling from the start, exactly as Linux does with tcp_rmem's maximum.
 *
 * Heap-allocated per connection, so the cost is paid by connections that
 * exist rather than by the image; and it is what makes the advertised window
 * scale below non-zero, which is the only way to express more than 64 KiB.
 */
#define TCP_RECV_BUF_INIT 65536
#define TCP_RECV_BUF_MAX 1048576
#define TCP_RECV_BUF_SIZE TCP_RECV_BUF_MAX
/* The window field of a SYN is never scaled, so it cannot express more than
 * 65535 no matter how large the buffer is. */
#define TCP_SYN_WINDOW                                                         \
  (TCP_RECV_BUF_SIZE > 65535 ? 65535 : TCP_RECV_BUF_SIZE)
#define TCP_SEND_BUF_SIZE 4096
#define TCP_MSS 1460
/* Initial congestion window. RFC 6928 raised it to 10 segments and Linux has
 * shipped that as TCP_INIT_CWND for a decade: one segment per RTT of ramp made
 * every short transfer — a page fetch, an HTTP request — pay several round
 * trips before the link was used at all. */
#define TCP_INIT_CWND (10u * TCP_MSS)
/* Ceiling on the congestion window. The old 65535 was a 64 KiB cap on the data
 * in flight, which pinned throughput to 64 KiB per round trip however large the
 * receiver's window was — it silently undid the 1 MiB receive buffer. Linux has
 * no byte cap of this kind: the window is bounded by the peer's advertised
 * window (already applied at the send site) and by the send buffer. This is
 * that send-buffer bound, matched to the receive buffer we ourselves offer. */
#define TCP_CWND_MAX TCP_RECV_BUF_MAX
/* Smallest receive buffer SO_RCVBUF may set. Linux's SOCK_MIN_RCVBUF is a
 * little over 2 KiB for the same reason: a window below one full segment plus
 * its bookkeeping cannot make forward progress. */
#define TCP_RCVBUF_MIN (2u * TCP_MSS)
#define TCP_TIME_WAIT_TICKS 200

/* ── M84: TCP options ──────────────────────────────────────────────────────
 * Before this the stack neither sent nor parsed a single option: every SYN
 * went out with a bare 20-byte header, so the peer's advertised window was
 * read unscaled (capped at 64 KB no matter what the peer offered) and its MSS
 * was ignored. */
#define TCP_OPT_END       0
#define TCP_OPT_NOP       1
#define TCP_OPT_MSS       2
#define TCP_OPT_WSCALE    3
#define TCP_OPT_SACK_PERM 4
#define TCP_OPT_SACK      5

/* NOP, NOP, kind, len + up to 3 eight-byte blocks. Three is what every real
 * stack emits: it is what fits alongside a timestamp option, and it covers the
 * holes that matter in practice. */
#define TCP_SACK_MAX_BLOCKS 3
#define TCP_SACK_OPT_LEN(n) ((n) ? (4 + 8 * (n)) : 0)
#define TCP_ACK_MAX_LEN                                                        \
  (sizeof(struct tcp_header) + TCP_SACK_OPT_LEN(TCP_SACK_MAX_BLOCKS))

/* MSS(4) + SACK-permitted(2) + window-scale(3) + NOP,NOP,EOL(3) — 12 bytes,
 * 4-byte aligned so the data offset is exactly 8 words. */
#define TCP_SYN_OPT_LEN 12
#define TCP_OPT_EOL_PAD TCP_OPT_END
#define TCP_SYN_HDR_LEN (sizeof(struct tcp_header) + TCP_SYN_OPT_LEN)

/* The shift we advertise: the smallest one that lets our receive buffer be
 * expressed in the 16-bit window field. At 16 KiB that is 0 — we still send
 * the option, because a window-scale option in our SYN is what permits the
 * *peer* to scale its own (potentially much larger) window. */
/* The ladder stopped at 3, which expresses 65535 << 3 = 512 KiB — half of
 * TCP_RECV_BUF_MAX. The top half of a grown buffer could therefore never be
 * advertised, so the peer throttled to a window the receiver had already left
 * behind. It now runs to 7 (8 MiB), the same range Linux covers, and stays
 * within RFC 7323's maximum of 14. */
#define TCP_RCV_WSCALE                                                         \
  (TCP_RECV_BUF_SIZE <= 65535 ? 0                                              \
   : TCP_RECV_BUF_SIZE <= 131071 ? 1                                           \
   : TCP_RECV_BUF_SIZE <= 262143 ? 2                                           \
   : TCP_RECV_BUF_SIZE <= 524287 ? 3                                           \
   : TCP_RECV_BUF_SIZE <= 1048575 ? 4                                          \
   : TCP_RECV_BUF_SIZE <= 2097151 ? 5                                          \
   : TCP_RECV_BUF_SIZE <= 4194303 ? 6                                          \
                                  : 7)

/* Out-of-order reassembly budget. A segment arriving past a hole is buffered
 * here instead of being dropped (which forced the peer into a retransmit
 * timeout for every reordered or single-lost packet). */
/* 16 segments is ~23 KiB of MSS-sized data — a reassembly budget an order of
 * magnitude below the 1 MiB receive buffer it is meant to protect, so the 17th
 * reordered segment on a fast link was dropped and cost the peer a retransmit
 * timeout. The queue is a kmalloc'd linked list, and the byte budget below is
 * the real bound; the segment count only has to be large enough not to be the
 * thing that binds first. Linux bounds its own out-of-order queue by the
 * receive buffer for the same reason. */
#define TCP_OOO_MAX_SEGS 256
#define TCP_OOO_MAX_BYTES TCP_RECV_BUF_SIZE

struct tcp_opts {
  u16 mss;
  u8 wscale;
  u8 has_wscale;
  u8 sack_ok;
  /* SACK blocks carried by this segment (kind 5), left/right edges. */
  u8 nsack;
  u32 sack_left[TCP_SACK_MAX_BLOCKS + 1];
  u32 sack_right[TCP_SACK_MAX_BLOCKS + 1];
};

/* One buffered out-of-order segment. The list is kept sorted by sequence
 * number; overlaps are resolved when it drains, not on insert. */
struct tcp_ooo_seg {
  u32 seq;
  u32 len;
  u8 *data;
  struct tcp_ooo_seg *next;
};

struct tcp_retransmit_pkt {
  u8 *data;
  usize len;
  u32 seq;
  /* M84: sequence space this packet occupies (payload bytes, or 1 for a bare
   * SYN/FIN) — needed to decide whether a SACK block covers it. */
  u32 dlen;
  /* M84: the peer reported this segment as received out of order. It must not
   * be retransmitted, but it stays queued until the cumulative ACK covers it,
   * because SACK information is advisory and may be reneged. */
  u8 sacked;
  /* M84 (RFC 6675): the scoreboard declared this segment lost — enough
   * higher-sequence data has been SACKed that it cannot still be in flight.
   * `retransmitted` keeps the pipe estimate honest after we resend it. */
  u8 lost;
  u8 retransmitted;
  u64 timestamp;
  int retries;
  struct tcp_retransmit_pkt *next;
};

/* Keepalive, as Linux spells it: after `keepidle` seconds with nothing on the
 * connection, probe every `keepintvl` seconds, and give up after `keepcnt`
 * unanswered probes. The defaults are Linux's (2 hours / 75 s / 9). A probe is
 * an ACK carrying one byte less than the next sequence number, which the peer
 * is obliged to answer and which carries no data. */
#define TCP_KEEPIDLE_DEFAULT  7200
#define TCP_KEEPINTVL_DEFAULT 75
#define TCP_KEEPCNT_DEFAULT   9
#define TCP_TICKS_PER_SEC     100

struct tcp_conn {
  int used;
  int state;
  u8 keepalive;         /* SO_KEEPALIVE */
  u8 keepalive_probes;  /* unanswered probes since the last activity */
  u32 keepidle;
  u32 keepintvl;
  u32 keepcnt;
  u64 last_activity;    /* uptime ticks of the last segment either way */
  u8 family; /* B1NIX_AF_INET or B1NIX_AF_INET6 */
  struct ipv4_addr remote_ip;
  struct in6_addr_k remote_ip6;
  u16 remote_port;
  u16 local_port;
  u32 snd_una; /* oldest unacked sequence number */
  u32 snd_nxt; /* next sequence number to send */
  u32 rcv_nxt; /* next expected receive sequence number */
  /* A FIN that arrived ahead of data still missing from the stream.
   *
   * The flag rides on a segment, and that segment can overtake a lost one.
   * Acting on it then declares the stream finished with a hole still in it,
   * which is how large downloads came back a few tens of kilobytes short and
   * looked like corrupt files. Remembered here and honoured once the sequence
   * it sits at is the one being waited for. */
  u32 fin_seq;
  u8 fin_pending;
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
  u8 *recv_buf; /* recv_cap bytes, grown on demand — see tcp_recv_grow() */
  u32 recv_cap; /* current size of recv_buf */
  /* Ceiling on recv_cap. TCP_RECV_BUF_MAX by default, which is what lets the
   * buffer auto-tune; SO_RCVBUF lowers it, and as on Linux setting it is what
   * turns auto-tuning off for that socket. Zero means "not set yet" and is
   * read as the default. */
  u32 recv_cap_max;
  u32 recv_len;
  u32 recv_read;
  u8 wnd_closed; /* last advertised receive window was < 1 MSS (peer throttled) */
  /* M84: negotiated options.
   *   snd_wscale — shift to apply to the peer's advertised window. Non-zero
   *                only when both SYNs carried a window-scale option.
   *   snd_mss    — largest segment the peer accepts (its MSS option, or the
   *                RFC 1122 default of 536 when it sent none).
   *   sack_ok    — peer permitted SACK. We do not emit SACK blocks yet, but
   *                the flag records the negotiation. */
  u8 snd_wscale;
  u8 rcv_wscale;
  u8 wscale_ok;
  u8 sack_ok;
  u16 snd_mss;
  /* M84: SACK scoreboard (RFC 6675) and DSACK (RFC 2883) state.
   *
   *   sacked_bytes   — bytes above snd_una the peer reported as received.
   *   high_sack      — highest right edge ever SACKed in this recovery.
   *   in_recovery    — inside SACK-based loss recovery. While set, cwnd is
   *                    NOT inflated per dup-ACK; transmission is governed by
   *                    the pipe estimate instead.
   *   recovery_point — snd_nxt when recovery started. Recovery ends once the
   *                    cumulative ACK reaches it.
   *   prior_*        — cwnd/ssthresh before recovery, so a DSACK proving the
   *                    retransmission was spurious can undo the reduction.
   *   dsack_*        — a duplicate we owe the peer a D-SACK report for. */
  u32 sacked_bytes;
  u32 high_sack;
  u8 in_recovery;
  u8 dsack_pending;
  u32 recovery_point;
  u32 prior_cwnd;
  u32 prior_ssthresh;
  u32 dsack_left;
  u32 dsack_right;
  u32 dsack_seen;   /* count of D-SACKs received (spurious retransmits) */
  /* Out-of-order reassembly queue, sorted by sequence number. */
  struct tcp_ooo_seg *ooo_queue;
  u32 ooo_bytes;
  u32 ooo_segs;
  int handed_to_user;
  u64 time_wait_since;
  struct tcp_retransmit_pkt *retransmit_queue;
};

static struct tcp_conn tcp_conns[MAX_TCP_CONNS_CEIL];
static u16 next_local_port = 1025;
static u32 tcp_iss_counter = 0;
static volatile int tcp_queue_lock;

static u64 irq_save(void) {
  return interrupts_save();
}

static void irq_restore(u64 flags) {
  interrupts_restore(flags);
}

/* Defined in kernel/arch/x86_64/tlb.c. tcp_lock() is always taken with IRQs
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

static u16 bswap16(u16 v);
static u32 bswap32(u32 v);

/* ── M84: option parsing / emission ──────────────────────────────────────── */

static void tcp_parse_options(const void *segment, usize data_offset,
                              struct tcp_opts *o) {
  o->mss = 0;
  o->wscale = 0;
  o->has_wscale = 0;
  o->sack_ok = 0;
  o->nsack = 0;
  if (data_offset <= sizeof(struct tcp_header))
    return;

  const u8 *p = (const u8 *)segment + sizeof(struct tcp_header);
  usize left = data_offset - sizeof(struct tcp_header);
  while (left > 0) {
    u8 kind = p[0];
    if (kind == TCP_OPT_END)
      break;
    if (kind == TCP_OPT_NOP) {
      p++;
      left--;
      continue;
    }
    if (left < 2)
      break;
    u8 len = p[1];
    /* A length below 2 would not advance the walk (infinite loop), and one
     * past the option area is a malformed segment. */
    if (len < 2 || (usize)len > left)
      break;
    if (kind == TCP_OPT_MSS && len == 4) {
      o->mss = (u16)(((u16)p[2] << 8) | p[3]);
    } else if (kind == TCP_OPT_WSCALE && len == 3) {
      o->has_wscale = 1;
      /* RFC 7323: shifts above 14 must be clamped. */
      o->wscale = p[2] > 14 ? 14 : p[2];
    } else if (kind == TCP_OPT_SACK_PERM && len == 2) {
      o->sack_ok = 1;
    } else if (kind == TCP_OPT_SACK && len >= 10 && ((len - 2) % 8) == 0) {
      u8 n = (u8)((len - 2) / 8);
      if (n > TCP_SACK_MAX_BLOCKS + 1)
        n = TCP_SACK_MAX_BLOCKS + 1;
      for (u8 b = 0; b < n; b++) {
        const u8 *q = p + 2 + b * 8;
        o->sack_left[b] = ((u32)q[0] << 24) | ((u32)q[1] << 16) |
                          ((u32)q[2] << 8) | q[3];
        o->sack_right[b] = ((u32)q[4] << 24) | ((u32)q[5] << 16) |
                           ((u32)q[6] << 8) | q[7];
      }
      o->nsack = n;
    }
    p += len;
    left -= len;
  }
}

/* Fill the TCP_SYN_OPT_LEN option area of a SYN / SYN-ACK. */
static void tcp_build_syn_options(u8 *opt) {
  opt[0] = TCP_OPT_MSS;
  opt[1] = 4;
  opt[2] = (u8)(TCP_MSS >> 8);
  opt[3] = (u8)(TCP_MSS & 0xFF);
  opt[4] = TCP_OPT_SACK_PERM;
  opt[5] = 2;
  opt[6] = TCP_OPT_WSCALE;
  opt[7] = 3;
  opt[8] = (u8)TCP_RCV_WSCALE;
  opt[9] = TCP_OPT_NOP;
  opt[10] = TCP_OPT_NOP;
  opt[11] = TCP_OPT_EOL_PAD;
}

/* The ceiling this connection's receive buffer may grow to: whatever SO_RCVBUF
 * asked for, or the auto-tuning maximum when nothing asked. */
static u32 tcp_recv_cap_max(const struct tcp_conn *conn) {
  return conn->recv_cap_max ? conn->recv_cap_max : TCP_RECV_BUF_MAX;
}

/* The window we advertise, already shifted by our own scale factor. */
static u16 tcp_adv_window(const struct tcp_conn *conn) {
  u32 cap = conn->recv_cap;
  u32 limit = tcp_recv_cap_max(conn);
  /* SO_RCVBUF set below the buffer we already hold still bounds what we invite
   * the peer to send; the buffer itself is not shrunk under live data. */
  if (cap > limit)
    cap = limit;
  u32 free_wnd = cap > conn->recv_len ? cap - conn->recv_len : 0;
  u32 scaled = free_wnd >> conn->rcv_wscale;
  if (scaled > 65535)
    scaled = 65535;
  return (u16)scaled;
}

/* The per-connection receive buffer is heap-allocated (see TCP_RECV_BUF_SIZE).
 * Allocation always happens outside the TCP lock — kmalloc takes the heap lock,
 * and that nesting is what every other send path here avoids. */
static u8 *tcp_alloc_recv_buf(void) { return kmalloc(TCP_RECV_BUF_INIT); }

static void tcp_free_recv_buf(struct tcp_conn *conn) {
  u8 *buf;
  u64 irq = irq_save();
  tcp_lock();
  buf = conn->recv_buf;
  conn->recv_buf = 0;
  conn->recv_cap = 0;
  conn->recv_len = 0;
  conn->recv_read = 0;
  tcp_unlock();
  irq_restore(irq);
  if (buf)
    kfree(buf);
}

/* ── M84: out-of-order reassembly ────────────────────────────────────────── */

static void tcp_free_ooo_list(struct tcp_ooo_seg *head) {
  while (head) {
    struct tcp_ooo_seg *next = head->next;
    kfree(head->data);
    kfree(head);
    head = next;
  }
}

/*
 * Give a connection a bigger receive buffer, up to the ceiling.
 *
 * Called when the buffer filled: that is the sender proving it can use more
 * than we offered. Doubling keeps the number of reallocations logarithmic, and
 * the data already in the buffer moves with it — the receive queue is a plain
 * linear buffer, so a copy is all it takes.
 *
 * Failure is not an error: the connection keeps the buffer it has and the
 * window stays where it was.
 */
static void tcp_recv_grow(struct tcp_conn *conn) {
  u32 want;
  u8 *bigger;

  u32 cap_max = tcp_recv_cap_max(conn);
  if (!conn->recv_buf || conn->recv_cap >= cap_max)
    return;
  want = conn->recv_cap * 2;
  if (want > cap_max)
    want = cap_max;
  bigger = kmalloc(want);
  if (!bigger)
    return;
  memcpy(bigger, conn->recv_buf, conn->recv_len);
  kfree(conn->recv_buf);
  conn->recv_buf = bigger;
  conn->recv_cap = want;
}

/* Append in-order bytes to the receive buffer, clamped to the free space, and
 * advance rcv_nxt by exactly what was accepted. Returns the accepted count. */
static u32 tcp_recv_append(struct tcp_conn *conn, const u8 *data, u32 len) {
  if (!conn->recv_buf)
    return 0; /* buffer allocation failed — behave as a zero window */
  u32 space = conn->recv_cap - conn->recv_len;
  if (len > space)
    len = space;
  if (len == 0)
    return 0;
  memcpy(conn->recv_buf + conn->recv_len, data, len);
  conn->recv_len += len;
  conn->rcv_nxt += len;
  /* Filled it: this connection can use more than it was given. */
  if (conn->recv_len == conn->recv_cap)
    tcp_recv_grow(conn);
  return len;
}

/* Move every queued segment that is now contiguous with rcv_nxt into the
 * receive buffer. Returns the consumed segments as a detached list for the
 * caller to free outside the lock. */
static struct tcp_ooo_seg *tcp_ooo_drain(struct tcp_conn *conn) {
  struct tcp_ooo_seg *freed = 0;
  while (conn->ooo_queue) {
    struct tcp_ooo_seg *s = conn->ooo_queue;
    if ((i32)(s->seq - conn->rcv_nxt) > 0)
      break; /* still a hole before this segment */
    u32 off = conn->rcv_nxt - s->seq;
    if (off < s->len) {
      u32 want = s->len - off;
      if (tcp_recv_append(conn, s->data + off, want) < want)
        break; /* receive buffer full — keep the remainder queued */
    }
    conn->ooo_queue = s->next;
    conn->ooo_bytes -= s->len;
    conn->ooo_segs--;
    s->next = freed;
    freed = s;
  }
  return freed;
}

/* Insert a pre-allocated segment into the sorted queue. Returns 0 on success;
 * -1 means the caller must free the node (duplicate or over budget). */
static int tcp_ooo_insert(struct tcp_conn *conn, struct tcp_ooo_seg *node) {
  if (conn->ooo_segs >= TCP_OOO_MAX_SEGS ||
      conn->ooo_bytes + node->len > TCP_OOO_MAX_BYTES)
    return -1;

  struct tcp_ooo_seg **prev = &conn->ooo_queue;
  while (*prev && (i32)((*prev)->seq - node->seq) < 0)
    prev = &(*prev)->next;
  /* Exact duplicate of a segment we already hold: drop the retransmission. */
  if (*prev && (*prev)->seq == node->seq && (*prev)->len >= node->len)
    return -1;
  node->next = *prev;
  *prev = node;
  conn->ooo_bytes += node->len;
  conn->ooo_segs++;
  return 0;
}

/* Build the SACK blocks describing what the reassembly queue holds. Adjacent
 * queued segments are merged into one block. RFC 2018 wants the block covering
 * the most recently received segment first, so `recent_seq` (the segment that
 * just arrived out of order, or 0) is promoted to the front. Returns the block
 * count; must be called with the TCP lock held. */
static u8 tcp_build_sack_blocks(const struct tcp_conn *conn, u32 recent_seq,
                                u32 *left, u32 *right) {
  u8 n = 0;
  const struct tcp_ooo_seg *s = conn->ooo_queue;
  while (s && n < TCP_SACK_MAX_BLOCKS + 1) {
    u32 l = s->seq;
    u32 r = s->seq + s->len;
    const struct tcp_ooo_seg *next = s->next;
    while (next && (i32)(next->seq - r) <= 0) {
      if ((i32)((next->seq + next->len) - r) > 0)
        r = next->seq + next->len;
      next = next->next;
    }
    left[n] = l;
    right[n] = r;
    n++;
    s = next;
  }

  /* Promote the block containing the newest segment. */
  if (recent_seq) {
    for (u8 i = 1; i < n; i++) {
      if ((i32)(recent_seq - left[i]) >= 0 && (i32)(right[i] - recent_seq) > 0) {
        u32 tl = left[i], tr = right[i];
        for (u8 j = i; j > 0; j--) {
          left[j] = left[j - 1];
          right[j] = right[j - 1];
        }
        left[0] = tl;
        right[0] = tr;
        break;
      }
    }
  }
  if (n > TCP_SACK_MAX_BLOCKS)
    n = TCP_SACK_MAX_BLOCKS;
  return n;
}

/* Fill a bare ACK (optionally carrying SACK blocks) for `conn`. Returns the
 * segment length. Must be called with the TCP lock held. */
static usize tcp_build_ack(struct tcp_conn *conn, u8 *pkt, int with_sack,
                           u32 recent_seq) {
  memset(pkt, 0, TCP_ACK_MAX_LEN);
  struct tcp_header *a = (struct tcp_header *)pkt;
  a->src_port = bswap16(conn->local_port);
  a->dst_port = bswap16(conn->remote_port);
  a->seq_num = bswap32(conn->snd_nxt);
  a->ack_num = bswap32(conn->rcv_nxt);
  a->data_offset = (5 << 4);
  a->flags = TCP_ACK;
  a->window = bswap16(tcp_adv_window(conn));

  usize len = sizeof(struct tcp_header);
  if (with_sack && conn->sack_ok && (conn->ooo_queue || conn->dsack_pending)) {
    u32 l[TCP_SACK_MAX_BLOCKS + 1], r[TCP_SACK_MAX_BLOCKS + 1];
    u8 n = conn->ooo_queue ? tcp_build_sack_blocks(conn, recent_seq, l, r) : 0;
    if (conn->dsack_pending) {
      /* RFC 2883: the D-SACK block reporting duplicated data goes first, and
       * the ordinary blocks follow it. */
      if (n >= TCP_SACK_MAX_BLOCKS)
        n = TCP_SACK_MAX_BLOCKS - 1;
      for (u8 i = n; i > 0; i--) {
        l[i] = l[i - 1];
        r[i] = r[i - 1];
      }
      l[0] = conn->dsack_left;
      r[0] = conn->dsack_right;
      n++;
    }
    if (n) {
      u8 *o = pkt + sizeof(struct tcp_header);
      o[0] = TCP_OPT_NOP;
      o[1] = TCP_OPT_NOP;
      o[2] = TCP_OPT_SACK;
      o[3] = (u8)(2 + 8 * n);
      for (u8 i = 0; i < n; i++) {
        u8 *q = o + 4 + i * 8;
        q[0] = (u8)(l[i] >> 24); q[1] = (u8)(l[i] >> 16);
        q[2] = (u8)(l[i] >> 8);  q[3] = (u8)l[i];
        q[4] = (u8)(r[i] >> 24); q[5] = (u8)(r[i] >> 16);
        q[6] = (u8)(r[i] >> 8);  q[7] = (u8)r[i];
      }
      len += TCP_SACK_OPT_LEN(n);
      a->data_offset = (u8)((len / 4) << 4);
      conn->dsack_pending = 0;
    }
  }
  return len;
}

/* ── M84: SACK scoreboard (RFC 6675) ─────────────────────────────────────
 * Everything here runs with the TCP lock held. */

/* Recompute sacked_bytes and the per-segment `lost` marks. A segment counts as
 * lost once DUPTHRESH (3) segments *above* it have been SACKed — the same rule
 * that justifies a fast retransmit, applied per hole instead of once. */
#define TCP_DUPTHRESH 3

static void tcp_scoreboard_update(struct tcp_conn *conn) {
  conn->sacked_bytes = 0;
  for (struct tcp_retransmit_pkt *rp = conn->retransmit_queue; rp;
       rp = rp->next) {
    if (rp->sacked)
      conn->sacked_bytes += rp->dlen;
  }

  for (struct tcp_retransmit_pkt *rp = conn->retransmit_queue; rp;
       rp = rp->next) {
    if (rp->sacked) {
      rp->lost = 0;
      continue;
    }
    u32 above = 0;
    for (struct tcp_retransmit_pkt *q = rp->next; q; q = q->next) {
      if (q->sacked)
        above++;
    }
    rp->lost = (u8)(above >= TCP_DUPTHRESH ? 1 : 0);
  }
}

/* RFC 6675 pipe: the sender's estimate of how much data is actually in the
 * network. Segments the peer SACKed have left it; segments the scoreboard
 * declared lost have left it too (they were dropped), except for the copy we
 * retransmitted. */
static u32 tcp_pipe(const struct tcp_conn *conn) {
  u32 pipe = 0;
  for (const struct tcp_retransmit_pkt *rp = conn->retransmit_queue; rp;
       rp = rp->next) {
    if (rp->sacked)
      continue;
    if (!rp->lost)
      pipe += rp->dlen;
    else if (rp->retransmitted)
      pipe += rp->dlen;
  }
  return pipe;
}

/* Enter SACK-based loss recovery. Unlike the Reno path this does not inflate
 * cwnd by 3 MSS and then by 1 MSS per dup-ACK: the scoreboard already tells us
 * how much has left the network, so cwnd stays at ssthresh and the pipe
 * estimate governs what may be sent. */
static void tcp_enter_recovery(struct tcp_conn *conn) {
  if (conn->in_recovery)
    return;
  conn->prior_cwnd = conn->cwnd;
  conn->prior_ssthresh = conn->ssthresh;
  conn->ssthresh = conn->cwnd / 2;
  if (conn->ssthresh < 2 * TCP_MSS)
    conn->ssthresh = 2 * TCP_MSS;
  conn->cwnd = conn->ssthresh;
  conn->in_recovery = 1;
  conn->recovery_point = conn->snd_nxt;
}

/* A D-SACK proved the retransmission was unnecessary (the original had merely
 * been reordered, not lost), so the congestion reduction it caused was wrong:
 * put cwnd and ssthresh back. */
static void tcp_undo_recovery(struct tcp_conn *conn) {
  if (conn->prior_cwnd > conn->cwnd)
    conn->cwnd = conn->prior_cwnd;
  if (conn->prior_ssthresh > conn->ssthresh)
    conn->ssthresh = conn->prior_ssthresh;
  conn->in_recovery = 0;
  conn->dup_acks = 0;
  for (struct tcp_retransmit_pkt *rp = conn->retransmit_queue; rp; rp = rp->next)
    rp->lost = 0;
}

/* Pick the next segment to resend during recovery: the first one the
 * scoreboard says is lost and that is not already back in flight. */
static struct tcp_retransmit_pkt *tcp_next_retransmit(struct tcp_conn *conn) {
  for (struct tcp_retransmit_pkt *rp = conn->retransmit_queue; rp;
       rp = rp->next) {
    if (!rp->sacked && (rp->lost || rp == conn->retransmit_queue) &&
        !rp->retransmitted)
      return rp;
  }
  for (struct tcp_retransmit_pkt *rp = conn->retransmit_queue; rp;
       rp = rp->next) {
    if (!rp->sacked)
      return rp;
  }
  return 0;
}

/* Mark every queued retransmission the peer selectively acknowledged. Those
 * segments arrived; retransmitting them wastes the window. Must be called with
 * the TCP lock held. Returns the number newly marked. */
static int tcp_apply_sack(struct tcp_conn *conn, const struct tcp_opts *o,
                          u32 cum_ack) {
  int marked = 0;
  for (u8 b = 0; b < o->nsack; b++) {
    u32 l = o->sack_left[b];
    u32 r = o->sack_right[b];
    if ((i32)(r - l) <= 0)
      continue;
    /* RFC 2883: a first block at or below the cumulative ACK is a D-SACK —
     * the peer is reporting a duplicate, which means our retransmission was
     * spurious. Undo the congestion reduction it caused instead of treating
     * the block as new SACK information. */
    if (b == 0 && (i32)(r - cum_ack) <= 0) {
      conn->dsack_seen++;
      tcp_undo_recovery(conn);
      continue;
    }
    for (struct tcp_retransmit_pkt *rp = conn->retransmit_queue; rp;
         rp = rp->next) {
      if (rp->sacked || rp->dlen == 0)
        continue;
      if ((i32)(rp->seq - l) >= 0 && (i32)(r - (rp->seq + rp->dlen)) >= 0) {
        rp->sacked = 1;
        marked++;
      }
    }
    if ((i32)(r - conn->high_sack) > 0)
      conn->high_sack = r;
  }
  if (marked)
    tcp_scoreboard_update(conn);
  return marked;
}

/* Detach and free the reassembly queue. Freeing happens outside the TCP lock
 * (kfree takes the heap lock, and that nesting is what the send paths already
 * avoid). Every path that releases a connection slot must call this, otherwise
 * the memset over a reused slot leaks the queued segments. */
static void tcp_clear_ooo_queue(struct tcp_conn *conn) {
  struct tcp_ooo_seg *detached;
  u64 irq = irq_save();
  tcp_lock();
  detached = conn->ooo_queue;
  conn->ooo_queue = 0;
  conn->ooo_bytes = 0;
  conn->ooo_segs = 0;
  tcp_unlock();
  irq_restore(irq);
  tcp_free_ooo_list(detached);
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
                        usize len, u32 tx_flags) {
  if (family == B1NIX_AF_INET6)
    net_proto_ipv6_send(*v6, IP_PROTO_TCP, pkt, len);
  else
    ipv4_send_tx(v4, IP_PROTO_TCP, pkt, len, tx_flags);
}

/*
 * Prepare a segment's checksum and report the IPV4_TX_F_* flags it must be
 * handed to the IP layer with.
 *
 * IPv4 segments leave the field zero: only the IP layer knows the source
 * address that will actually be on the wire (it rewrites it for loopback, which
 * is what used to make a loopback segment's checksum wrong) and which interface
 * the FIB picked, so only there can it choose between computing the sum and
 * leaving a partial one for the NIC. IPv6 still computes its own.
 */
static u32 tcp_set_checksum(u8 family, struct ipv4_addr v4,
                            const struct in6_addr_k *v6, u8 *pkt, usize len) {
  struct tcp_header *t = (struct tcp_header *)pkt;
  (void)v4;
  t->checksum = 0;
  if (family == B1NIX_AF_INET6) {
    t->checksum = bswap16(tcp6_checksum(*v6, *v6, pkt, len));
    return 0;
  }
  return IPV4_TX_F_CSUM_L4;
}

/* Checksum + transmit a segment to a connection's peer. */
static void tcp_conn_emit(struct tcp_conn *conn, u8 *pkt, usize len) {
  u32 tx = tcp_set_checksum(conn->family, conn->remote_ip, &conn->remote_ip6,
                            pkt, len);
  tcp_l3_send(conn->family, conn->remote_ip, &conn->remote_ip6, pkt, len, tx);
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
  for (int i = 0; i < (int)resource_caps_tcp_max(); i++) {
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

static struct tcp_conn *tcp_connect_start_af(u8 family, struct ipv4_addr v4,
                                             struct in6_addr_k v6,
                                             u16 dst_port) {
  struct tcp_retransmit_pkt *rp = kmalloc(sizeof(struct tcp_retransmit_pkt));
  if (!rp)
    return 0;
  rp->data = kmalloc(TCP_SYN_HDR_LEN);
  if (!rp->data) {
    kfree(rp);
    return 0;
  }
  /* M84: the receive buffer is per-connection heap memory now. Allocate it
   * here, outside the TCP lock, so the heap lock is never taken under it. */
  u8 *rcvbuf = tcp_alloc_recv_buf();
  if (!rcvbuf) {
    kfree(rp->data);
    kfree(rp);
    return 0;
  }

  u64 irq = irq_save();
  tcp_lock();

  struct tcp_conn *conn = 0;
  for (int i = 0; i < (int)resource_caps_tcp_max(); i++) {
    if (!tcp_conns[i].used) {
      conn = &tcp_conns[i];
      break;
    }
  }
  if (!conn) {
    tcp_unlock();
    irq_restore(irq);
    kfree(rcvbuf);
    kfree(rp->data);
    kfree(rp);
    k_info("tcp", "no free connection slots");
    return 0;
  }

  memset(conn, 0, sizeof(*conn));
  conn->used = 1;
  conn->keepidle = TCP_KEEPIDLE_DEFAULT;
  conn->keepintvl = TCP_KEEPINTVL_DEFAULT;
  conn->keepcnt = TCP_KEEPCNT_DEFAULT;
  conn->last_activity = scheduler_get_uptime_ticks();
  conn->recv_buf = rcvbuf;
  conn->recv_cap = TCP_RECV_BUF_INIT;
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
  conn->snd_wnd = TCP_RECV_BUF_INIT;  /* assume peer advertises >=1 segment */
  conn->cwnd = TCP_INIT_CWND;          /* slow start: RFC 6928 IW10 */
  /* Slow start runs until a loss says otherwise. Starting the threshold at
   * 65535 ended it at 64 KiB in flight even on a clean link; Linux starts at
   * TCP_INFINITE_SSTHRESH for exactly that reason, and our equivalent of
   * "infinite" is the largest window we would ever allow. */
  conn->ssthresh = TCP_CWND_MAX;
  conn->dup_acks = 0;
  /* M84: option state. Until the SYN-ACK is parsed we assume no scaling and
   * the RFC 1122 default MSS. */
  conn->rcv_wscale = (u8)TCP_RCV_WSCALE;
  conn->snd_wscale = 0;
  conn->wscale_ok = 0;
  conn->snd_mss = 536;

  u8 packet[TCP_SYN_HDR_LEN];
  memset(packet, 0, sizeof(packet));
  struct tcp_header *tcp = (struct tcp_header *)packet;
  tcp->src_port = bswap16(conn->local_port);
  tcp->dst_port = bswap16(dst_port);
  tcp->seq_num = bswap32(conn->iss);
  tcp->ack_num = 0;
  /* M84: SYN carries MSS + window-scale, so the header is 7 words. The window
   * in a SYN is never scaled (RFC 7323), and our buffer fits 16 bits. */
  tcp->data_offset = (u8)((5 + TCP_SYN_OPT_LEN / 4) << 4);
  tcp->flags = TCP_SYN;
  tcp->window = bswap16(TCP_SYN_WINDOW);
  tcp_build_syn_options(packet + sizeof(struct tcp_header));

  conn->state = TCP_SYN_SENT;

  // Set up rp under lock
  memcpy(rp->data, packet, sizeof(packet));
  rp->len = sizeof(packet);
  rp->seq = conn->iss;
  rp->timestamp = scheduler_get_uptime_ticks();
  rp->dlen = 1;
  rp->sacked = 0;
  rp->retries = 0;
  rp->next = 0;

  // Insert rp into queue
  struct tcp_retransmit_pkt **prev = &conn->retransmit_queue;
  while (*prev)
    prev = &(*prev)->next;
  *prev = rp;

  tcp_unlock();
  irq_restore(irq);

  tcp_conn_emit(conn, packet, sizeof(packet));
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
  for (int tries = 0; tries < 400; tries++) {
    u64 irq = irq_save();
    tcp_lock();
    int state = conn->state;
    tcp_unlock();
    irq_restore(irq);

    if (state != TCP_SYN_SENT)
      break;

    net_poll();

    irq = irq_save();
    tcp_lock();
    state = conn->state;
    tcp_unlock();
    irq_restore(irq);

    if (state != TCP_SYN_SENT)
      break;

    scheduler_sleep_ticks(1);
  }

  u64 irq = irq_save();
  tcp_lock();
  if (conn->state != TCP_ESTABLISHED) {
    /* A refused or unanswered connection is an ordinary result handed back to
     * the caller, not an event the kernel should narrate. Printing it per
     * attempt flooded the console: a browser start-up makes hundreds, every
     * line takes the console lock and goes out the serial port, and a second
     * CPU waiting on that lock long enough declares a spinlock lockup and
     * panics. Rate-limited to the first few, which is all a bring-up needs. */
    {
      static unsigned reported;
      if (reported < 8) {
        reported++;
        k_err("tcp", "connect failed");
      }
    }
    conn->used = 0;
    tcp_unlock();
    irq_restore(irq);
    return 0;
  }
  tcp_unlock();
  irq_restore(irq);
  /* Success is not news either; it was one line per connection. */
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
  if (!conn)
    return 0;
  u64 irq = irq_save();
  tcp_lock();
  int res = conn->used && (conn->state == TCP_ESTABLISHED);
  tcp_unlock();
  irq_restore(irq);
  return res;
}

/* SO_RCVBUF: cap this connection's receive buffer, and with it the window it
 * advertises. Linux's setsockopt(SO_RCVBUF) does the same two things — it sets
 * the ceiling and it stops the auto-tuner from moving it — and it also refuses
 * to go below a floor, because a window under one segment cannot make progress.
 * `bytes` is the value the caller asked for, already clamped by the socket
 * layer to the system maximum. */

/* SO_KEEPALIVE and the three TCP_KEEP* knobs.
 *
 * Values arrive in seconds, which is what setsockopt(2) takes; the timer works
 * in ticks. Zero is refused rather than stored: Linux rejects it, and a zero
 * interval would turn the probe into a busy loop. */
void tcp_set_keepalive(struct tcp_conn *conn, int on) {
  if (!conn)
    return;
  u64 irq = irq_save();
  tcp_lock();
  if (conn->used) {
    conn->keepalive = on ? 1 : 0;
    conn->keepalive_probes = 0;
    conn->last_activity = scheduler_get_uptime_ticks();
  }
  tcp_unlock();
  irq_restore(irq);
}

int tcp_set_keepalive_param(struct tcp_conn *conn, int which, u32 seconds) {
  if (!conn)
    return -1;
  if (seconds == 0)
    return -1;
  u64 irq = irq_save();
  tcp_lock();
  if (conn->used) {
    if (which == 0)
      conn->keepidle = seconds;
    else if (which == 1)
      conn->keepintvl = seconds;
    else
      conn->keepcnt = seconds;
  }
  tcp_unlock();
  irq_restore(irq);
  return 0;
}

u32 tcp_get_keepalive_param(struct tcp_conn *conn, int which) {
  if (!conn)
    return 0;
  if (which == 0)
    return conn->keepidle;
  if (which == 1)
    return conn->keepintvl;
  return conn->keepcnt;
}

/* Send one keepalive probe: an ACK whose sequence number is one behind the
 * next byte we would send. The peer sees a segment it has already
 * acknowledged, so it answers with an ACK and discards nothing — which is
 * exactly the point, an answer without touching the data stream. Caller holds
 * the TCP lock; the emit happens after it is dropped. */
static usize tcp_build_keepalive(struct tcp_conn *conn, u8 *pkt) {
  usize len = tcp_build_ack(conn, pkt, 0, 0);
  struct tcp_header *h = (struct tcp_header *)pkt;
  h->seq_num = bswap32(conn->snd_nxt - 1);
  return len;
}

void tcp_set_rcvbuf(struct tcp_conn *conn, u32 bytes) {
  if (!conn)
    return;
  if (bytes < TCP_RCVBUF_MIN)
    bytes = TCP_RCVBUF_MIN;
  if (bytes > TCP_RECV_BUF_MAX)
    bytes = TCP_RECV_BUF_MAX;
  u64 irq = irq_save();
  tcp_lock();
  if (conn->used)
    conn->recv_cap_max = bytes;
  tcp_unlock();
  irq_restore(irq);
}

/* Bytes a reader could take right now — what FIONREAD reports. */
usize tcp_bytes_available(struct tcp_conn *conn) {
  if (!conn)
    return 0;
  u64 irq = irq_save();
  tcp_lock();
  usize n = 0;
  if (conn->used && conn->recv_len > conn->recv_read)
    n = (usize)(conn->recv_len - conn->recv_read);
  tcp_unlock();
  irq_restore(irq);
  return n;
}

int tcp_is_readable(struct tcp_conn *conn) {
  if (!conn)
    return 1;
  u64 irq = irq_save();
  tcp_lock();
  if (!conn->used) {
    tcp_unlock();
    irq_restore(irq);
    return 1;
  }
  if (conn->recv_len > conn->recv_read) {
    tcp_unlock();
    irq_restore(irq);
    return 1;
  }
  if (conn->state == TCP_CLOSE_WAIT || conn->state == TCP_CLOSED ||
      conn->state == TCP_TIME_WAIT || conn->state == TCP_LAST_ACK ||
      conn->state == TCP_CLOSING) {
    tcp_unlock();
    irq_restore(irq);
    return 1;
  }
  tcp_unlock();
  irq_restore(irq);
  return 0;
}

int tcp_is_close_wait(struct tcp_conn *conn) {
  if (!conn)
    return 0;
  u64 irq = irq_save();
  tcp_lock();
  int res = conn->used && (conn->state == TCP_CLOSE_WAIT);
  tcp_unlock();
  irq_restore(irq);
  return res;
}

int tcp_is_closed(struct tcp_conn *conn) {
  if (!conn)
    return 1;
  u64 irq = irq_save();
  tcp_lock();
  if (!conn->used) {
    tcp_unlock();
    irq_restore(irq);
    return 1;
  }
  int res = (conn->state != TCP_ESTABLISHED && conn->state != TCP_CLOSE_WAIT);
  tcp_unlock();
  irq_restore(irq);
  return res;
}


/* ── TCP Listen ── */
struct tcp_conn *tcp_listen(u16 local_port, int backlog) {
  (void)backlog;
  u64 irq = irq_save();
  tcp_lock();
  for (int i = 0; i < (int)resource_caps_tcp_max(); i++) {
    if (!tcp_conns[i].used) {
      memset(&tcp_conns[i], 0, sizeof(struct tcp_conn));
      tcp_conns[i].used = 1;
      tcp_conns[i].state = TCP_LISTEN;
      tcp_conns[i].local_port = local_port;
      struct tcp_conn *res = &tcp_conns[i];
      tcp_unlock();
      irq_restore(irq);
      return res;
    }
  }
  tcp_unlock();
  irq_restore(irq);
  return 0;
}

/* ── Check for pending connections (for poll) ── */
int tcp_pending_connections(u16 local_port) {
  u64 irq = irq_save();
  tcp_lock();
  for (int i = 0; i < (int)resource_caps_tcp_max(); i++) {
    if (tcp_conns[i].used && tcp_conns[i].state == TCP_ESTABLISHED &&
        tcp_conns[i].local_port == local_port && !tcp_conns[i].handed_to_user) {
      tcp_unlock();
      irq_restore(irq);
      return 1;
    }
  }
  tcp_unlock();
  irq_restore(irq);
  return 0;
}
/* ── TCP Accept ── */
struct tcp_conn *tcp_accept(u16 local_port, struct ipv4_addr *client_ip,
                            u16 *client_port) {
  u64 irq = irq_save();
  tcp_lock();
  for (int i = 0; i < (int)resource_caps_tcp_max(); i++) {
    if (tcp_conns[i].used && tcp_conns[i].state == TCP_ESTABLISHED &&
        tcp_conns[i].local_port == local_port &&
        tcp_conns[i].family == B1NIX_AF_INET &&
        !tcp_conns[i].handed_to_user) {
      tcp_conns[i].handed_to_user = 1;
      if (client_ip)
        *client_ip = tcp_conns[i].remote_ip;
      if (client_port)
        *client_port = tcp_conns[i].remote_port;
      struct tcp_conn *res = &tcp_conns[i];
      tcp_unlock();
      irq_restore(irq);
      return res;
    }
  }
  tcp_unlock();
  irq_restore(irq);
  return 0;
}

struct tcp_conn *tcp_accept6(u16 local_port, struct in6_addr_k *client_ip6,
                             u16 *client_port) {
  u64 irq = irq_save();
  tcp_lock();
  for (int i = 0; i < (int)resource_caps_tcp_max(); i++) {
    if (tcp_conns[i].used && tcp_conns[i].state == TCP_ESTABLISHED &&
        tcp_conns[i].local_port == local_port &&
        tcp_conns[i].family == B1NIX_AF_INET6 &&
        !tcp_conns[i].handed_to_user) {
      tcp_conns[i].handed_to_user = 1;
      if (client_ip6)
        *client_ip6 = tcp_conns[i].remote_ip6;
      if (client_port)
        *client_port = tcp_conns[i].remote_port;
      struct tcp_conn *res = &tcp_conns[i];
      tcp_unlock();
      irq_restore(irq);
      return res;
    }
  }
  tcp_unlock();
  irq_restore(irq);
  return 0;
}

/* ── TCP send data ── */
int tcp_send(struct tcp_conn *conn, const void *data, usize len) {
  if (!conn)
    return -1;
  if (len == 0)
    return 0;

  /* Sending is activity too: a connection carrying a steady stream one way
   * is not idle, and probing it would be noise. Only the probe path itself
   * leaves last_activity alone, so an unanswered probe still ages. */
  conn->last_activity = scheduler_get_uptime_ticks();

  /* Pre-allocate packet and retransmit packet buffers BEFORE taking the lock
   * to avoid heap_lock deadlock. */
  usize to_alloc = len > TCP_MSS ? TCP_MSS : len;
  usize packet_len = sizeof(struct tcp_header) + to_alloc;
  u8 *packet = kzalloc(packet_len);
  if (!packet)
    return -1;

  struct tcp_retransmit_pkt *rp = kmalloc(sizeof(struct tcp_retransmit_pkt));
  if (!rp) {
    kfree(packet);
    return -1;
  }
  rp->data = kmalloc(packet_len);
  if (!rp->data) {
    kfree(rp);
    kfree(packet);
    return -1;
  }

  u64 irq = irq_save();
  tcp_lock();

  if (conn->state != TCP_ESTABLISHED) {
    tcp_unlock();
    irq_restore(irq);
    kfree(rp->data);
    kfree(rp);
    kfree(packet);
    return -1;
  }

  u32 window = conn->snd_wnd < conn->cwnd ? conn->snd_wnd : conn->cwnd;
  /* M84: inside SACK recovery the amount actually in the network is the RFC
   * 6675 pipe estimate, not everything between snd_una and snd_nxt — segments
   * the peer already SACKed have left the network and must not hold the
   * window hostage. */
  u32 inflight = conn->in_recovery ? tcp_pipe(conn)
                                   : (conn->snd_nxt - conn->snd_una);
  if (inflight >= window) {
    tcp_unlock();
    irq_restore(irq);
    kfree(rp->data);
    kfree(rp);
    kfree(packet);
    /* The congestion/peer window is full: more data cannot be sent until ACKs
     * drain. A non-blocking caller must see -EAGAIN; a blocking caller's vfs
     * socket layer will yield / retry. Returning 0 here would make a
     * 500-iteration unidirectional send loop appear to deliver all data
     * instantly (never throttling), which masks real write-side congestion. */
    return -EAGAIN;
  }
  u32 usable = window - inflight;
  /* M84: never emit a segment larger than the MSS the peer advertised. */
  u32 eff_mss = conn->snd_mss && conn->snd_mss < TCP_MSS ? conn->snd_mss
                                                         : TCP_MSS;
  usize to_send = len;
  if (to_send > eff_mss)
    to_send = eff_mss;
  if (to_send > usable)
    to_send = usable;
  if (to_send == 0) {
    tcp_unlock();
    irq_restore(irq);
    kfree(rp->data);
    kfree(rp);
    kfree(packet);
    return 0;
  }

  packet_len = sizeof(struct tcp_header) + to_send;

  struct tcp_header *tcp = (struct tcp_header *)packet;
  tcp->src_port = bswap16(conn->local_port);
  tcp->dst_port = bswap16(conn->remote_port);
  tcp->seq_num = bswap32(conn->snd_nxt);
  tcp->ack_num = bswap32(conn->rcv_nxt);
  tcp->data_offset = (5 << 4);
  tcp->flags = TCP_PSH | TCP_ACK;
  tcp->window = bswap16(tcp_adv_window(conn));

  memcpy(packet + sizeof(struct tcp_header), data, to_send);

  u32 seq_start = conn->snd_nxt;
  conn->snd_nxt += (u32)to_send;

  memcpy(rp->data, packet, packet_len);
  rp->len = packet_len;
  rp->seq = seq_start;
  rp->timestamp = scheduler_get_uptime_ticks();
  rp->dlen = (u32)to_send;
  rp->sacked = 0;
  rp->retries = 0;
  rp->next = 0;

  struct tcp_retransmit_pkt **prev = &conn->retransmit_queue;
  while (*prev)
    prev = &(*prev)->next;
  *prev = rp;

  tcp_unlock();
  irq_restore(irq);

  tcp_conn_emit(conn, packet, packet_len);
  kfree(packet);

  return (int)to_send;
}

/* ── TCP receive data (non-blocking) ── */
int tcp_recv(struct tcp_conn *conn, void *buf, usize max_len, int flags) {
  if (!conn)
    return -1;

  u64 irq = irq_save();
  tcp_lock();

  if (conn->recv_read >= conn->recv_len) {
    /* Poll for new data */
    tcp_unlock();
    irq_restore(irq);
    net_poll();
    irq = irq_save();
    tcp_lock();
  }

  if (conn->recv_read >= conn->recv_len) {
    tcp_unlock();
    irq_restore(irq);
    return 0;
  }

  usize avail = conn->recv_len - conn->recv_read;
  if (avail > max_len)
    avail = max_len;

  memcpy(buf, conn->recv_buf + conn->recv_read, avail);
  u8 wnd_update_pkt[sizeof(struct tcp_header)];
  int send_wnd_update = 0;
  if (!(flags & B1NIX_MSG_PEEK)) {
    conn->recv_read += (u32)avail;

    /*
     * Compact what has been read out of the buffer, not merely when it happens
     * to empty.
     *
     * The free space the receive window advertises is measured from recv_len,
     * so bytes already handed to the application still counted against it until
     * the buffer drained completely. A reader that keeps up but never quite
     * empties the buffer — which is every streaming download — therefore drove
     * the window to zero at 64 KiB and left it there: transfers died a little
     * past that point, and the failure looked like a corrupt package rather
     * than a stalled connection.
     */
    if (conn->recv_read >= conn->recv_len) {
      conn->recv_len = 0;
      conn->recv_read = 0;
    } else if (conn->recv_read > 0) {
      memmove(conn->recv_buf, conn->recv_buf + conn->recv_read,
              conn->recv_len - conn->recv_read);
      conn->recv_len -= conn->recv_read;
      conn->recv_read = 0;
    }

    /* If we had throttled the peer below 1 MSS and the app has now freed at
     * least 1 MSS of buffer, send an unsolicited window-update ACK. Without
     * this the peer waits out its zero-window persist timer (exponential
     * backoff, ~5 s) before probing — the dominant cost of a fresh HTTPS
     * connection whose cert flight overflowed the receive buffer. */
    /* Against the buffer this connection actually has, not the ceiling it
     * may grow to: measuring free space against the maximum meant a full
     * 64 KiB buffer still looked like it had ~1 MiB free, so wnd_closed was
     * never set and this window update — the whole point of which is to keep
     * the peer off its zero-window persist timer — never fired. */
    u32 free_wnd = conn->recv_cap - conn->recv_len;
    if (conn->wnd_closed && free_wnd >= TCP_MSS) {
      conn->wnd_closed = 0;
      memset(wnd_update_pkt, 0, sizeof(wnd_update_pkt));
      struct tcp_header *a = (struct tcp_header *)wnd_update_pkt;
      a->src_port = bswap16(conn->local_port);
      a->dst_port = bswap16(conn->remote_port);
      a->seq_num = bswap32(conn->snd_nxt);
      a->ack_num = bswap32(conn->rcv_nxt);
      a->data_offset = (5 << 4);
      a->flags = TCP_ACK;
      a->window = bswap16(tcp_adv_window(conn));
      send_wnd_update = 1;
    }
  }

  tcp_unlock();
  irq_restore(irq);

  if (send_wnd_update)
    tcp_conn_emit(conn, wnd_update_pkt, sizeof(wnd_update_pkt));

  return (int)avail;
}

/* ── TCP close ── */
int tcp_close(struct tcp_conn *conn) {
  if (!conn || !conn->used)
    return -1;

  u64 irq = irq_save();
  tcp_lock();

  if (!conn->used) {
    tcp_unlock();
    irq_restore(irq);
    return -1;
  }

  if (conn->state == TCP_LISTEN) {
    /* A listening socket has no peer and never queued retransmits: reclaim the
     * pool slot immediately instead of running the FIN/close handshake. Before
     * this, a closed listener leaked its slot forever (nothing stored the conn
     * on the socket, so teardown never reached tcp_close). */
    conn->used = 0;
    tcp_unlock();
    irq_restore(irq);
    return 0;
  }

  if (conn->state == TCP_CLOSE_WAIT) {
    conn->state = TCP_LAST_ACK;
  }

  tcp_unlock();
  irq_restore(irq);

  struct tcp_retransmit_pkt *rp = kmalloc(sizeof(struct tcp_retransmit_pkt));
  if (!rp)
    return -1;
  rp->data = kmalloc(sizeof(struct tcp_header));
  if (!rp->data) {
    kfree(rp);
    return -1;
  }

  irq = irq_save();
  tcp_lock();

  if (!conn->used) {
    /* Freed by the retransmit/connect-abort paths between the two lock
     * acquisitions (the allocations above run outside the lock). */
    tcp_unlock();
    irq_restore(irq);
    kfree(rp->data);
    kfree(rp);
    return -1;
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
  tcp->window = bswap16(tcp_adv_window(conn));

  u32 seq_start = conn->snd_nxt;
  conn->state = TCP_FIN_WAIT1;
  conn->snd_nxt++;

  // Set up rp under lock
  memcpy(rp->data, packet, sizeof(packet));
  rp->len = sizeof(packet);
  rp->seq = seq_start;
  rp->timestamp = scheduler_get_uptime_ticks();
  rp->dlen = 1;
  rp->sacked = 0;
  rp->retries = 0;
  rp->next = 0;

  // Insert rp into queue
  struct tcp_retransmit_pkt **prev = &conn->retransmit_queue;
  while (*prev)
    prev = &(*prev)->next;
  *prev = rp;

  tcp_unlock();
  irq_restore(irq);

  // Emit FIN segment outside the lock (calls kzalloc, loopback enqueue)
  tcp_conn_emit(conn, packet, sizeof(packet));

  /* Wait for FIN-ACK (poll a bit) */
  for (int tries = 0; tries < 50; tries++) {
    irq = irq_save();
    tcp_lock();
    int state = conn->state;
    tcp_unlock();
    irq_restore(irq);

    if (state == TCP_CLOSED || state == TCP_TIME_WAIT)
      break;

    net_poll();
  }

  irq = irq_save();
  tcp_lock();
  if (conn->state == TCP_CLOSED) {
    tcp_unlock();
    irq_restore(irq);
    tcp_clear_retransmit_queue(conn);
    tcp_clear_ooo_queue(conn);
    tcp_free_recv_buf(conn);
    irq = irq_save();
    tcp_lock();
    conn->used = 0;
  } else if (conn->state != TCP_TIME_WAIT) {
    tcp_enter_time_wait(conn);
  }
  tcp_unlock();
  irq_restore(irq);

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

  u64 irq = irq_save();
  tcp_lock();

  /* Find connection */
  struct tcp_conn *conn =
      tcp_find_conn_af(family, v4src, &v6src, src_port, dst_port);

  /* Anything arriving on the connection — data, an ACK, or the answer to a
   * keepalive probe — means the peer is still there, which is the whole
   * question keepalive asks. */
  if (conn) {
    conn->last_activity = scheduler_get_uptime_ticks();
    conn->keepalive_probes = 0;
  }

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
    /* M84: the window field of a SYN is never scaled (RFC 7323 §2.2); every
     * later segment is, by the shift the peer advertised in its SYN. */
    conn->snd_wnd = (flags & TCP_SYN) ? (u32)wnd_new
                                      : ((u32)wnd_new << conn->snd_wscale);
    if (ack_new == conn->snd_una && payload_size == 0) {
      conn->dup_acks++;
      /* M84: a duplicate ACK may carry SACK blocks telling us exactly which
       * segments past the hole already arrived — and a D-SACK telling us a
       * retransmission was unnecessary. */
      int sacked_now = 0;
      if (conn->sack_ok) {
        struct tcp_opts dopt;
        tcp_parse_options(data, data_offset, &dopt);
        if (dopt.nsack)
          sacked_now = tcp_apply_sack(conn, &dopt, ack_new);
      }
      (void)sacked_now;

      /* Enter recovery on the third duplicate ACK, or as soon as the
       * scoreboard declares any segment lost — whichever comes first. */
      int have_lost = 0;
      for (struct tcp_retransmit_pkt *rp = conn->retransmit_queue; rp;
           rp = rp->next) {
        if (rp->lost && !rp->retransmitted) {
          have_lost = 1;
          break;
        }
      }
      if (!conn->in_recovery && (conn->dup_acks >= TCP_DUPTHRESH || have_lost))
        tcp_enter_recovery(conn);

      if (conn->in_recovery && tcp_pipe(conn) < conn->cwnd) {
        /* Retransmit what the scoreboard says is missing. Unlike Reno this is
         * not necessarily the queue head: the head may already be sitting in
         * the peer's reassembly queue. */
        struct tcp_retransmit_pkt *hole = tcp_next_retransmit(conn);
        u8 *resend_data = 0;
        usize resend_len = 0;
        if (hole) {
          resend_data = hole->data;
          resend_len = hole->len;
          hole->timestamp = scheduler_get_uptime_ticks();
          hole->retries++;
          hole->retransmitted = 1;
        }
        tcp_unlock();
        irq_restore(irq);
        if (resend_data && resend_len) {
          /* Re-stamp the checksum on the stored copy: it decides, and reports,
           * whether this transmit is a partial-checksum one. The computation is
           * idempotent, so a retransmit gets exactly what the original sent. */
          u32 tx = tcp_set_checksum(conn->family, conn->remote_ip,
                                    &conn->remote_ip6, resend_data, resend_len);
          tcp_l3_send(conn->family, conn->remote_ip, &conn->remote_ip6,
                      resend_data, resend_len, tx);
        }
        irq = irq_save();
        tcp_lock();
      }
    } else if (ack_new > conn->snd_una) {
      conn->dup_acks = 0;
      /* A cumulative ACK may also carry a D-SACK (RFC 2883 allows one on a
       * non-duplicate ACK) and it retires scoreboard state. */
      if (conn->sack_ok) {
        struct tcp_opts aopt;
        tcp_parse_options(data, data_offset, &aopt);
        if (aopt.nsack)
          tcp_apply_sack(conn, &aopt, ack_new);
      }
      if (conn->in_recovery) {
        /* Recovery ends when everything outstanding at entry is acknowledged;
         * a partial ACK keeps it going (and frees another retransmission). */
        if ((i32)(ack_new - conn->recovery_point) >= 0) {
          conn->in_recovery = 0;
          conn->cwnd = conn->ssthresh;
          conn->sacked_bytes = 0;
          conn->high_sack = 0;
        }
      }
      if (conn->in_recovery) {
        /* No growth inside recovery: the pipe estimate governs sending. */
      } else if (conn->cwnd < conn->ssthresh) {
        /* Slow start: exponential — +MSS per new ACK. */
        conn->cwnd += TCP_MSS;
      } else {
        /* Congestion avoidance: additive — +MSS²/cwnd per RTT
         * (approximated per-ACK as MSS/cwnd-segments). */
        u32 inc = (TCP_MSS * TCP_MSS) / (conn->cwnd ? conn->cwnd : 1);
        if (inc < 1) inc = 1;
        conn->cwnd += inc;
      }
      if (conn->cwnd > TCP_CWND_MAX) conn->cwnd = TCP_CWND_MAX;
    }
  }

  if (!conn && (flags & TCP_SYN)) {
    /* Check for listener */
    for (int i = 0; i < (int)resource_caps_tcp_max(); i++) {
      if (tcp_conns[i].used && tcp_conns[i].state == TCP_LISTEN &&
          tcp_conns[i].local_port == dst_port) {
        /* Found a listener, create a new connection for the client */
        struct tcp_conn *new_conn = 0;
        for (int j = 0; j < (int)resource_caps_tcp_max(); j++) {
          if (!tcp_conns[j].used) {
            new_conn = &tcp_conns[j];
            break;
          }
        }
        if (new_conn) {
          memset(new_conn, 0, sizeof(*new_conn));
          new_conn->used = 1;
          new_conn->keepidle = TCP_KEEPIDLE_DEFAULT;
          new_conn->keepintvl = TCP_KEEPINTVL_DEFAULT;
          new_conn->keepcnt = TCP_KEEPCNT_DEFAULT;
          new_conn->last_activity = scheduler_get_uptime_ticks();
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
          new_conn->cwnd = TCP_INIT_CWND;
          new_conn->ssthresh = TCP_CWND_MAX;
          new_conn->dup_acks = 0;

          /* M84: negotiate options from the client's SYN. Window scaling is
           * symmetric — we may only scale our advertisement (and interpret
           * theirs) when both SYNs carried the option, so a client that sent
           * none pins both shifts to 0. */
          struct tcp_opts sopt;
          tcp_parse_options(data, data_offset, &sopt);
          new_conn->snd_mss = sopt.mss ? sopt.mss : 536;
          new_conn->sack_ok = sopt.sack_ok;
          if (sopt.has_wscale) {
            new_conn->wscale_ok = 1;
            new_conn->snd_wscale = sopt.wscale;
            new_conn->rcv_wscale = (u8)TCP_RCV_WSCALE;
          } else {
            new_conn->wscale_ok = 0;
            new_conn->snd_wscale = 0;
            new_conn->rcv_wscale = 0;
          }

          /* Send SYN-ACK */
          u8 packet[TCP_SYN_HDR_LEN];
          usize packet_len = sizeof(struct tcp_header);
          memset(packet, 0, sizeof(packet));
          struct tcp_header *tcp_hdr = (struct tcp_header *)packet;
          tcp_hdr->src_port = bswap16(new_conn->local_port);
          tcp_hdr->dst_port = bswap16(new_conn->remote_port);
          tcp_hdr->seq_num = bswap32(new_conn->iss);
          tcp_hdr->ack_num = bswap32(new_conn->rcv_nxt);
          tcp_hdr->data_offset = (5 << 4);
          tcp_hdr->flags = TCP_SYN | TCP_ACK;
          tcp_hdr->window = bswap16(TCP_SYN_WINDOW);
          if (new_conn->wscale_ok || new_conn->sack_ok) {
            u8 *sopts = packet + sizeof(struct tcp_header);
            tcp_build_syn_options(sopts);
            /* RFC 2018: only offer SACK back if the client asked for it; RFC
             * 7323: same for window scaling. */
            if (!new_conn->sack_ok) {
              sopts[4] = TCP_OPT_NOP;
              sopts[5] = TCP_OPT_NOP;
            }
            if (!new_conn->wscale_ok) {
              sopts[6] = TCP_OPT_NOP;
              sopts[7] = TCP_OPT_NOP;
              sopts[8] = TCP_OPT_NOP;
            }
            tcp_hdr->data_offset = (u8)((5 + TCP_SYN_OPT_LEN / 4) << 4);
            packet_len = TCP_SYN_HDR_LEN;
          }

          tcp_unlock();
          irq_restore(irq);

          struct tcp_retransmit_pkt *rp = kmalloc(sizeof(struct tcp_retransmit_pkt));
          u8 *rp_data = rp ? kmalloc(packet_len) : 0;
          /* Same rule as the active open: heap allocation happens while the
           * TCP lock is dropped. tcp_recv_append() treats a NULL buffer as a
           * closed window, so a failure here degrades rather than crashes. */
          u8 *acc_buf = tcp_alloc_recv_buf();
          if (rp && rp_data) {
            memcpy(rp_data, packet, packet_len);
            rp->data = rp_data;
            rp->len = packet_len;
            rp->seq = new_conn->iss;
            rp->timestamp = scheduler_get_uptime_ticks();
            rp->dlen = 1;
            rp->sacked = 0;
            rp->retries = 0;
            rp->next = 0;
          } else {
            if (rp) {
              kfree(rp);
              rp = 0;
            }
          }

          tcp_conn_emit(new_conn, packet, packet_len);

          irq = irq_save();
          tcp_lock();

          u8 *stale_buf = 0;
          if (acc_buf && new_conn->used && !new_conn->recv_buf) {
            new_conn->recv_buf = acc_buf;
            /* Capacity travels with the buffer. Assigning one without the
             * other left an accepted connection advertising a zero window and
             * refusing every out-of-order segment. */
            new_conn->recv_cap = TCP_RECV_BUF_INIT;
          }
          else
            stale_buf = acc_buf; /* freed below, outside the lock */

          if (rp && rp_data && new_conn->used && new_conn->state == TCP_SYN_RECEIVED) {
            struct tcp_retransmit_pkt **prev = &new_conn->retransmit_queue;
            while (*prev)
              prev = &(*prev)->next;
            *prev = rp;
          } else if (rp) {
            if (rp_data) kfree(rp_data);
            kfree(rp);
          }
          tcp_unlock();
          irq_restore(irq);
          if (stale_buf)
            kfree(stale_buf);
          return;
        }
        tcp_unlock();
        irq_restore(irq);
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

      tcp_unlock();
      irq_restore(irq);

      u32 tx = tcp_set_checksum(family, v4src, &v6src, rst, sizeof(rst));
      tcp_l3_send(family, v4src, &v6src, rst, sizeof(rst), tx);
      return;
    }
    tcp_unlock();
    irq_restore(irq);
    return;
  }

  switch (conn->state) {
  case TCP_SYN_RECEIVED:
    if (flags & TCP_ACK) {
      if (ack == conn->snd_nxt + 1) {
        conn->snd_una = ack;
        conn->snd_nxt = ack;
        conn->state = TCP_ESTABLISHED;

        /* The SYN-ACK acknowledges our SYN, so drop it from the retransmit
         * queue.
         *
         * Nothing did that before: the queue is only drained by the ACK
         * handling in the ESTABLISHED state, which needs a later ACK to
         * arrive. A connection that goes quiet the moment it is established —
         * a browser holding a socket open between requests, a client waiting
         * for the server to speak first — never produced one, so its own SYN
         * sat in the queue, was retransmitted five times half a second apart,
         * and at the fifth the timer declared the peer dead and closed a
         * connection that was fine. Every idle connection died about two and a
         * half seconds after connect(2). */
        {
          struct tcp_retransmit_pkt *acked = 0;
          while (conn->retransmit_queue &&
                 (isize)(conn->snd_una - conn->retransmit_queue->seq) > 0) {
            struct tcp_retransmit_pkt *rp = conn->retransmit_queue;
            conn->retransmit_queue = rp->next;
            rp->next = acked;
            acked = rp;
          }
          if (acked) {
            tcp_unlock();
            irq_restore(irq);
            tcp_free_retransmit_list(acked);
            irq = irq_save();
            tcp_lock();
          }
        }

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

        /* M84: our SYN always offers window scaling, so the peer's SYN-ACK
         * decides whether it is in effect. If it did not echo the option,
         * both directions stay unscaled and our advertisement must not be
         * shifted either. Re-scale snd_wnd, which was recorded unscaled
         * above (the SYN-ACK window field is never scaled). */
        struct tcp_opts sopt;
        tcp_parse_options(data, data_offset, &sopt);
        conn->snd_mss = sopt.mss ? sopt.mss : 536;
        conn->sack_ok = sopt.sack_ok;
        if (sopt.has_wscale) {
          conn->wscale_ok = 1;
          conn->snd_wscale = sopt.wscale;
          conn->rcv_wscale = (u8)TCP_RCV_WSCALE;
        } else {
          conn->wscale_ok = 0;
          conn->snd_wscale = 0;
          conn->rcv_wscale = 0;
        }

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
        a->window = bswap16(tcp_adv_window(conn));

        tcp_unlock();
        irq_restore(irq);
        tcp_conn_emit(conn, ack_pkt, sizeof(ack_pkt));
        irq = irq_save();
        tcp_lock();

        conn->state = TCP_ESTABLISHED;

        /* Same as the passive side above: the SYN-ACK acknowledges our SYN,
         * so it leaves the retransmit queue here rather than waiting for an
         * ACK that an idle connection never sends. */
        {
          struct tcp_retransmit_pkt *acked = 0;
          while (conn->retransmit_queue &&
                 (isize)(conn->snd_una - conn->retransmit_queue->seq) > 0) {
            struct tcp_retransmit_pkt *rp = conn->retransmit_queue;
            conn->retransmit_queue = rp->next;
            rp->next = acked;
            acked = rp;
          }
          if (acked) {
            tcp_unlock();
            irq_restore(irq);
            tcp_free_retransmit_list(acked);
            irq = irq_save();
            tcp_lock();
          }
        }
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
        while (conn->retransmit_queue &&
               (isize)(ack - conn->retransmit_queue->seq) > 0) {
          struct tcp_retransmit_pkt *rp = conn->retransmit_queue;
          conn->retransmit_queue = rp->next;
          rp->next = detached;
          detached = rp;
        }
        if (detached) {
          tcp_unlock();
          irq_restore(irq);
          tcp_free_retransmit_list(detached);
          irq = irq_save();
          tcp_lock();
        }
      }
    }

    /* Receive data.
     *
     * M84: a segment that does not start exactly at rcv_nxt used to be
     * dropped on the floor, so a single lost or reordered packet cost the
     * peer a full retransmit timeout. Now:
     *   - bytes already delivered (a retransmission) are trimmed off the
     *     front rather than rejecting the whole segment;
     *   - a segment past a hole is queued for reassembly and dup-ACKed, which
     *     is what drives the peer's fast retransmit;
     *   - filling the hole drains everything that became contiguous. */
    if (payload_size > 0 && (flags & (TCP_PSH | TCP_ACK))) {
      const u8 *seg = payload;
      u32 seg_seq = seq;
      u32 seg_len = (u32)payload_size;
      int send_ack = 0;
      u32 ooo_recent = 0; /* seq of a segment just queued out of order */
      struct tcp_ooo_seg *ooo_freed = 0;

      if ((i32)(conn->rcv_nxt - seg_seq) > 0) {
        u32 already = conn->rcv_nxt - seg_seq;
        /* RFC 2883: report the duplicated range back so the sender can tell a
         * spurious retransmission (reordering) from a real loss. */
        conn->dsack_pending = 1;
        conn->dsack_left = seg_seq;
        conn->dsack_right =
            (already >= seg_len) ? seg_seq + seg_len : conn->rcv_nxt;
        if (already >= seg_len) {
          seg_len = 0; /* pure duplicate — still ACK so the peer moves on */
          send_ack = 1;
        } else {
          seg += already;
          seg_len -= already;
          seg_seq += already;
        }
      }

      if (seg_len > 0 && seg_seq == conn->rcv_nxt) {
        tcp_recv_append(conn, seg, seg_len);
        ooo_freed = tcp_ooo_drain(conn);
        send_ack = 1;
      } else if (seg_len > 0 && (i32)(seg_seq - conn->rcv_nxt) > 0) {
        /* Past a hole. Only buffer what fits inside the window we advertised;
         * anything beyond it the peer should not have sent. */
        u32 wnd = conn->recv_cap - conn->recv_len;
        if ((u32)(seg_seq - conn->rcv_nxt) + seg_len <= wnd &&
            conn->ooo_segs < TCP_OOO_MAX_SEGS &&
            conn->ooo_bytes + seg_len <= TCP_OOO_MAX_BYTES) {
          /* Allocate outside the lock: kmalloc takes the heap lock, and this
           * path must not nest it under the TCP lock. */
          tcp_unlock();
          irq_restore(irq);
          struct tcp_ooo_seg *node = kmalloc(sizeof(struct tcp_ooo_seg));
          u8 *copy = node ? kmalloc(seg_len) : 0;
          if (node && copy)
            memcpy(copy, seg, seg_len);
          irq = irq_save();
          tcp_lock();
          if (node && copy && conn->used && conn->state == TCP_ESTABLISHED &&
              (i32)(seg_seq - conn->rcv_nxt) > 0) {
            node->seq = seg_seq;
            node->len = seg_len;
            node->data = copy;
            node->next = 0;
            if (tcp_ooo_insert(conn, node) != 0) {
              /* Rejected as a duplicate of something already queued — that is
               * also worth a D-SACK. */
              conn->dsack_pending = 1;
              conn->dsack_left = seg_seq;
              conn->dsack_right = seg_seq + seg_len;
              kfree(copy);
              kfree(node);
            } else {
              ooo_recent = seg_seq;
            }
          } else {
            /* The hole closed (or the connection went away) while the lock
             * was dropped: discard the copy rather than queueing a segment
             * that is now in the past. */
            if (copy)
              kfree(copy);
            if (node)
              kfree(node);
          }
        }
        /* Dup-ACK for rcv_nxt: tells the peer exactly which byte is missing. */
        send_ack = 1;
      } else if (seg_len == 0 && (i32)(seg_seq - conn->rcv_nxt) < 0) {
        /* An empty segment from behind the window: that is what a keepalive
         * probe is (RFC 1122 4.2.3.6 — sequence number one below the next
         * byte, deliberately unacceptable so the peer has to say something).
         * Answering it is the whole mechanism; staying quiet makes every
         * probe look unanswered and the sender eventually tears down a
         * connection that was fine. */
        send_ack = 1;
      }

      if (send_ack) {
        /* M84: when the reassembly queue is non-empty the ACK carries SACK
         * blocks, so the peer learns which segments past the hole already
         * arrived and retransmits only the hole. */
        u8 ack_pkt[TCP_ACK_MAX_LEN];
        usize ack_len = tcp_build_ack(conn, ack_pkt, 1, ooo_recent);
        u32 adv_wnd = conn->recv_cap - conn->recv_len;
        /* Remember if we just throttled the peer below 1 MSS so tcp_recv() knows
         * to send an unsolicited window-update once the app drains the buffer,
         * instead of leaving the peer parked on its zero-window persist timer. */
        if (adv_wnd < TCP_MSS)
          conn->wnd_closed = 1;

        tcp_unlock();
        irq_restore(irq);
        tcp_conn_emit(conn, ack_pkt, ack_len);
        tcp_free_ooo_list(ooo_freed);
        irq = irq_save();
        tcp_lock();

        extern void *vfs_poll_chan;
        scheduler_wake_all(vfs_poll_chan);
      } else if (ooo_freed) {
        tcp_unlock();
        irq_restore(irq);
        tcp_free_ooo_list(ooo_freed);
        irq = irq_save();
        tcp_lock();
      }
    }

    /* Handle FIN — but only when the stream has actually reached it. */
    int accept_fin = 0;

    if (flags & TCP_FIN) {
      u32 fin_at = seq + (u32)payload_size;

      if (fin_at == conn->rcv_nxt) {
        accept_fin = 1;
      } else {
        conn->fin_seq = fin_at;
        conn->fin_pending = 1;
      }
    }
    if (!accept_fin && conn->fin_pending && conn->rcv_nxt == conn->fin_seq)
      accept_fin = 1;
    if (accept_fin) {
      conn->fin_pending = 0;
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

      tcp_unlock();
      irq_restore(irq);
      tcp_conn_emit(conn, ack_pkt, sizeof(ack_pkt));
      irq = irq_save();
      tcp_lock();

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

  tcp_unlock();
  irq_restore(irq);
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

static volatile int tcp_timer_ticking = 0;

void tcp_timer_tick(void) {
  if (__atomic_test_and_set(&tcp_timer_ticking, __ATOMIC_ACQUIRE)) {
    return;
  }
  u64 now = scheduler_get_uptime_ticks();
  for (int i = 0; i < (int)resource_caps_tcp_max(); i++) {
    struct tcp_conn *conn = &tcp_conns[i];
    
    u64 irq = irq_save();
    tcp_lock();
    
    if (!conn->used) {
      tcp_unlock();
      irq_restore(irq);
      continue;
    }

    if (conn->state == TCP_TIME_WAIT) {
      if (now - conn->time_wait_since >= TCP_TIME_WAIT_TICKS) {
        tcp_unlock();
        irq_restore(irq);
        tcp_clear_retransmit_queue(conn);
        tcp_clear_ooo_queue(conn);
        tcp_free_recv_buf(conn);
        irq = irq_save();
        tcp_lock();
        conn->used = 0;
      }
      tcp_unlock();
      irq_restore(irq);
      continue;
    }

    /* Keepalive. Only an established connection with nothing queued is
     * probed: while data is in flight the retransmit timer is already asking
     * the same question, and a probe on top of it would just add a segment. */
    if (conn->keepalive && conn->state == TCP_ESTABLISHED &&
        !conn->retransmit_queue) {
      u64 idle = now - conn->last_activity;
      u64 first = (u64)conn->keepidle * TCP_TICKS_PER_SEC;
      u64 again = (u64)conn->keepintvl * TCP_TICKS_PER_SEC;
      u64 due = conn->keepalive_probes ? first + (u64)conn->keepalive_probes * again
                                       : first;

      if (idle >= due) {
        if (conn->keepalive_probes >= conn->keepcnt) {
          /* The peer has stopped answering: the connection is dead, and a
           * reader parked on it must be told rather than left waiting. */
          conn->state = TCP_CLOSED;
          conn->keepalive_probes = 0;
          k_info("tcp", "keepalive found a dead peer, connection closed");
          tcp_unlock();
          irq_restore(irq);
          tcp_clear_retransmit_queue(conn);
          tcp_clear_ooo_queue(conn);
          tcp_free_recv_buf(conn);
          irq = irq_save();
          tcp_lock();
          conn->used = 0;
          tcp_unlock();
          irq_restore(irq);
          continue;
        }
        u8 probe[TCP_ACK_MAX_LEN];
        usize probe_len = tcp_build_keepalive(conn, probe);
        conn->keepalive_probes++;
        tcp_unlock();
        irq_restore(irq);
        tcp_conn_emit(conn, probe, probe_len);
        irq = irq_save();
        tcp_lock();
      }
    }

    struct tcp_retransmit_pkt *rp = conn->retransmit_queue;
    while (rp) {
      /* M84: a selectively-acknowledged segment reached the peer. Do not burn
       * the window retransmitting it; it stays queued until the cumulative ACK
       * releases it. */
      if (rp->sacked) {
        rp = rp->next;
        continue;
      }
      if (now - rp->timestamp >= 50) { // 500ms
        if (rp->retries >= 5) {
          conn->state = TCP_CLOSED;
          tcp_unlock();
          irq_restore(irq);
          tcp_clear_retransmit_queue(conn);
          tcp_clear_ooo_queue(conn);
          tcp_free_recv_buf(conn);
          irq = irq_save();
          tcp_lock();
          conn->used = 0;
          break;
        }
        
        // Copy retransmit packet to stack to avoid Use-After-Free
        u8 family = conn->family;
        struct ipv4_addr remote_ip = conn->remote_ip;
        struct in6_addr_k remote_ip6 = conn->remote_ip6;
        u8 stack_buf[1500];
        usize pkt_len = rp->len;
        if (pkt_len > sizeof(stack_buf))
          pkt_len = sizeof(stack_buf);
        memcpy(stack_buf, rp->data, pkt_len);
        
        rp->timestamp = now;
        rp->retries++;
        
        tcp_unlock();
        irq_restore(irq);
        
        u32 tx = tcp_set_checksum(family, remote_ip, &remote_ip6, stack_buf,
                                  pkt_len);
        tcp_l3_send(family, remote_ip, &remote_ip6, stack_buf, pkt_len, tx);
        
        irq = irq_save();
        tcp_lock();
        
        // Restart iteration since lock was released and queue could be modified
        rp = conn->retransmit_queue;
        continue;
      }
      rp = rp->next;
    }
    tcp_unlock();
    irq_restore(irq);
  }
  __atomic_clear(&tcp_timer_ticking, __ATOMIC_RELEASE);
}

/* Map a b1nix TCP state to the Linux /proc/net/tcp "st" code that BusyBox
 * netstat parses. */
static int tcp_linux_st(int state) {
  switch (state) {
  case TCP_ESTABLISHED:  return 0x01;
  case TCP_SYN_SENT:     return 0x02;
  case TCP_SYN_RECEIVED: return 0x03;
  case TCP_FIN_WAIT1:    return 0x04;
  case TCP_FIN_WAIT2:    return 0x05;
  case TCP_TIME_WAIT:    return 0x06;
  case TCP_CLOSED:       return 0x07;
  case TCP_CLOSE_WAIT:   return 0x08;
  case TCP_LAST_ACK:     return 0x09;
  case TCP_LISTEN:       return 0x0A;
  case TCP_CLOSING:      return 0x0B;
  default:               return 0x07;
  }
}

usize tcp_conn_snapshot(struct net_sock_info *out, usize max) {
  usize n = 0;
  struct ipv4_addr myip = net_get_ip();
  u64 irq = irq_save();
  tcp_lock();
  for (int i = 0; i < (int)resource_caps_tcp_max() && n < max; i++) {
    if (!tcp_conns[i].used)
      continue;
    struct net_sock_info *e = &out[n++];
    memset(e, 0, sizeof(*e));
    e->family = (tcp_conns[i].family == B1NIX_AF_INET6) ? 6 : 4;
    e->local_port = tcp_conns[i].local_port;
    e->remote_port = tcp_conns[i].remote_port;
    e->state = tcp_linux_st(tcp_conns[i].state);
    if (e->family == 6) {
      memcpy(e->remote_ip, tcp_conns[i].remote_ip6.bytes, 16);
      /* listeners bind the wildcard address; established use the host IP6 */
      if (tcp_conns[i].state != TCP_LISTEN) {
        struct in6_addr_k l = net_get_ip6();
        memcpy(e->local_ip, l.bytes, 16);
      }
    } else {
      memcpy(e->remote_ip, tcp_conns[i].remote_ip.bytes, 4);
      if (tcp_conns[i].state != TCP_LISTEN)
        memcpy(e->local_ip, myip.bytes, 4);
    }
  }
  tcp_unlock();
  irq_restore(irq);
  return n;
}

/* ── M84 self-test: options, window scaling, out-of-order reassembly ───────
 * White-box, in-kernel: the option parser is driven directly with a crafted
 * header, and the reassembly path is exercised by injecting segments into
 * tcp_input() through the loopback datapath — the same code an off-link peer
 * reaches, minus the wire. */
void tcp_robustness_smoke(void) {
  /* 1. Option parsing: MSS + NOP-padded window scale + SACK-permitted. */
  {
    u8 seg[sizeof(struct tcp_header) + 12];
    memset(seg, 0, sizeof(seg));
    struct tcp_header *h = (struct tcp_header *)seg;
    h->data_offset = (u8)((5 + 3) << 4);
    u8 *o = seg + sizeof(struct tcp_header);
    o[0] = TCP_OPT_MSS;  o[1] = 4;  o[2] = 0x05; o[3] = 0xB4; /* 1460 */
    o[4] = TCP_OPT_SACK_PERM; o[5] = 2;
    o[6] = TCP_OPT_NOP;
    o[7] = TCP_OPT_WSCALE; o[8] = 3; o[9] = 7;
    o[10] = TCP_OPT_END;
    o[11] = 0;

    struct tcp_opts opt;
    tcp_parse_options(seg, (5 + 3) * 4, &opt);
    if (opt.mss == 1460 && opt.has_wscale && opt.wscale == 7 && opt.sack_ok)
      k_info(NULL, "M84-TCP: ok opt-parse");
    else
      k_info(NULL, "M84-TCP: FAIL opt-parse");

    /* A truncated / malformed option must terminate the walk instead of
     * running off the end or spinning on a zero length. */
    memset(seg, 0, sizeof(seg));
    h->data_offset = (u8)((5 + 3) << 4);
    o[0] = TCP_OPT_WSCALE; o[1] = 0; /* illegal length */
    tcp_parse_options(seg, (5 + 3) * 4, &opt);
    if (!opt.has_wscale && opt.mss == 0)
      k_info(NULL, "M84-TCP: ok opt-malformed");
    else
      k_info(NULL, "M84-TCP: FAIL opt-malformed");
  }

  /* 2/3/4. Live loopback connection: MSS negotiation, scaled window
   * interpretation, and out-of-order reassembly. */
  struct ipv4_addr lo = {{127, 0, 0, 1}};
  u16 port = 7940;
  struct tcp_conn *srv = tcp_listen(port, 1);
  if (!srv) {
    k_info(NULL, "M84-TCP: FAIL listen");
    return;
  }
  struct tcp_conn *cli = tcp_connect(lo, port);
  if (!cli) {
    k_info(NULL, "M84-TCP: FAIL connect");
    tcp_close(srv);
    return;
  }
  struct ipv4_addr peer_ip;
  u16 peer_port = 0;
  struct tcp_conn *acc = 0;
  for (int i = 0; i < 100 && !acc; i++) {
    acc = tcp_accept(port, &peer_ip, &peer_port);
    if (!acc)
      net_poll();
  }
  if (!acc) {
    k_info(NULL, "M84-TCP: FAIL accept");
    tcp_close(cli);
    tcp_close(srv);
    return;
  }

  /* Both SYNs carried our MSS, window-scale and SACK-permitted options, so
   * each side learned the other's. */
  if (cli->snd_mss == TCP_MSS && acc->snd_mss == TCP_MSS && cli->wscale_ok &&
      acc->wscale_ok && cli->sack_ok && acc->sack_ok)
    k_info(NULL, "M84-TCP: ok mss-negotiated");
  else
    k_info(NULL, "M84-TCP: FAIL mss-negotiated");

  /* The advertised window is our real buffer, shifted by the scale we
   * negotiated — a receive window past the unscaled 16-bit ceiling. The
   * buffer starts at TCP_RECV_BUF_INIT and doubles under load, so the
   * advertisement is checked against this connection's current capacity; the
   * scale is fixed at handshake time from the ceiling the buffer may reach,
   * because the shift cannot be renegotiated later. */
  {
    u32 advertised = (u32)tcp_adv_window(acc) << acc->rcv_wscale;
    if (acc->rcv_wscale > 0 && acc->recv_cap > 0 &&
        advertised == acc->recv_cap && TCP_RECV_BUF_MAX > 65535)
      k_info(NULL, "M84-TCP: ok rcv-wscale");
    else
      k_info(NULL, "M84-TCP: FAIL rcv-wscale");
  }

  /* Window scaling: with a shift of 3 in effect, a 1000-byte advertisement
   * must be read as 8000 bytes of peer buffer. */
  {
    cli->snd_wscale = 3;
    u8 ackseg[sizeof(struct tcp_header)];
    memset(ackseg, 0, sizeof(ackseg));
    struct tcp_header *h = (struct tcp_header *)ackseg;
    h->src_port = bswap16(port);
    h->dst_port = bswap16(cli->local_port);
    h->seq_num = bswap32(cli->rcv_nxt);
    h->ack_num = bswap32(cli->snd_nxt);
    h->data_offset = (5 << 4);
    h->flags = TCP_ACK;
    h->window = bswap16(1000);
    tcp_receive(lo, ackseg, sizeof(ackseg));

    if (cli->snd_wnd == 8000)
      k_info(NULL, "M84-TCP: ok wscale");
    else
      k_info(NULL, "M84-TCP: FAIL wscale");
    cli->snd_wscale = 0;
  }

  /* Out-of-order reassembly: deliver "world" (the second half) first, then
   * "hello". The first must be queued, not dropped, and the pair must read
   * back as one contiguous stream. */
  {
    u32 base = acc->rcv_nxt;
    u8 seg[sizeof(struct tcp_header) + 5];
    struct tcp_header *h = (struct tcp_header *)seg;

    memset(seg, 0, sizeof(seg));
    h->src_port = bswap16(cli->local_port);
    h->dst_port = bswap16(port);
    h->seq_num = bswap32(base + 5);
    h->ack_num = bswap32(acc->snd_nxt);
    h->data_offset = (5 << 4);
    h->flags = TCP_ACK | TCP_PSH;
    h->window = bswap16(TCP_SYN_WINDOW);
    memcpy(seg + sizeof(struct tcp_header), "world", 5);
    tcp_receive(lo, seg, sizeof(seg));

    int queued = (acc->ooo_segs == 1 && acc->recv_len == 0 &&
                  acc->rcv_nxt == base);
    if (queued)
      k_info(NULL, "M84-TCP: ok ooo-queued");
    else
      k_info(NULL, "M84-TCP: FAIL ooo-queued");

    memset(seg, 0, sizeof(seg));
    h->src_port = bswap16(cli->local_port);
    h->dst_port = bswap16(port);
    h->seq_num = bswap32(base);
    h->ack_num = bswap32(acc->snd_nxt);
    h->data_offset = (5 << 4);
    h->flags = TCP_ACK | TCP_PSH;
    h->window = bswap16(TCP_SYN_WINDOW);
    memcpy(seg + sizeof(struct tcp_header), "hello", 5);
    tcp_receive(lo, seg, sizeof(seg));

    char buf[16];
    memset(buf, 0, sizeof(buf));
    int n = tcp_recv(acc, buf, sizeof(buf) - 1, 0);
    if (n == 10 && memcmp(buf, "helloworld", 10) == 0 && acc->ooo_segs == 0 &&
        acc->rcv_nxt == base + 10)
      k_info(NULL, "M84-TCP: ok ooo-reassembly");
    else
      k_info(NULL, "M84-TCP: FAIL ooo-reassembly");

    /* A retransmission of already-delivered bytes must be trimmed, not
     * re-appended to the stream. */
    memset(seg, 0, sizeof(seg));
    h->src_port = bswap16(cli->local_port);
    h->dst_port = bswap16(port);
    h->seq_num = bswap32(base);
    h->ack_num = bswap32(acc->snd_nxt);
    h->data_offset = (5 << 4);
    h->flags = TCP_ACK | TCP_PSH;
    h->window = bswap16(TCP_SYN_WINDOW);
    memcpy(seg + sizeof(struct tcp_header), "hello", 5);
    tcp_receive(lo, seg, sizeof(seg));
    if (acc->rcv_nxt == base + 10 && acc->recv_len == 0)
      k_info(NULL, "M84-TCP: ok dup-trim");
    else
      k_info(NULL, "M84-TCP: FAIL dup-trim");
  }

  /* SACK emission: with a hole in the stream, the ACK the receiver builds must
   * carry a block describing exactly the queued range. */
  {
    u32 base = acc->rcv_nxt;
    u8 seg[sizeof(struct tcp_header) + 4];
    struct tcp_header *h = (struct tcp_header *)seg;
    memset(seg, 0, sizeof(seg));
    h->src_port = bswap16(cli->local_port);
    h->dst_port = bswap16(port);
    h->seq_num = bswap32(base + 8); /* 8-byte hole in front */
    h->ack_num = bswap32(acc->snd_nxt);
    h->data_offset = (5 << 4);
    h->flags = TCP_ACK | TCP_PSH;
    h->window = bswap16(TCP_SYN_WINDOW);
    memcpy(seg + sizeof(struct tcp_header), "SACK", 4);
    tcp_receive(lo, seg, sizeof(seg));

    u8 ack[TCP_ACK_MAX_LEN];
    u64 irq = irq_save();
    tcp_lock();
    usize alen = tcp_build_ack(acc, ack, 1, base + 8);
    tcp_unlock();
    irq_restore(irq);

    struct tcp_opts o;
    tcp_parse_options(ack, ((ack[12] >> 4) * 4), &o);
    if (alen > sizeof(struct tcp_header) && o.nsack == 1 &&
        o.sack_left[0] == base + 8 && o.sack_right[0] == base + 12)
      k_info(NULL, "M84-TCP: ok sack-emit");
    else
      k_info(NULL, "M84-TCP: FAIL sack-emit");
  }

  /* SACK consumption: a dup-ACK carrying a block covering the second queued
   * segment must mark it, leaving the first (the real hole) unmarked so the
   * fast-retransmit path resends that one.
   *
   * This runs on its own connection pair: the injections above deliberately
   * faked data the client never sent, which leaves the first client's send
   * sequence state ahead of itself (its peer ACKed bytes it never queued), and
   * tcp_send() would correctly refuse to send more on it. */
  {
    u16 port2 = 7941;
    struct tcp_conn *srv2 = tcp_listen(port2, 1);
    struct tcp_conn *cli2 = srv2 ? tcp_connect(lo, port2) : 0;
    struct tcp_conn *acc2 = 0;
    for (int i = 0; i < 100 && cli2 && !acc2; i++) {
      acc2 = tcp_accept(port2, &peer_ip, &peer_port);
      if (!acc2)
        net_poll();
    }

    int rc1 = cli2 ? tcp_send(cli2, "0123456789", 10) : -1;
    int rc2 = cli2 ? tcp_send(cli2, "abcdefghij", 10) : -1;
    struct tcp_retransmit_pkt *first = cli2 ? cli2->retransmit_queue : 0;
    struct tcp_retransmit_pkt *second = first ? first->next : 0;
    int ok = (acc2 && rc1 == 10 && rc2 == 10 && first && second);

    if (ok) {
      struct tcp_conn *cli = cli2;
      u16 port = port2;
      u8 sackack[sizeof(struct tcp_header) + 12];
      memset(sackack, 0, sizeof(sackack));
      struct tcp_header *h = (struct tcp_header *)sackack;
      h->src_port = bswap16(port);
      h->dst_port = bswap16(cli->local_port);
      h->seq_num = bswap32(cli->rcv_nxt);
      h->ack_num = bswap32(cli->snd_una); /* duplicate ACK: hole not filled */
      h->data_offset = (u8)((5 + 3) << 4);
      h->flags = TCP_ACK;
      h->window = bswap16(1000);
      u8 *o = sackack + sizeof(struct tcp_header);
      o[0] = TCP_OPT_NOP;
      o[1] = TCP_OPT_NOP;
      o[2] = TCP_OPT_SACK;
      o[3] = 10;
      u32 l = second->seq, r = second->seq + second->dlen;
      o[4] = (u8)(l >> 24); o[5] = (u8)(l >> 16);
      o[6] = (u8)(l >> 8);  o[7] = (u8)l;
      o[8] = (u8)(r >> 24); o[9] = (u8)(r >> 16);
      o[10] = (u8)(r >> 8); o[11] = (u8)r;
      tcp_receive(lo, sackack, sizeof(sackack));

      ok = (second->sacked == 1 && first->sacked == 0);
    }
    console_write(ok ? "M84-TCP: ok sack-consume\n"
                     : "M84-TCP: FAIL sack-consume\n");

    /* Scoreboard: with three segments SACKed above the head, the head is
     * declared lost, the pipe estimate drops by what left the network, and
     * entering recovery leaves cwnd at ssthresh instead of the Reno
     * ssthresh + 3*MSS inflation. */
    if (ok && cli2) {
      int sb_ok = 1;
      /* Build the scenario inside the lock and re-check it there: the net_task
       * daemon drains the loopback queue in the background, so peer ACKs can
       * retire queued segments between two of our calls. Send until at least
       * four segments are outstanding at the instant we look. */
      u64 irq = 0;
      struct tcp_retransmit_pkt *head = 0;
      u32 total = 0;
      int n = 0;
      for (int attempt = 0; attempt < 12; attempt++) {
        irq = irq_save();
        tcp_lock();
        n = 0;
        for (struct tcp_retransmit_pkt *rp = cli2->retransmit_queue; rp;
             rp = rp->next)
          n++;
        tcp_unlock();
        irq_restore(irq);
        if (n >= 4)
          break;
        if (tcp_send(cli2, "xxxxxxxxxx", 10) != 10)
          sb_ok = 0;
      }

      irq = irq_save();
      tcp_lock();
      head = cli2->retransmit_queue;
      total = 0;
      n = 0;
      for (struct tcp_retransmit_pkt *rp = head; rp; rp = rp->next) {
        total += rp->dlen;
        n++;
        /* Scenario: the head is the hole, everything above it arrived. The
         * head may already carry a SACK mark from the previous step, so clear
         * it explicitly rather than assuming. */
        rp->sacked = (u8)(n > 1);
        rp->retransmitted = 0;
        rp->lost = 0;
      }
      /* Start from a clean congestion state: earlier steps in this test drove
       * duplicate ACKs through the connection. */
      cli2->in_recovery = 0;
      cli2->dup_acks = 0;
      tcp_scoreboard_update(cli2);
      u32 pipe = tcp_pipe(cli2);
      int head_lost = head && head->lost;
      u32 sacked = cli2->sacked_bytes;
      cli2->cwnd = 8 * TCP_MSS;
      tcp_enter_recovery(cli2);
      u32 cwnd = cli2->cwnd, ssthresh = cli2->ssthresh;
      tcp_unlock();
      irq_restore(irq);

      sb_ok = sb_ok && n >= 4 && head_lost && sacked == total - head->dlen &&
              pipe == 0 && cwnd == ssthresh && cwnd == 4 * TCP_MSS;
      if (sb_ok) {
        console_write("M84-TCP: ok scoreboard-pipe\n");
      } else {
        console_write("M84-TCP: FAIL scoreboard-pipe n=");
        console_write_dec((u64)n);
        console_write(" lost=");
        console_write_dec((u64)head_lost);
        console_write(" sacked=");
        console_write_dec((u64)sacked);
        console_write(" total=");
        console_write_dec((u64)total);
        console_write(" pipe=");
        console_write_dec((u64)pipe);
        console_write(" cwnd=");
        console_write_dec((u64)cwnd);
        console_write(" ssthresh=");
        console_write_dec((u64)ssthresh);
        console_write("\n");
      }

      /* D-SACK undo: a block below the cumulative ACK says the retransmission
       * was spurious, so the congestion reduction must be rolled back. */
      u8 dpkt[sizeof(struct tcp_header) + 12];
      memset(dpkt, 0, sizeof(dpkt));
      struct tcp_header *h = (struct tcp_header *)dpkt;
      h->src_port = bswap16(port2);
      h->dst_port = bswap16(cli2->local_port);
      h->seq_num = bswap32(cli2->rcv_nxt);
      h->ack_num = bswap32(cli2->snd_una);
      h->data_offset = (u8)((5 + 3) << 4);
      h->flags = TCP_ACK;
      h->window = bswap16(4096);
      u8 *o = dpkt + sizeof(struct tcp_header);
      o[0] = TCP_OPT_NOP;
      o[1] = TCP_OPT_NOP;
      o[2] = TCP_OPT_SACK;
      o[3] = 10;
      u32 dl = cli2->snd_una - 10, dr = cli2->snd_una;
      o[4] = (u8)(dl >> 24); o[5] = (u8)(dl >> 16);
      o[6] = (u8)(dl >> 8);  o[7] = (u8)dl;
      o[8] = (u8)(dr >> 24); o[9] = (u8)(dr >> 16);
      o[10] = (u8)(dr >> 8); o[11] = (u8)dr;
      u32 prior = cli2->prior_cwnd;
      u32 seen_before = cli2->dsack_seen;
      tcp_receive(lo, dpkt, sizeof(dpkt));
      int undo_ok = cli2->dsack_seen == seen_before + 1 &&
                    !cli2->in_recovery && cli2->cwnd == prior;
      console_write(undo_ok ? "M84-TCP: ok dsack-undo\n"
                            : "M84-TCP: FAIL dsack-undo\n");

      /* End-to-end D-SACK: let the peer receive a segment, then replay it.
       * The receiver must report the duplicate as a D-SACK block, and this
       * sender must recognise it — neither side is faked, the block travels
       * over the loopback datapath. */
      for (int i = 0; i < 60 && acc2 && cli2->snd_una != cli2->snd_nxt; i++)
        net_poll();
      u32 dsack_before = cli2->dsack_seen;
      u32 dup_seq = cli2->snd_nxt - 10;
      u8 dup[sizeof(struct tcp_header) + 10];
      memset(dup, 0, sizeof(dup));
      struct tcp_header *dh = (struct tcp_header *)dup;
      dh->src_port = bswap16(cli2->local_port);
      dh->dst_port = bswap16(port2);
      dh->seq_num = bswap32(dup_seq);
      dh->ack_num = bswap32(acc2->snd_nxt);
      dh->data_offset = (5 << 4);
      dh->flags = TCP_ACK | TCP_PSH;
      dh->window = bswap16(TCP_SYN_WINDOW);
      memcpy(dup + sizeof(struct tcp_header), "xxxxxxxxxx", 10);
      tcp_receive(lo, dup, sizeof(dup));
      for (int i = 0; i < 60 && cli2->dsack_seen == dsack_before; i++)
        net_poll();
      console_write(cli2->dsack_seen > dsack_before
                        ? "M84-TCP: ok dsack-emit\n"
                        : "M84-TCP: FAIL dsack-emit\n");
    }
    if (acc2)
      tcp_close(acc2);
    if (cli2)
      tcp_close(cli2);
    if (srv2)
      tcp_close(srv2);
  }

  tcp_close(acc);
  tcp_close(cli);
  tcp_close(srv);
}
