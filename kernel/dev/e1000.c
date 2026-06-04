/*
 * Intel Gigabit Ethernet (e1000 / e1000e family) driver.
 *
 * Real-hardware NIC support for M37. Implements the generic struct netdev with
 * legacy (8254x-style) descriptor rings, which the whole family — from the
 * QEMU-emulated 82540EM (-device e1000) and 82574L (-device e1000e) up to the
 * PCH-integrated I217/I218/I219 — exposes at the same MMIO register offsets.
 *
 * Verified in QEMU against -device e1000 and -device e1000e. The host's
 * I219-V (8086:15b8) is in the device table and attaches via the same legacy
 * path (MAC read from RAL/RAH, SLU, descriptor rings); its PHY/ULP bring-up
 * quirks may need extending after on-metal testing — see docs.
 *
 * Polling driver: net_task pumps poll() ~100 Hz, and RX interrupts (if wired)
 * only set a pending flag. All hardware wait loops are bounded so a wedged or
 * absent device can never hang the boot.
 */
#include <b1nix/console.h>
#include <b1nix/net.h>
#include <b1nix/netdev.h>
#include <b1nix/pci.h>
#include <b1nix/mm.h>
#include <b1nix/sched.h>
#include <b1nix/arch.h>
#include <b1nix/io.h>
#include <b1nix/bootinfo.h>
#include <string.h>

/* ── Register offsets (byte offsets into MMIO BAR0) ──────────────────────── */
#define E1000_CTRL    0x0000   /* Device Control            */
#define E1000_STATUS  0x0008   /* Device Status             */
#define E1000_EECD    0x0010   /* EEPROM/Flash Control      */
#define E1000_EERD    0x0014   /* EEPROM Read               */
#define E1000_ICR     0x00C0   /* Interrupt Cause Read      */
#define E1000_ITR     0x00C4   /* Interrupt Throttling      */
#define E1000_IMS     0x00D0   /* Interrupt Mask Set        */
#define E1000_IMC     0x00D8   /* Interrupt Mask Clear      */
#define E1000_RCTL    0x0100   /* Receive Control           */
#define E1000_TCTL    0x0400   /* Transmit Control          */
#define E1000_TIPG    0x0410   /* Transmit Inter-Packet Gap */
#define E1000_RDBAL   0x2800   /* RX Descriptor Base Low    */
#define E1000_RDBAH   0x2804   /* RX Descriptor Base High   */
#define E1000_RDLEN   0x2808   /* RX Descriptor Length      */
#define E1000_RDH     0x2810   /* RX Descriptor Head        */
#define E1000_RDT     0x2818   /* RX Descriptor Tail        */
#define E1000_TDBAL   0x3800   /* TX Descriptor Base Low    */
#define E1000_TDBAH   0x3804   /* TX Descriptor Base High   */
#define E1000_TDLEN   0x3808   /* TX Descriptor Length      */
#define E1000_TDH     0x3810   /* TX Descriptor Head        */
#define E1000_TDT     0x3818   /* TX Descriptor Tail        */
#define E1000_MTA     0x5200   /* Multicast Table Array     */
#define E1000_RAL     0x5400   /* Receive Address Low       */
#define E1000_RAH     0x5404   /* Receive Address High      */

#define CTRL_FD       (1u << 0)    /* Full Duplex             */
#define CTRL_ASDE     (1u << 5)    /* Auto-Speed Detect Enable*/
#define CTRL_SLU      (1u << 6)    /* Set Link Up             */
#define CTRL_RST      (1u << 26)   /* Device Reset            */
#define CTRL_PHY_RST  (1u << 31)   /* PHY Reset               */

#define STATUS_LU     (1u << 1)    /* Link Up                 */

#define RCTL_EN       (1u << 1)    /* Receiver Enable         */
#define RCTL_UPE      (1u << 3)    /* Unicast Promiscuous     */
#define RCTL_MPE      (1u << 4)    /* Multicast Promiscuous   */
#define RCTL_LPE      (1u << 5)    /* Long Packet Enable      */
#define RCTL_BAM      (1u << 15)   /* Broadcast Accept Mode   */
#define RCTL_BSIZE_2048 (0u << 16) /* 2048-byte buffers       */
#define RCTL_SECRC    (1u << 26)   /* Strip Ethernet CRC      */

#define TCTL_EN       (1u << 1)    /* Transmit Enable         */
#define TCTL_PSP      (1u << 3)    /* Pad Short Packets       */
#define TCTL_CT_SHIFT 4            /* Collision Threshold     */
#define TCTL_COLD_SHIFT 12         /* Collision Distance      */

#define RXD_STAT_DD   (1u << 0)    /* Descriptor Done         */
#define RXD_STAT_EOP  (1u << 1)    /* End Of Packet           */

#define TXD_CMD_EOP   (1u << 0)    /* End Of Packet           */
#define TXD_CMD_IFCS  (1u << 1)    /* Insert FCS              */
#define TXD_CMD_RS    (1u << 3)    /* Report Status           */
#define TXD_STAT_DD   (1u << 0)    /* Descriptor Done         */

#define EERD_START    (1u << 0)
#define EERD_DONE     (1u << 4)    /* DONE bit (82540 layout) */

#define RAH_AV        (1u << 31)   /* Address Valid           */

/* Legacy descriptors (16 bytes). DMA-coherent on x86 — accessed volatile. */
struct e1000_rx_desc {
	volatile u64 addr;
	volatile u16 length;
	volatile u16 checksum;
	volatile u8  status;
	volatile u8  errors;
	volatile u16 special;
} __attribute__((packed));

struct e1000_tx_desc {
	volatile u64 addr;
	volatile u16 length;
	volatile u8  cso;
	volatile u8  cmd;
	volatile u8  status;
	volatile u8  css;
	volatile u16 special;
} __attribute__((packed));

/* Ring depth and per-descriptor buffer size are fixed device-DMA parameters
 * (not policy caps): 32 descriptors × 2048 B = one 64 KiB contiguous region
 * each for RX and TX, and 2048 matches RCTL's BSIZE encoding. */
#define E1000_NUM_RX  32
#define E1000_NUM_TX  32
#define E1000_BUF_SZ  2048

static volatile u8 *e1000_regs;
static int e1000_inited;

static struct e1000_rx_desc *rx_ring;
static struct e1000_tx_desc *tx_ring;
static u64 rx_buf_phys, tx_buf_phys;
static u8 *rx_buf_virt, *tx_buf_virt;
static u16 rx_cur, tx_cur;

static volatile int e1000_tx_lock;
static volatile int e1000_rx_lock;

static struct netdev e1000_netdev;
static struct mac_addr e1000_mac;

/* ── Known Intel gigabit device IDs (vendor 0x8086) ─────────────────────── */
static const u16 e1000_ids[] = {
	0x100E,  /* 82540EM      — QEMU -device e1000   */
	0x100F,  /* 82545EM                              */
	0x1010,  /* 82546EB                              */
	0x10D3,  /* 82574L       — QEMU -device e1000e   */
	0x10F5,  /* 82567LM                              */
	0x1502,  /* 82579LM                              */
	0x1503,  /* 82579V                               */
	0x153A,  /* I217-LM      */
	0x153B,  /* I217-V       */
	0x15A0,  /* I218-LM      */
	0x15A1,  /* I218-V       */
	0x15A2,  /* I218-LM(2)   */
	0x15A3,  /* I218-V(2)    */
	0x156F,  /* I219-LM      */
	0x1570,  /* I219-V       */
	0x15B7,  /* I219-LM(2)   */
	0x15B8,  /* I219-V(2)    — the development host  */
	0x15D6,  /* I219-V(5)    */
	0x15D7,  /* I219-LM(4)   */
	0x15D8,  /* I219-V(4)    */
	0x15E3,  /* I219-LM(5)   */
	0x0DC7,  /* I219-V(11)   */
};

static int e1000_id_known(u16 dev)
{
	for (usize i = 0; i < sizeof(e1000_ids) / sizeof(e1000_ids[0]); i++)
		if (e1000_ids[i] == dev)
			return 1;
	return 0;
}

/* ── MMIO + small helpers ───────────────────────────────────────────────── */
static inline u32 e1000_read(u32 reg)  { return *(volatile u32 *)(e1000_regs + reg); }
static inline void e1000_write(u32 reg, u32 v) { *(volatile u32 *)(e1000_regs + reg) = v; }

/* Coarse ~1 µs-per-iteration delay via the classic port-0x80 dummy read. */
static void e1000_udelay(int loops)
{
	for (int i = 0; i < loops; i++)
		(void)inb(0x80);
}

static void e1000_print_mac(struct mac_addr m)
{
	const char *d = "0123456789abcdef";
	for (int i = 0; i < 6; i++) {
		console_putc(d[(m.bytes[i] >> 4) & 0xf]);
		console_putc(d[m.bytes[i] & 0xf]);
		if (i < 5) console_putc(':');
	}
}

static u16 e1000_eeprom_read(u8 addr)
{
	e1000_write(E1000_EERD, ((u32)addr << 8) | EERD_START);
	u32 v = 0;
	for (int spins = 0; spins < 100000; spins++) {
		v = e1000_read(E1000_EERD);
		if (v & EERD_DONE)
			break;
	}
	return (u16)(v >> 16);
}

static void e1000_read_mac(struct mac_addr *mac)
{
	u32 ral = e1000_read(E1000_RAL);
	u32 rah = e1000_read(E1000_RAH);
	mac->bytes[0] = (u8)(ral & 0xff);
	mac->bytes[1] = (u8)((ral >> 8) & 0xff);
	mac->bytes[2] = (u8)((ral >> 16) & 0xff);
	mac->bytes[3] = (u8)((ral >> 24) & 0xff);
	mac->bytes[4] = (u8)(rah & 0xff);
	mac->bytes[5] = (u8)((rah >> 8) & 0xff);

	int zero = 1;
	for (int i = 0; i < 6; i++)
		if (mac->bytes[i]) { zero = 0; break; }
	if (!zero)
		return;

	/* RAL/RAH empty — fall back to the EEPROM (words 0..2 hold the MAC). */
	for (int w = 0; w < 3; w++) {
		u16 word = e1000_eeprom_read((u8)w);
		mac->bytes[w * 2]     = (u8)(word & 0xff);
		mac->bytes[w * 2 + 1] = (u8)((word >> 8) & 0xff);
	}
}

/* ── Hardware bring-up ──────────────────────────────────────────────────── */
static void e1000_reset(void)
{
	/* Mask + clear all interrupts before touching anything. */
	e1000_write(E1000_IMC, 0xFFFFFFFF);
	(void)e1000_read(E1000_ICR);

	u32 ctrl = e1000_read(E1000_CTRL);
	e1000_write(E1000_CTRL, ctrl | CTRL_RST);
	/* Reset self-clears within ~1 µs; bound the wait. */
	for (int i = 0; i < 1000; i++) {
		if (!(e1000_read(E1000_CTRL) & CTRL_RST))
			break;
		e1000_udelay(1);
	}

	e1000_write(E1000_IMC, 0xFFFFFFFF);
	(void)e1000_read(E1000_ICR);
}

static void e1000_rx_init(void)
{
	u64 ring_phys = pmm_alloc_frames(1);
	rx_ring = (struct e1000_rx_desc *)(usize)(ring_phys + vmm_direct_map_base());
	memset((void *)rx_ring, 0, PAGE_SIZE);

	usize buf_frames = (E1000_NUM_RX * E1000_BUF_SZ + PAGE_SIZE - 1) / PAGE_SIZE;
	rx_buf_phys = pmm_alloc_frames(buf_frames);
	rx_buf_virt = (u8 *)(usize)(rx_buf_phys + vmm_direct_map_base());

	for (u16 i = 0; i < E1000_NUM_RX; i++) {
		rx_ring[i].addr = rx_buf_phys + (u64)i * E1000_BUF_SZ;
		rx_ring[i].status = 0;
	}
	rx_cur = 0;

	e1000_write(E1000_RDBAL, (u32)(ring_phys & 0xFFFFFFFF));
	e1000_write(E1000_RDBAH, (u32)(ring_phys >> 32));
	e1000_write(E1000_RDLEN, E1000_NUM_RX * sizeof(struct e1000_rx_desc));
	e1000_write(E1000_RDH, 0);
	e1000_write(E1000_RDT, E1000_NUM_RX - 1);

	e1000_write(E1000_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC | RCTL_BSIZE_2048);
}

static void e1000_tx_init(void)
{
	u64 ring_phys = pmm_alloc_frames(1);
	tx_ring = (struct e1000_tx_desc *)(usize)(ring_phys + vmm_direct_map_base());
	memset((void *)tx_ring, 0, PAGE_SIZE);

	usize buf_frames = (E1000_NUM_TX * E1000_BUF_SZ + PAGE_SIZE - 1) / PAGE_SIZE;
	tx_buf_phys = pmm_alloc_frames(buf_frames);
	tx_buf_virt = (u8 *)(usize)(tx_buf_phys + vmm_direct_map_base());

	/* Pretend each descriptor is already "done" so the first reuse-wait in
	 * e1000_xmit() passes immediately. */
	for (u16 i = 0; i < E1000_NUM_TX; i++)
		tx_ring[i].status = TXD_STAT_DD;
	tx_cur = 0;

	e1000_write(E1000_TDBAL, (u32)(ring_phys & 0xFFFFFFFF));
	e1000_write(E1000_TDBAH, (u32)(ring_phys >> 32));
	e1000_write(E1000_TDLEN, E1000_NUM_TX * sizeof(struct e1000_tx_desc));
	e1000_write(E1000_TDH, 0);
	e1000_write(E1000_TDT, 0);

	e1000_write(E1000_TCTL, TCTL_EN | TCTL_PSP |
	            (0x10u << TCTL_CT_SHIFT) | (0x40u << TCTL_COLD_SHIFT));
	e1000_write(E1000_TIPG, 0x0060200A); /* IEEE 802.3 recommended IPG */
}

/* Low-level frame transmit. Returns 0 on success. Shared by the netdev op and
 * the self-test. */
static int e1000_xmit(const u8 hdr[14], const void *payload, usize plen)
{
	if (!e1000_inited)
		return -1;
	usize total = 14 + plen;
	if (total > E1000_BUF_SZ)
		return -1;

	while (__atomic_test_and_set(&e1000_tx_lock, __ATOMIC_ACQUIRE))
		scheduler_yield();

	u16 i = tx_cur;
	/* Wait for this descriptor to drain (bounded). */
	for (int spins = 0; spins < 500000; spins++) {
		if (tx_ring[i].status & TXD_STAT_DD)
			break;
	}

	u8 *buf = tx_buf_virt + (usize)i * E1000_BUF_SZ;
	memcpy(buf, hdr, 14);
	memcpy(buf + 14, payload, plen);

	tx_ring[i].addr = tx_buf_phys + (u64)i * E1000_BUF_SZ;
	tx_ring[i].length = (u16)total;
	tx_ring[i].cso = 0;
	tx_ring[i].css = 0;
	tx_ring[i].special = 0;
	tx_ring[i].status = 0;
	tx_ring[i].cmd = TXD_CMD_EOP | TXD_CMD_IFCS | TXD_CMD_RS;
	__asm__ volatile("" ::: "memory");

	tx_cur = (u16)((i + 1) % E1000_NUM_TX);
	e1000_write(E1000_TDT, tx_cur);

	__atomic_clear(&e1000_tx_lock, __ATOMIC_RELEASE);
	return 0;
}

/* ── netdev ops ─────────────────────────────────────────────────────────── */
static int e1000_transmit(struct netdev *nd, const u8 hdr[14],
                          const void *payload, usize payload_len)
{
	(void)nd;
	return e1000_xmit(hdr, payload, payload_len);
}

static void e1000_poll(struct netdev *nd)
{
	(void)nd;
	if (!e1000_inited)
		return;
	if (__atomic_test_and_set(&e1000_rx_lock, __ATOMIC_ACQUIRE))
		return;

	while (rx_ring[rx_cur].status & RXD_STAT_DD) {
		u16 len = rx_ring[rx_cur].length;
		u8 *buf = rx_buf_virt + (usize)rx_cur * E1000_BUF_SZ;
		if ((rx_ring[rx_cur].status & RXD_STAT_EOP) && len > 0)
			ethernet_receive(buf, len);
		rx_ring[rx_cur].status = 0;
		u16 old = rx_cur;
		rx_cur = (u16)((rx_cur + 1) % E1000_NUM_RX);
		e1000_write(E1000_RDT, old);
	}

	__atomic_clear(&e1000_rx_lock, __ATOMIC_RELEASE);
}

static int e1000_irq_ack(struct netdev *nd)
{
	(void)nd;
	if (!e1000_inited)
		return 0;
	u32 icr = e1000_read(E1000_ICR); /* reading clears the cause bits */
	return icr ? 1 : 0;
}

/* ── Probe ──────────────────────────────────────────────────────────────── */
int e1000_probe(void)
{
	struct pci_device_info pci;
	int found = 0;

	/* Walk the ethernet-class (0x02/0x00) PCI devices and pick the first
	 * Intel gigabit part we recognise. This naturally skips a co-resident
	 * virtio-net (vendor 0x1AF4) when both NICs are attached. */
	for (u8 idx = 0; idx < 32; idx++) {
		if (!pci_find_class(0x02, 0x00, idx, &pci))
			break;
		if (pci.vendor_id == 0x8086 && e1000_id_known(pci.device_id)) {
			found = 1;
			break;
		}
	}
	if (!found)
		return 0;

	/* Enable memory space + bus-master. */
	u16 cmd = pci_config_read16(pci.bus, pci.slot, pci.func, 0x04);
	cmd |= 0x0006;
	pci_config_write16(pci.bus, pci.slot, pci.func, 0x04, cmd);

	/* BAR0: 32- or 64-bit memory BAR. */
	u32 bar0 = pci_config_read32(pci.bus, pci.slot, pci.func, 0x10);
	u64 mmio_phys;
	if (((bar0 >> 1) & 3) == 2) {
		u32 hi = pci_config_read32(pci.bus, pci.slot, pci.func, 0x14);
		mmio_phys = ((u64)hi << 32) | (bar0 & 0xFFFFFFF0u);
	} else {
		mmio_phys = bar0 & 0xFFFFFFF0u;
	}

#ifdef __x86_64__
	/* The direct map already covers PCI MMIO BARs (same as ahci/nvme). */
	e1000_regs = (volatile u8 *)(usize)(vmm_direct_map_base() + mmio_phys);
#else
	/* 32-bit: BARs live above the 1 GiB direct map — use the MMIO window. */
	e1000_regs = (volatile u8 *)vmm_map_mmio(mmio_phys, 0x20000,
	                                         VMM_WRITABLE | VMM_PCD);
#endif

	console_write("e1000: ");
	console_write_hex32(pci.device_id);
	console_write(" BAR0 0x");
	console_write_hex64(mmio_phys);
	console_write("\n");

	e1000_reset();

	/* Set link up + auto-speed. */
	u32 ctrl = e1000_read(E1000_CTRL);
	e1000_write(E1000_CTRL, ctrl | CTRL_SLU | CTRL_ASDE);

	/* Clear the multicast table (128 dwords). */
	for (int i = 0; i < 128; i++)
		e1000_write(E1000_MTA + i * 4, 0);

	e1000_read_mac(&e1000_mac);

	/* Program the station address so the unicast filter accepts our frames. */
	e1000_write(E1000_RAL, (u32)e1000_mac.bytes[0] |
	            ((u32)e1000_mac.bytes[1] << 8) |
	            ((u32)e1000_mac.bytes[2] << 16) |
	            ((u32)e1000_mac.bytes[3] << 24));
	e1000_write(E1000_RAH, (u32)e1000_mac.bytes[4] |
	            ((u32)e1000_mac.bytes[5] << 8) | RAH_AV);

	e1000_rx_init();
	e1000_tx_init();

	e1000_inited = 1;

	e1000_netdev.name = "e1000";
	e1000_netdev.mac = e1000_mac;
	e1000_netdev.irq = (pci.irq_line == 0xFF) ? -1 : (int)pci.irq_line;
	e1000_netdev.transmit = e1000_transmit;
	e1000_netdev.poll = e1000_poll;
	e1000_netdev.irq_ack = e1000_irq_ack;
	e1000_netdev.priv = 0;
	netdev_register(&e1000_netdev);

	console_write("e1000: initialized with MAC ");
	e1000_print_mac(e1000_mac);
	console_write("\n");

	if (e1000_netdev.irq >= 0)
		x86_pic_unmask((u16)e1000_netdev.irq);

	return 1;
}

/* ── Self-test (test mode only) ─────────────────────────────────────────── *
 * Drives an ARP request/reply against the QEMU SLIRP gateway directly through
 * the e1000 ring, reading received frames from the ring itself (NOT via
 * ethernet_receive) so it never injects into the active NIC's protocol stack.
 * The SLIRP defaults (guest 10.0.2.15, gateway 10.0.2.2) are fixed test
 * fixtures, hardcoded here only for deterministic verification. */

/* Dequeue one frame straight from the RX ring. Returns its length, or 0. */
static usize e1000_selftest_recv(u8 *out, usize maxlen)
{
	if (!(rx_ring[rx_cur].status & RXD_STAT_DD))
		return 0;
	usize len = rx_ring[rx_cur].length;
	if (len > maxlen)
		len = maxlen;
	memcpy(out, rx_buf_virt + (usize)rx_cur * E1000_BUF_SZ, len);
	rx_ring[rx_cur].status = 0;
	u16 old = rx_cur;
	rx_cur = (u16)((rx_cur + 1) % E1000_NUM_RX);
	e1000_write(E1000_RDT, old);
	return len;
}

void e1000_selftest(void)
{
	if (!e1000_inited) {
		console_write("M37-E1000: skip no-device\n");
		return;
	}
	console_write("M37-E1000: ok init\n");

	console_write("M37-E1000: ok mac ");
	e1000_print_mac(e1000_mac);
	console_write("\n");

	/* Poll the link-up bit (bounded). */
	int up = 0;
	for (int i = 0; i < 200; i++) {
		if (e1000_read(E1000_STATUS) & STATUS_LU) { up = 1; break; }
		scheduler_sleep_ticks(1);
	}
	console_write(up ? "M37-E1000: ok link\n"
	                 : "M37-E1000: link down (continuing)\n");

	/* Build an ARP request: who-has 10.0.2.2, tell 10.0.2.15. */
	u8 eth[14];
	memset(eth, 0xff, 6);                 /* broadcast dst */
	memcpy(eth + 6, e1000_mac.bytes, 6);  /* our src       */
	eth[12] = 0x08; eth[13] = 0x06;       /* ARP           */

	u8 arp[28];
	arp[0] = 0x00; arp[1] = 0x01;         /* htype: ethernet */
	arp[2] = 0x08; arp[3] = 0x00;         /* ptype: IPv4     */
	arp[4] = 6; arp[5] = 4;               /* hlen/plen       */
	arp[6] = 0x00; arp[7] = 0x01;         /* op: request     */
	memcpy(arp + 8, e1000_mac.bytes, 6);  /* sender MAC      */
	arp[14] = 10; arp[15] = 0; arp[16] = 2; arp[17] = 15;  /* sender 10.0.2.15 */
	memset(arp + 18, 0, 6);               /* target MAC      */
	arp[24] = 10; arp[25] = 0; arp[26] = 2; arp[27] = 2;   /* target 10.0.2.2  */

	if (e1000_xmit(eth, arp, sizeof(arp)) == 0)
		console_write("M37-E1000: ok tx\n");
	else
		console_write("M37-E1000: FAIL tx\n");

	/* Poll for an ARP reply from 10.0.2.2 (bounded ~2s). */
	int got = 0;
	u8 frame[E1000_BUF_SZ];
	for (int i = 0; i < 200 && !got; i++) {
		usize len;
		while ((len = e1000_selftest_recv(frame, sizeof(frame))) > 0) {
			if (len >= 42 &&
			    frame[12] == 0x08 && frame[13] == 0x06 &&  /* ARP   */
			    frame[20] == 0x00 && frame[21] == 0x02 &&  /* reply */
			    frame[28] == 10 && frame[29] == 0 &&
			    frame[30] == 2 && frame[31] == 2) {        /* from 10.0.2.2 */
				got = 1;
				break;
			}
		}
		if (!got)
			scheduler_sleep_ticks(1);
	}
	console_write(got ? "M37-E1000: ok rx-arp\n"
	                  : "M37-E1000: FAIL rx-arp (no reply)\n");
}
