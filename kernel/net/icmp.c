#include <b1nix/mm.h>
#include <b1nix/net.h>
#include <b1nix/console.h>
#include <string.h>

#define ICMP_TYPE_ECHO_REPLY 0
#define ICMP_TYPE_ECHO_REQUEST 8

struct icmp_header {
  u8 type;
  u8 code;
  u16 checksum;
  u16 id;
  u16 seq;
} __attribute__((packed));

static u16 bswap16(u16 value) { return (u16)((value << 8) | (value >> 8)); }

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
	} else if (hdr->type == ICMP_TYPE_ECHO_REPLY) {
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
