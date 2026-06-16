#include <b1nix/net.h>
#include <b1nix/console.h>
#include <b1nix/bootinfo.h>
#include <string.h>

#define ARP_HW_ETHERNET 1
#define ARP_PROTO_IPV4 0x0800
#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY 2

struct arp_packet {
	u16 hw_type;
	u16 proto_type;
	u8 hw_len;
	u8 proto_len;
	u16 op;
	struct mac_addr sender_mac;
	struct ipv4_addr sender_ip;
	struct mac_addr target_mac;
	struct ipv4_addr target_ip;
} __attribute__((packed));

static u16 bswap16(u16 value)
{
	return (u16)((value << 8) | (value >> 8));
}

#define ARP_TABLE_SIZE 64
struct arp_entry {
	struct ipv4_addr ip;
	struct mac_addr mac;
	int valid;
};
static struct arp_entry arp_table[ARP_TABLE_SIZE];
static int arp_smoke_resolution_logged;
static int arp_smoke_request_logged;
static int arp_smoke_reply_logged;

static void arp_smoke_mark_resolution(void)
{
	if (bootinfo_has_flag("b1nix.test=1") && !arp_smoke_resolution_logged) {
		arp_smoke_resolution_logged = 1;
		console_write("\nARP-SMOKE: resolution-ready\n");
	}
}

void arp_init(void)
{
	arp_smoke_resolution_logged = 0;
	arp_smoke_request_logged = 0;
	arp_smoke_reply_logged = 0;
	for (int i = 0; i < ARP_TABLE_SIZE; i++) {
		arp_table[i].valid = 0;
	}
}

static void arp_cache_put(struct ipv4_addr ip, struct mac_addr mac)
{
	for (int i = 0; i < ARP_TABLE_SIZE; i++) {
		if (arp_table[i].valid && memcmp(arp_table[i].ip.bytes, ip.bytes, 4) == 0) {
			arp_table[i].mac = mac;
			return;
		}
	}
	for (int i = 0; i < ARP_TABLE_SIZE; i++) {
		if (!arp_table[i].valid) {
			arp_table[i].ip = ip;
			arp_table[i].mac = mac;
			arp_table[i].valid = 1;
			return;
		}
	}
}

int arp_resolve(struct ipv4_addr ip, struct mac_addr *mac)
{
	int found = 0;
	for (int i = 0; i < ARP_TABLE_SIZE; i++) {
		if (arp_table[i].valid && memcmp(arp_table[i].ip.bytes, ip.bytes, 4) == 0) {
			*mac = arp_table[i].mac;
			found = 1;
			break;
		}
	}

	int smoke_probe = bootinfo_has_flag("b1nix.test=1") &&
	                  !arp_smoke_request_logged;
	if (!found || smoke_probe) {
		// Send ARP request
		struct arp_packet req;
		req.hw_type = bswap16(ARP_HW_ETHERNET);
		req.proto_type = bswap16(ARP_PROTO_IPV4);
		req.hw_len = 6;
		req.proto_len = 4;
		req.op = bswap16(ARP_OP_REQUEST);
		req.sender_mac = net_get_mac();
		req.sender_ip = net_get_ip();
		memset(req.target_mac.bytes, 0, 6);
		req.target_ip = ip;

		struct mac_addr bcast = { { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF } };
		net_send_ethernet(bcast, 0x0806, &req, sizeof(req));
		
			if (bootinfo_has_flag("b1nix.test=1") && !arp_smoke_request_logged) {
			console_write("\nARP-SMOKE: request-sent\n");
			arp_smoke_request_logged = 1;
		}
	}

	if (found) {
		arp_smoke_mark_resolution();
		return 1;
	}

	// We don't block here. Returning 0 means not found yet. The upper layer should retry later.
	return 0;
}

void arp_receive(const void *data, usize size)
{
	if (size < sizeof(struct arp_packet)) return;
	const struct arp_packet *pkt = data;

	if (bswap16(pkt->hw_type) != ARP_HW_ETHERNET || bswap16(pkt->proto_type) != ARP_PROTO_IPV4) return;

	arp_cache_put(pkt->sender_ip, pkt->sender_mac);
	arp_smoke_mark_resolution();
	
	if (bswap16(pkt->op) == ARP_OP_REPLY) {
		if (bootinfo_has_flag("b1nix.test=1") && !arp_smoke_reply_logged) {
			console_write("\nARP-SMOKE: reply-received\n");
			arp_smoke_reply_logged = 1;
		}
	}

	if (bswap16(pkt->op) == ARP_OP_REQUEST) {
		struct ipv4_addr my_ip = net_get_ip();
		if (memcmp(pkt->target_ip.bytes, my_ip.bytes, 4) == 0 && my_ip.bytes[0] != 0) {
			struct arp_packet reply;
			reply.hw_type = bswap16(ARP_HW_ETHERNET);
			reply.proto_type = bswap16(ARP_PROTO_IPV4);
			reply.hw_len = 6;
			reply.proto_len = 4;
			reply.op = bswap16(ARP_OP_REPLY);
			reply.sender_mac = net_get_mac();
			reply.sender_ip = my_ip;
			reply.target_mac = pkt->sender_mac;
			reply.target_ip = pkt->sender_ip;

			net_send_ethernet(pkt->sender_mac, 0x0806, &reply, sizeof(reply));
		}
	}
}
