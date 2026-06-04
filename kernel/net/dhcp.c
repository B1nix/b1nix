#include <b1nix/net.h>
#include <b1nix/console.h>
#include <b1nix/sched.h>
#include <b1nix/bootinfo.h>
#include <string.h>

#define DHCP_OP_REQUEST 1
#define DHCP_OP_REPLY   2

#define DHCP_OPT_MSG_TYPE 53
#define DHCP_OPT_SUBNET_MASK 1
#define DHCP_OPT_ROUTER 3
#define DHCP_OPT_DNS 6
#define DHCP_OPT_REQUESTED_IP 50
#define DHCP_OPT_LEASE_TIME 51
#define DHCP_OPT_SERVER_ID 54
#define DHCP_OPT_PARAM_REQUEST_LIST 55
#define DHCP_OPT_END 255

struct dhcp_packet {
	u8 op;
	u8 htype;
	u8 hlen;
	u8 hops;
	u32 xid;
	u16 secs;
	u16 flags;
	struct ipv4_addr ciaddr;
	struct ipv4_addr yiaddr;
	struct ipv4_addr siaddr;
	struct ipv4_addr giaddr;
	u8 chaddr[16];
	u8 sname[64];
	u8 file[128];
	u32 magic_cookie;
	u8 options[312];
} __attribute__((packed));

static u32 current_xid = 0x11223344;
static int dhcp_state = 0; // 0=init, 1=offered, 2=bound, 3=renewing, 4=rebinding
static struct ipv4_addr dhcp_server = {{0, 0, 0, 0}};
static u32 lease_seconds = 300;
static u64 lease_expire_ticks = 0;
static u64 renew_ticks = 0;
static u64 rebind_ticks = 0;
static u64 last_request_ticks = 0;
static int dhcp_udp_registered = 0;
static int dhcp_fallback_announced = 0;
static struct ipv4_addr offered_gw = {{0, 0, 0, 0}};
static struct ipv4_addr offered_mask = {{0, 0, 0, 0}};
static struct ipv4_addr offered_dns = {{0, 0, 0, 0}};

struct dhcp_reply_diag {
	int valid;
	struct ipv4_addr yiaddr;
	struct ipv4_addr gw;
	struct ipv4_addr mask;
	struct ipv4_addr dns;
	struct ipv4_addr srv;
	u32 xid;
};

static struct dhcp_reply_diag last_offer_diag;
static struct dhcp_reply_diag last_ack_diag;

static u32 be32(const u8 *p) {
  return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}

static int ipv4_is_zero(struct ipv4_addr ip)
{
	return ip.bytes[0] == 0 && ip.bytes[1] == 0 &&
	       ip.bytes[2] == 0 && ip.bytes[3] == 0;
}

static void dhcp_print_ipv4(struct ipv4_addr ip)
{
	console_write_dec(ip.bytes[0]); console_write(".");
	console_write_dec(ip.bytes[1]); console_write(".");
	console_write_dec(ip.bytes[2]); console_write(".");
	console_write_dec(ip.bytes[3]);
}

static void dhcp_save_reply_diag(struct dhcp_reply_diag *diag,
                                 struct ipv4_addr yiaddr,
                                 struct ipv4_addr gw,
                                 struct ipv4_addr mask,
                                 struct ipv4_addr dns,
                                 struct ipv4_addr srv)
{
	diag->valid = 1;
	diag->yiaddr = yiaddr;
	diag->gw = gw;
	diag->mask = mask;
	diag->dns = dns;
	diag->srv = srv;
	diag->xid = current_xid;
}

static void dhcp_dump_saved_reply(const char *kind,
                                  const struct dhcp_reply_diag *diag)
{
	console_write(" dhcp-");
	console_write(kind);
	console_write(": ");
	if (!diag->valid) {
		console_write("none\n");
		return;
	}
	console_write("yiaddr=");
	dhcp_print_ipv4(diag->yiaddr);
	console_write(" gw=");
	dhcp_print_ipv4(diag->gw);
	console_write(" mask=");
	dhcp_print_ipv4(diag->mask);
	console_write(" dns=");
	dhcp_print_ipv4(diag->dns);
	console_write(" srv=");
	dhcp_print_ipv4(diag->srv);
	console_write(" xid=0x");
	console_write_hex32(diag->xid);
	console_write("\n");
}

void dhcp_dump_info(void)
{
	dhcp_dump_saved_reply("offer", &last_offer_diag);
	dhcp_dump_saved_reply("ack", &last_ack_diag);
}

static int dhcp_chaddr_matches_local(const struct dhcp_packet *pkt)
{
	struct mac_addr mac = net_get_mac();
	if (pkt->htype != 1 || pkt->hlen != 6)
		return 0;
	for (int i = 0; i < 6; i++) {
		if (pkt->chaddr[i] != mac.bytes[i])
			return 0;
	}
	return 1;
}

static u32 dhcp_seed_xid_from_mac(void)
{
	struct mac_addr mac = net_get_mac();
	u32 x = 0x11223344u;
	for (int i = 0; i < 6; i++)
		x = (x * 33u) ^ mac.bytes[i];
	x ^= (u32)scheduler_get_uptime_ticks();
	return x ? x : 0x11223344u;
}

static void dhcp_send_request(struct ipv4_addr requested_ip, int broadcast) {
  struct dhcp_packet req;
  memset(&req, 0, sizeof(req));
  req.op = DHCP_OP_REQUEST;
  req.htype = 1;
  req.hlen = 6;
  req.xid = current_xid;
  if (broadcast)
    req.flags = 0x0080;   /* broadcast flag (wire 0x8000) — see dhcp_init */
  req.magic_cookie = 0x63538263;
  struct mac_addr mac = net_get_mac();
  memcpy(req.chaddr, mac.bytes, 6);

  req.options[0] = DHCP_OPT_MSG_TYPE;
  req.options[1] = 1;
  req.options[2] = 3; // DHCP Request
	req.options[3] = DHCP_OPT_REQUESTED_IP;
	req.options[4] = 4;
	memcpy(&req.options[5], requested_ip.bytes, 4);
	req.options[9] = DHCP_OPT_PARAM_REQUEST_LIST;
	req.options[10] = 3;
	req.options[11] = DHCP_OPT_SUBNET_MASK;
	req.options[12] = DHCP_OPT_ROUTER;
	req.options[13] = DHCP_OPT_DNS;
	req.options[14] = DHCP_OPT_END;

  struct ipv4_addr dst = broadcast ? (struct ipv4_addr){{255, 255, 255, 255}} : dhcp_server;
  udp_send(dst, 68, 67, &req, sizeof(req));
  last_request_ticks = scheduler_get_uptime_ticks();
}

void dhcp_init(void)
{
	if (!dhcp_udp_registered) {
		udp_register_handler(68, dhcp_receive);
		dhcp_udp_registered = 1;
	}
	if (current_xid == 0x11223344u)
		current_xid = dhcp_seed_xid_from_mac();
	else
		current_xid++;
	dhcp_state = 0;
	dhcp_fallback_announced = 0;
	offered_gw = (struct ipv4_addr){{0, 0, 0, 0}};
	offered_mask = (struct ipv4_addr){{0, 0, 0, 0}};
	offered_dns = (struct ipv4_addr){{0, 0, 0, 0}};
	struct dhcp_packet pkt;
	memset(&pkt, 0, sizeof(pkt));

	pkt.op = DHCP_OP_REQUEST;
	pkt.htype = 1; // Ethernet
	pkt.hlen = 6;
	pkt.xid = current_xid;
	/* Set the BROADCAST flag (wire 0x8000; little-endian u16 == 0x0080) so the
	 * server broadcasts its OFFER/ACK instead of unicasting it to the not-yet-
	 * assigned IP. We have no IP yet (0.0.0.0), so a unicast reply is dropped by
	 * ipv4_receive — which is exactly why DHCP got no lease on real hardware
	 * (QEMU's SLIRP server happened to reply in a way we accepted). */
	pkt.flags = 0x0080;
	pkt.magic_cookie = 0x63538263; // 99.130.83.99 network byte order
	
	struct mac_addr mac = net_get_mac();
	memcpy(pkt.chaddr, mac.bytes, 6);

	pkt.options[0] = DHCP_OPT_MSG_TYPE;
	pkt.options[1] = 1; // len
	pkt.options[2] = 1; // DHCP Discover
	pkt.options[3] = DHCP_OPT_PARAM_REQUEST_LIST;
	pkt.options[4] = 3;
	pkt.options[5] = DHCP_OPT_SUBNET_MASK;
	pkt.options[6] = DHCP_OPT_ROUTER;
	pkt.options[7] = DHCP_OPT_DNS;
	pkt.options[8] = DHCP_OPT_END;

	struct ipv4_addr bcast = { { 255, 255, 255, 255 } };
	udp_send(bcast, 68, 67, &pkt, sizeof(pkt));
	last_request_ticks = scheduler_get_uptime_ticks();

	/* Announce the first discover only. net_task re-runs dhcp_init() every few
	 * seconds while there is no lease, so printing on every attempt floods the
	 * console (and buries the shell prompt) when no DHCP server answers — e.g.
	 * on a link with no DHCP. Retries continue silently; a successful bind still
	 * prints "dhcp bound to ..." exactly once. */
	static int discover_announced = 0;
	if (!discover_announced) {
		console_write("net: dhcp discover sent (retrying silently until a lease "
		              "or link)\n");
		discover_announced = 1;
	}
}

void dhcp_receive(const void *data, usize size)
{
	if (size < sizeof(struct dhcp_packet) - 312) return;
	const struct dhcp_packet *pkt = data;

	if (pkt->op != DHCP_OP_REPLY) return;
	if (pkt->magic_cookie != 0x63538263) return;
	if (pkt->xid != current_xid) return;
	if (!dhcp_chaddr_matches_local(pkt)) {
		console_write("dhcp: ignoring reply for foreign chaddr\n");
		return;
	}

	u8 msg_type = 0;
	struct ipv4_addr offer_ip = pkt->yiaddr;
	/* Parsed from options — collected for diagnostics */
	struct ipv4_addr parsed_gw   = {{0,0,0,0}};
	struct ipv4_addr parsed_mask = {{0,0,0,0}};
	struct ipv4_addr parsed_srv  = {{0,0,0,0}};
	struct ipv4_addr parsed_dns  = {{0,0,0,0}};

	const u8 *opt = pkt->options;
	while (opt < (const u8 *)data + size && *opt != DHCP_OPT_END) {
		u8 type = *opt++;
		if (type == 0) continue; // Pad
		if (opt >= (const u8 *)data + size) break;
		u8 len = *opt++;
		if ((usize)((const u8 *)data + size - opt) < len) break;
		if (type == DHCP_OPT_MSG_TYPE && len == 1) {
			msg_type = *opt;
		} else if (type == DHCP_OPT_SUBNET_MASK && len >= 4) {
			memcpy(parsed_mask.bytes, opt, 4);
		} else if (type == DHCP_OPT_ROUTER && len >= 4) {
			memcpy(parsed_gw.bytes, opt, 4);
		} else if (type == DHCP_OPT_DNS && len >= 4) {
			memcpy(parsed_dns.bytes, opt, 4);
		} else if (type == DHCP_OPT_SERVER_ID && len >= 4) {
			memcpy(parsed_srv.bytes, opt, 4);
			memcpy(dhcp_server.bytes, opt, 4);
		} else if (type == DHCP_OPT_LEASE_TIME && len >= 4) {
			lease_seconds = be32(opt);
			if (lease_seconds < 60) {
				lease_seconds = 60;
			}
		}
		opt += len;
	}

	if (msg_type == 2 && dhcp_state == 0) { // Offer
		dhcp_save_reply_diag(&last_offer_diag, offer_ip, parsed_gw,
		                     parsed_mask, parsed_dns, parsed_srv);
		offered_gw = parsed_gw;
		offered_mask = parsed_mask;
		offered_dns = parsed_dns;
		dhcp_state = 1;
		dhcp_send_request(offer_ip, 1);

	} else if (msg_type == 5 &&
	           (dhcp_state == 1 || dhcp_state == 3 || dhcp_state == 4)) { // Ack
		if (ipv4_is_zero(parsed_gw))
			parsed_gw = offered_gw;
		if (ipv4_is_zero(parsed_mask))
			parsed_mask = offered_mask;
		if (ipv4_is_zero(parsed_dns))
			parsed_dns = offered_dns;
		dhcp_save_reply_diag(&last_ack_diag, pkt->yiaddr, parsed_gw,
		                     parsed_mask, parsed_dns, parsed_srv);
		net_set_ip(pkt->yiaddr);
		net_set_gateway(parsed_gw);
		if (parsed_dns.bytes[0] || parsed_dns.bytes[1] ||
		    parsed_dns.bytes[2] || parsed_dns.bytes[3])
			dns_set_server(parsed_dns);
		dhcp_state = 2; // Bound
		if (bootinfo_has_flag("b1nix.test=1"))
			console_write("DHCP-SMOKE: lease-acquired\n");
		u64 now = scheduler_get_uptime_ticks();
		lease_expire_ticks = now + (u64)lease_seconds * 100;
		renew_ticks = now + ((u64)lease_seconds * 100) / 2;
		rebind_ticks = now + ((u64)lease_seconds * 100 * 7) / 8;

		console_write("net: dhcp bound to ");
		dhcp_print_ipv4(pkt->yiaddr);
		console_write("\n");
	}
}

void dhcp_tick(u64 now_ticks) {
  if (dhcp_state == 0 && now_ticks - last_request_ticks >= 600 && !dhcp_fallback_announced) {
    struct ipv4_addr fallback_ip = {{10, 0, 2, 15}};
    struct ipv4_addr fallback_gw = {{10, 0, 2, 2}};
    net_set_ip(fallback_ip);
	    net_set_gateway(fallback_gw);
	    dhcp_state = 2;
	    dhcp_fallback_announced = 1;
	    if (bootinfo_has_flag("b1nix.test=1"))
	      console_write("DHCP-SMOKE: fallback-static\n");
	    return;
  }
  if (dhcp_state == 2) {
    if (renew_ticks && now_ticks >= renew_ticks) {
      dhcp_state = 3;
      dhcp_send_request(net_get_ip(), 0);
    }
    return;
  }
  if (dhcp_state == 3) {
    if (rebind_ticks && now_ticks >= rebind_ticks) {
      dhcp_state = 4;
    }
    if (now_ticks - last_request_ticks >= 200) {
      dhcp_send_request(net_get_ip(), 0);
    }
    return;
  }
  if (dhcp_state == 4) {
    if (lease_expire_ticks && now_ticks >= lease_expire_ticks) {
      struct ipv4_addr zero = {{0, 0, 0, 0}};
      net_set_ip(zero);
      net_set_gateway(zero);
      dhcp_init();
      return;
    }
    if (now_ticks - last_request_ticks >= 200) {
      dhcp_send_request(net_get_ip(), 1);
    }
  }
}
