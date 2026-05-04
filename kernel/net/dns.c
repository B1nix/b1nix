#include <b1nix/net.h>
#include <b1nix/console.h>
#include <string.h>

struct dns_header {
	u16 id;
	u16 flags;
	u16 qdcount;
	u16 ancount;
	u16 nscount;
	u16 arcount;
} __attribute__((packed));

static u16 bswap16(u16 value)
{
	return (u16)((value << 8) | (value >> 8));
}

void dns_resolve(const char *domain)
{
	u8 buffer[512];
	memset(buffer, 0, sizeof(buffer));

	struct dns_header *hdr = (struct dns_header *)buffer;
	hdr->id = bswap16(0x1234);
	hdr->flags = bswap16(0x0100); // Standard query
	hdr->qdcount = bswap16(1);

	u8 *qname = buffer + sizeof(struct dns_header);
	usize offset = 0;
	
	const char *start = domain;
	while (*start) {
		const char *dot = start;
		while (*dot && *dot != '.') dot++;
		
		usize len = dot - start;
		qname[offset++] = (u8)len;
		memcpy(qname + offset, start, len);
		offset += len;
		
		if (*dot == '.') start = dot + 1;
		else start = dot;
	}
	qname[offset++] = 0; // Terminate QNAME

	// QTYPE A
	qname[offset++] = 0;
	qname[offset++] = 1;
	// QCLASS IN
	qname[offset++] = 0;
	qname[offset++] = 1;

	usize req_size = sizeof(struct dns_header) + offset;
	
	struct ipv4_addr dns_server = { { 10, 0, 2, 3 } }; // QEMU DNS
	udp_send(dns_server, 12345, 53, buffer, req_size);

	console_write("net: dns query sent for ");
	console_write(domain);
	console_write("\n");
}

void dns_receive(const void *data, usize size)
{
	if (size < sizeof(struct dns_header)) return;
	const struct dns_header *hdr = data;

	if (bswap16(hdr->id) != 0x1234) return;
	
	u16 flags = bswap16(hdr->flags);
	if ((flags & 0x8000) == 0) return; // Not a response
	
	u16 ancount = bswap16(hdr->ancount);
	if (ancount == 0) {
		console_write("net: dns returned 0 answers\n");
		return;
	}

	const u8 *ptr = (const u8 *)data + sizeof(struct dns_header);
	
	// Skip queries
	u16 qdcount = bswap16(hdr->qdcount);
	for (int i = 0; i < qdcount; i++) {
		while (ptr < (const u8 *)data + size && *ptr != 0) {
			if ((*ptr & 0xC0) == 0xC0) { ptr += 2; break; } // Pointer
			ptr += *ptr + 1;
		}
		if ((*ptr & 0xC0) != 0xC0) ptr++; // Null byte
		ptr += 4; // QTYPE and QCLASS
	}

	// Parse first answer
	if (ptr + 12 <= (const u8 *)data + size) {
		if ((*ptr & 0xC0) == 0xC0) {
			ptr += 2; // Pointer
		} else {
			while (*ptr != 0) ptr += *ptr + 1;
			ptr++;
		}
		
		u16 type = (ptr[0] << 8) | ptr[1];
		ptr += 8; // TYPE, CLASS, TTL
		u16 rdlength = (ptr[0] << 8) | ptr[1];
		ptr += 2;

		if (type == 1 && rdlength == 4 && ptr + 4 <= (const u8 *)data + size) {
			console_write("net: dns resolved to ");
			for (int i = 0; i < 4; i++) {
				console_write_dec(ptr[i]);
				if (i < 3) console_write(".");
			}
			console_write("\n");
		}
	}
}
