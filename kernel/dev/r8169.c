/* kernel/dev/r8169.c — Realtek RTL8169/8168/8111/8101/810xE Gigabit/Fast
 * Ethernet driver (the Linux "r8169" family), plugged into the generic netdev
 * model.
 *
 * Covers the most common consumer NIC family: PCIe Realtek parts found on the
 * vast majority of laptops and desktop boards (vendor 0x10EC). The dev target
 * is the Acer Aspire One ZG5's wired NIC — an RTL8102E/8103E (device 0x8136) —
 * but the same descriptor-ring datapath drives the whole RTL8168/8111/810x line.
 *
 * Hardened like the e1000 driver after the I219-V metal hang: the device is
 * powered to D0 before any MMIO, every poll loop is bounded, bring-up is
 * non-fatal (the kernel always reaches a shell), and `b1nix.skip-r8169` skips
 * it entirely.
 *
 * QEMU has no RTL8169 model (only the unrelated RTL8139), so this path is
 * exercised on real hardware; on QEMU r8169_probe() simply finds nothing.
 *
 * Polling driver: net_task pumps poll() ~100 Hz (RX interrupts are masked).
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

/* ── Register offsets (from the MMIO base) ──────────────────────────────── */
#define R_IDR0      0x00   /* MAC address, 6 bytes                          */
#define R_MAR0      0x08   /* Multicast filter, 8 bytes                     */
#define R_TXDESC_LO 0x20   /* Tx normal-priority descriptor ring phys (lo)  */
#define R_TXDESC_HI 0x24
#define R_CMD       0x37   /* ChipCmd (8-bit)                               */
#define   CMD_RST   0x10
#define   CMD_RXE   0x08
#define   CMD_TXE   0x04
#define R_TXPOLL    0x38   /* (8-bit) write NPQ to kick normal-prio Tx      */
#define   TXPOLL_NPQ 0x40
#define R_IMR       0x3C   /* Interrupt mask (16-bit)                       */
#define R_ISR       0x3E   /* Interrupt status (16-bit, write-1-clear)      */
#define   INT_ROK       0x0001   /* Receive OK                              */
#define   INT_RER       0x0002   /* Receive error                           */
#define   INT_RXDU      0x0010   /* RX descriptor unavailable               */
#define   INT_LINKCHG   0x0020   /* Link status change                      */
#define   INT_FIFOOVW   0x0040   /* RX FIFO overflow                        */
#define R8169_INTR_MASK (INT_ROK | INT_RER | INT_RXDU | INT_LINKCHG | INT_FIFOOVW)
#define R_TXCFG     0x40   /* Tx config (32-bit)                            */
#define R_RXCFG     0x44   /* Rx config (32-bit)                            */
#define R_CFG9346   0x50   /* EEPROM/lock register (8-bit)                  */
#define   CFG9346_UNLOCK 0xC0
#define   CFG9346_LOCK   0x00
#define R_PHYSTATUS 0x6C   /* PHY status; bit 1 is link status               */
#define   PHYSTATUS_LINK 0x02
#define R_RXMAXSZ   0xDA   /* Rx max packet size (16-bit)                   */
#define R_CPCMD     0xE0   /* C+ command (16-bit)                           */
#define R_RXDESC_LO 0xE4   /* Rx descriptor ring phys (lo)                  */
#define R_RXDESC_HI 0xE8
#define R_MTPS      0xEC   /* Max Tx packet size (8-bit, 128-byte units)    */

/* Descriptor opts1 bits */
#define DESC_OWN  (1u << 31)
#define DESC_EOR  (1u << 30)   /* end of descriptor ring */
#define DESC_FS   (1u << 29)   /* first segment */
#define DESC_LS   (1u << 28)   /* last segment  */
#define DESC_LEN_MASK 0x3FFFu

/* Rx config: accept broadcast + multicast + our-MAC, unlimited DMA burst and
 * Rx-FIFO threshold (take the whole packet). */
#define RXCFG_VAL  (0x0Eu | (7u << 8) | (7u << 13))
/* Tx config: unlimited DMA burst + standard IFG. */
#define TXCFG_VAL  ((6u << 8) | (3u << 24))

#define R8169_NUM_RX 32
#define R8169_NUM_TX 32
#define R8169_BUF_SZ 2048

struct r8169_desc {
	volatile u32 opts1;
	volatile u32 opts2;
	volatile u64 addr;
} __attribute__((aligned(16)));

static volatile u8 *r_regs;
static int r8169_inited;
static struct mac_addr r8169_mac;
static struct netdev r8169_netdev;

static struct r8169_desc *rx_ring;
static struct r8169_desc *tx_ring;
static u8 *rx_buf_virt; static u64 rx_buf_phys;
static u8 *tx_buf_virt; static u64 tx_buf_phys;
static u16 rx_cur, tx_cur;
static volatile int r8169_tx_lock, r8169_rx_lock;

/* Recognised RTL8169/8168/8111/8101/810x device IDs (vendor 0x10EC). The whole
 * family shares this descriptor-ring datapath. 0x8136 is the ZG5's RTL8102E. */
static const u16 r8169_ids[] = {
	0x8136, /* RTL8101E/8102E/8103E/8136 Fast Ethernet (the ZG5)            */
	0x8167, /* RTL8110SC/8169SC                                            */
	0x8168, /* RTL8168/8111 Gigabit (8168B..8168H, very common)            */
	0x8169, /* RTL8169 Gigabit                                             */
	0x8161, /* RTL8168 variant                                            */
	0x8129, /* RTL8129 (older)                                            */
	0x2502, 0x2600, /* RTL8101 variants                                   */
};

static int r8169_id_known(u16 dev)
{
	for (unsigned i = 0; i < sizeof(r8169_ids) / sizeof(r8169_ids[0]); i++)
		if (r8169_ids[i] == dev)
			return 1;
	return 0;
}

/* ── MMIO helpers ───────────────────────────────────────────────────────── */
static inline u8  r_r8(u32 o)        { return *(volatile u8  *)(r_regs + o); }
static inline u16 r_r16(u32 o)       { return *(volatile u16 *)(r_regs + o); }
static inline void r_w8(u32 o, u8 v)  { *(volatile u8  *)(r_regs + o) = v; }
static inline void r_w16(u32 o, u16 v){ *(volatile u16 *)(r_regs + o) = v; }
static inline void r_w32(u32 o, u32 v){ *(volatile u32 *)(r_regs + o) = v; }

static void r_udelay(int loops) { for (int i = 0; i < loops; i++) (void)inb(0x80); }

/* Hint the CPU (and the hypervisor) that this is a spin-wait. A bare
 * `while (test_and_set()) ;` loop pegs the core and, under virtualisation,
 * keeps the vCPU from being descheduled in favour of the lock holder. */
static inline void r8169_relax(void) { __asm__ volatile("pause" ::: "memory"); }

/* ~10 ms budget: one iteration is a pause plus a single ~1 us port access. */
#define R8169_TX_WAIT_LOOPS 10000

/* Force the device into PCI power state D0 before any MMIO (a D3-parked NIC
 * hangs the CPU on the first register access — see dev/e1000.c). Pure config
 * space, cannot itself hang. */
static void r8169_pci_set_d0(const struct pci_device_info *p)
{
	u16 status = pci_config_read16(p->bus, p->slot, p->func, 0x06);
	if (!(status & (1u << 4)))
		return;
	u8 cap = pci_config_read8(p->bus, p->slot, p->func, 0x34) & 0xFC;
	for (int g = 0; cap && g < 48; g++) {
		if (pci_config_read8(p->bus, p->slot, p->func, cap) == 0x01) {
			u16 pmcsr = pci_config_read16(p->bus, p->slot, p->func, cap + 4);
			if ((pmcsr & 3) != 0) {
				console_write("r8169: device in D");
				console_putc('0' + (char)(pmcsr & 3));
				console_write(", forcing D0\n");
				pci_config_write16(p->bus, p->slot, p->func, cap + 4, pmcsr & ~3u);
				r_udelay(10000);
			}
			return;
		}
		cap = pci_config_read8(p->bus, p->slot, p->func, cap + 1) & 0xFC;
	}
}

static void r8169_print_mac(struct mac_addr m)
{
	const char *d = "0123456789abcdef";
	for (int i = 0; i < 6; i++) {
		console_putc(d[(m.bytes[i] >> 4) & 0xf]);
		console_putc(d[m.bytes[i] & 0xf]);
		if (i < 5) console_putc(':');
	}
}

static void r8169_reset(void)
{
	r_w8(R_CMD, CMD_RST);
	for (int i = 0; i < 1000; i++) {       /* self-clears in ~µs; bound it */
		if (!(r_r8(R_CMD) & CMD_RST))
			break;
		r_udelay(1);
	}
}

static void r8169_rx_init(void)
{
	u64 ring_phys = pmm_alloc_frames(1);
	rx_ring = (struct r8169_desc *)(usize)(ring_phys + vmm_direct_map_base());
	memset((void *)rx_ring, 0, PAGE_SIZE);

	usize buf_frames = (R8169_NUM_RX * R8169_BUF_SZ + PAGE_SIZE - 1) / PAGE_SIZE;
	rx_buf_phys = pmm_alloc_frames(buf_frames);
	rx_buf_virt = (u8 *)(usize)(rx_buf_phys + vmm_direct_map_base());

	for (u16 i = 0; i < R8169_NUM_RX; i++) {
		rx_ring[i].addr  = rx_buf_phys + (u64)i * R8169_BUF_SZ;
		rx_ring[i].opts2 = 0;
		u32 o1 = DESC_OWN | (u32)R8169_BUF_SZ;
		if (i == R8169_NUM_RX - 1)
			o1 |= DESC_EOR;
		rx_ring[i].opts1 = o1;
	}
	rx_cur = 0;

	r_w32(R_RXDESC_LO, (u32)(ring_phys & 0xFFFFFFFF));
	r_w32(R_RXDESC_HI, (u32)(ring_phys >> 32));
}

static void r8169_tx_init(void)
{
	u64 ring_phys = pmm_alloc_frames(1);
	tx_ring = (struct r8169_desc *)(usize)(ring_phys + vmm_direct_map_base());
	memset((void *)tx_ring, 0, PAGE_SIZE);

	usize buf_frames = (R8169_NUM_TX * R8169_BUF_SZ + PAGE_SIZE - 1) / PAGE_SIZE;
	tx_buf_phys = pmm_alloc_frames(buf_frames);
	tx_buf_virt = (u8 *)(usize)(tx_buf_phys + vmm_direct_map_base());

	for (u16 i = 0; i < R8169_NUM_TX; i++) {
		tx_ring[i].addr  = tx_buf_phys + (u64)i * R8169_BUF_SZ;
		tx_ring[i].opts2 = 0;
		tx_ring[i].opts1 = (i == R8169_NUM_TX - 1) ? DESC_EOR : 0; /* not OWNed */
	}
	tx_cur = 0;

	r_w32(R_TXDESC_LO, (u32)(ring_phys & 0xFFFFFFFF));
	r_w32(R_TXDESC_HI, (u32)(ring_phys >> 32));
}

static int r8169_xmit(const u8 hdr[14], const void *payload, usize plen)
{
	if (!r8169_inited)
		return -1;
	usize total = 14 + plen;
	if (total > R8169_BUF_SZ)
		return -1;

	while (__atomic_test_and_set(&r8169_tx_lock, __ATOMIC_ACQUIRE))
		r8169_relax();

	/* Wait (bounded) for the current descriptor to be free (OWN clear).
	 * The bound is a real time budget (~10 ms), not an arbitrary iteration
	 * count: the old 500000 x inb(0x80) loop could burn most of a second of
	 * CPU on a hypervisor, where every I/O-port access is a VM exit. */
	int ok = 0;
	for (int spins = 0; spins < R8169_TX_WAIT_LOOPS; spins++) {
		if (!(tx_ring[tx_cur].opts1 & DESC_OWN)) { ok = 1; break; }
		r8169_relax();
		r_udelay(1);
	}
	if (!ok) {
		__atomic_clear(&r8169_tx_lock, __ATOMIC_RELEASE);
		return -1;
	}

	u8 *buf = tx_buf_virt + (usize)tx_cur * R8169_BUF_SZ;
	memcpy(buf, hdr, 14);
	if (plen)
		memcpy(buf + 14, payload, plen);

	u32 o1 = DESC_OWN | DESC_FS | DESC_LS | (u32)(total & DESC_LEN_MASK);
	if (tx_cur == R8169_NUM_TX - 1)
		o1 |= DESC_EOR;
	tx_ring[tx_cur].opts2 = 0;
	tx_ring[tx_cur].opts1 = o1;

	__asm__ volatile("" ::: "memory");
	r_w8(R_TXPOLL, TXPOLL_NPQ);          /* kick the normal-priority queue */

	tx_cur = (u16)((tx_cur + 1) % R8169_NUM_TX);
	__atomic_clear(&r8169_tx_lock, __ATOMIC_RELEASE);
	return 0;
}

static int r8169_transmit(struct netdev *nd, const u8 hdr[14],
                          const void *payload, usize payload_len, u32 tx_flags)
{
	(void)nd;
	/* No checksum offload on this device: the stack never sets
	 * NETDEV_TX_F_PARTIAL_CSUM unless every interface advertised it. */
	(void)tx_flags;
	return r8169_xmit(hdr, payload, payload_len);
}

static void r8169_poll(struct netdev *nd)
{
	(void)nd;
	if (!r8169_inited)
		return;
	if (__atomic_test_and_set(&r8169_rx_lock, __ATOMIC_ACQUIRE))
		return;

	int budget = R8169_NUM_RX * 2;       /* bound the drain per poll */
	while (budget-- > 0 && !(rx_ring[rx_cur].opts1 & DESC_OWN)) {
		u32 o1 = rx_ring[rx_cur].opts1;
		u32 len = o1 & DESC_LEN_MASK;
		/* FS+LS => a complete single-buffer frame; strip the 4-byte CRC. */
		if ((o1 & DESC_FS) && (o1 & DESC_LS) && len > 4) {
			u8 *buf = rx_buf_virt + (usize)rx_cur * R8169_BUF_SZ;
			ethernet_receive(buf, len - 4);
		}
		/* Re-arm this descriptor for the device. */
		u32 n1 = DESC_OWN | (u32)R8169_BUF_SZ;
		if (rx_cur == R8169_NUM_RX - 1)
			n1 |= DESC_EOR;
		rx_ring[rx_cur].opts2 = 0;
		rx_ring[rx_cur].opts1 = n1;
		rx_cur = (u16)((rx_cur + 1) % R8169_NUM_RX);
	}

	r_w16(R_ISR, 0xFFFF);                 /* ack any pending RX/TX causes */
	__atomic_clear(&r8169_rx_lock, __ATOMIC_RELEASE);
}

static int r8169_irq_ack(struct netdev *nd)
{
	(void)nd;
	if (!r8169_inited)
		return 0;
	u16 isr = r_r16(R_ISR);
	if (isr)
		r_w16(R_ISR, isr);                /* write-1-clear */
	return isr ? 1 : 0;
}

static int r8169_link_up(struct netdev *nd)
{
	(void)nd;
	return r8169_inited && (r_r8(R_PHYSTATUS) & PHYSTATUS_LINK) ? 1 : 0;
}

/* ── Probe ──────────────────────────────────────────────────────────────── */
int r8169_probe(void)
{
	struct pci_device_info pci;
	int found = 0;

	if (bootinfo_has_flag("b1nix.skip-r8169")) {
		console_write("r8169: skipped (b1nix.skip-r8169)\n");
		return 0;
	}

	for (u8 idx = 0; idx < 32; idx++) {
		if (!pci_find_class(0x02, 0x00, idx, &pci))
			break;
		if (pci.vendor_id == 0x10EC && r8169_id_known(pci.device_id)) {
			found = 1;
			break;
		}
	}
	if (!found)
		return 0;

	/* Enable memory space + bus-master, and clear PCI command bit 10 so INTx
	 * delivery is not disabled behind our back by firmware. */
	u16 cmd = pci_config_read16(pci.bus, pci.slot, pci.func, 0x04);
	cmd |= 0x0006;
	cmd &= (u16)~0x0400u;
	pci_config_write16(pci.bus, pci.slot, pci.func, 0x04, cmd);

	r8169_pci_set_d0(&pci);

	/* Find the first MEMORY BAR (RTL810x exposes both an I/O and an MMIO BAR;
	 * the register block must be reached through the MMIO one). */
	u64 mmio_phys = 0;
	int bar64 = 0;
	for (int b = 0; b < 6 && !mmio_phys; b++) {
		u32 bar = pci_config_read32(pci.bus, pci.slot, pci.func, 0x10 + b * 4);
		if (bar & 1)                      /* I/O BAR — skip */
			continue;
		if (((bar >> 1) & 3) == 2) {      /* 64-bit memory BAR */
			u32 hi = pci_config_read32(pci.bus, pci.slot, pci.func, 0x10 + (b + 1) * 4);
			mmio_phys = ((u64)hi << 32) | (bar & 0xFFFFFFF0u);
			bar64 = 1;
		} else {                          /* 32-bit memory BAR */
			mmio_phys = bar & 0xFFFFFFF0u;
		}
		if (bar64) b++;
	}
	if (!mmio_phys) {
		console_write("r8169: no memory BAR found, skipping\n");
		return 0;
	}

#ifdef __x86_64__
	r_regs = (volatile u8 *)(usize)(vmm_direct_map_base() + mmio_phys);
#else
	r_regs = (volatile u8 *)vmm_map_mmio(mmio_phys, 0x1000,
	                                     VMM_WRITABLE | VMM_PCD);
#endif

	console_write("r8169: ");
	console_write_hex32(pci.device_id);
	console_write(" BAR 0x");
	console_write_hex64(mmio_phys);
	console_write("\n");

	r8169_reset();

	for (int i = 0; i < 6; i++)
		r8169_mac.bytes[i] = r_r8(R_IDR0 + i);

	r_w8(R_CFG9346, CFG9346_UNLOCK);      /* unlock config registers */

	r_w16(R_CPCMD, 0x0000);               /* plain frames: no checksum/VLAN offload */
	r_w16(R_RXMAXSZ, R8169_BUF_SZ);
	r_w8(R_MTPS, 0x3F);

	r8169_tx_init();
	r8169_rx_init();

	r_w32(R_TXCFG, TXCFG_VAL);
	r_w32(R_RXCFG, RXCFG_VAL);

	r_w8(R_CMD, CMD_RXE | CMD_TXE);       /* enable Rx + Tx */
	r_w32(R_RXCFG, RXCFG_VAL);            /* some chips want this after enable */

	/* Arm receive interrupts instead of leaving the device purely polled.
	 * Without this the only thing that ever looked at the ring was net_task on
	 * its ~100 Hz tick, so every inbound frame paid up to 10 ms of latency.
	 * ROK/RER deliver packets, RxDU and RxFIFOOver report a ring that ran dry
	 * or overran, and LinkChg reports carrier. The cause register is
	 * write-1-clear and r8169_irq_ack() clears it on every interrupt, which is
	 * what keeps a level-triggered INTx line from re-asserting forever. */
	r_w16(R_ISR, 0xFFFF);
	r_w16(R_IMR, R8169_INTR_MASK);
	r_w8(R_CFG9346, CFG9346_LOCK);        /* re-lock config registers */

	r8169_inited = 1;

	r8169_netdev.name = "r8169";
	r8169_netdev.mac = r8169_mac;
	r8169_netdev.irq = (pci.irq_line == 0xFF) ? -1 : (int)pci.irq_line;
	r8169_netdev.transmit = r8169_transmit;
	r8169_netdev.poll = r8169_poll;
	r8169_netdev.irq_ack = r8169_irq_ack;
	r8169_netdev.link_up = r8169_link_up;
	r8169_netdev.priv = 0;
	netdev_register(&r8169_netdev);

	console_write("r8169: initialized with MAC ");
	r8169_print_mac(r8169_mac);
	console_write("\n");

	if (r8169_netdev.irq >= 0)
		x86_pic_unmask((u16)r8169_netdev.irq);
	return 1;
}
