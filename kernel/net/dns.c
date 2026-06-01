#include <b1nix/net.h>
#include <b1nix/console.h>
#include <b1nix/sched.h>
#include <b1nix/vfs.h>
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

static int dns_udp_registered = 0;

/* Resolver configuration + last result. The default server is the QEMU
 * user-mode gateway resolver; /etc/resolv.conf overrides it lazily. */
static struct ipv4_addr g_dns_server = { { 10, 0, 2, 3 } };
static int g_resolv_loaded = 0;
static volatile int g_dns_have = 0;
static u8 g_dns_ip[4];

void dns_receive(const void *data, usize size);

void dns_set_server(struct ipv4_addr server)
{
	g_dns_server = server;
}

struct ipv4_addr dns_get_server(void)
{
	return g_dns_server;
}

/* Parse "a.b.c.d" into out[4]; returns 1 on success. */
static int parse_dotted(const char *s, u8 out[4])
{
	int part = 0, val = 0, digits = 0;
	for (;; s++) {
		if (*s >= '0' && *s <= '9') {
			val = val * 10 + (*s - '0');
			if (val > 255) return 0;
			digits++;
		} else if (*s == '.' || *s == '\0' || *s == '\n' || *s == ' ' ||
			   *s == '\t' || *s == '\r') {
			if (!digits) return 0;
			if (part > 3) return 0;
			out[part++] = (u8)val;
			val = 0;
			digits = 0;
			if (*s != '.') break;
		} else {
			return 0;
		}
	}
	return part == 4;
}

int dns_load_resolv_conf(void)
{
	if (g_resolv_loaded)
		return 1;
	g_resolv_loaded = 1; /* attempt once regardless of outcome */

	int fd = vfs_open("/etc/resolv.conf");
	if (fd < 0)
		return 0;

	char buf[256];
	isize n = vfs_read(fd, buf, sizeof(buf) - 1);
	vfs_close(fd);
	if (n <= 0)
		return 0;
	buf[n] = '\0';

	/* Look for the first "nameserver <ip>" line. */
	const char *line = buf;
	while (*line) {
		const char *eol = line;
		while (*eol && *eol != '\n') eol++;
		if (strncmp(line, "nameserver", 10) == 0) {
			const char *p = line + 10;
			while (*p == ' ' || *p == '\t') p++;
			u8 ip[4];
			if (parse_dotted(p, ip)) {
				memcpy(g_dns_server.bytes, ip, 4);
				return 1;
			}
		}
		line = (*eol == '\n') ? eol + 1 : eol;
	}
	return 0;
}

void dns_resolve(const char *domain)
{
	if (!domain || !*domain) return;
	dns_load_resolv_conf();
	if (!dns_udp_registered) {
		udp_register_handler(12345, dns_receive);
		dns_udp_registered = 1;
	}
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
		if (len == 0 || len > 63) return;
		if (offset + 1 + len + 5 > sizeof(buffer) - sizeof(struct dns_header)) return;
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

	udp_send(g_dns_server, 12345, 53, buffer, req_size);

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
			memcpy(g_dns_ip, ptr, 4);
			g_dns_have = 1;
			console_write("net: dns resolved to ");
			for (int i = 0; i < 4; i++) {
				console_write_dec(ptr[i]);
				if (i < 3) console_write(".");
			}
			console_write("\n");
		}
	}
}

int dns_last_result(u8 out[4])
{
	if (!g_dns_have) return 0;
	memcpy(out, g_dns_ip, 4);
	return 1;
}

int dns_resolve_sync(const char *domain, u8 out[4])
{
	if (!domain || !*domain || !out) return -1;
	g_dns_have = 0;
	dns_resolve(domain);
	/* Poll the network for the response (a few hundred ms total). */
	for (int i = 0; i < 100 && !g_dns_have; i++) {
		net_poll();
		if (g_dns_have) break;
		scheduler_sleep_ticks(1);
	}
	if (!g_dns_have) return -1;
	memcpy(out, g_dns_ip, 4);
	return 0;
}
