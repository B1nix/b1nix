/*
 * xHCI (USB 3.x host controller) driver + USB HID boot keyboard (M37).
 *
 * Real hardware has no PS/2 controller, so keystrokes on the metal must come
 * from a USB keyboard. This is a deliberately minimal, polling xHCI driver: it
 * brings up the controller, enumerates a single HID boot-protocol keyboard, and
 * polls its interrupt-IN endpoint for 8-byte boot reports. Each report is
 * translated to PS/2 set-1 make/break scancodes and fed through
 * ps2_kbd_handle_byte(), so USB keys reuse the existing shift/ctrl/signal and
 * line-discipline handling for free.
 *
 * Verified in QEMU with -device qemu-xhci -device usb-kbd. Every hardware wait
 * loop is bounded so an absent/wedged controller can never hang the boot.
 *
 * Scope (intentional): one controller, one keyboard, no hubs, no interrupts
 * (timer-driven polling), no isoch/bulk. Enough for a console on real hardware.
 */
#include <b1nix/console.h>
#include <b1nix/pci.h>
#include <b1nix/mm.h>
#include <b1nix/io.h>
#include <b1nix/sched.h>
#include <b1nix/usb.h>
#include <b1nix/bootinfo.h>
#include <string.h>

/* ── Capability registers ───────────────────────────────────────────────── */
#define XHCI_CAP_CAPLENGTH   0x00  /* u8                                      */
#define XHCI_CAP_HCSPARAMS1  0x04
#define XHCI_CAP_HCSPARAMS2  0x08
#define XHCI_CAP_HCCPARAMS1  0x10
#define XHCI_CAP_DBOFF       0x14
#define XHCI_CAP_RTSOFF      0x18

/* Extended capability IDs (offset from BAR0, linked via HCCPARAMS1.xECP). */
#define XHCI_EXT_LEGACY      1
#define XHCI_LEGACY_BIOS_OWNED (1u << 16)
#define XHCI_LEGACY_OS_OWNED   (1u << 24)

/* ── Operational registers (relative to op base = BAR0 + CAPLENGTH) ─────── */
#define XHCI_OP_USBCMD       0x00
#define XHCI_OP_USBSTS       0x04
#define XHCI_OP_PAGESIZE     0x08
#define XHCI_OP_CRCR         0x18  /* u64 */
#define XHCI_OP_DCBAAP       0x30  /* u64 */
#define XHCI_OP_CONFIG       0x38
#define XHCI_OP_PORTS        0x400 /* PORTSC array, stride 0x10 */

#define USBCMD_RUN   (1u << 0)
#define USBCMD_HCRST (1u << 1)
#define USBSTS_HCH   (1u << 0)   /* HC halted          */
#define USBSTS_CNR   (1u << 11)  /* controller not ready */

#define PORTSC_CCS   (1u << 0)   /* current connect status */
#define PORTSC_PED   (1u << 1)   /* port enabled           */
#define PORTSC_PR    (1u << 4)   /* port reset             */
#define PORTSC_PRC   (1u << 21)  /* port reset change      */
#define PORTSC_SPEED_SHIFT 10
#define PORTSC_SPEED_MASK  0xf
#define PORTSC_RW1C_MASK ((1u << 17) | (1u << 18) | (1u << 19) | \
                          (1u << 20) | (1u << 21) | (1u << 22) | \
                          (1u << 23))

/* ── Runtime / interrupter 0 (relative to rt base = BAR0 + RTSOFF) ──────── */
#define XHCI_IR0             0x20
#define IR_IMAN              0x00
#define IR_ERSTSZ            0x08
#define IR_ERSTBA            0x10  /* u64 */
#define IR_ERDP              0x18  /* u64 */

/* ── TRB types ──────────────────────────────────────────────────────────── */
#define TRB_NORMAL        1
#define TRB_SETUP         2
#define TRB_DATA          3
#define TRB_STATUS        4
#define TRB_LINK          6
#define TRB_ENABLE_SLOT   9
#define TRB_ADDRESS_DEV   11
#define TRB_CONFIG_EP     12
#define TRB_EVT_TRANSFER  32
#define TRB_EVT_CMD_COMP  33
#define TRB_EVT_PORT_CHG  34

#define TRB_CYCLE         (1u << 0)
#define TRB_IOC           (1u << 5)   /* interrupt on completion */
#define TRB_IDT           (1u << 6)   /* immediate data (setup)  */
#define TRB_TYPE_SHIFT    10

#define CC_SUCCESS        1

#define EP_TYPE_CONTROL   4
#define EP_TYPE_INT_IN    7

struct trb { u64 param; u32 status; u32 control; } __attribute__((packed));

#define RING_LEN 16   /* TRBs per ring (small; one report in flight) */
#define XHCI_MMIO_WINDOW 0x10000u

/* ── Driver state ───────────────────────────────────────────────────────── */
static volatile u8 *cap_base;     /* BAR0                       */
static volatile u8 *op_base;      /* operational registers      */
static volatile u8 *rt_base;      /* runtime registers          */
static volatile u32 *db_array;    /* doorbell array             */
static u32 ctx_bytes;             /* 32 or 64 (CSZ)             */
static u32 num_ports;
static int xhci_ready;

static u64 *dcbaa;                /* device context base addr array */
static struct trb *cmd_ring;     u64 cmd_ring_phys; u32 cmd_enq; u32 cmd_cycle;
static struct trb *evt_ring;     u64 evt_ring_phys; u32 evt_deq; u32 evt_cycle;
static u64 *erst;                u64 erst_phys;

/* Enumerated keyboard */
static int kbd_slot = -1;
static int kbd_ep_dci = -1;       /* interrupt-IN endpoint DCI */
static struct trb *ep0_ring;     u64 ep0_ring_phys; u32 ep0_enq; u32 ep0_cycle;
static struct trb *int_ring;     u64 int_ring_phys; u32 int_enq; u32 int_cycle;
static u8 *int_buf;              u64 int_buf_phys;   /* 8-byte report buffer */
static u8 *xfer_buf;            u64 xfer_buf_phys;   /* control-transfer DMA  */
static u8 prev_report[8];

/* ── MMIO helpers ───────────────────────────────────────────────────────── */
static inline u32 rd32(volatile u8 *b, u32 off) { return *(volatile u32 *)(b + off); }
static inline u8 rd8(volatile u8 *b, u32 off) { return *(volatile u8 *)(b + off); }
static inline void wr8(volatile u8 *b, u32 off, u8 v) { *(volatile u8 *)(b + off) = v; }
static inline void wr32(volatile u8 *b, u32 off, u32 v) { *(volatile u32 *)(b + off) = v; }
static inline void wr64(volatile u8 *b, u32 off, u64 v)
{
	/* Two 32-bit writes (low then high) — valid on both 32- and 64-bit and on
	 * controllers that latch on the high dword. */
	*(volatile u32 *)(b + off) = (u32)(v & 0xFFFFFFFF);
	*(volatile u32 *)(b + off + 4) = (u32)(v >> 32);
}

static void udelay(int loops) { for (int i = 0; i < loops; i++) (void)inb(0x80); }

static void xhci_pci_set_d0(const struct pci_device_info *p)
{
	u16 status = pci_config_read16(p->bus, p->slot, p->func, 0x06);
	if (!(status & (1u << 4)))
		return;
	u8 cap = pci_config_read8(p->bus, p->slot, p->func, 0x34) & 0xFC;
	for (int guard = 0; cap && guard < 48; guard++) {
		u8 id = pci_config_read8(p->bus, p->slot, p->func, cap);
		if (id == 0x01) {
			u16 pmcsr = pci_config_read16(p->bus, p->slot, p->func, cap + 4);
			if (pmcsr & 0x3) {
				pci_config_write16(p->bus, p->slot, p->func, cap + 4, pmcsr & ~0x3u);
				udelay(10000);
			}
			return;
		}
		cap = pci_config_read8(p->bus, p->slot, p->func, cap + 1) & 0xFC;
	}
}

static void xhci_legacy_handoff(u32 hcc1)
{
	u32 off = (hcc1 >> 16) & 0xffff;
	for (int guard = 0; off && guard < 64; guard++) {
		u32 cap = rd32(cap_base, off);
		u8 id = (u8)(cap & 0xff);
		u8 next = (u8)((cap >> 8) & 0xff);
		if (id == XHCI_EXT_LEGACY) {
			if (cap & XHCI_LEGACY_BIOS_OWNED) {
				/* The spec requires byte accesses so BIOS-owned and OS-owned
				 * semaphores can be modified independently. */
				wr8(cap_base, off + 3, rd8(cap_base, off + 3) | 0x01);
				for (int i = 0; i < 100000; i++) {
					cap = rd32(cap_base, off);
					if (!(cap & XHCI_LEGACY_BIOS_OWNED))
						break;
					udelay(10);
				}
			}
			/* Disable legacy SMI sources if the BIOS left them armed. */
			wr32(cap_base, off + 4, 0);
			return;
		}
		if (!next)
			break;
		off += (u32)next * 4;
	}
}

/* Allocate one zeroed, page-aligned, physically contiguous frame. */
static void *dma_alloc(u64 *phys_out)
{
	u64 phys = pmm_alloc_frames(1);
	if (!phys) return 0;
	void *v = (void *)(usize)(phys + vmm_direct_map_base());
	memset(v, 0, PAGE_SIZE);
	if (phys_out) *phys_out = phys;
	return v;
}

/* ── Event ring ─────────────────────────────────────────────────────────── *
 * Poll for the next event TRB. Returns 1 and copies it out, or 0 on timeout. */
static int evt_poll(struct trb *out, int timeout_loops)
{
	for (int t = 0; t < timeout_loops; t++) {
		struct trb *e = &evt_ring[evt_deq];
		if ((e->control & TRB_CYCLE) == evt_cycle) {
			*out = *e;
			evt_deq++;
			if (evt_deq == RING_LEN) { evt_deq = 0; evt_cycle ^= 1; }
			/* Advance the dequeue pointer so the controller can reuse slots. */
			wr64(rt_base, XHCI_IR0 + IR_ERDP,
			     (evt_ring_phys + evt_deq * sizeof(struct trb)) | (1u << 3));
			return 1;
		}
		udelay(2);
	}
	return 0;
}

/* Drain the event ring until a TRB of the requested type appears (or timeout).
 * Returns the matching TRB via out. */
static int evt_wait_type(u32 type, struct trb *out, int timeout_loops)
{
	for (int t = 0; t < timeout_loops; t++) {
		struct trb e;
		if (evt_poll(&e, 4000)) {
			u32 et = (e.control >> TRB_TYPE_SHIFT) & 0x3f;
			if (et == type) { *out = e; return 1; }
			/* swallow unrelated events (port change, etc.) */
		}
	}
	return 0;
}

/* ── Command ring ───────────────────────────────────────────────────────── */
static void ring_doorbell(u32 slot, u32 target)
{
	db_array[slot] = target;
}

/* Post a command TRB and wait for its Command Completion Event. Returns the
 * completion code, and the slot id via slot_out (if non-NULL). */
static int cmd_exec(u64 param, u32 status, u32 control_type_flags, int *slot_out)
{
	struct trb *t = &cmd_ring[cmd_enq];
	t->param = param;
	t->status = status;
	t->control = control_type_flags | cmd_cycle;
	__asm__ volatile("" ::: "memory");

	cmd_enq++;
	if (cmd_enq == RING_LEN - 1) {
		/* Link TRB back to the start, toggling cycle. */
		cmd_ring[cmd_enq].param = cmd_ring_phys;
		cmd_ring[cmd_enq].status = 0;
		cmd_ring[cmd_enq].control = (TRB_LINK << TRB_TYPE_SHIFT) | (1u << 1) | cmd_cycle;
		cmd_enq = 0;
		cmd_cycle ^= 1;
	}

	ring_doorbell(0, 0); /* command ring doorbell */

	struct trb e;
	if (!evt_wait_type(TRB_EVT_CMD_COMP, &e, 16))
		return -1;
	if (slot_out)
		*slot_out = (int)((e.control >> 24) & 0xff);
	return (int)((e.status >> 24) & 0xff);
}

/* ── Context accessors (32- or 64-byte contexts) ────────────────────────── */
static u32 *ctx_at(u8 *base, int index)
{
	return (u32 *)(base + (usize)index * ctx_bytes);
}

/* ── Control transfer on EP0 ────────────────────────────────────────────── *
 * Performs a standard SETUP (+ optional IN data) + STATUS sequence. Reads up to
 * len bytes into xfer_buf. Returns 0 on success. */
static int ctrl_xfer(u8 bmRequestType, u8 bRequest, u16 wValue, u16 wIndex,
                     u16 wLength)
{
	u32 c = ep0_cycle;
	int dir_in = (bmRequestType & 0x80) != 0;

	/* Setup stage (immediate data). */
	struct trb *s = &ep0_ring[ep0_enq];
	s->param = (u64)bmRequestType | ((u64)bRequest << 8) | ((u64)wValue << 16) |
	           ((u64)wIndex << 32) | ((u64)wLength << 48);
	s->status = 8; /* TRB transfer length = 8 (setup data) */
	/* TRT: 3=IN data, 2=OUT data, 0=no data */
	u32 trt = wLength ? (dir_in ? 3u : 2u) : 0u;
	s->control = (TRB_SETUP << TRB_TYPE_SHIFT) | TRB_IDT | (trt << 16) | c;
	ep0_enq++;

	/* Data stage (optional). */
	if (wLength) {
		struct trb *d = &ep0_ring[ep0_enq];
		d->param = xfer_buf_phys;
		d->status = wLength;
		d->control = (TRB_DATA << TRB_TYPE_SHIFT) | (dir_in ? (1u << 16) : 0) | c;
		ep0_enq++;
	}

	/* Status stage (opposite direction, IOC). */
	struct trb *st = &ep0_ring[ep0_enq];
	st->param = 0;
	st->status = 0;
	st->control = (TRB_STATUS << TRB_TYPE_SHIFT) | TRB_IOC |
	              ((wLength && dir_in) ? 0 : (1u << 16)) | c;
	ep0_enq++;

	if (ep0_enq >= RING_LEN - 1) {
		ep0_ring[ep0_enq].param = ep0_ring_phys;
		ep0_ring[ep0_enq].status = 0;
		ep0_ring[ep0_enq].control = (TRB_LINK << TRB_TYPE_SHIFT) | (1u << 1) | c;
		ep0_enq = 0;
		ep0_cycle ^= 1;
	}

	__asm__ volatile("" ::: "memory");
	ring_doorbell((u32)kbd_slot, 1); /* EP0 DCI = 1 */

	struct trb e;
	if (!evt_wait_type(TRB_EVT_TRANSFER, &e, 16))
		return -1;
	int cc = (int)((e.status >> 24) & 0xff);
	return (cc == CC_SUCCESS || cc == 13 /* short packet */) ? 0 : -1;
}

/* ── Enumeration ────────────────────────────────────────────────────────── */
static u32 mps0_for_speed(u32 speed)
{
	switch (speed) {
	case 2: return 8;    /* Low Speed  */
	case 1: return 8;    /* Full Speed (8 is always valid; HW splits) */
	case 3: return 64;   /* High Speed */
	case 4: return 512;  /* Super Speed */
	default: return 8;
	}
}

static int find_connected_port(u32 *speed_out)
{
	for (u32 p = 1; p <= num_ports; p++) {
		u32 portsc = rd32(op_base, XHCI_OP_PORTS + (p - 1) * 0x10);
		if (portsc & PORTSC_CCS) {
			if (speed_out)
				*speed_out = (portsc >> PORTSC_SPEED_SHIFT) & PORTSC_SPEED_MASK;
			return (int)p;
		}
	}
	return -1;
}

static int reset_port(u32 port)
{
	u32 off = XHCI_OP_PORTS + (port - 1) * 0x10;
	u32 portsc = rd32(op_base, off);
	/* Preserve state, keep RW1C bits clear unless intentionally clearing PRC. */
	wr32(op_base, off, (portsc & ~PORTSC_RW1C_MASK & ~PORTSC_PED) | PORTSC_PR);
	for (int i = 0; i < 100000; i++) {
		portsc = rd32(op_base, off);
		if (portsc & PORTSC_PRC) {            /* reset complete */
			wr32(op_base, off, (portsc & ~PORTSC_RW1C_MASK) | PORTSC_PRC);
			return (portsc & PORTSC_PED) ? 0 : -1;
		}
		udelay(10);
	}
	return -1;
}

/* Walk a configuration descriptor for the HID interrupt-IN endpoint. Fills
 * *ep_addr / *ep_mps / *ep_interval. Returns 0 on success. */
static int parse_config(const u8 *buf, u16 total, u8 *ep_addr, u16 *ep_mps,
                        u8 *ep_interval)
{
	u16 i = 0;
	int in_hid_kbd = 0;
	while (i + 2 <= total) {
		u8 len = buf[i];
		u8 type = buf[i + 1];
		if (len == 0) break;
		if (type == 0x04 && i + 9 <= total) {           /* INTERFACE */
			u8 cls = buf[i + 5], sub = buf[i + 6], proto = buf[i + 7];
			in_hid_kbd = (cls == 3 /* HID */ &&
			              (proto == 1 /* keyboard */ || sub == 1 /* boot */));
		} else if (type == 0x05 && i + 7 <= total) {     /* ENDPOINT */
			u8 addr = buf[i + 2];
			u8 attr = buf[i + 3];
			if (in_hid_kbd && (addr & 0x80) && (attr & 0x03) == 0x03) { /* int IN */
				*ep_addr = addr;
				*ep_mps = (u16)(buf[i + 4] | (buf[i + 5] << 8));
				*ep_interval = buf[i + 6];
				return 0;
			}
		}
		i += len;
	}
	return -1;
}

static int enumerate_keyboard(void)
{
	u32 speed = 1;
	int port = find_connected_port(&speed);
	if (port < 0) {
		console_write("xhci: no connected port\n");
		return 0;
	}
	if (reset_port((u32)port) != 0) {
		console_write("xhci: port reset failed\n");
		return 0;
	}
	console_write("M37-USB: ok port-reset\n");

	/* Enable Slot. */
	int slot = -1;
	if (cmd_exec(0, 0, (TRB_ENABLE_SLOT << TRB_TYPE_SHIFT), &slot) != CC_SUCCESS ||
	    slot <= 0) {
		console_write("xhci: enable slot failed\n");
		return 0;
	}
	kbd_slot = slot;
	console_write("M37-USB: ok slot-enabled\n");

	/* Device + input contexts, EP0 transfer ring. */
	u64 devctx_phys, inctx_phys;
	u8 *devctx = dma_alloc(&devctx_phys);
	u8 *inctx = dma_alloc(&inctx_phys);
	ep0_ring = dma_alloc(&ep0_ring_phys);
	ep0_enq = 0; ep0_cycle = 1;
	if (!devctx || !inctx || !ep0_ring) return 0;
	dcbaa[slot] = devctx_phys;

	/* Input Control Context: add Slot (bit0) + EP0 (bit1). */
	u32 *icc = ctx_at(inctx, 0);
	icc[1] = (1u << 0) | (1u << 1);

	/* Slot context (index 1): context entries=1, speed, root hub port. */
	u32 *slotc = ctx_at(inctx, 1);
	slotc[0] = (1u << 27) | (speed << 20);   /* Context Entries=1, Speed */
	slotc[1] = ((u32)port << 16);            /* Root Hub Port Number     */

	/* EP0 context (index 2). */
	u32 *ep0 = ctx_at(inctx, 2);
	ep0[1] = (EP_TYPE_CONTROL << 3) | (mps0_for_speed(speed) << 16) | (3u << 1);
	ep0[2] = (u32)((ep0_ring_phys | 1u) & 0xFFFFFFFF); /* TR dequeue + DCS */
	ep0[3] = (u32)(ep0_ring_phys >> 32);

	/* Address Device. */
	if (cmd_exec(inctx_phys, 0, (TRB_ADDRESS_DEV << TRB_TYPE_SHIFT) |
	             ((u32)slot << 24), 0) != CC_SUCCESS) {
		console_write("xhci: address device failed\n");
		return 0;
	}
	console_write("M37-USB: ok device-addressed\n");

	/* GET_DESCRIPTOR(device, 18 bytes). */
	if (ctrl_xfer(0x80, 6, (1 << 8), 0, 18) != 0) {
		console_write("xhci: get device descriptor failed\n");
		return 0;
	}
	console_write("M37-USB: ok descriptors vid=0x");
	console_write_hex32((u32)(xfer_buf[8] | (xfer_buf[9] << 8)));
	console_write(" pid=0x");
	console_write_hex32((u32)(xfer_buf[10] | (xfer_buf[11] << 8)));
	console_write("\n");

	/* GET_DESCRIPTOR(config, 9 bytes) to learn wTotalLength, then full. */
	if (ctrl_xfer(0x80, 6, (2 << 8), 0, 9) != 0)
		return 0;
	u16 total = (u16)(xfer_buf[2] | (xfer_buf[3] << 8));
	if (total > PAGE_SIZE) total = PAGE_SIZE;
	if (ctrl_xfer(0x80, 6, (2 << 8), 0, total) != 0)
		return 0;

	u8 cfg_buf[256];
	u16 clen = total > sizeof(cfg_buf) ? sizeof(cfg_buf) : total;
	memcpy(cfg_buf, xfer_buf, clen);
	u8 cfg_value = cfg_buf[5];

	u8 ep_addr = 0x81; u16 ep_mps = 8; u8 ep_interval = 10;
	if (parse_config(cfg_buf, clen, &ep_addr, &ep_mps, &ep_interval) != 0) {
		console_write("xhci: no HID keyboard endpoint\n");
		return 0;
	}

	/* SET_CONFIGURATION. */
	if (ctrl_xfer(0x00, 9, cfg_value, 0, 0) != 0)
		return 0;

	/* HID class requests on the interface: SET_PROTOCOL(boot=0), SET_IDLE(0). */
	(void)ctrl_xfer(0x21, 0x0B, 0 /* boot */, 0, 0);  /* SET_PROTOCOL */
	(void)ctrl_xfer(0x21, 0x0A, 0 /* idle */, 0, 0);  /* SET_IDLE     */

	/* Configure the interrupt-IN endpoint. DCI = ep_num*2 + 1 (IN). */
	int ep_num = ep_addr & 0x0f;
	int dci = ep_num * 2 + 1;
	kbd_ep_dci = dci;
	int_ring = dma_alloc(&int_ring_phys);
	int_buf = dma_alloc(&int_buf_phys);
	int_enq = 0; int_cycle = 1;
	if (!int_ring || !int_buf) return 0;

	memset(inctx, 0, PAGE_SIZE);
	icc = ctx_at(inctx, 0);
	icc[1] = (1u << 0) | (1u << dci);        /* add slot + this EP */
	slotc = ctx_at(inctx, 1);
	slotc[0] = ((u32)dci << 27) | (speed << 20); /* context entries up to dci */
	slotc[1] = ((u32)port << 16);
	u32 *epc = ctx_at(inctx, dci + 1);
	epc[0] = ((u32)ep_interval << 16);
	epc[1] = (EP_TYPE_INT_IN << 3) | ((u32)ep_mps << 16) | (3u << 1);
	epc[2] = (u32)((int_ring_phys | 1u) & 0xFFFFFFFF);
	epc[3] = (u32)(int_ring_phys >> 32);
	epc[4] = ep_mps;                          /* Average TRB Length */

	if (cmd_exec(inctx_phys, 0, (TRB_CONFIG_EP << TRB_TYPE_SHIFT) |
	             ((u32)slot << 24), 0) != CC_SUCCESS) {
		console_write("xhci: configure endpoint failed\n");
		return 0;
	}

	/* Queue the first interrupt-IN read. */
	struct trb *n = &int_ring[int_enq];
	n->param = int_buf_phys;
	n->status = 8;
	n->control = (TRB_NORMAL << TRB_TYPE_SHIFT) | TRB_IOC | int_cycle;
	int_enq++;
	__asm__ volatile("" ::: "memory");
	ring_doorbell((u32)slot, (u32)dci);

	console_write("M37-USB: ok hid-config ep=0x");
	console_write_hex32(ep_addr);
	console_write("\n");
	return 1;
}

/* ── Probe ──────────────────────────────────────────────────────────────── */
int xhci_probe(void)
{
	struct pci_device_info pci;
	int found = 0;

	if (bootinfo_has_flag("b1nix.skip-xhci")) {
		console_write("xhci: skipped (b1nix.skip-xhci)\n");
		return 0;
	}

	int xhci_run_requested = bootinfo_has_flag("b1nix.xhci.run") ||
	                         bootinfo_has_flag("b1nix.xhci.enum") ||
	                         bootinfo_has_flag("b1nix.test=1");
	if (!xhci_run_requested) {
		/* On metal, touching xHCI and then not running it tears down firmware
		 * USB ownership, which kills USB keyboards. Leave the controller fully
		 * alone unless the caller explicitly opts into the native driver path. */
		return 0;
	}

	for (u8 idx = 0; idx < 16; idx++) {
		if (!pci_find_class(0x0C, 0x03, idx, &pci)) break;
		if (pci.prog_if == 0x30) { found = 1; break; } /* xHCI */
	}
	if (!found)
		return 0;

	xhci_pci_set_d0(&pci);

	u16 cmd = pci_config_read16(pci.bus, pci.slot, pci.func, 0x04);
	cmd |= 0x0006; /* memory space + bus master */
	pci_config_write16(pci.bus, pci.slot, pci.func, 0x04, cmd);

	u32 bar0 = pci_config_read32(pci.bus, pci.slot, pci.func, 0x10);
	u64 mmio;
	if (((bar0 >> 1) & 3) == 2) {
		u32 hi = pci_config_read32(pci.bus, pci.slot, pci.func, 0x14);
		mmio = ((u64)hi << 32) | (bar0 & 0xFFFFFFF0u);
	} else {
		mmio = bar0 & 0xFFFFFFF0u;
	}
	if (!mmio) {
		console_write("xhci: no MMIO BAR, skipping\n");
		return 0;
	}

	/* Use explicit cache-disabled MMIO mappings on all x86 variants. Real xHCI
	 * BARs can place doorbells/runtime registers well past the first 16 KiB, so
	 * map a larger controller window than the minimal QEMU path needed. */
	cap_base = (volatile u8 *)vmm_map_mmio(mmio, XHCI_MMIO_WINDOW,
	                                       VMM_WRITABLE | VMM_PCD);

	u8 caplen = *(volatile u8 *)(cap_base + XHCI_CAP_CAPLENGTH);
	if (caplen < 0x20 || caplen > 0x80) {
		console_write("xhci: invalid caplength 0x");
		console_write_hex32(caplen);
		console_write("\n");
		return 0;
	}
	op_base = cap_base + caplen;
	u32 rtsoff = rd32(cap_base, XHCI_CAP_RTSOFF) & ~0x1fu;
	u32 dboff = rd32(cap_base, XHCI_CAP_DBOFF) & ~0x3u;
	if (rtsoff >= XHCI_MMIO_WINDOW || dboff >= XHCI_MMIO_WINDOW) {
		console_write("xhci: BAR window too small rt=0x");
		console_write_hex32(rtsoff);
		console_write(" db=0x");
		console_write_hex32(dboff);
		console_write("\n");
		return 0;
	}
	rt_base = cap_base + rtsoff;
	db_array = (volatile u32 *)(cap_base + dboff);

	u32 hcs1 = rd32(cap_base, XHCI_CAP_HCSPARAMS1);
	num_ports = (hcs1 >> 24) & 0xff;
	u32 max_mapped_ports = 0;
	if ((u32)caplen + XHCI_OP_PORTS < XHCI_MMIO_WINDOW)
		max_mapped_ports = (XHCI_MMIO_WINDOW - (u32)caplen - XHCI_OP_PORTS) / 0x10;
	if (num_ports > max_mapped_ports) {
		num_ports = max_mapped_ports;
	}
	u32 max_slots = hcs1 & 0xff;
	u32 hcc1 = rd32(cap_base, XHCI_CAP_HCCPARAMS1);
	ctx_bytes = (hcc1 & (1u << 2)) ? 64 : 32;

	xhci_legacy_handoff(hcc1);

	/* Reset the controller. */
	u32 sts = rd32(op_base, XHCI_OP_USBSTS);
	if (!(sts & USBSTS_HCH)) {
		wr32(op_base, XHCI_OP_USBCMD, rd32(op_base, XHCI_OP_USBCMD) & ~USBCMD_RUN);
		int halted = 0;
		for (int i = 0; i < 100000; i++) {
			if (rd32(op_base, XHCI_OP_USBSTS) & USBSTS_HCH) {
				halted = 1;
				break;
			}
			udelay(1);
		}
		if (!halted) {
			console_write("xhci: halt timeout, skipping\n");
			return 0;
		}
	}
	wr32(op_base, XHCI_OP_USBCMD, USBCMD_HCRST);
	int reset_done = 0;
	for (int i = 0; i < 100000; i++) {
		if (!(rd32(op_base, XHCI_OP_USBCMD) & USBCMD_HCRST) &&
		    !(rd32(op_base, XHCI_OP_USBSTS) & USBSTS_CNR)) {
			reset_done = 1;
			break;
		}
		udelay(10);
	}
	if (!reset_done) {
		console_write("xhci: reset timeout, skipping\n");
		return 0;
	}

	/* Program MaxSlotsEnabled. */
	wr32(op_base, XHCI_OP_CONFIG, max_slots);

	/* DCBAA. */
	u64 dcbaa_phys;
	dcbaa = dma_alloc(&dcbaa_phys);
	if (!dcbaa) return 0;
	wr64(op_base, XHCI_OP_DCBAAP, dcbaa_phys);

	/* Command ring. */
	cmd_ring = dma_alloc(&cmd_ring_phys);
	cmd_enq = 0; cmd_cycle = 1;
	if (!cmd_ring) return 0;
	wr64(op_base, XHCI_OP_CRCR, cmd_ring_phys | 1u /* RCS */);

	/* Event ring + ERST (one segment). */
	evt_ring = dma_alloc(&evt_ring_phys);
	erst = dma_alloc(&erst_phys);
	evt_deq = 0; evt_cycle = 1;
	if (!evt_ring || !erst) return 0;
	erst[0] = evt_ring_phys;           /* segment base */
	erst[1] = RING_LEN;                /* segment size (TRB count) */
	wr32(rt_base, XHCI_IR0 + IR_ERSTSZ, 1);
	wr64(rt_base, XHCI_IR0 + IR_ERDP, evt_ring_phys);
	wr64(rt_base, XHCI_IR0 + IR_ERSTBA, erst_phys);

	/* Control-transfer DMA buffer. */
	xfer_buf = dma_alloc(&xfer_buf_phys);
	if (!xfer_buf) return 0;

	/* Run. */
	wr32(op_base, XHCI_OP_USBCMD, rd32(op_base, XHCI_OP_USBCMD) | USBCMD_RUN);
	int running = 0;
	for (int i = 0; i < 100000; i++) {
		if (!(rd32(op_base, XHCI_OP_USBSTS) & USBSTS_HCH)) {
			running = 1;
			break;
		}
		udelay(1);
	}
	if (!running) {
		console_write("xhci: run timeout, skipping\n");
		return 0;
	}

	xhci_ready = 1;
	console_write("xhci: ");
	console_write_hex32(pci.device_id);
	console_write(" ports=");
	console_write_dec(num_ports);
	console_write(" ctx=");
	console_write_dec(ctx_bytes);
	console_write("\n");
	console_write("M37-USB: ok xhci-init\n");

	/* The controller is usable at this point. Device enumeration is opt-in on
	 * metal while we harden the port/slot path; a bad root-port transaction
	 * must not block the rest of boot. */
	if (bootinfo_has_flag("b1nix.xhci.enum") ||
	    bootinfo_has_flag("b1nix.test=1")) {
		for (int i = 0; i < 500 && find_connected_port(0) < 0; i++)
			udelay(1000);
		enumerate_keyboard();
	}
	return 1;
}

/* ── HID boot report → PS/2 set-1 scancodes ─────────────────────────────── */
extern void ps2_kbd_handle_byte(u8 scancode);

/* USB HID Usage (Keyboard/Keypad page 0x07) → PS/2 set-1 make code.
 * 0 = no mapping. Index by HID usage id (0x00..0x65). */
static const u8 hid_to_set1[0x68] = {
	[0x04] = 0x1E, [0x05] = 0x30, [0x06] = 0x2E, [0x07] = 0x20, /* a b c d */
	[0x08] = 0x12, [0x09] = 0x21, [0x0A] = 0x22, [0x0B] = 0x23, /* e f g h */
	[0x0C] = 0x17, [0x0D] = 0x24, [0x0E] = 0x25, [0x0F] = 0x26, /* i j k l */
	[0x10] = 0x32, [0x11] = 0x31, [0x12] = 0x18, [0x13] = 0x19, /* m n o p */
	[0x14] = 0x10, [0x15] = 0x13, [0x16] = 0x1F, [0x17] = 0x14, /* q r s t */
	[0x18] = 0x16, [0x19] = 0x2F, [0x1A] = 0x11, [0x1B] = 0x2D, /* u v w x */
	[0x1C] = 0x15, [0x1D] = 0x2C,                               /* y z     */
	[0x1E] = 0x02, [0x1F] = 0x03, [0x20] = 0x04, [0x21] = 0x05, /* 1 2 3 4 */
	[0x22] = 0x06, [0x23] = 0x07, [0x24] = 0x08, [0x25] = 0x09, /* 5 6 7 8 */
	[0x26] = 0x0A, [0x27] = 0x0B,                               /* 9 0     */
	[0x28] = 0x1C, /* Enter */   [0x29] = 0x01, /* Esc */
	[0x2A] = 0x0E, /* Backsp */  [0x2B] = 0x0F, /* Tab */
	[0x2C] = 0x39, /* Space */   [0x2D] = 0x0C, /* - */
	[0x2E] = 0x0D, /* = */       [0x2F] = 0x1A, /* [ */
	[0x30] = 0x1B, /* ] */       [0x31] = 0x2B, /* \ */
	[0x33] = 0x27, /* ; */       [0x34] = 0x28, /* ' */
	[0x35] = 0x29, /* ` */       [0x36] = 0x33, /* , */
	[0x37] = 0x34, /* . */       [0x38] = 0x35, /* / */
	[0x55] = 0x37, /* KP * */    [0x56] = 0x4A, /* KP - */
	[0x57] = 0x4E, /* KP + */    [0x59] = 0x4F, /* KP 1 */
	[0x5A] = 0x50, /* KP 2 */    [0x5B] = 0x51, /* KP 3 */
	[0x5C] = 0x4B, /* KP 4 */    [0x5D] = 0x4C, /* KP 5 */
	[0x5E] = 0x4D, /* KP 6 */    [0x5F] = 0x47, /* KP 7 */
	[0x60] = 0x48, /* KP 8 */    [0x61] = 0x49, /* KP 9 */
	[0x62] = 0x52, /* KP 0 */    [0x63] = 0x53, /* KP . */
};

/* Extended (E0-prefixed) usages: arrows, etc. Returns set1 code or 0. */
static u8 hid_to_set1_ext(u8 usage)
{
	switch (usage) {
	case 0x54: return 0x35; /* Keypad / */
	case 0x58: return 0x1C; /* Keypad Enter */
	case 0x4F: return 0x4D; /* Right */
	case 0x50: return 0x4B; /* Left  */
	case 0x51: return 0x50; /* Down  */
	case 0x52: return 0x48; /* Up    */
	default:   return 0;
	}
}

static void emit_make(u8 usage)
{
	if (usage >= 4) {
		u8 ext = hid_to_set1_ext(usage);
		if (ext) { ps2_kbd_handle_byte(0xE0); ps2_kbd_handle_byte(ext); return; }
		if (usage < 0x68 && hid_to_set1[usage])
			ps2_kbd_handle_byte(hid_to_set1[usage]);
	}
}

static void emit_break(u8 usage)
{
	if (usage >= 4) {
		u8 ext = hid_to_set1_ext(usage);
		if (ext) { ps2_kbd_handle_byte(0xE0); ps2_kbd_handle_byte(ext | 0x80); return; }
		if (usage < 0x68 && hid_to_set1[usage])
			ps2_kbd_handle_byte(hid_to_set1[usage] | 0x80);
	}
}

/* Modifier bit (report byte 0) → set1 make code, and whether it is E0-extended. */
static const u8 mod_set1[8] = {
	0x1D, /* LCtrl  */ 0x2A, /* LShift */ 0x38, /* LAlt */ 0x5B, /* LGUI (ext) */
	0x1D, /* RCtrl (ext) */ 0x36, /* RShift */ 0x38, /* RAlt (ext) */ 0x5C /* RGUI (ext) */
};
static const u8 mod_ext[8] = { 0, 0, 0, 1, 1, 0, 1, 1 };

void usb_hid_translate_report(const u8 report[8])
{
	u8 prev_mod = prev_report[0];
	u8 cur_mod = report[0];

	/* Modifier transitions. */
	for (int b = 0; b < 8; b++) {
		u8 mask = (u8)(1u << b);
		int was = (prev_mod & mask) != 0, now = (cur_mod & mask) != 0;
		if (now && !was) {
			if (mod_ext[b]) ps2_kbd_handle_byte(0xE0);
			ps2_kbd_handle_byte(mod_set1[b]);
		} else if (!now && was) {
			if (mod_ext[b]) ps2_kbd_handle_byte(0xE0);
			ps2_kbd_handle_byte(mod_set1[b] | 0x80);
		}
	}

	/* Released keys: present in prev (bytes 2..7) but not in current. */
	for (int i = 2; i < 8; i++) {
		u8 u = prev_report[i];
		if (u < 4) continue;
		int still = 0;
		for (int j = 2; j < 8; j++) if (report[j] == u) { still = 1; break; }
		if (!still) emit_break(u);
	}
	/* Newly pressed keys: present in current but not in prev. */
	for (int i = 2; i < 8; i++) {
		u8 u = report[i];
		if (u < 4) continue;
		int had = 0;
		for (int j = 2; j < 8; j++) if (prev_report[j] == u) { had = 1; break; }
		if (!had) emit_make(u);
	}

	memcpy(prev_report, report, 8);
}

/* ── Interrupt-endpoint polling (timer-driven) ──────────────────────────── */
void usb_kbd_poll(void)
{
	if (!xhci_ready || kbd_slot < 0 || kbd_ep_dci < 0)
		return;

	/* Look for a completed interrupt-IN transfer event without blocking. */
	struct trb e;
	if (evt_poll(&e, 1)) {
		u32 et = (e.control >> TRB_TYPE_SHIFT) & 0x3f;
		if (et == TRB_EVT_TRANSFER) {
			usb_hid_translate_report(int_buf);
			/* Re-queue the read. */
			struct trb *n = &int_ring[int_enq];
			n->param = int_buf_phys;
			n->status = 8;
			n->control = (TRB_NORMAL << TRB_TYPE_SHIFT) | TRB_IOC | int_cycle;
			int_enq++;
			if (int_enq >= RING_LEN - 1) {
				int_ring[int_enq].param = int_ring_phys;
				int_ring[int_enq].status = 0;
				int_ring[int_enq].control =
				    (TRB_LINK << TRB_TYPE_SHIFT) | (1u << 1) | int_cycle;
				int_enq = 0;
				int_cycle ^= 1;
			}
			__asm__ volatile("" ::: "memory");
			ring_doorbell((u32)kbd_slot, (u32)kbd_ep_dci);
		}
	}
}

/* ── Self-test ──────────────────────────────────────────────────────────── */
void usb_selftest(void)
{
	if (!xhci_ready) {
		console_write("M37-USB: skip no-controller\n");
		return;
	}
	/* Verify the HID->scancode translation deterministically: a synthetic boot
	 * report for 'a' (usage 0x04) must push 'a' into the keyboard ring. */
	extern char ps2_kbd_getc(void);
	while (ps2_kbd_getc()) { } /* drain */
	memset(prev_report, 0, 8);
	u8 press_a[8] = { 0, 0, 0x04, 0, 0, 0, 0, 0 };
	u8 release[8] = { 0 };
	usb_hid_translate_report(press_a);
	usb_hid_translate_report(release);
	char c = ps2_kbd_getc();
	if (c == 'a')
		console_write("M37-USB: ok hid-translate\n");
	else
		console_write("M37-USB: FAIL hid-translate\n");
}
