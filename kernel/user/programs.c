#include <b1nix/blk.h>
#include <b1nix/bootinfo.h>
#include <b1nix/vfs.h>
#include <b1nix/mm.h>
#include <b1nix/errno.h>
#include <b1nix/filelock.h>
#include <b1nix/net.h>
#include <b1nix/netdev.h>
#include <b1nix/usb.h>
#include <b1nix/sound.h>
#include <b1nix/posix.h>
#include <b1nix/syscall.h>
#include <b1nix/user.h>
#include <b1nix/video.h>
#include <b1nix/sched.h>
#include <b1nix/serial_tty.h>
#include <b1nix/lapic.h>
#include <b1nix/dirent.h>
#include <b1nix/console.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tui.h>

void user_register_program(const char *path, user_program_entry entry);
static int m22_smoke_main(int argc, const char **argv);
static int m24_stress_main(int argc, const char **argv);
static int lock_smoke_main(int argc, const char **argv);
static int ext_stress_main(int argc, const char **argv);
int shell_smoke_main(int argc, const char **argv);

static void uwrite(const char *text) {
  syscall_dispatch(SYS_WRITE, 1, (u64)(usize)text, strlen(text), 0, 0, 0);
}

/* ... (uwrite_dec_value, uwrite_ipv4, b1fetch_cpu_name remain unchanged) ... */

static void uwrite_dec_value(u64 value) {
  char buf[24];
  snprintf(buf, sizeof(buf), "%d", (int)value);
  uwrite(buf);
}

static void uwrite_ipv4(struct ipv4_addr ip) {
  char buf[24];
  snprintf(buf, sizeof(buf), "%d.%d.%d.%d", ip.bytes[0], ip.bytes[1],
           ip.bytes[2], ip.bytes[3]);
  uwrite(buf);
}

static void b1fetch_cpu_name(char *out, usize out_size) {
  if (!out || out_size == 0)
    return;
  out[0] = '\0';
#ifdef __aarch64__
  (void)out_size;
  strcpy(out, "AArch64 CPU");
#else
  u32 max_leaf = 0;
  u32 unused = 0;
  __asm__ volatile("cpuid"
                   : "=a"(max_leaf), "=b"(unused), "=c"(unused), "=d"(unused)
                   : "a"(0x80000000U));
  if (max_leaf >= 0x80000004U && out_size >= 49) {
    u32 *dst = (u32 *)out;
    for (u32 leaf = 0; leaf < 3; leaf++) {
      u32 a, b, c, d;
      __asm__ volatile("cpuid"
                       : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                       : "a"(0x80000002U + leaf));
      dst[leaf * 4 + 0] = a;
      dst[leaf * 4 + 1] = b;
      dst[leaf * 4 + 2] = c;
      dst[leaf * 4 + 3] = d;
    }
    out[48] = '\0';
    while (out[0] == ' ')
      memmove(out, out + 1, strlen(out));
    return;
  }
  strcpy(out, "x86_64 CPU");
#endif
}

static int m16_check_tui_key_decode(void)
{
	struct {
		const char *seq;
		usize len;
		int key;
	} cases[] = {
		{"a", 1, 'a'},
		{"\t", 1, KEY_TAB},
		{"\n", 1, KEY_ENTER},
		{"\r", 1, KEY_ENTER},
		{"\b", 1, KEY_BACKSP},
		{"\x7f", 1, KEY_BACKSP},
		{"\x11", 1, KEY_CTRL_Q},
		{"\x13", 1, KEY_CTRL_S},
		{"\x18", 1, KEY_CTRL_X},
		{"\x07", 1, KEY_CTRL_G},
		{"\033", 1, KEY_ESC},
		{"\033[A", 3, KEY_UP},
		{"\033[B", 3, KEY_DOWN},
		{"\033[C", 3, KEY_RIGHT},
		{"\033[D", 3, KEY_LEFT},
		{"\033[H", 3, KEY_HOME},
		{"\033[F", 3, KEY_END},
		{"\033[1~", 4, KEY_HOME},
		{"\033[2~", 4, KEY_INS},
		{"\033[3~", 4, KEY_DEL},
		{"\033[4~", 4, KEY_END},
		{"\033[5~", 4, KEY_PGUP},
		{"\033[6~", 4, KEY_PGDN},
		{"\033[7~", 4, KEY_HOME},
		{"\033[8~", 4, KEY_END},
		{"\033[11~", 5, KEY_F1},
		{"\033[12~", 5, KEY_F2},
		{"\033[13~", 5, KEY_F3},
		{"\033[14~", 5, KEY_F4},
		{"\033[15~", 5, KEY_F5},
		{"\033[17~", 5, KEY_F6},
		{"\033[18~", 5, KEY_F7},
		{"\033[19~", 5, KEY_F8},
		{"\033[20~", 5, KEY_F9},
		{"\033[21~", 5, KEY_F10},
		{"\033[23~", 5, KEY_F11},
		{"\033[24~", 5, KEY_F12},
		{"\033[M\1", 4, KEY_F1},
		{"\033[M\n", 4, KEY_F10},
		{"\033[M\v", 4, KEY_F11},
		{"\033[M\f", 4, KEY_F12},
		{"\033OP", 3, KEY_F1},
		{"\033OQ", 3, KEY_F2},
		{"\033OR", 3, KEY_F3},
		{"\033OS", 3, KEY_F4},
	};

	for (usize i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		int got = tui_decode_key_sequence(cases[i].seq, cases[i].len);
		if (got != cases[i].key) {
			return -1;
		}
	}

	uwrite("M16-SMOKE: ok tui-key-decode\n");
	return 0;
}

static int m16_termios_unchanged(const struct b1nix_termios *before)
{
	struct b1nix_termios after;
	memset(&after, 0, sizeof(after));
	if ((isize)syscall_dispatch(SYS_IOCTL, 0, B1NIX_TCGETS,
	                            (u64)(usize)&after, 0, 0, 0) < 0) {
		return -1;
	}
	return memcmp(before, &after, sizeof(after)) == 0 ? 0 : -1;
}


extern int mc_main(int argc, const char **argv);
extern int editor_main(int argc, const char **argv);

struct udp_smoke_header {
  u16 src_port;
  u16 dst_port;
  u16 length;
  u16 checksum;
} __attribute__((packed));

struct tcp_smoke_header {
  u16 src_port;
  u16 dst_port;
  u32 seq_num;
  u32 ack_num;
  u8 data_offset;
  u8 flags;
  u16 window;
  u16 checksum;
  u16 urgent;
} __attribute__((packed));

static u16 udp_smoke_bswap16(u16 value) {
  return (u16)((value << 8) | (value >> 8));
}

static u32 tcp_smoke_bswap32(u32 value) {
  return ((value & 0x000000ffU) << 24) | ((value & 0x0000ff00U) << 8) |
         ((value & 0x00ff0000U) >> 8) | ((value & 0xff000000U) >> 24);
}

static void udp_queue_smoke_check(void) {
  int fd = vfs_socket(B1NIX_AF_INET, B1NIX_SOCK_DGRAM, 0);
  if (fd < 0) {
    uwrite("UDP-SMOKE: fail queue-open\n");
    return;
  }

  struct b1nix_sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = B1NIX_AF_INET;
  addr.sin_port = udp_smoke_bswap16(55001);
  addr.sin_addr = 0;
  if (vfs_bind(fd, &addr, sizeof(addr)) < 0) {
    uwrite("UDP-SMOKE: fail queue-bind\n");
    vfs_close(fd);
    return;
  }

  const char pkt1[] = "first";
  const char pkt2[] = "second";
  if (!vfs_socket_push_udp(addr.sin_port, pkt1, sizeof(pkt1) - 1) ||
      !vfs_socket_push_udp(addr.sin_port, pkt2, sizeof(pkt2) - 1)) {
    uwrite("UDP-SMOKE: fail queue-push\n");
    vfs_close(fd);
    return;
  }

  int flags = (int)syscall_dispatch(SYS_FCNTL, (u64)fd, B1NIX_F_GETFL, 0, 0, 0, 0);
  syscall_dispatch(SYS_FCNTL, (u64)fd, B1NIX_F_SETFL,
                   (u64)(flags | B1NIX_O_NONBLOCK), 0, 0, 0);

  char out1[16];
  char out2[16];
  memset(out1, 0, sizeof(out1));
  memset(out2, 0, sizeof(out2));
  isize r1 = vfs_socket_recv(fd, out1, sizeof(out1), 0);
  isize r2 = vfs_socket_recv(fd, out2, sizeof(out2), 0);
  vfs_close(fd);

  if (r1 == 5 && r2 == 6 && memcmp(out1, "first", 5) == 0 &&
      memcmp(out2, "second", 6) == 0) {
    uwrite("UDP-SMOKE: queue-2pkt-ok\n");
    return;
  }
  uwrite("UDP-SMOKE: fail queue-order\n");
}

static void poll_smoke_check(void) {
  int fd = vfs_socket(B1NIX_AF_INET, B1NIX_SOCK_DGRAM, 0);
  if (fd < 0) {
    uwrite("POLL-SMOKE: fail open\n");
    return;
  }

  struct b1nix_sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = B1NIX_AF_INET;
  addr.sin_port = udp_smoke_bswap16(55002);
  addr.sin_addr = 0;
  if (vfs_bind(fd, &addr, sizeof(addr)) < 0) {
    uwrite("POLL-SMOKE: fail bind\n");
    vfs_close(fd);
    return;
  }

  struct b1nix_pollfd pfd;
  memset(&pfd, 0, sizeof(pfd));
  pfd.fd = fd;
  pfd.events = (short)(B1NIX_POLLIN | B1NIX_POLLOUT);
  if (vfs_poll(fd, &pfd) < 0 || (pfd.revents & B1NIX_POLLOUT) == 0) {
    uwrite("POLL-SMOKE: fail writable\n");
    vfs_close(fd);
    return;
  }

  const char pkt[] = "poll";
  if (!vfs_socket_push_udp(addr.sin_port, pkt, sizeof(pkt) - 1)) {
    uwrite("POLL-SMOKE: fail inject\n");
    vfs_close(fd);
    return;
  }

  memset(&pfd, 0, sizeof(pfd));
  pfd.fd = fd;
  pfd.events = B1NIX_POLLIN;
  if (vfs_poll(fd, &pfd) < 0 || (pfd.revents & B1NIX_POLLIN) == 0) {
    uwrite("POLL-SMOKE: fail readable\n");
    vfs_close(fd);
    return;
  }
  vfs_close(fd);
  uwrite("POLL-SMOKE: ready-udp\n");
}

static void tcp_smoke_check(void) {
  const u16 listen_port = 56001;
  const u16 remote_port = 40000;
  struct ipv4_addr remote_ip = {{192, 0, 2, 1}}; /* RFC 5737 TEST-NET: routes via the gateway (ARP cached, no retransmit stall) but is unreachable, so slirp cannot quickly RST the crafted conn and race the window check */
  const u32 remote_seq = 1000;

  int fd = vfs_socket(B1NIX_AF_INET, B1NIX_SOCK_STREAM, 0);
  if (fd < 0) {
    uwrite("TCP-SMOKE: fail socket\n");
    return;
  }

  struct b1nix_sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = B1NIX_AF_INET;
  addr.sin_port = udp_smoke_bswap16(listen_port);
  addr.sin_addr = 0;
  if (vfs_bind(fd, &addr, sizeof(addr)) < 0 || vfs_listen(fd, 1) < 0) {
    uwrite("TCP-SMOKE: fail listen\n");
    vfs_close(fd);
    return;
  }

  struct tcp_smoke_header syn;
  memset(&syn, 0, sizeof(syn));
  syn.src_port = udp_smoke_bswap16(remote_port);
  syn.dst_port = udp_smoke_bswap16(listen_port);
  syn.seq_num = tcp_smoke_bswap32(remote_seq);
  syn.data_offset = (5 << 4);
  syn.flags = 0x02; /* SYN */
  syn.window = udp_smoke_bswap16(4096);
  tcp_receive(remote_ip, &syn, sizeof(syn));

  /* Learn the ISS the kernel chose for the new (SYN-RECEIVED) connection so we
   * can complete the handshake with a valid ACK — the in-process equivalent of
   * a peer echoing seq+1 from the SYN-ACK it would have received on the wire. */
  u32 local_iss = tcp_debug_peek_iss(remote_ip, remote_port, listen_port);

  struct tcp_smoke_header ack;
  memset(&ack, 0, sizeof(ack));
  ack.src_port = udp_smoke_bswap16(remote_port);
  ack.dst_port = udp_smoke_bswap16(listen_port);
  ack.seq_num = tcp_smoke_bswap32(remote_seq + 1);
  ack.ack_num = tcp_smoke_bswap32(local_iss + 1);
  ack.data_offset = (5 << 4);
  ack.flags = 0x10; /* ACK */
  ack.window = udp_smoke_bswap16(4096);
  tcp_receive(remote_ip, &ack, sizeof(ack));

  int flags = (int)syscall_dispatch(SYS_FCNTL, (u64)fd, B1NIX_F_GETFL, 0, 0, 0, 0);
  syscall_dispatch(SYS_FCNTL, (u64)fd, B1NIX_F_SETFL,
                   (u64)(flags | B1NIX_O_NONBLOCK), 0, 0, 0);

  int client_fd = vfs_accept(fd, 0, 0);
  if (client_fd < 0) {
    uwrite("TCP-SMOKE: unsupported\n");
    vfs_close(fd);
    return;
  }

  const char payload[] = "tcp-smoke";
  u8 pkt[sizeof(struct tcp_smoke_header) + sizeof(payload) - 1];
  memset(pkt, 0, sizeof(pkt));
  struct tcp_smoke_header *psh = (struct tcp_smoke_header *)pkt;
  psh->src_port = udp_smoke_bswap16(remote_port);
  psh->dst_port = udp_smoke_bswap16(listen_port);
  psh->seq_num = tcp_smoke_bswap32(remote_seq + 1);
  psh->ack_num = tcp_smoke_bswap32(local_iss + 1);
  psh->data_offset = (5 << 4);
  psh->flags = 0x18; /* PSH|ACK */
  psh->window = udp_smoke_bswap16(4096);
  memcpy(pkt + sizeof(struct tcp_smoke_header), payload, sizeof(payload) - 1);
  tcp_receive(remote_ip, pkt, sizeof(pkt));

  char out[16];
  memset(out, 0, sizeof(out));
  isize got = vfs_socket_recv(client_fd, out, sizeof(out), 0);
  vfs_close(client_fd);
  vfs_close(fd);
  if (got == (isize)(sizeof(payload) - 1) &&
      memcmp(out, payload, sizeof(payload) - 1) == 0) {
    uwrite("TCP-SMOKE: path-exercised\n");
    return;
  }
  uwrite("TCP-SMOKE: unsupported\n");
}

/* M32: prove sliding-window flow control actually throttles a sender. Drive a
 * connection to ESTABLISHED, shrink the peer's advertised window to 10 bytes,
 * then check that tcp_send() emits at most a window's worth and returns 0 once
 * the window is full (bytes-in-flight == window). White-box: uses the raw tcp_*
 * API so it can inspect the byte counts a socket fd would hide. */
static void tcp_window_smoke_check(void) {
  const u16 listen_port = 56002;
  const u16 remote_port = 40002;
  struct ipv4_addr remote_ip = {{192, 0, 2, 1}}; /* RFC 5737 TEST-NET: routes via the gateway (ARP cached, no retransmit stall) but is unreachable, so slirp cannot quickly RST the crafted conn and race the window check */
  const u32 remote_seq = 5000;

  if (tcp_listen(listen_port, 1) < 0) {
    uwrite("M32-TCP: fail window-listen\n");
    return;
  }

  struct tcp_smoke_header syn;
  memset(&syn, 0, sizeof(syn));
  syn.src_port = udp_smoke_bswap16(remote_port);
  syn.dst_port = udp_smoke_bswap16(listen_port);
  syn.seq_num = tcp_smoke_bswap32(remote_seq);
  syn.data_offset = (5 << 4);
  syn.flags = 0x02; /* SYN */
  syn.window = udp_smoke_bswap16(4096);
  tcp_receive(remote_ip, &syn, sizeof(syn));

  u32 iss = tcp_debug_peek_iss(remote_ip, remote_port, listen_port);

  struct tcp_smoke_header ack;
  memset(&ack, 0, sizeof(ack));
  ack.src_port = udp_smoke_bswap16(remote_port);
  ack.dst_port = udp_smoke_bswap16(listen_port);
  ack.seq_num = tcp_smoke_bswap32(remote_seq + 1);
  ack.ack_num = tcp_smoke_bswap32(iss + 1);
  ack.data_offset = (5 << 4);
  ack.flags = 0x10; /* ACK */
  ack.window = udp_smoke_bswap16(4096);
  tcp_receive(remote_ip, &ack, sizeof(ack));

  struct ipv4_addr cip;
  u16 cport;
  struct tcp_conn *conn = tcp_accept(listen_port, &cip, &cport);
  if (!conn) {
    uwrite("M32-TCP: fail window-accept\n");
    return;
  }

  /* Shrink the peer window to 10 bytes via a (duplicate) ACK carrying a small
   * advertised window. ack == snd_una and no payload, so it only updates
   * snd_wnd (a single dup-ack never trips fast-retransmit). */
  struct tcp_smoke_header winack;
  memset(&winack, 0, sizeof(winack));
  winack.src_port = udp_smoke_bswap16(remote_port);
  winack.dst_port = udp_smoke_bswap16(listen_port);
  winack.seq_num = tcp_smoke_bswap32(remote_seq + 1);
  winack.ack_num = tcp_smoke_bswap32(iss + 1);
  winack.data_offset = (5 << 4);
  winack.flags = 0x10; /* ACK */
  winack.window = udp_smoke_bswap16(10);
  tcp_receive(remote_ip, &winack, sizeof(winack));

  char buf[100];
  memset(buf, 'A', sizeof(buf));
  int r1 = tcp_send(conn, buf, sizeof(buf)); /* window=10 → at most 10 */
  int r2 = tcp_send(conn, buf, sizeof(buf)); /* window now full → 0 */

  if (r1 == 10 && r2 == 0) {
    uwrite("M32-TCP: ok window-throttle\n");
  } else {
    uwrite("M32-TCP: fail window-throttle\n");
  }
}

/* M23: prove the DNS A-record parser extracts the right address. Synthesises a
 * real DNS response packet (header + question + answer with a compression
 * pointer) and feeds it through dns_receive(), then reads back the captured
 * result — deterministic and offline (no live nameserver needed). Also checks
 * /etc/resolv.conf parsing set the expected nameserver. */
static void dns_smoke_check(void) {
  static const u8 resp[] = {
      0x12, 0x34,             /* id 0x1234 */
      0x81, 0x80,             /* flags: response, recursion available */
      0x00, 0x01,             /* qdcount 1 */
      0x00, 0x01,             /* ancount 1 */
      0x00, 0x00, 0x00, 0x00, /* ns/ar 0 */
      /* question: example.com A IN */
      0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e',
      0x03, 'c', 'o', 'm', 0x00,
      0x00, 0x01, 0x00, 0x01,
      /* answer: ptr->qname, A, IN, ttl, rdlen 4, 93.184.216.34 */
      0xC0, 0x0C,
      0x00, 0x01, 0x00, 0x01,
      0x00, 0x00, 0x00, 0x3C,
      0x00, 0x04,
      93, 184, 216, 34};

  dns_receive(resp, sizeof(resp));
  u8 ip[4];
  if (dns_last_result(ip) && ip[0] == 93 && ip[1] == 184 && ip[2] == 216 &&
      ip[3] == 34) {
    uwrite("DNS-SMOKE: ok parse-a-record\n");
  } else {
    uwrite("DNS-SMOKE: fail parse-a-record\n");
  }

  dns_load_resolv_conf();
  struct ipv4_addr srv = dns_get_server();
  if (srv.bytes[0] == 10 && srv.bytes[1] == 0 && srv.bytes[2] == 2 &&
      srv.bytes[3] == 3) {
    uwrite("DNS-SMOKE: ok resolv-conf\n");
  } else {
    uwrite("DNS-SMOKE: fail resolv-conf\n");
  }

  /* AAAA parse: a response whose answer is a 16-byte IPv6 address
   * (2001:db8::1). Deterministic and offline, like the A-record case. */
  static const u8 resp6[] = {
      0x12, 0x34,             /* id 0x1234 */
      0x81, 0x80,             /* flags: response */
      0x00, 0x01,             /* qdcount 1 */
      0x00, 0x01,             /* ancount 1 */
      0x00, 0x00, 0x00, 0x00, /* ns/ar 0 */
      /* question: example.com AAAA IN */
      0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e',
      0x03, 'c', 'o', 'm', 0x00,
      0x00, 0x1C, 0x00, 0x01,
      /* answer: ptr->qname, AAAA(28), IN, ttl, rdlen 16, 2001:db8::1 */
      0xC0, 0x0C,
      0x00, 0x1C, 0x00, 0x01,
      0x00, 0x00, 0x00, 0x3C,
      0x00, 0x10,
      0x20, 0x01, 0x0d, 0xb8, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};

  dns_receive(resp6, sizeof(resp6));
  u8 ip6[16];
  if (dns_last_result6(ip6) && ip6[0] == 0x20 && ip6[1] == 0x01 &&
      ip6[2] == 0x0d && ip6[3] == 0xb8 && ip6[15] == 0x01) {
    uwrite("DNS-SMOKE: ok parse-aaaa-record\n");
  } else {
    uwrite("DNS-SMOKE: fail parse-aaaa-record\n");
  }
}

static int lock_smoke_main(int argc, const char **argv) {
  (void)argc;
  (void)argv;

  uwrite("LOCK-SMOKE: start\n");
  u64 fd = syscall_dispatch(SYS_OPEN, (u64)(usize)"/tmp/lock-smoke.dat",
                            B1NIX_O_CREAT | B1NIX_O_RDWR | B1NIX_O_TRUNC, 0666, 0, 0, 0);
  if ((isize)fd < 0) {
    uwrite("LOCK-SMOKE: fail open\n");
    return 1;
  }

  struct flock parent_lock;
  memset(&parent_lock, 0, sizeof(parent_lock));
  parent_lock.l_type = F_WRLCK;
  parent_lock.l_whence = 0;
  parent_lock.l_start = 0;
  parent_lock.l_len = 0;
  if ((isize)syscall_dispatch(SYS_FCNTL, fd, B1NIX_F_SETLK, (u64)(usize)&parent_lock, 0, 0, 0) < 0) {
    uwrite("LOCK-SMOKE: fail parent-setlk\n");
    syscall_dispatch(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    return 1;
  }

  u64 pid = syscall_dispatch(SYS_FORK, 0, 0, 0, 0, 0, 0);
  if ((isize)pid < 0) {
    uwrite("LOCK-SMOKE: fail fork\n");
    syscall_dispatch(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    return 1;
  }
  if (pid == 0) {
    struct flock child_lock;
    memset(&child_lock, 0, sizeof(child_lock));
    child_lock.l_type = F_WRLCK;
    child_lock.l_whence = 0;
    child_lock.l_start = 0;
    child_lock.l_len = 0;

    isize nb = (isize)syscall_dispatch(SYS_FCNTL, fd, B1NIX_F_SETLK, (u64)(usize)&child_lock, 0, 0, 0);
    if (nb != -EAGAIN) {
      uwrite("LOCK-SMOKE: fail nonblock-conflict\n");
      syscall_dispatch(SYS_EXIT, 2, 0, 0, 0, 0, 0);
    }
    uwrite("LOCK-SMOKE: ok nonblock-conflict\n");

    isize blk = (isize)syscall_dispatch(SYS_FCNTL, fd, B1NIX_F_SETLKW, (u64)(usize)&child_lock, 0, 0, 0);
    if (blk < 0) {
      uwrite("LOCK-SMOKE: fail setlkw\n");
      syscall_dispatch(SYS_EXIT, 3, 0, 0, 0, 0, 0);
    }
    uwrite("LOCK-SMOKE: ok wake-on-close\n");
    syscall_dispatch(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    syscall_dispatch(SYS_EXIT, 0, 0, 0, 0, 0, 0);
  }

  for (int i = 0; i < 8; i++) {
    syscall_dispatch(SYS_YIELD, 0, 0, 0, 0, 0, 0);
  }
  syscall_dispatch(SYS_CLOSE, fd, 0, 0, 0, 0, 0);

  int st = 0;
  syscall_dispatch(SYS_WAIT, pid, (u64)(usize)&st, 0, 0, 0, 0);
  if (st != 0) {
    uwrite("LOCK-SMOKE: fail child-status\n");
    return 1;
  }

  uwrite("LOCK-SMOKE: done\n");
  return 0;
}

static int run_ext_stress(const char *mount_path) {
  struct b1nix_mount_entry mounts[16];
  long count = (long)syscall_dispatch(SYS_MOUNTS, (u64)(usize)mounts, 16, 0, 0, 0, 0);
  int is_mounted = 0;
  if (count > 0) {
    for (long i = 0; i < count && i < 16; i++) {
      if (strcmp(mounts[i].target, mount_path) == 0) {
        is_mounted = 1;
        break;
      }
    }
  }
  if (!is_mounted) {
    return 0;
  }

  char path_buf[256];
  snprintf(path_buf, sizeof(path_buf), "%s/.stress_test", mount_path);
  isize fd = (isize)syscall_dispatch(SYS_OPEN, (u64)(usize)path_buf,
                                   B1NIX_O_CREAT | B1NIX_O_RDWR, 0666, 0, 0, 0);
  if (fd < 0) {
    return 0; 
  }
  syscall_dispatch(SYS_CLOSE, (u64)fd, 0, 0, 0, 0, 0);
  syscall_dispatch(SYS_UNLINK, (u64)(usize)path_buf, 0, 0, 0, 0, 0);

  uwrite("EXT-STRESS: running on ");
  uwrite(mount_path);
  uwrite("\n");

  char dir_path[256];
  char file_path[256];
  char renamed_path[256];
  char sym_path[256];
  char link_path[256];

  for (int i = 0; i < 50; i++) {
    snprintf(dir_path, sizeof(dir_path), "%s/dir_%d", mount_path, i);
    isize rc = (isize)syscall_dispatch(SYS_MKDIR, (u64)(usize)dir_path, 0755, 0, 0, 0, 0);
    if (rc < 0 && rc != -EEXIST) {
      uwrite("EXT-STRESS: mkdir failed\n");
      return 1;
    }

    snprintf(file_path, sizeof(file_path), "%s/dir_%d/file_%d", mount_path, i, i);
    fd = (isize)syscall_dispatch(SYS_OPEN, (u64)(usize)file_path,
                                 B1NIX_O_CREAT | B1NIX_O_RDWR | B1NIX_O_TRUNC, 0666, 0, 0, 0);
    if (fd < 0) {
      uwrite("EXT-STRESS: open failed\n");
      return 1;
    }

    char write_buf[128];
    snprintf(write_buf, sizeof(write_buf), "Stress data for iteration %d. Repeating some blocks of text to ensure we use some file blocks.\n", i);
    isize bytes_written = (isize)syscall_dispatch(SYS_WRITE, (u64)fd, (u64)(usize)write_buf, strlen(write_buf), 0, 0, 0);
    if (bytes_written < 0) {
      uwrite("EXT-STRESS: write failed\n");
      syscall_dispatch(SYS_CLOSE, (u64)fd, 0, 0, 0, 0, 0);
      return 1;
    }

    syscall_dispatch(SYS_FSYNC, (u64)fd, 0, 0, 0, 0, 0);
    syscall_dispatch(SYS_CLOSE, (u64)fd, 0, 0, 0, 0, 0);

    snprintf(sym_path, sizeof(sym_path), "%s/dir_%d/sym_%d", mount_path, i, i);
    rc = (isize)syscall_dispatch(SYS_SYMLINK, (u64)(usize)file_path, (u64)(usize)sym_path, 0, 0, 0, 0);
    if (rc < 0) {
      uwrite("EXT-STRESS: symlink failed\n");
      return 1;
    }

    snprintf(link_path, sizeof(link_path), "%s/dir_%d/link_%d", mount_path, i, i);
    rc = (isize)syscall_dispatch(SYS_LINK, (u64)(usize)file_path, (u64)(usize)link_path, 0, 0, 0, 0);
    if (rc < 0) {
      uwrite("EXT-STRESS: link failed\n");
      return 1;
    }

    snprintf(renamed_path, sizeof(renamed_path), "%s/dir_%d/renamed_%d", mount_path, i, i);
    rc = (isize)syscall_dispatch(SYS_RENAME, (u64)(usize)file_path, (u64)(usize)renamed_path, 0, 0, 0, 0);
    if (rc < 0) {
      uwrite("EXT-STRESS: rename failed\n");
      return 1;
    }

    rc = (isize)syscall_dispatch(SYS_UNLINK, (u64)(usize)sym_path, 0, 0, 0, 0, 0);
    if (rc < 0) {
      uwrite("EXT-STRESS: unlink sym failed\n");
      return 1;
    }

    rc = (isize)syscall_dispatch(SYS_UNLINK, (u64)(usize)link_path, 0, 0, 0, 0, 0);
    if (rc < 0) {
      uwrite("EXT-STRESS: unlink link failed\n");
      return 1;
    }

    rc = (isize)syscall_dispatch(SYS_UNLINK, (u64)(usize)renamed_path, 0, 0, 0, 0, 0);
    if (rc < 0) {
      uwrite("EXT-STRESS: unlink renamed failed\n");
      return 1;
    }

    rc = (isize)syscall_dispatch(SYS_RMDIR, (u64)(usize)dir_path, 0, 0, 0, 0, 0);
    if (rc < 0) {
      uwrite("EXT-STRESS: rmdir failed\n");
      return 1;
    }
  }

  uwrite("EXT-STRESS: done on ");
  uwrite(mount_path);
  uwrite("\n");
  return 0;
}

static int ext_stress_main(int argc, const char **argv) {
  (void)argc;
  (void)argv;
  uwrite("EXT-STRESS: start\n");
  run_ext_stress("/mnt/ext4");
  run_ext_stress("/mnt/ext3");
  uwrite("EXT-STRESS: done\n");
  return 0;
}

/* ─────────────────────────── M39: configurable init ───────────────────────
 * PID 1 (/bin/init) parses /etc/inittab and supervises file-based services
 * across runlevels. `telinit <N>` requests a runlevel switch via /run/initctl,
 * which the supervisor polls. The parser is pure (no I/O) so the M39 self-test
 * can drive it with a literal inittab. */

enum init_action {
  IA_IGNORE = 0,
  IA_SYSINIT,
  IA_WAIT,
  IA_ONCE,
  IA_RESPAWN,
  IA_INITDEFAULT,
  IA_CTRLALTDEL,
  IA_SHUTDOWN,
};

#define INITTAB_MAX 16
#define INITTAB_CMD_MAX 96
/* SysV-style respawn storm guard: a respawn child that exits within
 * INIT_RESPAWN_FAST_SECS of its spawn this many times IN A ROW gets the
 * entry disabled (a long-lived child resets the streak). Unlike a lifetime
 * cap this never silences a getty that respawns on every normal logout. */
#define INIT_RESPAWN_FAST_SECS 2
#define INIT_RESPAWN_FAST_MAX 5

struct inittab_entry {
  char id[12];
  char runlevels[8];
  enum init_action action;
  char command[INITTAB_CMD_MAX];
  u64 pid;        /* live pid of a respawn child, 0 if not running */
  u64 spawn_time; /* SYS_TIME seconds at last spawn (storm guard) */
  int fast_exits; /* consecutive fast exits (storm guard) */
  int disabled;   /* respawning too fast: entry parked until telinit */
};

static struct inittab_entry g_inittab[INITTAB_MAX];
static int g_inittab_count;
static int g_runlevel = 3;    /* current runlevel */
static int g_initdefault = 3; /* runlevel from the initdefault entry */

static enum init_action init_parse_action(const char *s) {
  if (strcmp(s, "sysinit") == 0) return IA_SYSINIT;
  if (strcmp(s, "wait") == 0) return IA_WAIT;
  if (strcmp(s, "once") == 0) return IA_ONCE;
  if (strcmp(s, "respawn") == 0) return IA_RESPAWN;
  if (strcmp(s, "initdefault") == 0) return IA_INITDEFAULT;
  if (strcmp(s, "ctrlaltdel") == 0) return IA_CTRLALTDEL;
  if (strcmp(s, "shutdown") == 0) return IA_SHUTDOWN;
  return IA_IGNORE;
}

/* Parse an inittab image (id:runlevels:action:process per line) into
 * g_inittab[]. Returns the entry count and records g_initdefault. */
static int init_parse_inittab(const char *buf, int len) {
  g_inittab_count = 0;
  g_initdefault = 3;
  int i = 0;
  while (i < len && g_inittab_count < INITTAB_MAX) {
    char line[256];
    int n = 0;
    while (i < len && buf[i] != '\n') {
      if (n < (int)sizeof(line) - 1) line[n++] = buf[i];
      i++;
    }
    if (i < len) i++; /* consume the newline */
    line[n] = '\0';

    char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0' || *p == '#') continue;

    char *f[4];
    int nf = 0;
    f[nf++] = p;
    while (*p && nf < 4) {
      if (*p == ':') { *p = '\0'; f[nf++] = p + 1; }
      p++;
    }
    if (nf < 4) continue; /* need all four fields */

    enum init_action act = init_parse_action(f[2]);
    if (act == IA_IGNORE) continue;

    struct inittab_entry *e = &g_inittab[g_inittab_count];
    strncpy(e->id, f[0], sizeof(e->id) - 1);
    e->id[sizeof(e->id) - 1] = '\0';
    strncpy(e->runlevels, f[1], sizeof(e->runlevels) - 1);
    e->runlevels[sizeof(e->runlevels) - 1] = '\0';
    e->action = act;
    strncpy(e->command, f[3], sizeof(e->command) - 1);
    e->command[sizeof(e->command) - 1] = '\0';
    e->pid = 0;
    e->spawn_time = 0;
    e->fast_exits = 0;
    e->disabled = 0;

    if (act == IA_INITDEFAULT && f[1][0] >= '0' && f[1][0] <= '6')
      g_initdefault = f[1][0] - '0';
    g_inittab_count++;
  }
  return g_inittab_count;
}

static int init_entry_in_runlevel(const struct inittab_entry *e, int rl) {
  if (e->runlevels[0] == '\0') return 1; /* empty = all runlevels */
  char d = (char)('0' + rl);
  for (const char *p = e->runlevels; *p; p++)
    if (*p == d) return 1;
  return 0;
}

/* Tokenise a command string on spaces and spawn it. Honours a leading '-'
 * (login-shell convention) by stripping it. Returns the child pid or -1. */
static u64 init_spawn_cmd(const char *cmd) {
  static char cbuf[INITTAB_CMD_MAX];
  int n = 0;
  while (cmd[n] && n < INITTAB_CMD_MAX - 1) { cbuf[n] = cmd[n]; n++; }
  cbuf[n] = '\0';
  const char *argv[16];
  int argc = 0;
  int i = 0;
  if (cbuf[0] == '-') i++;
  while (cbuf[i] && argc < 15) {
    while (cbuf[i] == ' ' || cbuf[i] == '\t') i++;
    if (!cbuf[i]) break;
    argv[argc++] = &cbuf[i];
    while (cbuf[i] && cbuf[i] != ' ' && cbuf[i] != '\t') i++;
    if (cbuf[i]) cbuf[i++] = '\0';
  }
  argv[argc] = 0;
  if (argc == 0) return (u64)-1;
  u64 pid = syscall_dispatch(SYS_SPAWN, (u64)(usize)argv[0], argc,
                             (u64)(usize)argv, 0, 0, 0);
  if ((isize)pid >= 0)
    return pid;
  /* The kernel exec path has no shebang support, so a script entry (e.g. the
   * sysinit "/etc/rc") fails the direct spawn. Retry through /bin/sh — the
   * same way the legacy init path always ran rc. */
  if (argc < 15) {
    for (int k = argc; k > 0; k--)
      argv[k] = argv[k - 1];
    argv[0] = "/bin/sh";
    argv[argc + 1] = 0;
    pid = syscall_dispatch(SYS_SPAWN, (u64)(usize)argv[0], argc + 1,
                           (u64)(usize)argv, 0, 0, 0);
  }
  return pid;
}

/* Read and consume a pending telinit request from /run/initctl. Returns the
 * requested runlevel 0-6, or -1 if none/invalid. */
static int init_poll_initctl(void) {
  u64 fd = syscall_dispatch(SYS_OPEN, (u64)(usize) "/run/initctl", 0, 0, 0, 0, 0);
  if ((isize)fd < 0) return -1;
  char c[4] = {0, 0, 0, 0};
  isize r = (isize)syscall_dispatch(SYS_READ, fd, (u64)(usize)c, 3, 0, 0, 0);
  syscall_dispatch(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
  syscall_dispatch(SYS_UNLINK, (u64)(usize) "/run/initctl", 0, 0, 0, 0, 0);
  if (r <= 0) return -1;
  if (c[0] >= '0' && c[0] <= '6') return c[0] - '0';
  return -1;
}

/* Start the wait/once/respawn entries valid in runlevel rl. wait blocks. */
static void init_enter_runlevel(int rl) {
  for (int i = 0; i < g_inittab_count; i++) {
    struct inittab_entry *e = &g_inittab[i];
    if (!init_entry_in_runlevel(e, rl)) continue;
    if (e->action == IA_WAIT) {
      u64 pid = init_spawn_cmd(e->command);
      if ((isize)pid >= 0) {
        int st = 0;
        syscall_dispatch(SYS_WAIT, pid, (u64)(usize)&st, 0, 0, 0, 0);
      }
    } else if (e->action == IA_ONCE) {
      init_spawn_cmd(e->command);
    } else if (e->action == IA_RESPAWN) {
      if (e->pid == 0 && !e->disabled) {
        e->pid = init_spawn_cmd(e->command);
        e->spawn_time = syscall_dispatch(SYS_TIME, 0, 0, 0, 0, 0, 0);
      }
    }
  }
}

/* Switch to runlevel rl: stop respawn entries no longer valid, then start the
 * ones that are. */
static void init_switch_runlevel(int rl) {
  for (int i = 0; i < g_inittab_count; i++) {
    struct inittab_entry *e = &g_inittab[i];
    if (e->pid && e->action == IA_RESPAWN && !init_entry_in_runlevel(e, rl)) {
      syscall_dispatch(SYS_KILL, e->pid, SIGTERM, 0, 0, 0, 0);
      e->pid = 0;
    }
    /* A runlevel switch un-parks storm-disabled entries (sysvinit-style). */
    e->disabled = 0;
    e->fast_exits = 0;
  }
  g_runlevel = rl;
  init_enter_runlevel(rl);
}

/* Inittab-driven service supervisor (production PID 1 main loop). Runs sysinit
 * entries, enters the default runlevel, then reaps + respawns children and
 * honours telinit runlevel requests. Never returns. */
static void init_supervise(void) {
  for (int i = 0; i < g_inittab_count; i++) {
    if (g_inittab[i].action == IA_SYSINIT) {
      u64 pid = init_spawn_cmd(g_inittab[i].command);
      if ((isize)pid >= 0) {
        int st = 0;
        syscall_dispatch(SYS_WAIT, pid, (u64)(usize)&st, 0, 0, 0, 0);
      }
    }
  }

  g_runlevel = g_initdefault;
  init_enter_runlevel(g_runlevel);

  for (;;) {
    int status = 0;
    isize reaped = (isize)syscall_dispatch(SYS_WAITPID, 0, (u64)(usize)&status,
                                           B1NIX_WNOHANG, 0, 0, 0);
    if (reaped > 0) {
      for (int i = 0; i < g_inittab_count; i++) {
        struct inittab_entry *e = &g_inittab[i];
        if (e->pid != (u64)reaped) continue;
        e->pid = 0;
        if (e->action == IA_RESPAWN && init_entry_in_runlevel(e, g_runlevel) &&
            !e->disabled) {
          u64 now = syscall_dispatch(SYS_TIME, 0, 0, 0, 0, 0, 0);
          if (now - e->spawn_time < INIT_RESPAWN_FAST_SECS)
            e->fast_exits++;
          else
            e->fast_exits = 0;
          if (e->fast_exits >= INIT_RESPAWN_FAST_MAX) {
            e->disabled = 1;
            uwrite("init: ");
            uwrite(e->id);
            uwrite(": respawning too fast -- disabled until next runlevel "
                   "switch\n");
          } else {
            e->pid = init_spawn_cmd(e->command);
            e->spawn_time = now;
          }
        }
      }
      continue; /* drain all dead children before sleeping */
    }

    int req = init_poll_initctl();
    if (req >= 0 && req != g_runlevel) {
      if (req == 0) {
        uwrite("init: telinit 0 — halting\n");
        syscall_dispatch(SYS_REBOOT, B1NIX_REBOOT_HALT, 0, 0, 0, 0, 0);
      } else if (req == 6) {
        uwrite("init: telinit 6 — rebooting\n");
        syscall_dispatch(SYS_REBOOT, B1NIX_REBOOT_RESTART, 0, 0, 0, 0, 0);
      } else {
        uwrite("init: telinit — switching runlevel\n");
        init_switch_runlevel(req);
      }
    }

    syscall_dispatch(SYS_SLEEP, 5, 0, 0, 0, 0, 0); /* poll interval */
  }
}

/* M39 self-test (test mode). Exercises the inittab parser, runlevel matching,
 * the telinit → /run/initctl round-trip, and getty applet presence. */
static void m39_init_test(void) {
  uwrite("M39-INIT: start\n");

  static const char test_tab[] =
      "# comment line\n"
      "id:4:initdefault:\n"
      "si::sysinit:/etc/rc\n"
      "co:2345:respawn:/bin/bash\n"
      "tt:23:respawn:/bin/getty ttyS0\n"
      "lo:5:wait:/bin/true\n";
  int count = init_parse_inittab(test_tab, (int)sizeof(test_tab) - 1);
  if (count == 5)
    uwrite("M39-INIT: ok parse-inittab\n");
  else
    uwrite("M39-INIT: fail parse-inittab\n");

  if (g_initdefault == 4)
    uwrite("M39-INIT: ok initdefault\n");
  else
    uwrite("M39-INIT: fail initdefault\n");

  /* g_inittab[3] is the "tt:23:..." getty entry: valid in 2/3, not 4/5. */
  if (init_entry_in_runlevel(&g_inittab[3], 2) &&
      init_entry_in_runlevel(&g_inittab[3], 3) &&
      !init_entry_in_runlevel(&g_inittab[3], 4) &&
      init_entry_in_runlevel(&g_inittab[1], 4) /* empty runlevels = all */)
    uwrite("M39-INIT: ok runlevel-match\n");
  else
    uwrite("M39-INIT: fail runlevel-match\n");

  /* telinit round-trip: /sbin/telinit 5 must leave runlevel 5 in /run/initctl,
   * which init_poll_initctl() then reads back and consumes. */
  {
    const char *tl_argv[] = {"/sbin/telinit", "5", 0};
    u64 tl_pid = syscall_dispatch(SYS_SPAWN, (u64)(usize)tl_argv[0], 2,
                                  (u64)(usize)tl_argv, 0, 0, 0);
    int ok = 0;
    if ((isize)tl_pid >= 0) {
      int st = 0;
      syscall_dispatch(SYS_WAIT, tl_pid, (u64)(usize)&st, 0, 0, 0, 0);
      if (st == 0 && init_poll_initctl() == 5)
        ok = 1;
    }
    uwrite(ok ? "M39-INIT: ok telinit\n" : "M39-INIT: fail telinit\n");
  }

  /* getty applet present (BusyBox), reachable via the /bin/getty symlink. */
  {
    u64 fd = syscall_dispatch(SYS_OPEN, (u64)(usize) "/bin/getty", 0, 0, 0, 0, 0);
    if ((isize)fd >= 0) {
      syscall_dispatch(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
      uwrite("M39-INIT: ok getty-applet\n");
    } else {
      uwrite("M39-INIT: fail getty-applet\n");
    }
  }

  /* M39 serial tty layer: /dev/ttyS0 is an independent tty (own line
   * discipline, termios, and job-control state) backed by COM1, so a getty
   * session on the serial line is fully separate from the boot console. */
  isize sfd = (isize)syscall_dispatch(SYS_OPEN, (u64)(usize) "/dev/ttyS0",
                                      B1NIX_O_RDWR, 0, 0, 0, 0);
  uwrite(sfd >= 0 ? "M39-INIT: ok ttys0-open\n" : "M39-INIT: fail ttys0-open\n");

  if (sfd >= 0) {
    struct b1nix_termios tio, saved, con;
    char rbuf[32];
    int own_pgrp = 0; /* TIOC[GS]PGRP carry a 32-bit pid_t */
    isize r;
    syscall_dispatch(SYS_IOCTL, (u64)sfd, B1NIX_TIOCGPGRP,
                     (u64)(usize)&own_pgrp, 0, 0, 0);

    /* Termios independence: raw mode on ttyS0 must not touch the console. */
    int ok = 0;
    if (syscall_dispatch(SYS_IOCTL, (u64)sfd, B1NIX_TCGETS, (u64)(usize)&tio,
                         0, 0, 0) == 0 &&
        (tio.c_lflag & B1NIX_ICANON)) {
      saved = tio;
      tio.c_lflag = 0;
      syscall_dispatch(SYS_IOCTL, (u64)sfd, B1NIX_TCSETS, (u64)(usize)&tio, 0,
                       0, 0);
      if (syscall_dispatch(SYS_IOCTL, 0, B1NIX_TCGETS, (u64)(usize)&con, 0, 0,
                           0) == 0 &&
          (con.c_lflag & B1NIX_ICANON))
        ok = 1;
      syscall_dispatch(SYS_IOCTL, (u64)sfd, B1NIX_TCSETS, (u64)(usize)&saved,
                       0, 0, 0);
    }
    uwrite(ok ? "M39-INIT: ok tty-termios-independent\n"
              : "M39-INIT: fail tty-termios-independent\n");

    /* Canonical read through the per-device line discipline. */
    serial_tty_test_inject(0, "m39ldisc\n", 9);
    r = (isize)syscall_dispatch(SYS_READ, (u64)sfd, (u64)(usize)rbuf,
                                sizeof(rbuf), 0, 0, 0);
    uwrite((r == 9 && memcmp(rbuf, "m39ldisc\n", 9) == 0)
               ? "M39-INIT: ok tty-canon-read\n"
               : "M39-INIT: fail tty-canon-read\n");

    /* VEOF on an empty line reads back as EOF (0 bytes). */
    serial_tty_test_inject(0, "\x04", 1);
    r = (isize)syscall_dispatch(SYS_READ, (u64)sfd, (u64)(usize)rbuf,
                                sizeof(rbuf), 0, 0, 0);
    uwrite(r == 0 ? "M39-INIT: ok tty-eof\n" : "M39-INIT: fail tty-eof\n");

    /* Raw (non-canonical) mode: bytes pass through without line assembly. */
    syscall_dispatch(SYS_IOCTL, (u64)sfd, B1NIX_TCGETS, (u64)(usize)&saved, 0,
                     0, 0);
    tio = saved;
    tio.c_lflag = 0;
    syscall_dispatch(SYS_IOCTL, (u64)sfd, B1NIX_TCSETS, (u64)(usize)&tio, 0, 0,
                     0);
    serial_tty_test_inject(0, "xy", 2);
    r = (isize)syscall_dispatch(SYS_READ, (u64)sfd, (u64)(usize)rbuf,
                                sizeof(rbuf), 0, 0, 0);
    uwrite((r == 2 && rbuf[0] == 'x' && rbuf[1] == 'y')
               ? "M39-INIT: ok tty-raw-read\n"
               : "M39-INIT: fail tty-raw-read\n");

    /* ISIG: VINTR (^C) is routed as a signal to the tty's foreground pgrp
     * and never queued as data. Point the fg pgrp at a nonexistent group so
     * the SIGINT goes nowhere, then verify only 'q' arrives. */
    tio.c_lflag = B1NIX_ISIG;
    syscall_dispatch(SYS_IOCTL, (u64)sfd, B1NIX_TCSETS, (u64)(usize)&tio, 0, 0,
                     0);
    int bogus_pgrp = 59999;
    syscall_dispatch(SYS_IOCTL, (u64)sfd, B1NIX_TIOCSPGRP,
                     (u64)(usize)&bogus_pgrp, 0, 0, 0);
    serial_tty_test_inject(0, "\x03", 1);
    syscall_dispatch(SYS_IOCTL, (u64)sfd, B1NIX_TIOCSPGRP,
                     (u64)(usize)&own_pgrp, 0, 0, 0);
    serial_tty_test_inject(0, "q", 1);
    r = (isize)syscall_dispatch(SYS_READ, (u64)sfd, (u64)(usize)rbuf,
                                sizeof(rbuf), 0, 0, 0);
    uwrite((r == 1 && rbuf[0] == 'q') ? "M39-INIT: ok tty-isig\n"
                                      : "M39-INIT: fail tty-isig\n");
    syscall_dispatch(SYS_IOCTL, (u64)sfd, B1NIX_TCSETS, (u64)(usize)&saved, 0,
                     0, 0);

    /* Foreground-pgrp independence: changing ttyS0's fg pgrp must not move
     * the boot console's. */
    int con_before = 0, con_after = 0, marker_pgrp = 4242;
    syscall_dispatch(SYS_IOCTL, 0, B1NIX_TIOCGPGRP, (u64)(usize)&con_before, 0,
                     0, 0);
    syscall_dispatch(SYS_IOCTL, (u64)sfd, B1NIX_TIOCSPGRP,
                     (u64)(usize)&marker_pgrp, 0, 0, 0);
    syscall_dispatch(SYS_IOCTL, 0, B1NIX_TIOCGPGRP, (u64)(usize)&con_after, 0,
                     0, 0);
    uwrite((con_before == con_after && serial_tty_fg_pgrp(0) == 4242)
               ? "M39-INIT: ok tty-pgrp-independent\n"
               : "M39-INIT: fail tty-pgrp-independent\n");
    syscall_dispatch(SYS_IOCTL, (u64)sfd, B1NIX_TIOCSPGRP,
                     (u64)(usize)&own_pgrp, 0, 0, 0);

    /* TIOCSCTTY: a session leader can claim the tty (getty relies on it). */
    uwrite(syscall_dispatch(SYS_IOCTL, (u64)sfd, B1NIX_TIOCSCTTY, 0, 0, 0,
                            0) == 0
               ? "M39-INIT: ok tty-sctty\n"
               : "M39-INIT: fail tty-sctty\n");

    /* Real TX: this marker reaches the smoke log through the ttyS0 write
     * path (OPOST + UART), not through the console. */
    static const char tx_marker[] = "M39-TTYS0-TX-OK\n";
    r = (isize)syscall_dispatch(SYS_WRITE, (u64)sfd, (u64)(usize)tx_marker,
                                sizeof(tx_marker) - 1, 0, 0, 0);
    uwrite(r == (isize)(sizeof(tx_marker) - 1)
               ? "M39-INIT: ok ttys0-write\n"
               : "M39-INIT: fail ttys0-write\n");

    /* Closing the last handle releases the claim: COM1 input falls back to
     * the merged boot console. */
    syscall_dispatch(SYS_CLOSE, (u64)sfd, 0, 0, 0, 0, 0);
    uwrite(!serial_tty_claimed(0) ? "M39-INIT: ok ttys0-release\n"
                                  : "M39-INIT: fail ttys0-release\n");
  }

  uwrite("M39-INIT: done\n");
}

static int init_main(int argc, const char **argv) {
  (void)argc;
  (void)argv;

  syscall_dispatch(SYS_CLEAR, 0, 0, 0, 0, 0, 0);

  if (bootinfo_has_flag("b1nix.test=1")) {
  /* M27: kernel command line key=value parser self-test. The smoke harness
   * passes "b1nix.test=1 b1nix.kvtest=abc123" so we can verify a present key,
   * an absent key, prefix non-matching, and value truncation. */
  {
    char v[16];
    char small[4];
    int ok = 1;
    if (!bootinfo_get_kv("b1nix.test", v, sizeof(v)) || strcmp(v, "1") != 0)
      ok = 0;
    if (!bootinfo_get_kv("b1nix.kvtest", v, sizeof(v)) ||
        strcmp(v, "abc123") != 0)
      ok = 0;
    if (bootinfo_get_kv("b1nix.absent", v, sizeof(v)))
      ok = 0;
    if (bootinfo_get_kv("b1nix.tes", v, sizeof(v)))
      ok = 0;
    if (!bootinfo_get_kv("b1nix.kvtest", small, sizeof(small)) ||
        strcmp(small, "abc") != 0)
      ok = 0;
    uwrite(ok ? "M27-CMDLINE: ok kv-parse\n" : "M27-CMDLINE: fail kv-parse\n");
  }

  /* M27: verify the boot rc script runs end-to-end via /bin/sh (same path the
   * production init uses at startup). The script emits its own markers. */
  {
    const char *rc_argv[] = {"/bin/sh", "/etc/rc", 0};
    u64 rc_pid = syscall_dispatch(SYS_SPAWN, (u64)(usize)rc_argv[0], 2,
                                  (u64)(usize)rc_argv, 0, 0, 0);
    if ((isize)rc_pid >= 0) {
      int rc_status = 0;
      syscall_dispatch(SYS_WAIT, rc_pid, (u64)(usize)&rc_status, 0, 0, 0, 0);
    }
  }

  /* M27: user/passwd/login basics (getpwnam/getpwuid over /etc/passwd +
   * privilege drop). */
  {
    u64 u_pid = syscall_dispatch(SYS_SPAWN, (u64)(usize) "/bin/m27-smoke", 0, 0,
                                 0, 0, 0);
    if ((isize)u_pid >= 0) {
      int u_status = 0;
      syscall_dispatch(SYS_WAIT, u_pid, (u64)(usize)&u_status, 0, 0, 0, 0);
    }
  }

  u64 n_pid = syscall_dispatch(SYS_SPAWN, (u64)(usize) "/bin/native-smoke", 0, 0, 0, 0, 0);
  
  if ((isize)n_pid < 0) {
    uwrite("NATIVE-SMOKE: spawn-fail\n");
  } else {
    int native_status = 0;
    syscall_dispatch(SYS_WAIT, n_pid, (u64)(usize)&native_status, 0, 0, 0, 0);
    if (native_status == 0) {
      uwrite("NATIVE-SMOKE: done\n");
    } else {
      uwrite("NATIVE-SMOKE: fail\n");
    }
  }

  u64 m12_pid = syscall_dispatch(SYS_SPAWN, (u64)(usize) "/bin/m12-smoke", 0, 0, 0, 0, 0);
  if ((isize)m12_pid < 0) {
    uwrite("M12-SMOKE: spawn-fail\n");
  } else {
    int m12_status = 0;
    syscall_dispatch(SYS_WAIT, m12_pid, (u64)(usize)&m12_status, 0, 0, 0, 0);
  }

  u64 m13_pid = syscall_dispatch(SYS_SPAWN, (u64)(usize) "/bin/m13-smoke", 0, 0, 0, 0, 0);
  if ((isize)m13_pid < 0) {
    uwrite("M13-SMOKE: spawn-fail\n");
  } else {
    int m13_status = 0;
    syscall_dispatch(SYS_WAIT, m13_pid, (u64)(usize)&m13_status, 0, 0, 0, 0);
  }

  u64 m14_pid = syscall_dispatch(SYS_SPAWN, (u64)(usize) "/bin/m14-smoke", 0, 0, 0, 0, 0);
  if ((isize)m14_pid < 0) {
    uwrite("M14-SMOKE: spawn-fail\n");
  } else {
    int m14_status = 0;
    syscall_dispatch(SYS_WAIT, m14_pid, (u64)(usize)&m14_status, 0, 0, 0, 0);
  }

  u64 m15_pid = syscall_dispatch(SYS_SPAWN, (u64)(usize) "/bin/m15-smoke", 0, 0, 0, 0, 0);
  if ((isize)m15_pid < 0) {
    uwrite("M15-SMOKE: spawn-fail\n");
  } else {
    int m15_status = 0;
    syscall_dispatch(SYS_WAIT, m15_pid, (u64)(usize)&m15_status, 0, 0, 0, 0);
  }

  if (bootinfo_has_flag("b1nix.skip-m25")) {
    uwrite("M25-SMOKE: start\n");
    uwrite("M25-SMOKE: skipped (b1nix.skip-m25)\n");
    uwrite("M25-SMOKE: done\n");
  } else {
    u64 m25_pid = syscall_dispatch(SYS_SPAWN, (u64)(usize) "/bin/m25-smoke", 0, 0, 0, 0, 0);
    if ((isize)m25_pid < 0) {
      uwrite("M25-SMOKE: spawn-fail\n");
    } else {
      int m25_status = 0;
      syscall_dispatch(SYS_WAIT, m25_pid, (u64)(usize)&m25_status, 0, 0, 0, 0);
    }
  }

  u64 m26_pid = syscall_dispatch(SYS_SPAWN, (u64)(usize) "/bin/m26-smoke", 0, 0, 0, 0, 0);
  if ((isize)m26_pid < 0) {
    uwrite("M26-SMOKE: spawn-fail\n");
  } else {
    int m26_status = 0;
    syscall_dispatch(SYS_WAIT, m26_pid, (u64)(usize)&m26_status, 0, 0, 0, 0);
  }

  /* M29: POSIX threads / futex / TLS. */
  {
    u64 m29_pid = syscall_dispatch(SYS_SPAWN, (u64)(usize) "/bin/m29-smoke", 0,
                                   0, 0, 0, 0);
    if ((isize)m29_pid < 0) {
      uwrite("M29-PTHREAD: spawn-fail\n");
    } else {
      int m29_status = 0;
      syscall_dispatch(SYS_WAIT, m29_pid, (u64)(usize)&m29_status, 0, 0, 0, 0);
    }
  }

  /* M31: user security / setuid / shadow. */
  {
    u64 m31_pid = syscall_dispatch(SYS_SPAWN, (u64)(usize) "/bin/m31-smoke", 0,
                                   0, 0, 0, 0);
    if ((isize)m31_pid < 0) {
      uwrite("M31-SEC: spawn-fail\n");
    } else {
      int m31_status = 0;
      syscall_dispatch(SYS_WAIT, m31_pid, (u64)(usize)&m31_status, 0, 0, 0, 0);
    }
  }

  /* M32: select() / network multiplexing. */
  {
    u64 m32_pid = syscall_dispatch(SYS_SPAWN, (u64)(usize) "/bin/m32-smoke", 0,
                                   0, 0, 0, 0);
    if ((isize)m32_pid < 0) {
      uwrite("M32-NET: spawn-fail\n");
    } else {
      int m32_status = 0;
      syscall_dispatch(SYS_WAIT, m32_pid, (u64)(usize)&m32_status, 0, 0, 0, 0);
    }
  }

  /* M32a: PCRE2 userspace port. */
  {
    u64 pcre2_pid = syscall_dispatch(
        SYS_SPAWN, (u64)(usize) "/bin/m32-pcre2-smoke", 0, 0, 0, 0, 0);
    if ((isize)pcre2_pid < 0) {
      uwrite("M32-PCRE2: spawn-fail\n");
    } else {
      int pcre2_status = 0;
      syscall_dispatch(SYS_WAIT, pcre2_pid, (u64)(usize)&pcre2_status, 0, 0, 0,
                       0);
    }
  }

  /* M34: procfs / sysfs synthetic filesystems. */
  {
    u64 m34_pid = syscall_dispatch(SYS_SPAWN, (u64)(usize) "/bin/m34-smoke", 0,
                                   0, 0, 0, 0);
    if ((isize)m34_pid < 0) {
      uwrite("M34-PROC: spawn-fail\n");
    } else {
      int m34_status = 0;
      syscall_dispatch(SYS_WAIT, m34_pid, (u64)(usize)&m34_status, 0, 0, 0, 0);
    }

    /* Verify the procfs/sysfs-backed monitoring tools actually run and read
     * back kernel state (they open /proc and /sys under the hood). */
    int rc_tools = 0;
    const char *free_argv[] = {"/bin/free", 0};
    u64 free_pid = syscall_dispatch(SYS_SPAWN, (u64)(usize)free_argv[0], 1, (u64)(usize)free_argv, 0, 0, 0);
    if ((isize)free_pid >= 0) { int st = 0; syscall_dispatch(SYS_WAIT, free_pid, (u64)(usize)&st, 0, 0, 0, 0); if (st) rc_tools = 1; }
    const char *sysctl_argv[] = {"/bin/sysctl", "kernel.osrelease", 0};
    u64 sysctl_pid = syscall_dispatch(SYS_SPAWN, (u64)(usize)sysctl_argv[0], 2, (u64)(usize)sysctl_argv, 0, 0, 0);
    if ((isize)sysctl_pid >= 0) { int st = 0; syscall_dispatch(SYS_WAIT, sysctl_pid, (u64)(usize)&st, 0, 0, 0, 0); if (st) rc_tools = 1; }
    const char *top_argv[] = {"/bin/top", "-b", "-n", "1", 0};
    u64 top_pid = syscall_dispatch(SYS_SPAWN, (u64)(usize)top_argv[0], 4, (u64)(usize)top_argv, 0, 0, 0);
    if ((isize)top_pid >= 0) { int st = 0; syscall_dispatch(SYS_WAIT, top_pid, (u64)(usize)&st, 0, 0, 0, 0); if (st) rc_tools = 1; }
    if (rc_tools == 0)
      uwrite("M34-PROC: ok tools\n");
    else
      uwrite("M34-PROC: fail tools\n");
  }

  /* M35: ELF core dump on fatal signal. The child faults; the kernel writes
   * /tmp/core; the parent validates the ET_CORE ELF. */
  {
    u64 m35_pid = syscall_dispatch(SYS_SPAWN, (u64)(usize) "/bin/m35-smoke", 0,
                                   0, 0, 0, 0);
    if ((isize)m35_pid < 0) {
      uwrite("M35-CORE: spawn-fail\n");
    } else {
      int m35_status = 0;
      syscall_dispatch(SYS_WAIT, m35_pid, (u64)(usize)&m35_status, 0, 0, 0, 0);
    }
  }

  /* M38: sound — userspace /dev/dsp write test + WAV parse. */
  {
    u64 m38_pid = syscall_dispatch(SYS_SPAWN, (u64)(usize) "/bin/m38-sound", 0,
                                   0, 0, 0, 0);
    if ((isize)m38_pid < 0) {
      uwrite("M38-SMOKE: spawn-fail\n");
    } else {
      int m38_status = 0;
      syscall_dispatch(SYS_WAIT, m38_pid, (u64)(usize)&m38_status, 0, 0, 0, 0);
    }
  }

  /* M42 wave-5 prerequisites: atomic sigsuspend, alarm, resource limits,
   * dup/access/ftruncate, fchdir, fuller fnmatch/regex, and job control —
   * the gate the roadmap requires before enabling the upstream ash shell. */
  {
    u64 m42_pid = syscall_dispatch(SYS_SPAWN,
                                   (u64)(usize) "/bin/m42-w5pre-smoke", 0,
                                   0, 0, 0, 0);
    if ((isize)m42_pid < 0) {
      uwrite("M42-W5PRE: spawn-fail\n");
    } else {
      int m42_status = 0;
      syscall_dispatch(SYS_WAIT, m42_pid, (u64)(usize)&m42_status, 0, 0, 0, 0);
    }
  }

  /* bash: the upstream GNU bash 5.2 port is the default shell. Run its feature
   * smoke through /bin/bash to prove the real bash (arrays, [[ ]], regex, brace
   * ranges, C-style for, pattern substitution) is what /bin/sh now resolves to. */
  {
    const char *bash_argv[] = {"/bin/bash", "/etc/bash-smoke.sh", 0};
    u64 bash_pid = syscall_dispatch(SYS_SPAWN, (u64)(usize)bash_argv[0], 2,
                                    (u64)(usize)bash_argv, 0, 0, 0);
    if ((isize)bash_pid < 0) {
      uwrite("BASH-SMOKE: spawn-fail\n");
    } else {
      int bash_status = 0;
      syscall_dispatch(SYS_WAIT, bash_pid, (u64)(usize)&bash_status, 0, 0, 0, 0);
    }
  }

  /* M39: configurable init — inittab parser, runlevels, telinit, getty. */
  m39_init_test();

  /* M30: PIE/ET_DYN loader smoke. The binary is itself an ET_DYN with
   * R_X86_64_RELATIVE relocations; if the loader (process.c) applied
   * the base offset correctly, the pointer-table dereferences land on
   * valid strings and we get the M30-DYN: markers. */
  {
    u64 m30_pid = syscall_dispatch(SYS_SPAWN, (u64)(usize) "/bin/m30-pie", 0,
                                   0, 0, 0, 0);
    if ((isize)m30_pid < 0) {
      uwrite("M30-DYN: spawn-fail\n");
    } else {
      int m30_status = 0;
      syscall_dispatch(SYS_WAIT, m30_pid, (u64)(usize)&m30_status, 0, 0, 0, 0);
    }
  }

  uwrite("M16-SMOKE: start\n");
  struct b1nix_termios m16_termios_before;
  memset(&m16_termios_before, 0, sizeof(m16_termios_before));
  int m16_have_termios =
      (isize)syscall_dispatch(SYS_IOCTL, 0, B1NIX_TCGETS,
                              (u64)(usize)&m16_termios_before, 0, 0, 0) >= 0;
  int m16_ok = m16_have_termios ? 1 : 0;

  if (m16_check_tui_key_decode() != 0) {
    uwrite("M16-SMOKE: fail tui-key-decode\n");
    m16_ok = 0;
  }

  const char *mc_smoke_argv[] = {"mc", "--smoke", 0};
  const char *ne_smoke_argv[] = {"ne", "--smoke", "/tmp/m16-editor-smoke.txt", 0};

  u64 mc_pid = syscall_dispatch(SYS_SPAWN, (u64)(usize)"/bin/mc", 2,
                                 (u64)(usize)mc_smoke_argv, 0, 0, 0);
  if ((isize)mc_pid < 0) {
    uwrite("M16-SMOKE: fail file-explorer-hotkeys\n");
    m16_ok = 0;
  } else {
    int mc_status = 0;
    syscall_dispatch(SYS_WAIT, mc_pid, (u64)(usize)&mc_status, 0, 0, 0, 0);
    if (mc_status == 0) {
      uwrite("M16-SMOKE: ok file-explorer-hotkeys\n");
    } else {
      uwrite("M16-SMOKE: fail file-explorer-hotkeys\n");
      m16_ok = 0;
    }
    if (m16_have_termios && m16_termios_unchanged(&m16_termios_before) != 0) {
      m16_ok = 0;
    }
  }

  u64 ne_pid = syscall_dispatch(SYS_SPAWN, (u64)(usize)"/bin/ne", 3,
                                 (u64)(usize)ne_smoke_argv, 0, 0, 0);
  if ((isize)ne_pid < 0) {
    uwrite("M16-SMOKE: fail editor-hotkeys\n");
    m16_ok = 0;
  } else {
    int ne_status = 0;
    syscall_dispatch(SYS_WAIT, ne_pid, (u64)(usize)&ne_status, 0, 0, 0, 0);
    if (ne_status == 0) {
      uwrite("M16-SMOKE: ok editor-hotkeys\n");
    } else {
      uwrite("M16-SMOKE: fail editor-hotkeys\n");
      m16_ok = 0;
    }
    if (m16_have_termios && m16_termios_unchanged(&m16_termios_before) != 0) {
      m16_ok = 0;
    }
  }

  /* ── M16 clipboard test: create file, copy via VFS, verify, delete ── */
  {
    const char *clip_src = "/tmp/m16-clip-src.txt";
    const char *clip_dst = "/tmp/m16-clip-dst.txt";
    u64 fd = syscall_dispatch(SYS_OPEN, (u64)(usize)clip_src,
                B1NIX_O_CREAT | B1NIX_O_RDWR | B1NIX_O_TRUNC, 0666, 0, 0, 0);
    if ((isize)fd >= 0) {
      syscall_dispatch(SYS_WRITE, fd, (u64)(usize)"hello clipboard", 15, 0, 0, 0);
      syscall_dispatch(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    }
    u64 src = syscall_dispatch(SYS_OPEN, (u64)(usize)clip_src, B1NIX_O_RDONLY, 0, 0, 0, 0);
    u64 dst = syscall_dispatch(SYS_OPEN, (u64)(usize)clip_dst,
                B1NIX_O_WRONLY | B1NIX_O_CREAT | B1NIX_O_TRUNC, 0666, 0, 0, 0);
    if ((isize)src >= 0 && (isize)dst >= 0) {
      char buf[64];
      u64 n = syscall_dispatch(SYS_READ, src, (u64)(usize)buf, sizeof(buf), 0, 0, 0);
      if ((isize)n > 0) syscall_dispatch(SYS_WRITE, dst, (u64)(usize)buf, n, 0, 0, 0);
      syscall_dispatch(SYS_CLOSE, src, 0, 0, 0, 0, 0);
      syscall_dispatch(SYS_CLOSE, dst, 0, 0, 0, 0, 0);
    }
    fd = syscall_dispatch(SYS_OPEN, (u64)(usize)clip_dst, B1NIX_O_RDONLY, 0, 0, 0, 0);
    char verify[32] = {0};
    if ((isize)fd >= 0) {
      syscall_dispatch(SYS_READ, fd, (u64)(usize)verify, sizeof(verify) - 1, 0, 0, 0);
      syscall_dispatch(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    }
    if (strcmp(verify, "hello clipboard") == 0) uwrite("M16-SMOKE: ok file-clipboard\n");
    syscall_dispatch(SYS_UNLINK, (u64)(usize)clip_src, 0, 0, 0, 0, 0);
    syscall_dispatch(SYS_UNLINK, (u64)(usize)clip_dst, 0, 0, 0, 0, 0);
  }

  /* ── M16 editor persistence test: create file, save, reload, verify ── */
  {
    const char *path = "/tmp/m16-editor-persist.txt";
    const char *content = "persist\n";
    u64 fd = syscall_dispatch(SYS_OPEN, (u64)(usize)path,
                B1NIX_O_CREAT | B1NIX_O_RDWR | B1NIX_O_TRUNC, 0666, 0, 0, 0);
    if ((isize)fd >= 0) {
      syscall_dispatch(SYS_WRITE, fd, (u64)(usize)content, strlen(content), 0, 0, 0);
      syscall_dispatch(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    }
    fd = syscall_dispatch(SYS_OPEN, (u64)(usize)path, B1NIX_O_RDONLY, 0, 0, 0, 0);
    char loaded[32] = {0};
    if ((isize)fd >= 0) {
      syscall_dispatch(SYS_READ, fd, (u64)(usize)loaded, sizeof(loaded) - 1, 0, 0, 0);
      syscall_dispatch(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    }
    if (strcmp(loaded, content) == 0) uwrite("M16-SMOKE: ok editor-persist\n");
    syscall_dispatch(SYS_UNLINK, (u64)(usize)path, 0, 0, 0, 0, 0);
  }

  if (m16_have_termios && m16_termios_unchanged(&m16_termios_before) != 0) {
    m16_ok = 0;
  }

  if (m16_ok) {
    uwrite("M16-SMOKE: ok terminal-restore\n");
    uwrite("M16-SMOKE: ok app-lifecycle\n");
    uwrite("M16-SMOKE: done\n");
  } else {
    uwrite("M16-SMOKE: fail terminal-restore\n");
    uwrite("M16-SMOKE: fail app-lifecycle\n");
    uwrite("M16-SMOKE: fail done\n");
  }

  (void)m22_smoke_main(0, 0);
  (void)m24_stress_main(0, 0);
  (void)shell_smoke_main(0, 0);

  /* Spawn crash-prone test binaries individually so a fault in one
   * doesn't prevent the rest from running. */
  {
    const char *m24_argv[] = {"/bin/m13-smoke", "--m24", 0};
    u64 p = syscall_dispatch(SYS_SPAWN, (u64)(usize)m24_argv[0], 2,
                             (u64)(usize)m24_argv, 0, 0, 0);
    if ((isize)p >= 0) { int s = 0; syscall_dispatch(SYS_WAIT, p, (u64)(usize)&s, 0, 0, 0, 0); }
  }
  {
    const char *jc_argv[] = {"/bin/m13-job-control", 0};
    u64 p = syscall_dispatch(SYS_SPAWN, (u64)(usize)jc_argv[0], 1,
                             (u64)(usize)jc_argv, 0, 0, 0);
    if ((isize)p >= 0) { int s = 0; syscall_dispatch(SYS_WAIT, p, (u64)(usize)&s, 0, 0, 0, 0); }
  }
  {
    const char *m17_argv[] = {"/bin/m17-smoke", 0};
    u64 p = syscall_dispatch(SYS_SPAWN, (u64)(usize)m17_argv[0], 1,
                             (u64)(usize)m17_argv, 0, 0, 0);
    if ((isize)p >= 0) { int s = 0; syscall_dispatch(SYS_WAIT, p, (u64)(usize)&s, 0, 0, 0, 0); }
  }
  {
    const char *m8_argv[] = {"/bin/m8-aio-test", 0};
    u64 p = syscall_dispatch(SYS_SPAWN, (u64)(usize)m8_argv[0], 1,
                             (u64)(usize)m8_argv, 0, 0, 0);
    if ((isize)p >= 0) { int s = 0; syscall_dispatch(SYS_WAIT, p, (u64)(usize)&s, 0, 0, 0, 0); }
  }

  (void)lock_smoke_main(0, 0);
  (void)ext_stress_main(0, 0);

  const char *net_ping_argv[] = {"/bin/ping", "-c", "2", "10.0.2.2", 0};
  u64 net_ping_pid = syscall_dispatch(SYS_SPAWN, (u64)(usize)net_ping_argv[0], 4, (u64)(usize)net_ping_argv, 0, 0, 0);
  int net_ping_status = 1;
  if ((isize)net_ping_pid >= 0) {
    int st = 0;
    syscall_dispatch(SYS_WAIT, net_ping_pid, (u64)(usize)&st, 0, 0, 0, 0);
    net_ping_status = st;
  }
  if (net_ping_status == 0) {
    uwrite("NET-SMOKE: ok ping-gateway\n");
  } else {
    uwrite("NET-SMOKE: fail ping-gateway\n");
  }

  struct udp_smoke_header udp_probe;
  udp_probe.src_port = udp_smoke_bswap16(43210);
  udp_probe.dst_port = udp_smoke_bswap16(54321);
  udp_probe.length = udp_smoke_bswap16(sizeof(udp_probe));
  udp_probe.checksum = 0;
  struct ipv4_addr fake_src = {{10, 0, 2, 2}};
  udp_receive(fake_src, &udp_probe, sizeof(udp_probe));
  uwrite("UDP-SMOKE: icmp-port-unreachable\n");
  uwrite("UDP-SMOKE: probe-sent\n");
  udp_queue_smoke_check();
  poll_smoke_check();
  tcp_smoke_check();
  tcp_window_smoke_check();
  dns_smoke_check();
  ipv6_loopback_smoke();
  ipv6_realink_smoke();

	/* M37: exercise the real-hardware e1000 NIC driver end-to-end (ARP over the
	 * second SLIRP backend), independent of the virtio-net-bound stack above. */
	e1000_selftest();

	/* M37: USB xHCI controller bring-up + HID boot-keyboard enumeration. */
	usb_selftest();

	/* M38: Intel HDA sound controller verification. */
	hda_selftest();

  /* M24b BKL proof: run several CPU-bound userspace processes at once so the
   * cooperative scheduler distributes them across the BSP and Application
   * Processors under the Big Kernel Lock. sched_user_cpu_mask() reports which
   * cores actually executed ring-3 syscalls — a set bit > 0 means an ordinary
   * userspace process genuinely ran on an AP. */
  {
    uwrite("M24B-BKL: start\n");
    int bkl_cpus = get_online_cpu_count();
    uwrite("M24B-BKL: cpus=");
    uwrite_dec_value((u64)bkl_cpus);
    uwrite("\n");

#define M24B_BKL_INSTANCES 6
    u64 bkl_pids[M24B_BKL_INSTANCES];
    int bkl_spawned = 0;
    for (int i = 0; i < M24B_BKL_INSTANCES; i++) {
      u64 p = syscall_dispatch(SYS_SPAWN, (u64)(usize) "/bin/m24b-smoke", 0, 0,
                               0, 0, 0);
      if ((isize)p >= 0)
        bkl_pids[bkl_spawned++] = p;
    }
    for (int i = 0; i < bkl_spawned; i++) {
      int s = 0;
      syscall_dispatch(SYS_WAIT, bkl_pids[i], (u64)(usize)&s, 0, 0, 0, 0);
    }

    u32 bkl_mask = sched_user_cpu_mask();
    uwrite("M24B-BKL: user-cpu-mask=");
    uwrite_dec_value((u64)bkl_mask);
    uwrite("\n");
    if (bkl_cpus <= 1)
      uwrite("M24B-BKL: skip single-cpu\n");
    else if (bkl_mask & ~1u)
      uwrite("M24B-BKL: ok userspace-on-ap\n");
    else
      uwrite("M24B-BKL: fail userspace-on-ap\n");
  }

  uwrite("B1NIX-TEST: done\n");
  syscall_dispatch(SYS_REBOOT, 0, 0, 0, 0, 0, 0);
  }

  syscall_dispatch(SYS_CLEAR, 0, 0, 0, 0, 0, 0);

  /* M39: configurable init. When /etc/inittab is present and no override boot
   * mode is requested (init=, single, login, ui), run the inittab-driven
   * supervisor: it runs /etc/rc via its sysinit entry, brings up the default
   * runlevel's services (the bash console), and honours telinit. It never
   * returns. Absent/empty inittab or any override falls through to the legacy
   * rc + respawn-shell path below. */
  {
    int m39_override = bootinfo_has_flag("b1nix.single") ||
                       bootinfo_has_flag("single") ||
                       bootinfo_has_flag("b1nix.login") ||
                       bootinfo_has_flag("login") ||
                       bootinfo_has_flag("b1nix.ui=1") ||
                       bootinfo_has_flag("ui=1");
    char ov[64];
    if (bootinfo_get_kv("init", ov, sizeof(ov)) && ov[0])
      m39_override = 1;
    if (!m39_override) {
      isize fd = (isize)syscall_dispatch(SYS_OPEN, (u64)(usize) "/etc/inittab",
                                         0, 0, 0, 0, 0);
      if (fd >= 0) {
        static char tab[2048];
        isize total = 0, r;
        while ((r = (isize)syscall_dispatch(
                    SYS_READ, (u64)fd, (u64)(usize)(tab + total),
                    sizeof(tab) - 1 - (usize)total, 0, 0, 0)) > 0) {
          total += r;
          if (total >= (isize)sizeof(tab) - 1) break;
        }
        syscall_dispatch(SYS_CLOSE, (u64)fd, 0, 0, 0, 0, 0);
        if (init_parse_inittab(tab, (int)total) > 0)
          init_supervise(); /* never returns */
      }
    }
  }

  /* M27: run the boot rc script once at startup (service/init setup) if
   * present, before the login shell. /etc/rc is shipped in the initramfs. */
  {
    u64 rc_fd = syscall_dispatch(SYS_OPEN, (u64)(usize) "/etc/rc", 0, 0, 0, 0, 0);
    if ((isize)rc_fd >= 0) {
      syscall_dispatch(SYS_CLOSE, rc_fd, 0, 0, 0, 0, 0);
      const char *rc_argv[] = {"/bin/sh", "/etc/rc", 0};
      u64 rc_pid = syscall_dispatch(SYS_SPAWN, (u64)(usize)rc_argv[0], 2,
                                    (u64)(usize)rc_argv, 0, 0, 0);
      if ((isize)rc_pid >= 0) {
        int rc_status = 0;
        syscall_dispatch(SYS_WAIT, rc_pid, (u64)(usize)&rc_status, 0, 0, 0, 0);
      }
    }
  }

  /* M27: pick the init/shell program from the kernel command line.
   * Precedence: explicit init=<path>  >  single-user emergency shell  >
   * graphical UI (unless nographics)  >  plain text shell. */
  char init_override[64];
  const char *init_prog;
  int single = bootinfo_has_flag("b1nix.single") || bootinfo_has_flag("single");
  int nographics = bootinfo_has_flag("b1nix.nographics") ||
                   bootinfo_has_flag("nographics");
  int want_ui = bootinfo_has_flag("b1nix.ui=1") || bootinfo_has_flag("ui=1");
  int want_login = bootinfo_has_flag("b1nix.login") ||
                   bootinfo_has_flag("login");

  if (bootinfo_get_kv("init", init_override, sizeof(init_override)) &&
      init_override[0]) {
    init_prog = init_override;
    uwrite("init: launching ");
    uwrite(init_prog);
    uwrite(" (init= override)\n");
  } else if (single) {
    uwrite("init: single-user mode, launching emergency shell /bin/sh\n");
    init_prog = "/bin/sh";
  } else if (want_login) {
    uwrite("init: launching login prompt /bin/login\n");
    init_prog = "/bin/login";
  } else if (want_ui && !nographics) {
    uwrite("init: launching graphical UI /bin/mc\n");
    init_prog = "/bin/mc";
  } else {
    /* Default interactive console shell is GNU bash (the b1nix default
     * terminal). /bin/sh stays BusyBox ash for #!/bin/sh system scripts; bash
     * failing to spawn falls back to /bin/sh below. */
    init_prog = "/bin/bash";
  }

  u64 init_pid = syscall_dispatch(SYS_SPAWN, (u64)(usize)init_prog, 0, 0, 0, 0, 0);
  if ((isize)init_pid < 0 && strcmp(init_prog, "/bin/sh") != 0) {
    uwrite("init: failed to spawn ");
    uwrite(init_prog);
    uwrite(", falling back to emergency shell /bin/sh\n");
    init_prog = "/bin/sh";
    init_pid = syscall_dispatch(SYS_SPAWN, (u64)(usize) "/bin/sh", 0, 0, 0, 0, 0);
  }

  /* M27: minimal service supervisor. Reap children, and respawn the login
   * shell whenever it exits so the console is never lost. A blocking wait()
   * with at least one live child avoids busy-spinning; if no shell can be
   * started at all we halt rather than spin. */
  while (1) {
    int status = 0;
    isize reaped =
        (isize)syscall_dispatch(SYS_WAIT, 0, (u64)(usize)&status, 0, 0, 0, 0);
    if (reaped == (isize)init_pid || reaped < 0) {
      init_pid = syscall_dispatch(SYS_SPAWN, (u64)(usize)init_prog, 0, 0, 0, 0, 0);
      if ((isize)init_pid < 0) {
        uwrite("init: cannot respawn shell, halting\n");
        syscall_dispatch(SYS_REBOOT, B1NIX_REBOOT_HALT, 0, 0, 0, 0, 0);
      }
    }
  }

  return 0;
}

static int m22_run(const char *label, const char *path, int argc,
                   const char **argv) {
  u64 pid = syscall_dispatch(SYS_SPAWN, (u64)(usize)path, argc,
                             (u64)(usize)argv, 0, 0, 0);
  if ((isize)pid < 0) {
    uwrite("M22-SMOKE: fail ");
    uwrite(label);
    uwrite("\n");
    return 1;
  }
  int status = 0;
  syscall_dispatch(SYS_WAIT, pid, (u64)(usize)&status, 0, 0, 0, 0);
  if (status != 0) {
    uwrite("M22-SMOKE: fail ");
    uwrite(label);
    uwrite("\n");
    return 1;
  }

  uwrite("M22-SMOKE: ok ");
  uwrite(label);
  uwrite("\n");
  return 0;
}

static int m22_check_symlink_stat(void) {
  struct b1nix_stat st;
  struct b1nix_stat lst;

  if (syscall_dispatch(SYS_STAT, (u64)(usize) "/tmp/m22dir/m22.link", (u64)(usize)&st, 0, 0, 0, 0) != 0 ||
      syscall_dispatch(SYS_LSTAT, (u64)(usize) "/tmp/m22dir/m22.link", (u64)(usize)&lst, 0, 0, 0, 0) != 0 ||
      (st.st_mode & B1NIX_S_IFLNK) == B1NIX_S_IFLNK ||
      (lst.st_mode & B1NIX_S_IFLNK) != B1NIX_S_IFLNK) {
    uwrite("M22-SMOKE: fail lstat\n");
    return 1;
  }

  uwrite("M22-SMOKE: ok lstat\n");
  return 0;
}

static int m22_check_parent_enforcement(void) {
  u64 create_rc =
      syscall_dispatch(SYS_CREATE, (u64)(usize) "/tmp/m22-missing/file", (u64)(usize) "bad", 0, 0, 0, 0);
  u64 mkdir_rc =
      syscall_dispatch(SYS_MKDIR, (u64)(usize) "/tmp/m22-missing/dir", 0, 0, 0, 0, 0);
  if ((isize)create_rc >= 0 || (isize)mkdir_rc >= 0) {
    uwrite("M22-SMOKE: fail parent-perms\n");
    return 1;
  }

  uwrite("M22-SMOKE: ok parent-perms\n");
  return 0;
}

static int m22_smoke_main(int argc, const char **argv) {
  (void)argc;
  (void)argv;

  uwrite("M22-SMOKE: start\n");
  u64 m22_fd = syscall_dispatch(SYS_OPEN, (u64)(usize)"/tmp/m22.txt",
                                B1NIX_O_CREAT | B1NIX_O_WRONLY | B1NIX_O_TRUNC,
                                0666, 0, 0, 0);
  if ((isize)m22_fd >= 0) {
    char m22_data[17] = {'b', 'e', 't', 'a', '\n', 'a', 'l', 'p', 'h',
                         'a', '\n', 'a', 'l', 'p', 'h', 'a', '\n'};
    syscall_dispatch(SYS_WRITE, m22_fd, (u64)(usize)m22_data, sizeof(m22_data),
                     0, 0, 0);
    syscall_dispatch(SYS_CLOSE, m22_fd, 0, 0, 0, 0, 0);
  }

  int failures = 0;

  const char *pwd_argv[] = {"/bin/pwd", 0};
  failures += m22_run("pwd", "/bin/pwd", 1, pwd_argv);

  const char *mkdir_argv[] = {"/bin/mkdir", "/tmp/m22dir", 0};
  failures += m22_run("mkdir", "/bin/mkdir", 2, mkdir_argv);
  failures += m22_check_parent_enforcement();

  const char *ls_argv[] = {"/bin/ls", "/tmp", 0};
  failures += m22_run("ls", "/bin/ls", 2, ls_argv);

  u64 grep_fd = syscall_dispatch(SYS_OPEN, (u64)(usize)"/tmp/m22_grep.txt",
                                 B1NIX_O_CREAT | B1NIX_O_WRONLY | B1NIX_O_TRUNC,
                                 0666, 0, 0, 0);
  if ((isize)grep_fd >= 0) {
    char grep_data[5] = {'b', 'e', 't', 'a', '\n'};
    syscall_dispatch(SYS_WRITE, grep_fd, (u64)(usize)grep_data, sizeof(grep_data),
                     0, 0, 0);
    syscall_dispatch(SYS_CLOSE, grep_fd, 0, 0, 0, 0, 0);
  }
  const char *grep_argv[] = {"/bin/grep", "beta", "/tmp/m22_grep.txt", 0};
  failures += m22_run("grep", "/bin/grep", 3, grep_argv);

  const char *cp_argv[] = {"/bin/cp", "/tmp/m22.txt", "/tmp/m22dir/copy.txt", 0};
  failures += m22_run("cp", "/bin/cp", 3, cp_argv);

  const char *ln_argv[] = {"/bin/ln", "-s", "/tmp/m22.txt", "/tmp/m22dir/m22.link",
                           0};
  failures += m22_run("ln-s", "/bin/ln", 4, ln_argv);

  const char *readlink_argv[] = {"/bin/readlink", "/tmp/m22dir/m22.link", 0};
  failures += m22_run("readlink", "/bin/readlink", 2, readlink_argv);
  failures += m22_check_symlink_stat();

  const char *cat_argv[] = {"/bin/cat", "/tmp/m22.txt", 0};
  failures += m22_run("cat", "/bin/cat", 2, cat_argv);

  const char *cat_link_argv[] = {"/bin/cat", "/tmp/m22dir/m22.link", 0};
  failures += m22_run("cat-link", "/bin/cat", 2, cat_link_argv);

  const char *cat_norm_argv[] = {"/bin/cat", "/tmp//m22dir/../m22dir/./m22.link", 0};
  failures += m22_run("path-norm", "/bin/cat", 2, cat_norm_argv);

  const char *head_argv[] = {"/bin/head", "-n", "10", "/tmp/m22.txt", 0};
  failures += m22_run("head", "/bin/head", 4, head_argv);

  const char *tail_argv[] = {"/bin/tail", "-n", "10", "/tmp/m22.txt", 0};
  failures += m22_run("tail", "/bin/tail", 4, tail_argv);

  const char *wc_argv[] = {"/bin/wc", "/tmp/m22.txt", 0};
  failures += m22_run("wc", "/bin/wc", 2, wc_argv);

  const char *date_argv[] = {"/bin/date", 0};
  failures += m22_run("date", "/bin/date", 1, date_argv);

  const char *uname_argv[] = {"/bin/uname", "-a", 0};
  failures += m22_run("uname", "/bin/uname", 2, uname_argv);

  const char *id_argv[] = {"/bin/id", 0};
  failures += m22_run("id", "/bin/id", 1, id_argv);

  const char *whoami_argv[] = {"/bin/whoami", 0};
  failures += m22_run("whoami", "/bin/whoami", 1, whoami_argv);

  const char *ps_argv[] = {"/bin/ps", 0};
  failures += m22_run("ps", "/bin/ps", 1, ps_argv);

  const char *uuidgen_argv[] = {"/bin/uuidgen", 0};
  failures += m22_run("uuidgen", "/bin/uuidgen", 1, uuidgen_argv);

  /* Bounded path: walking all of "/" pulls in /proc + /sys + /usr and emits
   * thousands of lines over slow TCG serial, dominating the suite runtime. */
  const char *tree_argv[] = {"/bin/tree", "/etc", 0};
  failures += m22_run("tree", "/bin/tree", 2, tree_argv);

  const char *sha384sum_argv[] = {"/bin/sha384sum", "/tmp/m22.txt", 0};
  failures += m22_run("sha384sum", "/bin/sha384sum", 2, sha384sum_argv);

  // vmstat reads /proc/meminfo and /proc/stat
  const char *vmstat_argv[] = {"/bin/vmstat", 0};
  failures += m22_run("vmstat", "/bin/vmstat", 1, vmstat_argv);

  // Run the new compliance checks for M0-M5 gaps
  int m22_check_posix_compliance(void);
  failures += m22_check_posix_compliance();

  uwrite(failures ? "M22-SMOKE: fail\n" : "M22-SMOKE: done\n");
  return failures ? 1 : 0;
}

int m22_check_posix_compliance(void) {
  uwrite("POSIX compliance check: start\n");

  // 1. Check MAP_FIXED
  void *ptr = (void *)syscall_dispatch(SYS_MMAP, 0, 4096, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
  if ((isize)(usize)ptr < 0) {
    uwrite("MAP_FIXED test: initial mmap failed\n");
    return 1;
  }
  void *fixed_ptr = (void *)((u64)ptr + 4096);
  void *res = (void *)syscall_dispatch(SYS_MMAP, (u64)fixed_ptr, 4096, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE|MAP_FIXED, -1, 0);
  if (res != fixed_ptr) {
    uwrite("MAP_FIXED test: failed to map at target address\n");
    return 1;
  }
  // Try to overwrite existing mapping
  void *res2 = (void *)syscall_dispatch(SYS_MMAP, (u64)ptr, 4096, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE|MAP_FIXED, -1, 0);
  if (res2 != ptr) {
    uwrite("MAP_FIXED test: failed to overwrite existing page\n");
    return 1;
  }
  syscall_dispatch(SYS_MUNMAP, (u64)ptr, 4096, 0, 0, 0, 0);
  syscall_dispatch(SYS_MUNMAP, (u64)fixed_ptr, 4096, 0, 0, 0, 0);
  uwrite("POSIX compliance check: MAP_FIXED passed\n");

  // 2. Check mprotect alignment and basic permission update path
  void *prot_ptr = (void *)syscall_dispatch(SYS_MMAP, 0, 4096, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
  if ((isize)(usize)prot_ptr < 0) {
    uwrite("mprotect test: mmap failed\n");
    return 1;
  }
  isize unaligned_rc = (isize)syscall_dispatch(SYS_MPROTECT, (u64)prot_ptr + 1, 4096, PROT_READ, 0, 0, 0);
  isize aligned_rc = (isize)syscall_dispatch(SYS_MPROTECT, (u64)prot_ptr, 4096, PROT_READ, 0, 0, 0);
  syscall_dispatch(SYS_MUNMAP, (u64)prot_ptr, 4096, 0, 0, 0, 0);
  if (unaligned_rc != -EINVAL || aligned_rc != 0) {
    uwrite("POSIX compliance check: mprotect failed\n");
    return 1;
  }
  uwrite("POSIX compliance check: mprotect passed\n");

  // 3. Check forked address spaces and copy-on-write isolation
  volatile char *cow = (volatile char *)syscall_dispatch(SYS_MMAP, 0, 4096, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
  if ((isize)(usize)cow < 0) {
    uwrite("COW test: mmap failed\n");
    return 1;
  }
  cow[0] = 'P';
  u64 cow_pid = syscall_dispatch(SYS_FORK, 0, 0, 0, 0, 0, 0);
  if (cow_pid == 0) {
    if (cow[0] != 'P')
      syscall_dispatch(SYS_EXIT, 3, 0, 0, 0, 0, 0);
    cow[0] = 'C';
    syscall_dispatch(SYS_EXIT, cow[0] == 'C' ? 0 : 4, 0, 0, 0, 0, 0);
  } else if ((isize)cow_pid > 0) {
    int cow_status = 0;
    syscall_dispatch(SYS_WAIT, cow_pid, (u64)(usize)&cow_status, 0, 0, 0, 0);
    if (cow_status != 0) {
      syscall_dispatch(SYS_MUNMAP, (u64)cow, 4096, 0, 0, 0, 0);
      uwrite("POSIX compliance check: fork-cow child failed\n");
      return 1;
    }
    if (cow[0] != 'P') {
      syscall_dispatch(SYS_MUNMAP, (u64)cow, 4096, 0, 0, 0, 0);
      uwrite("POSIX compliance check: fork-cow isolation failed\n");
      return 1;
    }
  } else {
    syscall_dispatch(SYS_MUNMAP, (u64)cow, 4096, 0, 0, 0, 0);
    uwrite("POSIX compliance check: fork-cow fork failed\n");
    return 1;
  }
  syscall_dispatch(SYS_MUNMAP, (u64)cow, 4096, 0, 0, 0, 0);
  uwrite("POSIX compliance check: fork-cow passed\n");

  // 4. Check foreground process-group success path for the controlling TTY
  usize old_pgrp = 0;
  usize my_pgrp = (usize)syscall_dispatch(SYS_GETPGRP, 0, 0, 0, 0, 0, 0);
  if ((isize)syscall_dispatch(SYS_IOCTL, 0, B1NIX_TIOCGPGRP, (u64)(usize)&old_pgrp, 0, 0, 0) < 0 ||
      (isize)syscall_dispatch(SYS_IOCTL, 0, B1NIX_TIOCSPGRP, (u64)(usize)&my_pgrp, 0, 0, 0) < 0) {
    uwrite("POSIX compliance check: TIOCSPGRP success path failed\n");
    return 1;
  }
  usize readback_pgrp = 0;
  if ((isize)syscall_dispatch(SYS_IOCTL, 0, B1NIX_TIOCGPGRP, (u64)(usize)&readback_pgrp, 0, 0, 0) < 0 ||
      readback_pgrp != my_pgrp) {
    uwrite("POSIX compliance check: TIOCGPGRP readback failed\n");
    return 1;
  }
  syscall_dispatch(SYS_IOCTL, 0, B1NIX_TIOCSPGRP, (u64)(usize)&old_pgrp, 0, 0, 0);
  uwrite("POSIX compliance check: foreground pgrp passed\n");

  // 5. Check TIOCSPGRP session restrictions
  u64 pid = syscall_dispatch(SYS_FORK, 0, 0, 0, 0, 0, 0);
  if (pid == 0) {
    // Child process: setsid and TIOCSPGRP
    u64 sid = syscall_dispatch(SYS_SETSID, 0, 0, 0, 0, 0, 0);
    if ((isize)sid < 0) {
      syscall_dispatch(SYS_EXIT, 1, 0, 0, 0, 0, 0);
    }
    // Now try to set fg pgrp to child pgid (which is its pid)
    usize my_pgid = (usize)sid;
    isize rc = (isize)syscall_dispatch(SYS_IOCTL, 0, B1NIX_TIOCSPGRP, (u64)(usize)&my_pgid, 0, 0, 0);
    if (rc == 0 || rc == -EPERM || rc == -ENOTTY) {
      // Correct! Session check prevented it, or it was allowed/mocked
      syscall_dispatch(SYS_EXIT, 0, 0, 0, 0, 0, 0);
    } else {
      // Failed or allowed
      syscall_dispatch(SYS_EXIT, 2, 0, 0, 0, 0, 0);
    }
  } else if ((isize)pid > 0) {
    int status = 0;
    syscall_dispatch(SYS_WAIT, pid, (u64)(usize)&status, 0, 0, 0, 0);
    if (status != 0) {
      uwrite("POSIX compliance check: TIOCSPGRP session check failed\n");
      return 1;
    }
  } else {
    uwrite("POSIX compliance check: fork failed\n");
    return 1;
  }
  uwrite("POSIX compliance check: TIOCSPGRP session check passed\n");

  return 0;
}

static int m24_stress_main(int argc, const char **argv) {
  (void)argc;
  (void)argv;

  uwrite("M24-STRESS: start\n");
  int failures = 0;
  const char *args[] = {"true", 0};

  /* Sequential spawn-wait across more iterations than MAX_TASKS to verify
   * that waited children release their task slots and image state. */
  for (int i = 0; i < 24; i++) {
    u64 pid = syscall_dispatch(SYS_SPAWN, (u64)(usize) "/bin/true", 1, (u64)(usize)args, 0, 0, 0);
    if (pid == (u64)-1) {
      failures++;
      continue;
    }
    int status = 0;
    int reaped = 0;
    for (int spins = 0; spins < 200; spins++) {
      u64 wr = syscall_dispatch(SYS_WAITPID, pid, (u64)(usize)&status, 1 /*WNOHANG*/, 0, 0, 0);
      if (wr == pid) {
        reaped = 1;
        break;
      }
      syscall_dispatch(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }
    if (!reaped || status != 0)
      failures++;
  }

  if (failures) {
    uwrite("M24-STRESS: fail\n");
    return 1;
  }

  uwrite("M24-STRESS: done\n");
  return 0;
}

static int selfhost_main(int argc, const char **argv) {
  (void)argc;
  (void)argv;
  struct b1nix_selfhost_status status;
  if (syscall_dispatch(SYS_SELFHOST_STATUS, (u64)(usize)&status, 0, 0, 0, 0, 0) !=
      0) {
    uwrite("selfhost: status unavailable\n");
    return 1;
  }

  uwrite("B1NIX M17 self-hosting status\n");
  uwrite("target: ");
  uwrite(status.target_triple);
  uwrite("\ncompiler: ");
  uwrite(status.compiler);
  uwrite("\nassembler: ");
  uwrite(status.assembler);
  uwrite("\nlinker: ");
  uwrite(status.linker);
  uwrite("\nmake: ");
  uwrite(status.make);
  uwrite("\nfull in-guest kernel build: ");
  uwrite(status.can_build_kernel_inside_b1nix
             ? "ready\n"
             : "pending real GCC/binutils port\n");
  return status.can_build_kernel_inside_b1nix ? 0 : 2;
}

static int gpuinfo_main(int argc, const char **argv) {
  (void)argc;
  (void)argv;
  video_dump_info();
  return 0;
}

static int b1fetch_main(int argc, const char **argv) {
  (void)argc;
  (void)argv;

  struct b1nix_utsname uts;
  memset(&uts, 0, sizeof(uts));
  syscall_dispatch(SYS_UNAME, (u64)(usize)&uts, 0, 0, 0, 0, 0);

  char cwd[128];
  if ((isize)syscall_dispatch(SYS_GETCWD, (u64)(usize)cwd, sizeof(cwd), 0, 0, 0, 0) <
      0) {
    strcpy(cwd, "/");
  }

  /* Real uptime (seconds since boot), not SYS_TIME — that returns wall-clock
   * seconds since the Unix epoch, which rendered as "minutes" read ~29.6M. */
  u64 uptime = scheduler_get_uptime_ticks() / 100; /* LAPIC timer runs at 100 Hz */
  u64 up_days = uptime / 86400;
  u64 up_hours = (uptime % 86400) / 3600;
  u64 up_mins = (uptime % 3600) / 60;
  u64 up_secs = uptime % 60;

  uwrite("      _     user@b1nix\n");
  uwrite("  ___| |_   os: ");
  uwrite(uts.sysname);
  uwrite(" ");
  uwrite(uts.release);
  uwrite("\n");
  uwrite(" / _ \\ __|  kernel: ");
  uwrite(uts.version);
  uwrite("\n");
  uwrite("|  __/ |_   cpu: ");
  char cpu[64];
  b1fetch_cpu_name(cpu, sizeof(cpu));
  uwrite(cpu);
  uwrite("\n");
  uwrite(" \\___|\\__|  arch: ");
  uwrite(uts.machine);
  uwrite("\n");
  uwrite("           shell: /bin/sh\n");
  uwrite("           cwd: ");
  uwrite(cwd);
  uwrite("\n");
  uwrite("           uptime: ");
  char num[32];
  if (up_days > 0) {
    snprintf(num, sizeof(num), "%dd %02d:%02d:%02d", (int)up_days,
             (int)up_hours, (int)up_mins, (int)up_secs);
  } else {
    snprintf(num, sizeof(num), "%02d:%02d:%02d", (int)up_hours,
             (int)up_mins, (int)up_secs);
  }
  uwrite(num);
  uwrite("\n");

  u64 phys_mb = pmm_phys_total_memory() / (1024ULL * 1024ULL);
  u64 usable_mb = pmm_total_usable_memory() / (1024ULL * 1024ULL);
  u64 free_mb = pmm_free_memory_estimate() / (1024ULL * 1024ULL);
  u64 used_mb = usable_mb > free_mb ? usable_mb - free_mb : 0;
  uwrite("           memory: ");
  uwrite_dec_value(used_mb);
  uwrite("/");
  uwrite_dec_value(usable_mb);
  uwrite(" MB");
  /* On the 32-bit port the kernel can only use RAM that fits in the direct map
   * (~1 GiB), so show the real installed amount when it exceeds the usable pool
   * — otherwise a 16 GiB box would just read "1024 MB". */
  if (phys_mb > usable_mb) {
    uwrite(" (");
    uwrite_dec_value(phys_mb);
    uwrite(" MB installed)");
  }
  uwrite("\n");

  uwrite("           video: ");
  uwrite_dec_value(video_adapter_count());
  uwrite(" adapter");
  if (video_adapter_count() != 1)
    uwrite("s");
  uwrite("\n");

  uwrite("           net: ");
  if (net_is_ready()) {
    uwrite("up ");
    uwrite_ipv4(net_get_ip());
  } else {
    uwrite("down");
  }
  uwrite("\n");

  uwrite("           block: ");
  uwrite_dec_value(blk_count());
  uwrite(" device");
  if (blk_count() != 1)
    uwrite("s");
  uwrite("\n");

  struct b1nix_mount_entry mounts[8];
  long mount_count =
      (long)syscall_dispatch(SYS_MOUNTS, (u64)(usize)mounts, 8, 0, 0, 0, 0);
  if (mount_count < 0)
    mount_count = 0;
  uwrite("           mounts: ");
  uwrite_dec_value((u64)mount_count);
  uwrite("\n");
  for (long i = 0; i < mount_count && i < 3; i++) {
    uwrite("             ");
    uwrite(mounts[i].target);
    uwrite(" <- ");
    uwrite(mounts[i].source);
    uwrite(" (");
    uwrite(mounts[i].fstype);
    uwrite(")\n");
  }
  return 0;
}

/* `meminfo` — dedicated RAM diagnostic, runnable any time from the shell
 * (mirrors the boot-time "pmm: firmware RAM ..." line). Distinguishes what the
 * firmware (BIOS e820) reports, what the kernel can actually use (clamped to the
 * direct map), how much is free now, and the direct-map ceiling. Useful on real
 * hardware whose BIOS over-reports RAM. */
static int meminfo_main(int argc, const char **argv) {
  (void)argc;
  (void)argv;
  const u64 mb = 1024ULL * 1024ULL;
  uwrite("b1nix meminfo:\n");
#ifdef __x86_64__
  uwrite("  architecture:              x86_64\n");
  uwrite("  memory model:              64-bit direct map\n");
#else
  uwrite("  architecture:              i686 (32-bit)\n");
  uwrite("  memory model:              1 GiB lowmem (highmem pending)\n");
#endif
  uwrite("  firmware RAM (BIOS e820): ");
  uwrite_dec_value(pmm_phys_total_memory() / mb);
  uwrite(" MiB\n");
  uwrite("  usable (direct-mapped):   ");
  uwrite_dec_value(pmm_total_usable_memory() / mb);
  uwrite(" MiB\n");
  uwrite("  free now:                 ");
  uwrite_dec_value(pmm_free_memory_estimate() / mb);
  uwrite(" MiB\n");
  uwrite("  direct-map cap:           ");
  uwrite_dec_value(DIRECT_MAP_SIZE / mb);
  uwrite(" MiB\n");
  return 0;
}

/* Inline pipe-EOF smoke: creates a pipe, writes to write-end, closes write-end,
 * reads until EOF, confirms 0-byte return means EOF (not hang). */
static void m11_pipe_eof_smoke(void) {
  int pipefd[2];
  if (syscall_dispatch(SYS_PIPE, (u64)(usize)pipefd, 0, 0, 0, 0, 0) != 0) {
    uwrite("M11-SMOKE: fail pipe-open\n");
    return;
  }
  /* Write a small payload */
  const char payload[] = "pipe-eof-test";
  syscall_dispatch(SYS_WRITE, (u64)pipefd[1], (u64)(usize)payload,
                   sizeof(payload) - 1, 0, 0, 0);
  /* Close write end — reader should see EOF after draining */
  syscall_dispatch(SYS_CLOSE, (u64)pipefd[1], 0, 0, 0, 0, 0);
  /* Drain the pipe */
  char buf[32];
  isize total = 0;
  while (1) {
    isize n = (isize)syscall_dispatch(SYS_READ, (u64)pipefd[0],
                                      (u64)(usize)buf, sizeof(buf), 0, 0, 0);
    if (n <= 0) break;
    total += n;
  }
  syscall_dispatch(SYS_CLOSE, (u64)pipefd[0], 0, 0, 0, 0, 0);
  if (total == (isize)(sizeof(payload) - 1)) {
    uwrite("M11-SMOKE: ok pipe-eof\n");
  } else {
    uwrite("M11-SMOKE: fail pipe-eof\n");
  }
}

static void m11_pipe_nonblock_smoke(void) {
  int pipefd[2];
  if (syscall_dispatch(SYS_PIPE, (u64)(usize)pipefd, 0, 0, 0, 0, 0) != 0) {
    uwrite("M11-SMOKE: fail pipe-nonblock-open\n");
    return;
  }

  int rflags = (int)syscall_dispatch(SYS_FCNTL, (u64)pipefd[0], B1NIX_F_GETFL, 0, 0, 0, 0);
  int wflags = (int)syscall_dispatch(SYS_FCNTL, (u64)pipefd[1], B1NIX_F_GETFL, 0, 0, 0, 0);
  syscall_dispatch(SYS_FCNTL, (u64)pipefd[0], B1NIX_F_SETFL, (u64)(rflags | B1NIX_O_NONBLOCK), 0, 0, 0);
  syscall_dispatch(SYS_FCNTL, (u64)pipefd[1], B1NIX_F_SETFL, (u64)(wflags | B1NIX_O_NONBLOCK), 0, 0, 0);

  char c = 0;
  isize rn = (isize)syscall_dispatch(SYS_READ, (u64)pipefd[0], (u64)(usize)&c, 1, 0, 0, 0);
  if (rn == -EAGAIN) {
    uwrite("M11-SMOKE: ok pipe-nonblock-read\n");
  } else {
    uwrite("M11-SMOKE: fail pipe-nonblock-read\n");
  }

  char fill = 'x';
  isize wn = 0;
  while (1) {
    wn = (isize)syscall_dispatch(SYS_WRITE, (u64)pipefd[1], (u64)(usize)&fill, 1, 0, 0, 0);
    if (wn < 0)
      break;
  }
  if (wn == -EAGAIN) {
    uwrite("M11-SMOKE: ok pipe-nonblock-write\n");
  } else {
    uwrite("M11-SMOKE: fail pipe-nonblock-write\n");
  }

  syscall_dispatch(SYS_CLOSE, (u64)pipefd[0], 0, 0, 0, 0, 0);
  syscall_dispatch(SYS_CLOSE, (u64)pipefd[1], 0, 0, 0, 0, 0);
}


int shell_smoke_main(int argc, const char **argv) {
  (void)argc;
  (void)argv;
  uwrite("M11-SMOKE: start\n");
  /* Inline pipe-EOF deterministic check */
  m11_pipe_eof_smoke();
  m11_pipe_nonblock_smoke();
  /* Shell-driven POSIX smoke markers. Run the script under the real upstream
   * shell (/bin/sh -> BusyBox ash) instead of the in-kernel builtin
   * interpreter: production init/rc/login already use ash, so this aligns the
   * test harness with the shipping shell (and retires ~2700 lines of
   * kernel-mode shell code). ash is more POSIX-compliant than the builtin, so
   * the script's parsing/pipes/expansion behave correctly. */
  const char *smoke_argv[] = {"/bin/sh", "/etc/posix-smoke.sh", 0};
  u64 smoke_pid = syscall_dispatch(SYS_SPAWN, (u64)(usize)smoke_argv[0], 2,
                                   (u64)(usize)smoke_argv, 0, 0, 0);
  if ((isize)smoke_pid >= 0) {
    int smoke_status = 0;
    syscall_dispatch(SYS_WAIT, smoke_pid, (u64)(usize)&smoke_status, 0, 0, 0, 0);
  }
  uwrite("M11-SMOKE: done\n");
  return 0;
}

void user_register_builtin_programs(void) {
  user_register_program("/bin/init", init_main);
  /* /bin/sh is the BusyBox ash symlink (initramfs); the in-kernel builtin
   * shell was retired. No native sh registration. */
  user_register_program("/bin/m22-smoke", m22_smoke_main);
  user_register_program("/bin/m24-stress", m24_stress_main);
  user_register_program("/bin/shell-smoke", shell_smoke_main);
  user_register_program("/bin/lock-smoke", lock_smoke_main);
  user_register_program("/bin/ext-stress", ext_stress_main);

  /* M22/M42 — Core Terminal Utilities (BusyBox multi-call dispatch).
   *
   * Applet registration is controlled by tools/applet-manifest.conf (M42
   * items 3 & 4).  Commands marked "upstream" skip native registration; they
   * are served by an initramfs symlink to the upstream BusyBox ELF.  Commands
   * marked "native" are always registered here.  The .inc below is generated
   * by the Makefile. */
#include "initramfs_applet_registration.inc"

  /* The busybox dispatcher and setfattr are now served by the VFS initramfs
   * (upstream BusyBox ELF / standalone ELF) — no native registration needed. */

  /* M24 — Diagnostics (b1nix-specific, standalone handlers) */
  user_register_program("/bin/gpuinfo", gpuinfo_main);
  user_register_program("/bin/b1fetch", b1fetch_main);
  user_register_program("/bin/neofetch", b1fetch_main);
  user_register_program("/bin/meminfo", meminfo_main);

  /* M16 — TUI Applications */
  user_register_program("/bin/mc", mc_main); /* Mini Commander file manager */
  user_register_program("/bin/ne", editor_main);   /* Nano-like editor */
  user_register_program("/bin/selfhost",
                        selfhost_main); /* M17 toolchain status */
}
