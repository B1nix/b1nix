/*
 * M98 T1 — netconsole: the kernel log shipped as UDP datagrams.
 *
 * See kernel/include/b1nix/netconsole.h for the command-line syntax and for why
 * the send happens in a kernel thread instead of inside console_write().
 */

#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/klog.h>
#include <b1nix/net.h>
#include <b1nix/netconsole.h>
#include <b1nix/netdev.h>
#include <b1nix/sched.h>
#include <b1nix/spinlock.h>
#include <string.h>

/* One datagram's payload. Kept below the 1500-byte Ethernet MTU minus the IPv4
 * and UDP headers so netconsole never depends on IP fragmentation. */
#define NETCON_PAYLOAD_MAX 1024
#define NETCON_SRC_PORT 6665

static int netcon_configured;
static int netcon_thread_started;
static struct ipv4_addr netcon_dst;
static u16 netcon_dst_port;
static usize netcon_cursor;
static u64 netcon_sent;
static spinlock_t netcon_lock = SPINLOCK_INIT;
static char netcon_buf[NETCON_PAYLOAD_MAX + 1];

/* ── command-line parsing ───────────────────────────────────────── */

/* "a.b.c.d:port". Returns 1 on success. Strict: every field must be numeric and
 * in range, or the whole target is rejected (a half-parsed address would ship
 * the boot log to a random host). */
static int netcon_parse_target(const char *s, struct ipv4_addr *ip, u16 *port)
{
	u32 octet = 0;
	int digits = 0;
	int idx = 0;
	u8 bytes[4] = {0, 0, 0, 0};

	for (;; s++) {
		char c = *s;
		if (c >= '0' && c <= '9') {
			octet = octet * 10 + (u32)(c - '0');
			if (octet > 255)
				return 0;
			digits++;
			continue;
		}
		if (c == '.' || c == ':') {
			if (!digits || idx >= 4)
				return 0;
			bytes[idx++] = (u8)octet;
			octet = 0;
			digits = 0;
			if (c == ':')
				break;
			continue;
		}
		return 0;
	}
	if (idx != 4)
		return 0;

	s++; /* skip ':' */
	u32 p = 0;
	digits = 0;
	for (; *s; s++) {
		if (*s < '0' || *s > '9')
			return 0;
		p = p * 10 + (u32)(*s - '0');
		if (p > 65535)
			return 0;
		digits++;
	}
	if (!digits || p == 0)
		return 0;

	memcpy(ip->bytes, bytes, 4);
	*port = (u16)p;
	return 1;
}

/* ── draining ───────────────────────────────────────────────────── */

/* Move whatever the ring holds into datagrams. Runs from the drain thread or
 * from netconsole_flush(); the spinlock keeps the two out of each other's
 * cursor and shared buffer. udp_send() may allocate and touch the NIC, so the
 * lock is dropped around it — the cursor has already been advanced, so a
 * concurrent caller picks up where this one stopped instead of resending. */
int netconsole_flush(void)
{
	if (!netcon_configured)
		return 0;

	int packets = 0;
	for (int i = 0; i < 16; i++) {
		u64 flags;
		spin_lock_irqsave(&netcon_lock, &flags);
		usize n = klog_drain(&netcon_cursor, netcon_buf, sizeof(netcon_buf));
		char local[NETCON_PAYLOAD_MAX];
		if (n)
			memcpy(local, netcon_buf, n);
		spin_unlock_irqrestore(&netcon_lock, flags);
		if (!n)
			break;
		udp_send(netcon_dst, NETCON_SRC_PORT, netcon_dst_port, local, n);
		netcon_sent++;
		packets++;
	}
	return packets;
}

static void netconsole_thread(void *arg)
{
	(void)arg;
	for (;;) {
		netconsole_flush();
		/* 20 ms cadence: fast enough that a bare-metal boot log arrives while
		 * the machine is still alive, slow enough not to flood a link with
		 * one datagram per printed character. */
		scheduler_sleep_ticks(2);
	}
}

void netconsole_init(void)
{
	if (netcon_configured)
		return;

	char target[64];
	if (!bootinfo_get_kv("b1nix.netconsole", target, sizeof(target)) ||
	    target[0] == '\0')
		return;

	struct ipv4_addr ip;
	u16 port;
	if (!netcon_parse_target(target, &ip, &port)) {
		console_write("netconsole: bad target '");
		console_write(target);
		console_write("' (expected <ip>:<port>)\n");
		return;
	}

	netcon_dst = ip;
	netcon_dst_port = port;
	/* Start from the beginning of whatever the ring still holds, so the early
	 * boot lines printed before the network came up are shipped too. */
	netcon_cursor = 0;
	netcon_configured = 1;

	console_write("netconsole: logging to ");
	for (int i = 0; i < 4; i++) {
		console_write_dec(ip.bytes[i]);
		console_putc(i == 3 ? ':' : '.');
	}
	console_write_dec(port);
	console_write("\n");

	if (!netcon_thread_started &&
	    kthread_create("netconsole", netconsole_thread, 0) >= 0)
		netcon_thread_started = 1;
}

int netconsole_active(void)
{
	return netcon_configured && netcon_thread_started;
}

u64 netconsole_packets_sent(void)
{
	return netcon_sent;
}

/* ── self-test ───────────────────────────────────────────────────────
 *
 * The whole netconsole path is exercised end to end against the real UDP
 * stack: klog ring -> klog_drain -> udp_send -> ipv4_send -> loopback queue ->
 * ipv4_receive -> udp_receive -> a handler registered here. The marker is only
 * emitted when the handler has actually seen the exact magic string that was
 * written into the ring, so nothing short of the full path working can produce
 * it.
 */
#define NETCON_TEST_PORT 6666
static const char netcon_magic[] = "NETCONSOLE-SELFTEST-3f7a1c";
static volatile int netcon_test_hit;

static void netcon_test_handler(const void *data, usize size)
{
	const char *p = (const char *)data;
	usize mlen = sizeof(netcon_magic) - 1;
	if (size < mlen)
		return;
	for (usize i = 0; i + mlen <= size; i++) {
		if (memcmp(p + i, netcon_magic, mlen) == 0) {
			netcon_test_hit = 1;
			return;
		}
	}
}

void netconsole_selftest(void)
{
	if (!bootinfo_has_flag("b1nix.test=1"))
		return;

	/* Parser first: it decides where a bare-metal boot log is shipped, so a
	 * silent misparse is the worst failure mode this feature has. Checked
	 * against values decoded by hand, not against the parser's own output. */
	struct ipv4_addr ip;
	u16 port = 0;
	int good = netcon_parse_target("192.168.7.9:1234", &ip, &port) &&
	           ip.bytes[0] == 192 && ip.bytes[1] == 168 && ip.bytes[2] == 7 &&
	           ip.bytes[3] == 9 && port == 1234;
	int rejects = !netcon_parse_target("192.168.7:1234", &ip, &port) &&
	              !netcon_parse_target("192.168.7.9", &ip, &port) &&
	              !netcon_parse_target("300.1.1.1:80", &ip, &port) &&
	              !netcon_parse_target("1.2.3.4:70000", &ip, &port) &&
	              !netcon_parse_target("1.2.3.4:0", &ip, &port);
	if (good && rejects)
		console_write("M98-DRV-SMOKE: ok netconsole-cmdline\n");
	else
		console_write("M98-DRV-SMOKE: FAIL netconsole-cmdline\n");

	if (udp_register_handler(NETCON_TEST_PORT, netcon_test_handler) < 0) {
		console_write("M98-DRV-SMOKE: FAIL netconsole-udp no-handler-slot\n");
		return;
	}

	/* Point netconsole at ourselves over 127.0.0.1 for the duration of the
	 * test, then restore whatever the command line asked for. */
	int saved_configured = netcon_configured;
	struct ipv4_addr saved_dst = netcon_dst;
	u16 saved_port = netcon_dst_port;
	usize saved_cursor = netcon_cursor;

	struct ipv4_addr loop;
	loop.bytes[0] = 127;
	loop.bytes[1] = 0;
	loop.bytes[2] = 0;
	loop.bytes[3] = 1;
	netcon_dst = loop;
	netcon_dst_port = NETCON_TEST_PORT;
	netcon_cursor = klog_cursor_now();
	netcon_configured = 1;
	netcon_test_hit = 0;

	/* console_write feeds klog_putc, so this line lands in the ring exactly
	 * like any other kernel message. */
	console_write(netcon_magic);
	console_write("\n");

	int packets = netconsole_flush();
	/* udp_send drains the loopback queue itself for UDP, but poll once more in
	 * case the datagram was still queued behind other traffic. */
	for (int i = 0; i < 8 && !netcon_test_hit; i++)
		net_loopback_drain();

	netcon_configured = saved_configured;
	netcon_dst = saved_dst;
	netcon_dst_port = saved_port;
	netcon_cursor = saved_cursor;

	if (netcon_test_hit && packets > 0) {
		console_write("M98-DRV-SMOKE: ok netconsole-udp packets=");
		console_write_dec((u64)packets);
		console_write("\n");
	} else {
		console_write("M98-DRV-SMOKE: FAIL netconsole-udp hit=");
		console_write_dec((u64)netcon_test_hit);
		console_write(" packets=");
		console_write_dec((u64)packets);
		console_write("\n");
	}
}
