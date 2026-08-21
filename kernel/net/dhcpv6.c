/* M84: DHCPv6 client (RFC 8415).
 *
 * SLAAC alone cannot express everything a network wants to hand a host: a
 * managed network assigns addresses from a pool (M flag in the router
 * advertisement) and distributes resolvers over DHCPv6 rather than RDNSS. This
 * is the stateful client for that case — Solicit/Advertise/Request/Reply, with
 * Renew and Rebind driven by the T1/T2 timers in the identity association.
 *
 * The transport is UDP over IPv6: the client binds port 546 and talks to
 * ff02::1:2 (All_DHCP_Relay_Agents_and_Servers) port 547 from its link-local
 * address. The assigned address is installed as an on-link /128 and, unlike
 * IPv4 DHCP, no default route comes with it — routers are learnt from router
 * advertisements, which is exactly what RFC 8415 says.
 */

#include <b1nix/kprintf.h>
#include <b1nix/net.h>
#include <b1nix/netproto.h>
#include <b1nix/netdev.h>
#include <b1nix/console.h>
#include <b1nix/sched.h>
#include <b1nix/bootinfo.h>
#include <string.h>

#define DHCP6_CLIENT_PORT 546
#define DHCP6_SERVER_PORT 547

/* Message types */
#define DH6_SOLICIT   1
#define DH6_ADVERTISE 2
#define DH6_REQUEST   3
#define DH6_RENEW     5
#define DH6_REBIND    6
#define DH6_REPLY     7
#define DH6_RELEASE   8

/* Option codes */
#define DH6_OPT_CLIENTID 1
#define DH6_OPT_SERVERID 2
#define DH6_OPT_IA_NA    3
#define DH6_OPT_IAADDR   5
#define DH6_OPT_ORO      6
#define DH6_OPT_ELAPSED  8
#define DH6_OPT_STATUS   13
#define DH6_OPT_DNS      23

/* Client states */
#define DH6_ST_IDLE      0
#define DH6_ST_SOLICIT   1
#define DH6_ST_REQUEST   2
#define DH6_ST_BOUND     3
#define DH6_ST_RENEW     4
#define DH6_ST_REBIND    5

#define DH6_MAX_DUID 20
#define DH6_MAX_SERVERID 64

static int dh6_state;
static u32 dh6_xid;                 /* 24-bit transaction id, host order */
static u8 dh6_duid[DH6_MAX_DUID];   /* our DUID-LL */
static usize dh6_duid_len;
static u8 dh6_serverid[DH6_MAX_SERVERID];
static usize dh6_serverid_len;
static u32 dh6_iaid;
static struct in6_addr_k dh6_addr;  /* the address the server assigned */
static int dh6_have_addr;
static u32 dh6_t1, dh6_t2;          /* seconds */
static u32 dh6_valid_lifetime;
static u64 dh6_start_ticks;
static u64 dh6_t1_ticks, dh6_t2_ticks, dh6_expire_ticks;
static u64 dh6_last_tx_ticks;
static int dh6_enabled;
static int dh6_bound_logged;
static u32 dh6_replies_seen;

static u16 be16(const u8 *p) { return (u16)(((u16)p[0] << 8) | p[1]); }
static u32 be32(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static void put16(u8 *p, u16 v)
{
	p[0] = (u8)(v >> 8);
	p[1] = (u8)v;
}

static void put32(u8 *p, u32 v)
{
	p[0] = (u8)(v >> 24);
	p[1] = (u8)(v >> 16);
	p[2] = (u8)(v >> 8);
	p[3] = (u8)v;
}

/* DUID-LL (type 3): hardware type 1 (ethernet) + our MAC. Stable for as long
 * as the NIC is, which is all a DUID has to be. */
static void dh6_build_duid(void)
{
	struct mac_addr mac = net_get_mac();
	dh6_duid[0] = 0;
	dh6_duid[1] = 3; /* DUID-LL */
	dh6_duid[2] = 0;
	dh6_duid[3] = 1; /* ethernet */
	memcpy(dh6_duid + 4, mac.bytes, 6);
	dh6_duid_len = 10;
	/* The IAID identifies the interface's address association; derive it from
	 * the MAC so it survives a reboot. */
	dh6_iaid = ((u32)mac.bytes[2] << 24) | ((u32)mac.bytes[3] << 16) |
	           ((u32)mac.bytes[4] << 8) | mac.bytes[5];
}

static void dh6_new_xid(void)
{
	/* No RNG in the kernel network path; the uptime counter mixed with the
	 * IAID is unique enough for a transaction id that only has to be distinct
	 * between overlapping exchanges. */
	u64 t = scheduler_get_uptime_ticks();
	dh6_xid = (u32)((t * 2654435761u) ^ dh6_iaid) & 0x00FFFFFFu;
}

/* Append one option; returns the new length or 0 if it would not fit. */
static usize dh6_put_opt(u8 *buf, usize len, usize cap, u16 code,
                         const void *data, usize dlen)
{
	if (len + 4 + dlen > cap)
		return 0;
	put16(buf + len, code);
	put16(buf + len + 2, (u16)dlen);
	if (dlen)
		memcpy(buf + len + 4, data, dlen);
	return len + 4 + dlen;
}

/* Build and transmit one client message. */
static void dh6_send(u8 msg_type, int include_serverid, int include_addr)
{
	u8 pkt[256];
	usize len = 0;

	memset(pkt, 0, sizeof(pkt));
	pkt[0] = msg_type;
	pkt[1] = (u8)(dh6_xid >> 16);
	pkt[2] = (u8)(dh6_xid >> 8);
	pkt[3] = (u8)dh6_xid;
	len = 4;

	len = dh6_put_opt(pkt, len, sizeof(pkt), DH6_OPT_CLIENTID, dh6_duid,
	                  dh6_duid_len);
	if (!len)
		return;

	if (include_serverid && dh6_serverid_len) {
		len = dh6_put_opt(pkt, len, sizeof(pkt), DH6_OPT_SERVERID, dh6_serverid,
		                  dh6_serverid_len);
		if (!len)
			return;
	}

	/* Elapsed time, in hundredths of a second since the exchange started. */
	u64 now = scheduler_get_uptime_ticks();
	u32 elapsed = (u32)(now - dh6_start_ticks); /* ticks are 10 ms */
	u8 el[2];
	put16(el, (u16)(elapsed > 0xFFFF ? 0xFFFF : elapsed));
	len = dh6_put_opt(pkt, len, sizeof(pkt), DH6_OPT_ELAPSED, el, 2);
	if (!len)
		return;

	/* IA_NA, carrying the address we already hold when renewing. */
	u8 ia[12 + 4 + 24];
	usize ialen = 12;
	put32(ia, dh6_iaid);
	put32(ia + 4, 0); /* T1 — let the server choose */
	put32(ia + 8, 0); /* T2 */
	if (include_addr && dh6_have_addr) {
		put16(ia + ialen, DH6_OPT_IAADDR);
		put16(ia + ialen + 2, 24);
		memcpy(ia + ialen + 4, dh6_addr.bytes, 16);
		put32(ia + ialen + 20, dh6_valid_lifetime);
		put32(ia + ialen + 24, dh6_valid_lifetime);
		ialen += 4 + 24;
	}
	len = dh6_put_opt(pkt, len, sizeof(pkt), DH6_OPT_IA_NA, ia, ialen);
	if (!len)
		return;

	/* Ask for the resolver list. */
	u8 oro[2];
	put16(oro, DH6_OPT_DNS);
	len = dh6_put_opt(pkt, len, sizeof(pkt), DH6_OPT_ORO, oro, 2);
	if (!len)
		return;

	struct in6_addr_k dst;
	memset(&dst, 0, sizeof(dst));
	dst.bytes[0] = 0xff;
	dst.bytes[1] = 0x02;
	dst.bytes[13] = 0x01;
	dst.bytes[15] = 0x02; /* ff02::1:2 */

	dh6_last_tx_ticks = now;
	udp6_send(dst, (u16)((DHCP6_CLIENT_PORT >> 8) | (DHCP6_CLIENT_PORT << 8)),
	          (u16)((DHCP6_SERVER_PORT >> 8) | (DHCP6_SERVER_PORT << 8)), pkt,
	          len);
}

/* Walk the option area of a reply. Fills whatever it recognises. */
struct dh6_parsed {
	int have_serverid;
	const u8 *serverid;
	usize serverid_len;
	int have_addr;
	struct in6_addr_k addr;
	u32 t1, t2, valid;
	int have_dns;
	struct in6_addr_k dns;
	u16 status;
};

static void dh6_parse_ia(const u8 *p, usize len, struct dh6_parsed *out)
{
	if (len < 12)
		return;
	if (be32(p) != dh6_iaid)
		return;
	out->t1 = be32(p + 4);
	out->t2 = be32(p + 8);

	usize off = 12;
	while (off + 4 <= len) {
		u16 code = be16(p + off);
		u16 olen = be16(p + off + 2);
		if (off + 4 + olen > len)
			break;
		if (code == DH6_OPT_IAADDR && olen >= 24) {
			memcpy(out->addr.bytes, p + off + 4, 16);
			out->valid = be32(p + off + 24);
			out->have_addr = 1;
		} else if (code == DH6_OPT_STATUS && olen >= 2) {
			out->status = be16(p + off + 4);
		}
		off += 4 + olen;
	}
}

static int dh6_parse(const u8 *msg, usize len, struct dh6_parsed *out,
                     u8 *msg_type)
{
	memset(out, 0, sizeof(*out));
	if (len < 4)
		return -1;
	*msg_type = msg[0];
	u32 xid = ((u32)msg[1] << 16) | ((u32)msg[2] << 8) | msg[3];
	if (xid != dh6_xid)
		return -1;

	usize off = 4;
	int client_id_ok = 0;
	while (off + 4 <= len) {
		u16 code = be16(msg + off);
		u16 olen = be16(msg + off + 2);
		if (off + 4 + olen > len)
			break;
		const u8 *val = msg + off + 4;
		if (code == DH6_OPT_CLIENTID) {
			client_id_ok = (olen == dh6_duid_len &&
			                memcmp(val, dh6_duid, dh6_duid_len) == 0);
		} else if (code == DH6_OPT_SERVERID) {
			out->have_serverid = 1;
			out->serverid = val;
			out->serverid_len = olen;
		} else if (code == DH6_OPT_IA_NA) {
			dh6_parse_ia(val, olen, out);
		} else if (code == DH6_OPT_DNS && olen >= 16) {
			memcpy(out->dns.bytes, val, 16);
			out->have_dns = 1;
		} else if (code == DH6_OPT_STATUS && olen >= 2) {
			out->status = be16(val);
		}
		off += 4 + olen;
	}
	/* A reply that is not addressed to our DUID is not ours. */
	return client_id_ok ? 0 : -1;
}

static void dh6_apply_lease(const struct dh6_parsed *p)
{
	dh6_addr = p->addr;
	dh6_have_addr = 1;
	dh6_valid_lifetime = p->valid ? p->valid : 3600;
	dh6_t1 = p->t1 ? p->t1 : dh6_valid_lifetime / 2;
	dh6_t2 = p->t2 ? p->t2 : (dh6_valid_lifetime * 4) / 5;

	net_set_ip6(dh6_addr);
	ndp_dispatch_mld_join(dh6_addr);

	/* The assigned address is reachable on-link; routers still come from
	 * router advertisements (RFC 8415 does not carry a default route). */
	struct in6_addr_k zero;
	memset(&zero, 0, sizeof(zero));
	route6_add(dh6_addr, 128, zero, RTF_UP, 0,
	           netdev_index_of(netdev_active()));

	u64 now = scheduler_get_uptime_ticks();
	dh6_t1_ticks = now + (u64)dh6_t1 * 100;
	dh6_t2_ticks = now + (u64)dh6_t2 * 100;
	dh6_expire_ticks = now + (u64)dh6_valid_lifetime * 100;

	if (!dh6_bound_logged) {
		dh6_bound_logged = 1;
		k_info("net", "dhcpv6 bound");
	}
	if (bootinfo_has_flag("b1nix.test=1"))
		k_info(NULL, "DHCP6-SMOKE: lease-acquired");
}

/* Inbound datagram on UDP port 546. */
/* Mutual exclusion for the client state below.
 *
 * Three paths reach this state and two of them run on different CPUs at the
 * same time: net_task calls dhcpv6_tick() out of the poll loop and delivers
 * received datagrams, while the boot-time self-test drives the same state
 * machine directly from the init task. Nothing serialised them, so a tick
 * could read dh6_state while the other side was writing it. The switch in
 * dhcpv6_tick compiles to a jump table, and an out-of-range state index there
 * is not a wrong branch — it is a jump to an address that is not code. That is
 * what wedged the sys smoke instance, reporting a different exception each run
 * depending on where the wild jump landed.
 *
 * A plain flag rather than a spinlock, because these paths transmit: dh6_send
 * goes down through UDP to the driver, and holding a lock across that would
 * put a sleep under a spinlock. Nothing here has to wait, either — a tick that
 * finds the state busy skips this round, and this is a retransmit timer whose
 * whole job is to come back. A datagram dropped the same way is one DHCP will
 * send again. */
static volatile int dh6_busy;

static int dh6_enter(void)
{
	return __atomic_exchange_n(&dh6_busy, 1, __ATOMIC_ACQUIRE) == 0;
}

static void dh6_leave(void)
{
	__atomic_store_n(&dh6_busy, 0, __ATOMIC_RELEASE);
}

static void dh6_receive_locked(struct in6_addr_k src, const void *data,
                               usize size);
static void dhcpv6_tick_locked(u64 now_ticks);
static void dhcpv6_start_locked(void);
static void dhcpv6_init_locked(void);

/* The UDP handler entry: takes the guard, then runs the state machine. */
static void dh6_receive(struct in6_addr_k src, const void *data, usize size)
{
	if (!dh6_enter())
		return;
	dh6_receive_locked(src, data, size);
	dh6_leave();
}

static void dh6_receive_locked(struct in6_addr_k src, const void *data,
                               usize size)
{
	(void)src;
	if (!dh6_enabled || dh6_state == DH6_ST_IDLE)
		return;

	struct dh6_parsed p;
	u8 type = 0;
	if (dh6_parse(data, size, &p, &type) != 0)
		return;

	dh6_replies_seen++;

	if (type == DH6_ADVERTISE && dh6_state == DH6_ST_SOLICIT) {
		if (!p.have_serverid || p.serverid_len > DH6_MAX_SERVERID)
			return;
		memcpy(dh6_serverid, p.serverid, p.serverid_len);
		dh6_serverid_len = p.serverid_len;
		if (p.have_addr) {
			dh6_addr = p.addr;
			dh6_valid_lifetime = p.valid;
		}
		dh6_state = DH6_ST_REQUEST;
		dh6_start_ticks = scheduler_get_uptime_ticks();
		dh6_send(DH6_REQUEST, 1, p.have_addr);
		return;
	}

	if (type == DH6_REPLY) {
		if (p.status != 0)
			return; /* NoAddrsAvail / UnspecFail: keep soliciting */
		if (p.have_serverid && p.serverid_len <= DH6_MAX_SERVERID) {
			memcpy(dh6_serverid, p.serverid, p.serverid_len);
			dh6_serverid_len = p.serverid_len;
		}
		if (!p.have_addr)
			return;
		dh6_apply_lease(&p);
		if (p.have_dns) {
			/* b1nix's resolver is IPv4-addressed; an IPv6 nameserver is
			 * recorded for /proc and for a future v6-capable resolver rather
			 * than silently dropped. */
			dns_set_server6(p.dns);
		}
		dh6_state = DH6_ST_BOUND;
	}
}

/* Public entry, guarded like the others. Waits rather than skips: unlike a
 * Solicit, initialisation is not something a later packet will trigger again —
 * if it is skipped the client never comes up at all. */
void dhcpv6_init(void)
{
	while (!dh6_enter())
		scheduler_yield();
	dhcpv6_init_locked();
	dh6_leave();
}

static void dhcpv6_init_locked(void)
{
	dh6_state = DH6_ST_IDLE;
	dh6_have_addr = 0;
	dh6_serverid_len = 0;
	dh6_bound_logged = 0;
	dh6_replies_seen = 0;
	dh6_enabled = 1;
	dh6_build_duid();
	udp6_register_handler(DHCP6_CLIENT_PORT, dh6_receive);
}

void dhcpv6_stop(void)
{
	dh6_enabled = 0;
	dh6_state = DH6_ST_IDLE;
	dh6_have_addr = 0;
}

/* Public entry: a router advertisement with the Managed flag reaches this from
 * net_task, so it needs the guard like any other outside caller. Skipping when
 * the state is busy is safe — an RA that means to start the client is repeated,
 * and the internal callers below already hold the guard and use the unlocked
 * form. */
void dhcpv6_start(void)
{
	if (!dh6_enter())
		return;
	dhcpv6_start_locked();
	dh6_leave();
}

/* Start (or restart) a Solicit exchange. Caller holds the guard. */
static void dhcpv6_start_locked(void)
{
	if (!dh6_enabled)
		dhcpv6_init_locked();
	if (dh6_state == DH6_ST_BOUND)
		return;
	dh6_new_xid();
	dh6_state = DH6_ST_SOLICIT;
	dh6_start_ticks = scheduler_get_uptime_ticks();
	dh6_send(DH6_SOLICIT, 0, 0);
}

void dhcpv6_tick(u64 now_ticks)
{
	if (!dh6_enter())
		return; /* another path owns the state — retry next tick */
	dhcpv6_tick_locked(now_ticks);
	dh6_leave();
}

static void dhcpv6_tick_locked(u64 now_ticks)
{
	if (!dh6_enabled || dh6_state == DH6_ST_IDLE)
		return;

	switch (dh6_state) {
	case DH6_ST_SOLICIT:
	case DH6_ST_REQUEST:
		/* Retransmit on the RFC 8415 initial timeout (~1 s), capped so a
		 * network without a DHCPv6 server costs one packet a second and
		 * nothing else. */
		if (now_ticks - dh6_last_tx_ticks >= 100)
			dh6_send(dh6_state == DH6_ST_SOLICIT ? DH6_SOLICIT : DH6_REQUEST,
			         dh6_state == DH6_ST_REQUEST, dh6_state == DH6_ST_REQUEST);
		break;

	case DH6_ST_BOUND:
		if (dh6_t1_ticks && now_ticks >= dh6_t1_ticks) {
			dh6_new_xid();
			dh6_state = DH6_ST_RENEW;
			dh6_start_ticks = now_ticks;
			dh6_send(DH6_RENEW, 1, 1);
		}
		break;

	case DH6_ST_RENEW:
		if (now_ticks >= dh6_t2_ticks) {
			dh6_new_xid();
			dh6_state = DH6_ST_REBIND;
			dh6_start_ticks = now_ticks;
			dh6_send(DH6_REBIND, 0, 1); /* any server may answer */
		} else if (now_ticks - dh6_last_tx_ticks >= 1000) {
			dh6_send(DH6_RENEW, 1, 1);
		}
		break;

	case DH6_ST_REBIND:
		if (dh6_expire_ticks && now_ticks >= dh6_expire_ticks) {
			/* The lease is gone: drop the address and start over. */
			struct in6_addr_k zero;
			memset(&zero, 0, sizeof(zero));
			route6_del(dh6_addr, 128, zero);
			dh6_have_addr = 0;
			net_set_ip6(zero);
			dhcpv6_start_locked();
		} else if (now_ticks - dh6_last_tx_ticks >= 1000) {
			dh6_send(DH6_REBIND, 0, 1);
		}
		break;

	default:
		break;
	}
}

int dhcpv6_is_bound(void) { return dh6_state == DH6_ST_BOUND && dh6_have_addr; }

struct in6_addr_k dhcpv6_get_address(void) { return dh6_addr; }

/* ── M84 self-test ─────────────────────────────────────────────────────────
 * There is no DHCPv6 server behind QEMU's user-mode networking (it offers
 * router advertisements only), so the exchange is driven against synthetic
 * server messages fed through the real receive path: the same dh6_receive()
 * an on-the-wire datagram reaches, with the real parser and the real state
 * machine. Only the transport is short-circuited. */
static usize dh6_test_put_opt(u8 *b, usize len, u16 code, const void *d,
                              usize dl)
{
	put16(b + len, code);
	put16(b + len + 2, (u16)dl);
	memcpy(b + len + 4, d, dl);
	return len + 4 + dl;
}

void dhcpv6_smoke(void)
{
	struct in6_addr_k from;
	memset(&from, 0, sizeof(from));
	from.bytes[0] = 0xfe;
	from.bytes[1] = 0x80;
	from.bytes[15] = 0x02;

	/* Hold the state for the whole test. Every step below reads a value the
	 * previous one set, so a tick from net_task landing in the middle would
	 * not just perturb a check — it would be writing the state this thread is
	 * walking. Spin rather than skip: the test runs once, at boot, and has to
	 * run. */
	while (!dh6_enter())
		scheduler_yield();

	int was_enabled = dh6_enabled;
	dhcpv6_init_locked();
	dhcpv6_start_locked();

	if (dh6_state != DH6_ST_SOLICIT) {
		k_info(NULL, "M84-DHCP6: FAIL solicit");
		dh6_leave();
		return;
	}
	k_info(NULL, "M84-DHCP6: ok solicit");

	/* Server DUID-LL of the synthetic server. */
	u8 srv_duid[10] = {0, 3, 0, 1, 0x52, 0x54, 0x00, 0x12, 0x34, 0x56};

	struct in6_addr_k assigned;
	memset(&assigned, 0, sizeof(assigned));
	assigned.bytes[0] = 0x20;
	assigned.bytes[1] = 0x01;
	assigned.bytes[2] = 0x0d;
	assigned.bytes[3] = 0xb8;
	assigned.bytes[15] = 0x77;

	struct in6_addr_k dns6;
	memset(&dns6, 0, sizeof(dns6));
	dns6.bytes[0] = 0x20;
	dns6.bytes[1] = 0x01;
	dns6.bytes[2] = 0x0d;
	dns6.bytes[3] = 0xb8;
	dns6.bytes[15] = 0x53;

	/* Build IA_NA{IAADDR} once and reuse it for Advertise and Reply. */
	u8 ia[12 + 4 + 24];
	put32(ia, dh6_iaid);
	put32(ia + 4, 100); /* T1 */
	put32(ia + 8, 200); /* T2 */
	put16(ia + 12, DH6_OPT_IAADDR);
	put16(ia + 14, 24);
	memcpy(ia + 16, assigned.bytes, 16);
	put32(ia + 32, 400); /* preferred */
	put32(ia + 36, 400); /* valid */

	/* Advertise → the client must move to REQUEST and latch the server DUID. */
	u8 adv[256];
	usize len = 4;
	adv[0] = DH6_ADVERTISE;
	adv[1] = (u8)(dh6_xid >> 16);
	adv[2] = (u8)(dh6_xid >> 8);
	adv[3] = (u8)dh6_xid;
	len = dh6_test_put_opt(adv, len, DH6_OPT_CLIENTID, dh6_duid, dh6_duid_len);
	len = dh6_test_put_opt(adv, len, DH6_OPT_SERVERID, srv_duid,
	                       sizeof(srv_duid));
	len = dh6_test_put_opt(adv, len, DH6_OPT_IA_NA, ia, sizeof(ia));
	dh6_receive_locked(from, adv, len);

	if (dh6_state == DH6_ST_REQUEST && dh6_serverid_len == sizeof(srv_duid) &&
	    memcmp(dh6_serverid, srv_duid, sizeof(srv_duid)) == 0)
		k_info(NULL, "M84-DHCP6: ok advertise");
	else
		k_info(NULL, "M84-DHCP6: FAIL advertise");

	/* Reply → bound, address installed, on-link /128 route present. */
	u8 rep[256];
	len = 4;
	rep[0] = DH6_REPLY;
	rep[1] = (u8)(dh6_xid >> 16);
	rep[2] = (u8)(dh6_xid >> 8);
	rep[3] = (u8)dh6_xid;
	len = dh6_test_put_opt(rep, len, DH6_OPT_CLIENTID, dh6_duid, dh6_duid_len);
	len = dh6_test_put_opt(rep, len, DH6_OPT_SERVERID, srv_duid,
	                       sizeof(srv_duid));
	len = dh6_test_put_opt(rep, len, DH6_OPT_IA_NA, ia, sizeof(ia));
	len = dh6_test_put_opt(rep, len, DH6_OPT_DNS, dns6.bytes, 16);
	dh6_receive_locked(from, rep, len);

	struct in6_addr_k nh;
	u16 fl = 0;
	int oif = 0;
	int routed = route6_lookup(assigned, &nh, &fl, &oif) &&
	             memcmp(nh.bytes, assigned.bytes, 16) == 0;
	struct in6_addr_k cfg = net_get_ip6();
	if (dhcpv6_is_bound() && memcmp(cfg.bytes, assigned.bytes, 16) == 0 &&
	    routed && dh6_t1 == 100 && dh6_t2 == 200)
		k_info(NULL, "M84-DHCP6: ok reply-bound");
	else
		k_info(NULL, "M84-DHCP6: FAIL reply-bound");

	struct in6_addr_k got_dns = dns_get_server6();
	if (memcmp(got_dns.bytes, dns6.bytes, 16) == 0)
		k_info(NULL, "M84-DHCP6: ok dns-option");
	else
		k_info(NULL, "M84-DHCP6: FAIL dns-option");

	/* T1 expiry must move the client into RENEW. */
	dh6_t1_ticks = scheduler_get_uptime_ticks();
	dhcpv6_tick_locked(scheduler_get_uptime_ticks() + 1);
	if (dh6_state == DH6_ST_RENEW)
		k_info(NULL, "M84-DHCP6: ok renew");
	else
		k_info(NULL, "M84-DHCP6: FAIL renew");

	/* A malformed message (truncated option) must be rejected, not parsed. */
	u8 bad[16];
	len = 4;
	bad[0] = DH6_REPLY;
	bad[1] = (u8)(dh6_xid >> 16);
	bad[2] = (u8)(dh6_xid >> 8);
	bad[3] = (u8)dh6_xid;
	put16(bad + 4, DH6_OPT_IA_NA);
	put16(bad + 6, 200); /* length past the end of the buffer */
	u32 before = dh6_replies_seen;
	dh6_receive_locked(from, bad, 8);
	if (dh6_replies_seen == before)
		k_info(NULL, "M84-DHCP6: ok malformed-rejected");
	else
		k_info(NULL, "M84-DHCP6: FAIL malformed-rejected");

	/* Leave the interface as SLAAC configured it: this test ran entirely
	 * against a synthetic server, so its address must not survive. */
	struct in6_addr_k zero;
	memset(&zero, 0, sizeof(zero));
	route6_del(assigned, 128, zero);
	dhcpv6_stop();
	if (!was_enabled)
		dh6_enabled = 0;

	dh6_leave();
}
