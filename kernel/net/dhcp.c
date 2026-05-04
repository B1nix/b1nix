#include <b1nix/net.h>
#include <b1nix/console.h>
#include <string.h>

#define DHCP_OP_REQUEST 1
#define DHCP_OP_REPLY   2

#define DHCP_OPT_MSG_TYPE 53
#define DHCP_OPT_SUBNET_MASK 1
#define DHCP_OPT_ROUTER 3
#define DHCP_OPT_DNS 6
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
static int dhcp_state = 0; // 0 = init, 1 = offered

void dhcp_init(void)
{
	struct dhcp_packet pkt;
	memset(&pkt, 0, sizeof(pkt));

	pkt.op = DHCP_OP_REQUEST;
	pkt.htype = 1; // Ethernet
	pkt.hlen = 6;
	pkt.xid = current_xid;
	pkt.magic_cookie = 0x63538263; // 99.130.83.99 network byte order
	
	struct mac_addr mac = net_get_mac();
	memcpy(pkt.chaddr, mac.bytes, 6);

	pkt.options[0] = DHCP_OPT_MSG_TYPE;
	pkt.options[1] = 1; // len
	pkt.options[2] = 1; // DHCP Discover
	pkt.options[3] = DHCP_OPT_END;

	struct ipv4_addr bcast = { { 255, 255, 255, 255 } };
	udp_send(bcast, 68, 67, &pkt, sizeof(pkt));

	console_write("net: dhcp discover sent\n");
}

void dhcp_receive(const void *data, usize size)
{
	if (size < sizeof(struct dhcp_packet) - 312) return;
	const struct dhcp_packet *pkt = data;

	if (pkt->op != DHCP_OP_REPLY) return;
	if (pkt->xid != current_xid) return;

	u8 msg_type = 0;
	
	const u8 *opt = pkt->options;
	while (opt < (const u8 *)data + size && *opt != DHCP_OPT_END) {
		u8 type = *opt++;
		if (type == 0) continue; // Pad
		u8 len = *opt++;
		if (type == DHCP_OPT_MSG_TYPE && len == 1) {
			msg_type = *opt;
		} else if (type == DHCP_OPT_ROUTER && len >= 4) {
			struct ipv4_addr gw;
			memcpy(gw.bytes, opt, 4);
			net_set_gateway(gw);
		}
		opt += len;
	}

	if (msg_type == 2 && dhcp_state == 0) { // Offer
		// Send Request
		dhcp_state = 1;
		struct dhcp_packet req;
		memset(&req, 0, sizeof(req));
		req.op = DHCP_OP_REQUEST;
		req.htype = 1;
		req.hlen = 6;
		req.xid = current_xid;
		req.magic_cookie = 0x63538263;
		
		struct mac_addr mac = net_get_mac();
		memcpy(req.chaddr, mac.bytes, 6);

		req.options[0] = DHCP_OPT_MSG_TYPE;
		req.options[1] = 1;
		req.options[2] = 3; // DHCP Request
		
		// Request IP
		req.options[3] = 50; // Requested IP Option
		req.options[4] = 4;
		memcpy(&req.options[5], pkt->yiaddr.bytes, 4);

		req.options[9] = DHCP_OPT_END;

		struct ipv4_addr bcast = { { 255, 255, 255, 255 } };
		udp_send(bcast, 68, 67, &req, sizeof(req));

	} else if (msg_type == 5 && dhcp_state == 1) { // Ack
		net_set_ip(pkt->yiaddr);
		dhcp_state = 2; // Bound
		
		console_write("net: dhcp bound to ");
		for (int i = 0; i < 4; i++) {
			console_write_dec(pkt->yiaddr.bytes[i]);
			if (i < 3) console_write(".");
		}
		console_write("\n");
	}
}
