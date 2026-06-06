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
static int dhcp_state = -1; // -1=stopped, 0=discover, 1=request, 2=bound, 3=renew, 4=rebind
static struct ipv4_addr dhcp_server = {{0, 0, 0, 0}};
static u32 lease_seconds = 300;
static u64 lease_expire_ticks = 0;
static u64 renew_ticks = 0;
static u64 rebind_ticks = 0;
static u64 last_request_ticks = 0;
static u64 transaction_start_ticks = 0;
static u32 discover_attempts = 0;
static u32 request_attempts = 0;
static int dhcp_udp_registered = 0;
static int dhcp_fallback_announced = 0;
static struct ipv4_addr offered_gw = {{0, 0, 0, 0}};
static struct ipv4_addr offered_mask = {{0, 0, 0, 0}};
static struct ipv4_addr offered_dns = {{0, 0, 0, 0}};
static struct ipv4_addr offered_ip = {{0, 0, 0, 0}};

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
	static const char *states[] = {"discover", "request", "bound", "renew", "rebind"};
	console_write(" dhcp-state: ");
	if (dhcp_state < 0 || dhcp_state > 4)
		console_write("stopped");
	else
		console_write(states[dhcp_state]);
	console_write(" discovers=");
	console_write_dec(discover_attempts);
	console_write(" requests=");
	console_write_dec(request_attempts);
	console_write("\n");
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

static void dhcp_send_discover(void)
{
	struct dhcp_packet pkt;
	memset(&pkt, 0, sizeof(pkt));
	pkt.op = DHCP_OP_REQUEST;
	pkt.htype = 1;
	pkt.hlen = 6;
	pkt.xid = current_xid;
	pkt.flags = 0x0080; /* wire 0x8000 on little-endian x86 */
	pkt.magic_cookie = 0x63538263;
	struct mac_addr mac = net_get_mac();
	memcpy(pkt.chaddr, mac.bytes, 6);

	pkt.options[0] = DHCP_OPT_MSG_TYPE;
	pkt.options[1] = 1;
	pkt.options[2] = 1;
	pkt.options[3] = DHCP_OPT_PARAM_REQUEST_LIST;
	pkt.options[4] = 3;
	pkt.options[5] = DHCP_OPT_SUBNET_MASK;
	pkt.options[6] = DHCP_OPT_ROUTER;
	pkt.options[7] = DHCP_OPT_DNS;
	pkt.options[8] = DHCP_OPT_END;

	struct ipv4_addr bcast = {{255, 255, 255, 255}};
	udp_send(bcast, 68, 67, &pkt, sizeof(pkt));
	last_request_ticks = scheduler_get_uptime_ticks();
	discover_attempts++;
}

static void dhcp_send_request(struct ipv4_addr requested_ip, int broadcast)
{
	struct dhcp_packet req;
	memset(&req, 0, sizeof(req));
	req.op = DHCP_OP_REQUEST;
	req.htype = 1;
	req.hlen = 6;
	req.xid = current_xid;
	if (broadcast)
		req.flags = 0x0080; /* wire 0x8000 on little-endian x86 */
	req.magic_cookie = 0x63538263;
	struct mac_addr mac = net_get_mac();
	memcpy(req.chaddr, mac.bytes, 6);

	u8 *opt = req.options;
	*opt++ = DHCP_OPT_MSG_TYPE;
	*opt++ = 1;
	*opt++ = 3; /* DHCPREQUEST */

	if (dhcp_state == 1) {
		*opt++ = DHCP_OPT_REQUESTED_IP;
		*opt++ = 4;
		memcpy(opt, requested_ip.bytes, 4);
		opt += 4;
		if (!ipv4_is_zero(dhcp_server)) {
			*opt++ = DHCP_OPT_SERVER_ID;
			*opt++ = 4;
			memcpy(opt, dhcp_server.bytes, 4);
			opt += 4;
		}
	} else {
		/* RENEWING/REBINDING identifies the lease through ciaddr. */
		req.ciaddr = requested_ip;
	}

	*opt++ = DHCP_OPT_PARAM_REQUEST_LIST;
	*opt++ = 3;
	*opt++ = DHCP_OPT_SUBNET_MASK;
	*opt++ = DHCP_OPT_ROUTER;
	*opt++ = DHCP_OPT_DNS;
	*opt = DHCP_OPT_END;

	struct ipv4_addr dst = broadcast
		? (struct ipv4_addr){{255, 255, 255, 255}}
		: dhcp_server;
	udp_send(dst, 68, 67, &req, sizeof(req));
	last_request_ticks = scheduler_get_uptime_ticks();
	request_attempts++;
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
	discover_attempts = 0;
	request_attempts = 0;
	lease_seconds = 300;
	lease_expire_ticks = 0;
	renew_ticks = 0;
	rebind_ticks = 0;
	transaction_start_ticks = scheduler_get_uptime_ticks();
	offered_gw = (struct ipv4_addr){{0, 0, 0, 0}};
	offered_mask = (struct ipv4_addr){{0, 0, 0, 0}};
	offered_dns = (struct ipv4_addr){{0, 0, 0, 0}};
	offered_ip = (struct ipv4_addr){{0, 0, 0, 0}};
	dhcp_server = (struct ipv4_addr){{0, 0, 0, 0}};
	memset(&last_offer_diag, 0, sizeof(last_offer_diag));
	memset(&last_ack_diag, 0, sizeof(last_ack_diag));
	dhcp_send_discover();

	/* Retries are handled by dhcp_tick(), so keep the normal no-server case
	 * quiet while still showing that automatic configuration has started. */
	static int discover_announced = 0;
	if (!discover_announced) {
		console_write("net: dhcp discover sent (retrying silently until a lease "
		              "or link)\n");
		discover_announced = 1;
	}
}

void dhcp_stop(void)
{
	struct ipv4_addr zero = {{0, 0, 0, 0}};
	dhcp_state = -1;
	net_set_ip(zero);
	net_set_gateway(zero);
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
		offered_ip = offer_ip;
		dhcp_state = 1;
		transaction_start_ticks = scheduler_get_uptime_ticks();
		dhcp_send_request(offer_ip, 1);

	} else if (msg_type == 5 &&
	           (dhcp_state == 1 || dhcp_state == 3 || dhcp_state == 4)) { // Ack
		struct ipv4_addr ack_ip = pkt->yiaddr;
		if (ipv4_is_zero(ack_ip))
			ack_ip = net_get_ip();
		if (ipv4_is_zero(parsed_gw))
			parsed_gw = offered_gw;
		if (ipv4_is_zero(parsed_mask))
			parsed_mask = offered_mask;
		if (ipv4_is_zero(parsed_dns))
			parsed_dns = offered_dns;
		dhcp_save_reply_diag(&last_ack_diag, ack_ip, parsed_gw,
		                     parsed_mask, parsed_dns, parsed_srv);
		net_set_ip(ack_ip);
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
		dhcp_print_ipv4(ack_ip);
		console_write("\n");
	} else if (msg_type == 6) { // NAK
		console_write("net: dhcp NAK, restarting discovery\n");
		dhcp_init();
	}
}

void dhcp_tick(u64 now_ticks) {
  if (dhcp_state < 0)
    return;
  if (dhcp_state == 0 && bootinfo_has_flag("b1nix.test=1") &&
      now_ticks - transaction_start_ticks >= 600 && !dhcp_fallback_announced) {
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
  if (dhcp_state == 0) {
    if (now_ticks - transaction_start_ticks >= 3000) {
      console_write("net: dhcp transaction timed out, restarting\n");
      dhcp_init();
    } else if (now_ticks - last_request_ticks >= 300) {
      dhcp_send_discover();
    }
    return;
  }
  if (dhcp_state == 1) {
    if (now_ticks - transaction_start_ticks >= 3000) {
      console_write("net: dhcp request timed out, restarting\n");
      dhcp_init();
    } else if (now_ticks - last_request_ticks >= 300) {
      dhcp_send_request(offered_ip, 1);
    }
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
