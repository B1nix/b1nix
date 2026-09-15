/*
 * ARCHIVED — not built. Kernel helpers whose only callers were native-ABI
 * syscalls (see README.md), with the file each came from.
 */

/* ── kernel/syscall/syscall.c ── */
static int __attribute__((unused)) parse_ipv4_literal(const char *s, struct ipv4_addr *out) {
  if (!s || !out)
    return -EINVAL;

  struct ipv4_addr ip = {{0, 0, 0, 0}};
  int octet = 0;
  int value = 0;
  int has_digit = 0;

  for (const char *p = s;; p++) {
    char c = *p;
    if (c >= '0' && c <= '9') {
      has_digit = 1;
      value = value * 10 + (c - '0');
      if (value > 255)
        return -EINVAL;
      continue;
    }

    if (c == '.' || c == '\0') {
      if (!has_digit || octet >= 4)
        return -EINVAL;
      ip.bytes[octet++] = (u8)value;
      value = 0;
      has_digit = 0;
      if (c == '\0')
        break;
      continue;
    }

    return -EINVAL;
  }

  if (octet != 4)
    return -EINVAL;
  *out = ip;
  return 0;
}

/* ── kernel/sched/uidgid.c ── */
int cred_set_euid(struct cred *cred, u16 euid)
{
    if (!cred) return -EINVAL;
    if (!cred_has_cap(cred, CAP_SETUID)) {
        if (euid != cred->uid && euid != cred->suid && euid != cred->euid) {
            return -EPERM;
        }
    }
    cred->euid = euid;
    cred_refresh_caps(cred);
    cred_sync_fsids(cred);
    return 0;
}

/* ── kernel/sched/uidgid.c ── */
int cred_set_egid(struct cred *cred, u16 egid)
{
    if (!cred) return -EINVAL;
    int is_privileged = (cred->euid == ROOT_UID || cred_has_cap(cred, CAP_SETGID));
    if (!is_privileged) {
        if (egid != cred->gid && egid != cred->sgid && egid != cred->egid) {
            return -EPERM;
        }
    }
    cred->egid = egid;
    cred_refresh_caps(cred);
    cred_sync_fsids(cred);
    return 0;
}

/* ── kernel/include/b1nix/uidgid.h ── */
int  cred_set_euid(struct cred *cred, u16 euid);

/* ── kernel/include/b1nix/uidgid.h ── */
int  cred_set_egid(struct cred *cred, u16 egid);

/* ── kernel/net/net.c ── */
void net_dump_info(void)
{
	struct netdev *nd = netdev_active();
	console_write("Network\n");
	console_write(" driver: ");
	console_write(nd ? nd->name : "none");
	console_write("\n link:   ");
	int link = netdev_link_state(nd);
	console_write(link > 0 ? "up" : (link == 0 ? "down" : "unknown"));
	console_write("\n mac:    ");
	print_mac(local_mac);
	console_write("\n ip:     ");
	print_ipv4(net_get_ip());
	console_write("\n gateway:");
	console_putc(' ');
	struct ipv4_addr gw = net_get_gateway();
	print_ipv4(gw);
	console_write(" [raw:");
	console_write_hex32(((u32)gw.bytes[0] << 24) |
	                    ((u32)gw.bytes[1] << 16) |
	                    ((u32)gw.bytes[2] <<  8) |
	                     (u32)gw.bytes[3]);
	console_write("]");
	console_write("\n");
	dhcp_dump_info();

	if (net_adapter_count == 0) {
		k_info(NULL, " pci:    no network adapters found");
		return;
	}

	for (usize i = 0; i < net_adapter_count; i++) {
		const struct net_adapter *adapter = &net_adapters[i];
		const struct pci_device_info *pci = &adapter->pci;
		console_write(" pci:    ");
		console_write_dec(pci->bus);
		console_putc(':');
		console_write_dec(pci->slot);
		console_putc('.');
		console_write_dec(pci->func);
		console_putc(' ');
		console_write(net_vendor_name(pci->vendor_id));
		console_putc(' ');
		console_write(net_kind_name(pci->subclass));
		console_write(" vendor 0x");
		console_write_hex32(pci->vendor_id);
		console_write(" device 0x");
		console_write_hex32(pci->device_id);
		console_write(" prog_if 0x");
		console_write_hex32(pci->prog_if);
		console_write("\n");
	}
}

/* ── kernel/net/stub.c ── */
void net_dump_info(void) {}

/* ── kernel/include/b1nix/net.h ── */
void net_dump_info(void);

/* ── kernel/net/icmp.c ── */
u32 icmp_echo_reply_count(void) {
	return __atomic_load_n(&g_icmp_echo_replies, __ATOMIC_RELAXED);
}

/* ── kernel/include/b1nix/net.h ── */
u32 icmp_echo_reply_count(void);

/* ── kernel/net/dns.c ── */
void dns_resolve(const char *domain)
{
	dns_resolve_start(domain, 1);
}

/* ── kernel/net/dns.c ── */
int dns_resolve_sync(const char *domain, u8 out[4])
{
	return dns_resolve_sync_impl(domain, out, 1);
}

/* ── kernel/include/b1nix/net.h ── */
void dns_resolve(const char *domain);

/* ── kernel/include/b1nix/net.h ── */
int dns_resolve_sync(const char *domain, u8 out[4]);

/* ── kernel/sched/scheduler.c ── */
void scheduler_set_stdout(int fd) {
  interrupts_disable();
  if (current_task != 0) {
    current_task->stdout_fd = fd;
  }
  interrupts_enable();
}

/* ── kernel/include/b1nix/sched.h ── */
void scheduler_set_stdout(int fd);

/* ── kernel/net/net.c (formatters used only by net_dump_info) ── */
static const char *net_vendor_name(u16 vendor)
{
	switch (vendor) {
	case 0x10ec: return "Realtek";
	case 0x14e4: return "Broadcom";
	case 0x168c: return "Qualcomm/Atheros";
	case 0x1969: return "Atheros";
	case 0x1af4: return "VirtIO";
	case 0x8086: return "Intel";
	default: return "unknown";
	}
}

static const char *net_kind_name(u8 subclass)
{
	switch (subclass) {
	case 0x00: return "Ethernet";
	case 0x80: return "network";
	default: return "network";
	}
}

static void print_hex8(u8 value)
{
	const char *digits = "0123456789abcdef";
	console_putc(digits[(value >> 4) & 0xf]);
	console_putc(digits[value & 0xf]);
}

static void print_ipv4(struct ipv4_addr addr)
{
	for (int i = 0; i < 4; i++) {
		console_write_dec(addr.bytes[i]);
		if (i < 3) console_putc('.');
	}
}

static void print_mac(struct mac_addr mac)
{
	for (int i = 0; i < 6; i++) {
		print_hex8(mac.bytes[i]);
		if (i < 5) console_putc(':');
	}
}

