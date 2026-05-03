#include <string.h>
#include <b1nix/console.h>
#include <b1nix/net.h>
#include <b1nix/types.h>

#define ETHERTYPE_ARP 0x0806
#define ETHERTYPE_IPV4 0x0800
#define IP_PROTO_ICMP 1
#define IP_PROTO_UDP 17

struct mac_addr {
	u8 bytes[6];
};

struct ipv4_addr {
	u8 bytes[4];
};

struct ethernet_frame {
	struct mac_addr dst;
	struct mac_addr src;
	u16 ethertype;
	const u8 *payload;
	usize payload_size;
};

static struct mac_addr local_mac = { { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 } };
static struct ipv4_addr local_ip = { { 10, 0, 2, 15 } };
static struct ipv4_addr gateway_ip = { { 10, 0, 2, 2 } };

static u16 bswap16(u16 value)
{
	return (u16)((value << 8) | (value >> 8));
}

static u16 ipv4_checksum(const u8 *data, usize size)
{
	u32 sum = 0;

	for (usize i = 0; i + 1 < size; i += 2) {
		sum += ((u16)data[i] << 8) | data[i + 1];
	}

	if ((size & 1) != 0) {
		sum += (u16)data[size - 1] << 8;
	}

	while ((sum >> 16) != 0) {
		sum = (sum & 0xffff) + (sum >> 16);
	}

	return (u16)~sum;
}

static void print_ipv4(struct ipv4_addr ip)
{
	for (usize i = 0; i < 4; i++) {
		u8 value = ip.bytes[i];
		if (value >= 100) {
			console_putc((char)('0' + value / 100));
		}
		if (value >= 10) {
			console_putc((char)('0' + (value / 10) % 10));
		}
		console_putc((char)('0' + value % 10));
		if (i != 3) {
			console_write(".");
		}
	}
}

static void virtio_net_probe(void)
{
	console_write("virtio-net: pci/virtqueue layer pending, using loopback demo device\n");
}

static void ethernet_parse_demo(void)
{
	struct ethernet_frame frame = {
		.dst = local_mac,
		.src = local_mac,
		.ethertype = bswap16(ETHERTYPE_IPV4),
		.payload = 0,
		.payload_size = 0,
	};

	console_write("net: ethernet frame ethertype 0x");
	console_write_hex64(bswap16(frame.ethertype));
	console_write("\n");
}

static void arp_demo(void)
{
	console_write("net: arp learned gateway ");
	print_ipv4(gateway_ip);
	console_write(" -> 52:54:00:12:34:02\n");
}

static void ipv4_demo(void)
{
	u8 header[20];
	memset(header, 0, sizeof(header));
	header[0] = 0x45;
	header[2] = 0;
	header[3] = sizeof(header);
	header[8] = 64;
	header[9] = IP_PROTO_ICMP;
	header[12] = local_ip.bytes[0];
	header[13] = local_ip.bytes[1];
	header[14] = local_ip.bytes[2];
	header[15] = local_ip.bytes[3];
	header[16] = gateway_ip.bytes[0];
	header[17] = gateway_ip.bytes[1];
	header[18] = gateway_ip.bytes[2];
	header[19] = gateway_ip.bytes[3];

	u16 checksum = ipv4_checksum(header, sizeof(header));
	console_write("net: ipv4 checksum 0x");
	console_write_hex64(checksum);
	console_write("\n");
}

static void icmp_demo(void)
{
	console_write("net: icmp echo request -> reply locally synthesized\n");
}

static void udp_demo(void)
{
	const char *payload = "b1nix udp";
	console_write("net: udp send ");
	console_write_hex64(strlen(payload));
	console_write(" bytes to ");
	print_ipv4(gateway_ip);
	console_write(":68\n");
}

static void dhcp_demo(void)
{
	console_write("net: dhcp discover -> offer ");
	print_ipv4(local_ip);
	console_write(" (demo lease)\n");
}

void net_init(void)
{
	virtio_net_probe();
	console_write("net: stack initialized with demo ip ");
	print_ipv4(local_ip);
	console_write("\n");
}

void net_demo(void)
{
	ethernet_parse_demo();
	arp_demo();
	ipv4_demo();
	icmp_demo();
	udp_demo();
	dhcp_demo();
}
