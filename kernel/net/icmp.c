#include <b1nix/mm.h>
#include <b1nix/net.h>
#include <b1nix/console.h>
#include <b1nix/bootinfo.h>
#include <string.h>

#define ICMP_TYPE_ECHO_REPLY 0
#define ICMP_TYPE_ECHO_REQUEST 8
#define ICMP_TYPE_DEST_UNREACH 3

struct icmp_header {
  u8 type;
  u8 code;
  u16 checksum;
  u16 id;
  u16 seq;
} __attribute__((packed));

static u16 bswap16(u16 value) { return (u16)((value << 8) | (value >> 8)); }
static volatile u32 g_icmp_echo_replies = 0;

static u16 icmp_checksum(const u8 *data, usize size) {
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

void icmp_receive(struct ipv4_addr src, const void *data, usize size) {
  if (size < sizeof(struct icmp_header))
    return;
  /* Hand a copy to any SOCK_RAW/ICMP sockets (BusyBox ping reads echo replies
   * here); the built-in echo responder below still runs for echo requests. */
  vfs_socket_push_raw_icmp(src, data, size);
  const struct icmp_header *hdr = data;

	if (hdr->type == ICMP_TYPE_ECHO_REQUEST) {
		u8 *reply = kzalloc(size);
		if (!reply)
			return;

		memcpy(reply, data, size);
		struct icmp_header *rhdr = (struct icmp_header *)reply;
		rhdr->type = ICMP_TYPE_ECHO_REPLY;
		rhdr->code = 0;
		rhdr->checksum = 0;

		u16 csum = icmp_checksum(reply, size);
		rhdr->checksum = bswap16(csum);

		ipv4_send(src, 1 /* ICMP */, reply, size);
		kfree(reply);
	} else if (hdr->type == ICMP_TYPE_ECHO_REPLY) {
		__atomic_add_fetch(&g_icmp_echo_replies, 1, __ATOMIC_RELAXED);
		console_write("ping: reply from ");
		console_write_dec(src.bytes[0]);
		console_write(".");
		console_write_dec(src.bytes[1]);
		console_write(".");
		console_write_dec(src.bytes[2]);
		console_write(".");
		console_write_dec(src.bytes[3]);
		console_write(" bytes=");
		console_write_dec(size);
		console_write(" seq=");
		console_write_dec(hdr->seq); // Wait, seq might be swapped? Standard ping uses network byte order or host. Let's assume it's just what we sent.
		console_write("\n");
	}
}

u32 icmp_echo_reply_count(void) {
	return __atomic_load_n(&g_icmp_echo_replies, __ATOMIC_RELAXED);
}

void icmp_send_dest_unreachable(struct ipv4_addr dst, u8 code) {
  u8 payload[8];
  memset(payload, 0, sizeof(payload));
  payload[0] = ICMP_TYPE_DEST_UNREACH;
  payload[1] = code;
  payload[2] = 0;
  payload[3] = 0;
  payload[4] = 0;
  payload[5] = 0;
  payload[6] = 0;
  payload[7] = 0;

  u16 csum = icmp_checksum(payload, sizeof(payload));
  payload[2] = (u8)(csum >> 8);
  payload[3] = (u8)(csum & 0xff);
  ipv4_send(dst, 1 /* ICMP */, payload, sizeof(payload));
  if (bootinfo_has_flag("b1nix.test=1"))
    console_write("UDP-SMOKE: icmp-port-unreachable\n");
}
