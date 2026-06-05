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
#include <b1nix/blk.h>
#include <b1nix/spinlock.h>
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
#define TRB_DISABLE_SLOT  10
#define TRB_ADDRESS_DEV   11
#define TRB_CONFIG_EP     12
#define TRB_EVAL_CONTEXT  13
#define TRB_EVT_TRANSFER  32
#define TRB_EVT_CMD_COMP  33
#define TRB_EVT_PORT_CHG  34

#define TRB_CYCLE         (1u << 0)
#define TRB_IOC           (1u << 5)   /* interrupt on completion */
#define TRB_IDT           (1u << 6)   /* immediate data (setup)  */
#define TRB_TYPE_SHIFT    10

#define CC_SUCCESS        1

#define EP_TYPE_CONTROL   4
#define EP_TYPE_BULK_OUT  2
#define EP_TYPE_BULK_IN   6
#define EP_TYPE_INT_IN    7

struct trb { u64 param; u32 status; u32 control; } __attribute__((packed));

#define RING_LEN 256  /* TRBs per ring (fills exactly one 4KB page) */
#define XHCI_MMIO_WINDOW 0x10000u

struct usb_device {
	int slot;
	u32 speed;
	u32 port;
	struct trb *ep0_ring;
	u64 ep0_ring_phys;
	u32 ep0_enq;
	u32 ep0_cycle;
};

struct usb_msc_device {
	int slot;
	int bulk_in_dci;
	int bulk_out_dci;
	struct trb *in_ring;
	u64 in_ring_phys;
	u32 in_enq;
	u32 in_cycle;
	struct trb *out_ring;
	u64 out_ring_phys;
	u32 out_enq;
	u32 out_cycle;
	u32 block_size;
	u64 block_count;
	struct block_device bdev;
};

struct cbw_struct {
	u32 signature;       /* 0x43425355 = "USBC" */
	u32 tag;             /* Unique tag */
	u32 transfer_len;    /* Number of bytes to transfer */
	u8 flags;            /* Direction (0x80 = IN, 0x00 = OUT) */
	u8 lun;              /* Logical Unit Number (usually 0) */
	u8 cb_len;           /* SCSI command length (1 to 16) */
	u8 cb[16];           /* SCSI command bytes */
} __attribute__((packed));

struct csw_struct {
	u32 signature;       /* 0x53425355 = "USBS" */
	u32 tag;             /* Tag matching the CBW */
	u32 residue;         /* Difference between expected and actual transfer length */
	u8 status;           /* 0 = success, 1 = failure, 2 = phase error */
} __attribute__((packed));

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
static u32 kbd_mps = 8;
static struct trb *ep0_ring;     u64 ep0_ring_phys; u32 ep0_enq; u32 ep0_cycle;
static struct trb *int_ring;     u64 int_ring_phys; u32 int_enq; u32 int_cycle;
static u8 *int_buf;              u64 int_buf_phys;   /* report buffer */
static u8 *xfer_buf;            u64 xfer_buf_phys;   /* control-transfer DMA  */
static u8 prev_report[8];

/* USB Mass Storage state */
static struct usb_msc_device msc_dev;

/* Saved events circular buffer queue */
#define MAX_SAVED_EVENTS 32
static struct trb saved_events[MAX_SAVED_EVENTS];
static int saved_events_head = 0;
static int saved_events_tail = 0;

static void push_saved_event(struct trb *e)
{
	int next = (saved_events_head + 1) % MAX_SAVED_EVENTS;
	if (next != saved_events_tail) {
		saved_events[saved_events_head] = *e;
		saved_events_head = next;
	} else {
		console_write("xhci: saved_events queue overflow!\n");
	}
}

static int pop_saved_event(struct trb *out)
{
	if (saved_events_tail != saved_events_head) {
		*out = saved_events[saved_events_tail];
		saved_events_tail = (saved_events_tail + 1) % MAX_SAVED_EVENTS;
		return 1;
	}
	return 0;
}

static spinlock_t xhci_evt_lock = SPINLOCK_INIT;
static spinlock_t xhci_kbd_lock = SPINLOCK_INIT;

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
	u32 off = ((hcc1 >> 16) & 0xffff) << 2;
	console_write("xhci: xECP byte offset = 0x");
	console_write_hex32(off);
	console_write("\n");
	for (int guard = 0; off && guard < 64; guard++) {
		u32 cap = rd32(cap_base, off);
		u8 id = (u8)(cap & 0xff);
		u8 next = (u8)((cap >> 8) & 0xff);
		console_write("xhci: capability at 0x");
		console_write_hex32(off);
		console_write(" id=");
		console_write_dec(id);
		console_write(" next=");
		console_write_dec(next);
		console_write("\n");
		if (id == XHCI_EXT_LEGACY) {
			if (cap & XHCI_LEGACY_BIOS_OWNED) {
				console_write("xhci: BIOS owns controller, requesting handoff...\n");
				/* The spec requires byte accesses so BIOS-owned and OS-owned
				 * semaphores can be modified independently. */
				wr8(cap_base, off + 3, rd8(cap_base, off + 3) | 0x01);
				int acquired = 0;
				for (int i = 0; i < 100000; i++) {
					cap = rd32(cap_base, off);
					if (!(cap & XHCI_LEGACY_BIOS_OWNED)) {
						acquired = 1;
						break;
					}
					udelay(10);
				}
				if (acquired) {
					console_write("xhci: handoff successful\n");
				} else {
					console_write("xhci: handoff timed out, forcing...\n");
				}
			} else {
				console_write("xhci: OS already owns controller\n");
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

void usb_hid_translate_report(const u8 report[8]);
static void ring_doorbell(u32 slot, u32 target);

static void usb_handle_transfer_event(struct trb *e)
{
	u32 slot = (e->control >> 24) & 0xff;
	u32 dci = (e->control >> 16) & 0x1f;

	if ((int)slot == kbd_slot && (int)dci == kbd_ep_dci) {
		u64 flags;
		spin_lock_irqsave(&xhci_kbd_lock, &flags);
		u8 cc = (u8)((e->status >> 24) & 0xff);
		u32 length = e->status & 0xffffff;

		usb_hid_translate_report(int_buf);
		/* Re-queue the read. */
		struct trb *n = &int_ring[int_enq];
		n->param = int_buf_phys;
		n->status = kbd_mps;
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
		spin_unlock_irqrestore(&xhci_kbd_lock, flags);
	}
}

static int evt_poll_hw(struct trb *out, int timeout_loops)
{
	for (int t = 0; t < timeout_loops; t++) {
		u64 flags;
		spin_lock_irqsave(&xhci_evt_lock, &flags);
		struct trb *e = &evt_ring[evt_deq];
		u32 control = ((volatile struct trb *)e)->control;
		if ((control & TRB_CYCLE) == evt_cycle) {
			__asm__ volatile("lfence" ::: "memory");
			*out = *e;
			evt_deq++;
			if (evt_deq == RING_LEN) { evt_deq = 0; evt_cycle ^= 1; }
			/* Advance the dequeue pointer so the controller can reuse slots. */
			wr64(rt_base, XHCI_IR0 + IR_ERDP,
			     (evt_ring_phys + evt_deq * sizeof(struct trb)) | (1u << 3));
			spin_unlock_irqrestore(&xhci_evt_lock, flags);
			return 1;
		}
		spin_unlock_irqrestore(&xhci_evt_lock, flags);
		udelay(2);
	}
	return 0;
}

/* ── Event ring ─────────────────────────────────────────────────────────── *
 * Poll for the next event TRB. Returns 1 and copies it out, or 0 on timeout. */
static int evt_poll(struct trb *out, int timeout_loops)
{
	u64 flags;
	spin_lock_irqsave(&xhci_evt_lock, &flags);
	if (pop_saved_event(out)) {
		spin_unlock_irqrestore(&xhci_evt_lock, flags);
		return 1;
	}
	spin_unlock_irqrestore(&xhci_evt_lock, flags);
	return evt_poll_hw(out, timeout_loops);
}

/* Drain the event ring until a TRB of the requested type appears (or timeout).
 * Returns the matching TRB via out. */
static int evt_wait_type(u32 type, struct trb *out, int timeout_loops)
{
	for (int t = 0; t < timeout_loops; t++) {
		struct trb e;
		if (evt_poll(&e, 4000)) {
			u32 et = (e.control >> TRB_TYPE_SHIFT) & 0x3f;
			if (et == type) {
				if (et == TRB_EVT_TRANSFER) {
					u32 slot = (e.control >> 24) & 0xff;
					u32 dci = (e.control >> 16) & 0x1f;
					if ((int)slot == kbd_slot && (int)dci == kbd_ep_dci) {
						usb_handle_transfer_event(&e);
						continue;
					}
				}
				*out = e;
				return 1;
			}
			if (et == TRB_EVT_TRANSFER) {
				u32 slot = (e.control >> 24) & 0xff;
				u32 dci = (e.control >> 16) & 0x1f;
				if ((int)slot == kbd_slot && (int)dci == kbd_ep_dci) {
					usb_handle_transfer_event(&e);
				}
			}
		}
	}
	return 0;
}

static int evt_wait_transfer(int target_slot, int target_dci, struct trb *out, int timeout_loops)
{
	for (int t = 0; t < timeout_loops; t++) {
		struct trb e;
		if (evt_poll(&e, 4000)) {
			u32 et = (e.control >> TRB_TYPE_SHIFT) & 0x3f;
			u32 slot = (e.control >> 24) & 0xff;
			u32 dci = (e.control >> 16) & 0x1f;
			u8 cc = (u8)((e.status >> 24) & 0xff);
			/* Log all events — verbose but essential for real-HW diagnosis. */
			console_write("xhci: event type=");
			console_write_dec(et);
			console_write(" slot=");
			console_write_dec(slot);
			console_write(" dci=");
			console_write_dec(dci);
			console_write(" cc=");
			console_write_dec(cc);
			console_write("\n");
			if (et == TRB_EVT_TRANSFER) {
				if ((int)slot == target_slot && (int)dci == target_dci) {
					*out = e;
					return 1;
				}
				if ((int)slot == kbd_slot && (int)dci == kbd_ep_dci) {
					usb_handle_transfer_event(&e);
				}
				/* Stash unexpected transfer events so they are not lost.
				 * On real hardware, the controller may deliver events for
				 * endpoints we are not currently waiting on (e.g. old slot
				 * residue). Saving them prevents event-ring desync. */
				push_saved_event(&e);
			}
		}
	}
	return 0;
}

/* ── Command ring ───────────────────────────────────────────────────────── */
static void ring_doorbell(u32 slot, u32 target)
{
	__asm__ volatile("sfence" ::: "memory");
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
	/* 32 outer loops × ≤8 ms each = ≤256 ms; enough for slow real-HW controllers. */
	if (!evt_wait_type(TRB_EVT_CMD_COMP, &e, 32))
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
static int ctrl_xfer(struct usb_device *dev, u8 bmRequestType, u8 bRequest, u16 wValue, u16 wIndex,
                     u16 wLength)
{
	u32 c = dev->ep0_cycle;
	int dir_in = (bmRequestType & 0x80) != 0;

	/* Setup stage (immediate data). */
	struct trb *s = &dev->ep0_ring[dev->ep0_enq];
	s->param = (u64)bmRequestType | ((u64)bRequest << 8) | ((u64)wValue << 16) |
	           ((u64)wIndex << 32) | ((u64)wLength << 48);
	s->status = 8; /* TRB transfer length = 8 (setup data) */
	/* TRT: 3=IN data, 2=OUT data, 0=no data */
	u32 trt = wLength ? (dir_in ? 3u : 2u) : 0u;
	s->control = (TRB_SETUP << TRB_TYPE_SHIFT) | TRB_IDT | (trt << 16) | c;
	dev->ep0_enq++;

	/* Data stage (optional). */
	if (wLength) {
		struct trb *d = &dev->ep0_ring[dev->ep0_enq];
		d->param = xfer_buf_phys;
		d->status = wLength;
		d->control = (TRB_DATA << TRB_TYPE_SHIFT) | (dir_in ? (1u << 16) : 0) | c;
		dev->ep0_enq++;
	}

	/* Status stage (opposite direction, IOC). */
	struct trb *st = &dev->ep0_ring[dev->ep0_enq];
	st->param = 0;
	st->status = 0;
	st->control = (TRB_STATUS << TRB_TYPE_SHIFT) | TRB_IOC |
	              ((wLength && dir_in) ? 0 : (1u << 16)) | c;
	dev->ep0_enq++;

	if (dev->ep0_enq >= RING_LEN - 1) {
		dev->ep0_ring[dev->ep0_enq].param = dev->ep0_ring_phys;
		dev->ep0_ring[dev->ep0_enq].status = 0;
		dev->ep0_ring[dev->ep0_enq].control = (TRB_LINK << TRB_TYPE_SHIFT) | (1u << 1) | c;
		dev->ep0_enq = 0;
		dev->ep0_cycle ^= 1;
	}

	__asm__ volatile("" ::: "memory");
	ring_doorbell((u32)dev->slot, 1); /* EP0 DCI = 1 */

	struct trb e;
	/* 32 outer loops × ≤8 ms each = ≤256 ms; enough for slow real-HW devices. */
	if (!evt_wait_transfer(dev->slot, 1, &e, 32))
		return -1;
	int cc = (int)((e.status >> 24) & 0xff);
	return (cc == CC_SUCCESS || cc == 13 /* short packet */) ? 0 : -1;
}

/* ── Enumeration ────────────────────────────────────────────────────────── */
static u32 mps0_for_speed(u32 speed)
{
	switch (speed) {
	case 2: return 8;    /* Low Speed  */
	case 1: return 8;    /* Full Speed starts at 8 until bMaxPacketSize0 is read. */
	case 3: return 64;   /* High Speed */
	case 4: return 512;  /* Super Speed */
	default: return 8;
	}
}

static void prepare_address_context(u8 *devctx, u8 *inctx, struct usb_device *dev)
{
	memset(devctx, 0, PAGE_SIZE);
	memset(inctx, 0, PAGE_SIZE);
	memset(dev->ep0_ring, 0, PAGE_SIZE);
	dev->ep0_enq = 0;
	dev->ep0_cycle = 1;

	/* Input Control Context: add Slot (bit0) + EP0 (bit1). */
	u32 *icc = ctx_at(inctx, 0);
	icc[1] = (1u << 0) | (1u << 1);

	/* Slot context (index 1): context entries=1, speed, root hub port. */
	u32 *slotc = ctx_at(inctx, 1);
	slotc[0] = (1u << 27) | (dev->speed << 20);
	slotc[1] = (dev->port << 16);

	/* EP0 context (index 2). */
	u32 *ep0 = ctx_at(inctx, 2);
	ep0[1] = (EP_TYPE_CONTROL << 3) | (mps0_for_speed(dev->speed) << 16) | (3u << 1);
	ep0[2] = (u32)((dev->ep0_ring_phys | 1u) & 0xFFFFFFFF);
	ep0[3] = (u32)(dev->ep0_ring_phys >> 32);
	ep0[4] = 8;
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
			/* Clear PRC (and only PRC) — write 0 to other RW1C bits to preserve them. */
			wr32(op_base, off, (portsc & ~PORTSC_RW1C_MASK) | PORTSC_PRC);
			/* Log PLS (Port Link State) for real-HW diagnostics.
			 * PLS is bits [8:5]. After reset: 0=U0 (OK), 5=Rx.Detect (device missing),
			 * 7=Polling (SS link training in progress). */
			u32 pls = (portsc >> 5) & 0xf;
			console_write("xhci: port reset done pls=");
			console_write_dec(pls);
			console_write(" ped=");
			console_write_dec((portsc & PORTSC_PED) ? 1 : 0);
			console_write("\n");
			return (portsc & PORTSC_PED) ? 0 : -1;
		}
		udelay(10);
	}
	return -1;
}

static int bulk_xfer(struct usb_msc_device *msc, int is_in, void *buf, u64 buf_phys, u32 len)
{
	(void)buf;
	int dci = is_in ? msc->bulk_in_dci : msc->bulk_out_dci;
	struct trb *ring = is_in ? msc->in_ring : msc->out_ring;
	u64 ring_phys = is_in ? msc->in_ring_phys : msc->out_ring_phys;
	u32 *enq_ptr = is_in ? &msc->in_enq : &msc->out_enq;
	u32 *cycle_ptr = is_in ? &msc->in_cycle : &msc->out_cycle;

	u32 c = *cycle_ptr;
	struct trb *t = &ring[*enq_ptr];
	t->param = buf_phys;
	t->status = len;
	t->control = (TRB_NORMAL << TRB_TYPE_SHIFT) | TRB_IOC | c;
	(*enq_ptr)++;

	if (*enq_ptr >= RING_LEN - 1) {
		ring[*enq_ptr].param = ring_phys;
		ring[*enq_ptr].status = 0;
		ring[*enq_ptr].control = (TRB_LINK << TRB_TYPE_SHIFT) | (1u << 1) | c;
		*enq_ptr = 0;
		*cycle_ptr ^= 1;
	}

	__asm__ volatile("" ::: "memory");
	ring_doorbell((u32)msc->slot, (u32)dci);

	struct trb e;
	if (!evt_wait_transfer(msc->slot, dci, &e, 2000)) {
		console_write("msc: bulk xfer timeout\n");
		return -1;
	}

	int cc = (int)((e.status >> 24) & 0xff);
	if (cc != CC_SUCCESS && cc != 13 /* short packet */) {
		console_write("msc: bulk xfer error cc=");
		console_write_dec(cc);
		console_write("\n");
		return -1;
	}

	u32 residue = e.status & 0xffffff;
	return (int)(len - residue);
}

static int msc_exec_cmd(struct usb_msc_device *msc, const void *scsi_cmd, u8 scsi_cmd_len,
                        int is_in, void *data, u32 data_len)
{
	static u32 global_tag = 1;
	u32 tag = global_tag++;

	/* 1. Build and send CBW */
	struct cbw_struct *cbw = (struct cbw_struct *)xfer_buf;
	memset(cbw, 0, sizeof(*cbw));
	cbw->signature = 0x43425355; /* "USBC" */
	cbw->tag = tag;
	cbw->transfer_len = data_len;
	cbw->flags = is_in ? 0x80 : 0x00;
	cbw->lun = 0;
	cbw->cb_len = scsi_cmd_len;
	memcpy(cbw->cb, scsi_cmd, scsi_cmd_len);

	int ret = bulk_xfer(msc, 0, cbw, xfer_buf_phys, 31);
	if (ret != 31) {
		console_write("msc: cbw send failed\n");
		return -1;
	}

	/* 2. Optional Data Stage */
	if (data_len > 0) {
		if (data_len > PAGE_SIZE) {
			console_write("msc: data_len exceeds bounce buffer\n");
			return -1;
		}
		if (!is_in) {
			memcpy(xfer_buf, data, data_len);
		}
		ret = bulk_xfer(msc, is_in, xfer_buf, xfer_buf_phys, data_len);
		if (ret < 0) {
			console_write("msc: data transfer failed\n");
			return -1;
		}
		if (is_in) {
			memcpy(data, xfer_buf, data_len);
		}
	}

	/* 3. Send/Receive CSW */
	ret = bulk_xfer(msc, 1, xfer_buf, xfer_buf_phys, 13);
	if (ret != 13) {
		console_write("msc: csw read failed\n");
		return -1;
	}

	struct csw_struct *csw = (struct csw_struct *)xfer_buf;
	if (csw->signature != 0x53425355) {
		console_write("msc: invalid csw signature 0x");
		console_write_hex32(csw->signature);
		console_write("\n");
		return -1;
	}
	if (csw->tag != tag) {
		console_write("msc: csw tag mismatch\n");
		return -1;
	}
	if (csw->status != 0) {
		console_write("msc: csw status error status=");
		console_write_dec(csw->status);
		console_write("\n");
		return -1;
	}

	return 0;
}

static int msc_inquiry(struct usb_msc_device *msc)
{
	u8 cmd[6] = { 0x12, 0, 0, 0, 36, 0 };
	u8 data[36];
	for (int retry = 0; retry < 5; retry++) {
		if (msc_exec_cmd(msc, cmd, 6, 1, data, 36) == 0) {
			return 0;
		}
		udelay(10000);
	}
	return -1;
}

static int msc_read_capacity(struct usb_msc_device *msc)
{
	u8 cmd[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
	u8 data[8];
	for (int retry = 0; retry < 5; retry++) {
		if (msc_exec_cmd(msc, cmd, 10, 1, data, 8) == 0) {
			u32 max_lba = ((u32)data[0] << 24) | ((u32)data[1] << 16) |
			              ((u32)data[2] << 8) | (u32)data[3];
			u32 blk_len = ((u32)data[4] << 24) | ((u32)data[5] << 16) |
			              ((u32)data[6] << 8) | (u32)data[7];
			msc->block_size = blk_len;
			msc->block_count = (u64)max_lba + 1;
			return 0;
		}
		udelay(10000);
	}
	return -1;
}

static int msc_read_blocks(struct block_device *dev, u64 lba, u32 count, void *buffer)
{
	struct usb_msc_device *msc = (struct usb_msc_device *)dev->priv;
	u32 max_blocks = PAGE_SIZE / msc->block_size;
	if (max_blocks == 0) max_blocks = 1;

	u8 *ptr = (u8 *)buffer;
	u64 current_lba = lba;
	u32 remaining = count;

	while (remaining > 0) {
		u32 chunk = remaining > max_blocks ? max_blocks : remaining;
		u8 cmd[10] = {
			0x28, 0,
			(u8)(current_lba >> 24), (u8)(current_lba >> 16),
			(u8)(current_lba >> 8), (u8)current_lba,
			0,
			(u8)(chunk >> 8), (u8)chunk,
			0
		};
		int ok = 0;
		for (int retry = 0; retry < 3; retry++) {
			if (msc_exec_cmd(msc, cmd, 10, 1, ptr, chunk * msc->block_size) == 0) {
				ok = 1;
				break;
			}
			udelay(5000);
		}
		if (!ok) {
			console_write("msc: read blocks failed lba=");
			console_write_dec(current_lba);
			console_write("\n");
			return -1;
		}
		ptr += chunk * msc->block_size;
		current_lba += chunk;
		remaining -= chunk;
	}
	return 0;
}

static int msc_write_blocks(struct block_device *dev, u64 lba, u32 count, const void *buffer)
{
	struct usb_msc_device *msc = (struct usb_msc_device *)dev->priv;
	u32 max_blocks = PAGE_SIZE / msc->block_size;
	if (max_blocks == 0) max_blocks = 1;

	const u8 *ptr = (const u8 *)buffer;
	u64 current_lba = lba;
	u32 remaining = count;

	while (remaining > 0) {
		u32 chunk = remaining > max_blocks ? max_blocks : remaining;
		u8 cmd[10] = {
			0x2A, 0,
			(u8)(current_lba >> 24), (u8)(current_lba >> 16),
			(u8)(current_lba >> 8), (u8)current_lba,
			0,
			(u8)(chunk >> 8), (u8)chunk,
			0
		};
		int ok = 0;
		for (int retry = 0; retry < 3; retry++) {
			if (msc_exec_cmd(msc, cmd, 10, 0, (void *)ptr, chunk * msc->block_size) == 0) {
				ok = 1;
				break;
			}
			udelay(5000);
		}
		if (!ok) {
			console_write("msc: write blocks failed lba=");
			console_write_dec(current_lba);
			console_write("\n");
			return -1;
		}
		ptr += chunk * msc->block_size;
		current_lba += chunk;
		remaining -= chunk;
	}
	return 0;
}

static u32 xhci_interval_val(u32 speed, u32 bInterval)
{
	if (speed == 3 || speed == 4) {
		/* High Speed or Super Speed */
		if (bInterval < 1) bInterval = 1;
		if (bInterval > 16) bInterval = 16;
		return bInterval - 1;
	} else {
		/* Low Speed or Full Speed */
		if (bInterval < 1) bInterval = 1;
		if (bInterval > 255) bInterval = 255;
		u32 val = bInterval;
		u32 r = 0;
		while (val > 1) {
			val >>= 1;
			r++;
		}
		u32 interval = r + 3;
		if (interval < 3) interval = 3;
		if (interval > 11) interval = 11;
		return interval;
	}
}

static void usb_probe_port(u32 port, u32 speed)
{
	console_write("xhci: probe port ");
	console_write_dec(port);
	console_write(" speed=");
	console_write_dec(speed);
	console_write("\n");

	if (reset_port(port) != 0) {
		console_write("xhci: port reset failed\n");
		return;
	}
	console_write("xhci: port ");
	console_write_dec(port);
	console_write(" reset ok\n");
	udelay(300000); /* 300 ms recovery delay after reset (TRSTRCY) */

	console_write("xhci: enable slot...\n");
	int slot = -1;
	int cmd_res = cmd_exec(0, 0, (TRB_ENABLE_SLOT << TRB_TYPE_SHIFT), &slot);
	if (cmd_res != CC_SUCCESS || slot <= 0) {
		console_write("xhci: enable slot failed res=");
		console_write_dec(cmd_res);
		console_write(" slot=");
		console_write_dec(slot);
		console_write("\n");
		return;
	}
	console_write("xhci: slot ");
	console_write_dec(slot);
	console_write(" enabled\n");

	struct usb_device dev;
	memset(&dev, 0, sizeof(dev));
	dev.slot = slot;
	dev.speed = speed;
	dev.port = port;
	dev.ep0_ring = dma_alloc(&dev.ep0_ring_phys);
	dev.ep0_enq = 0; dev.ep0_cycle = 1;

	u64 devctx_phys, inctx_phys;
	u8 *devctx = dma_alloc(&devctx_phys);
	u8 *inctx = dma_alloc(&inctx_phys);
	if (!dev.ep0_ring || !devctx || !inctx) return;

	dcbaa[slot] = devctx_phys;
	prepare_address_context(devctx, inctx, &dev);

	console_write("xhci: address device (step 1: BSR=1)...\n");
	int addr_res = -1;
	int use_two_step = 1;

	/* Step 1: Address Device command with BSR = 1 */
	for (int retry = 0; retry < 5; retry++) {
		addr_res = cmd_exec(inctx_phys, 0, (TRB_ADDRESS_DEV << TRB_TYPE_SHIFT) |
		                    ((u32)slot << 24) | (1u << 9), 0);
		if (addr_res == CC_SUCCESS) break;

		console_write("xhci: address device BSR=1 failed, retrying... res=");
		console_write_dec(addr_res);
		console_write("\n");
		udelay(100000); // 100 ms recovery delay
	}

	if (addr_res != CC_SUCCESS) {
		console_write("xhci: address device BSR=1 failed, falling back to direct addressing\n");
		use_two_step = 0;
	}

	u32 actual_mps = mps0_for_speed(speed);

	if (use_two_step) {
		/* Short pause after BSR=1 Address Device before the first control
		 * transfer: some real devices (especially USB3 SS) need a few ms
		 * after being addressed before EP0 is ready to accept transfers. */
		udelay(5000); /* 5 ms */

		/* Step 2: Retrieve the first 8 bytes of the Device Descriptor to find the actual bMaxPacketSize0 */
		console_write("xhci: get initial device descriptor (8 bytes)...\n");
		if (ctrl_xfer(&dev, 0x80, 6, (1 << 8), 0, 8) == 0) {
			actual_mps = xfer_buf[7];
			console_write("xhci: read bMaxPacketSize0 = ");
			console_write_dec(actual_mps);
			console_write("\n");

			/* USB 3.x spec Table 9-8: for SuperSpeed devices bMaxPacketSize0
			 * is an exponent n such that MPS = 2^n (e.g. 9 → 512).  Decode
			 * it before the validity check so we never log "invalid". */
			if (speed == 4 && actual_mps <= 16) {
				actual_mps = 1u << actual_mps; /* e.g. 9 → 512 */
				console_write("xhci: SS exponent decoded MPS0=");
				console_write_dec(actual_mps);
				console_write("\n");
			}

			/* Validate bMaxPacketSize0 is reasonable for EP0 (8, 16, 32, 64, or 512 for SS) */
			if (actual_mps != 8 && actual_mps != 16 && actual_mps != 32 && actual_mps != 64 && actual_mps != 512) {
				console_write("xhci: invalid bMaxPacketSize0=");
				console_write_dec(actual_mps);
				console_write(", using speed default\n");
				actual_mps = mps0_for_speed(speed);
			}
		} else {
			console_write("xhci: get initial device descriptor failed, falling back to direct addressing\n");
			use_two_step = 0;

			/* Clean up the slot & port state for the fallback attempt */
			(void)cmd_exec(0, 0, (TRB_DISABLE_SLOT << TRB_TYPE_SHIFT) | ((u32)slot << 24), 0);
			if (reset_port(port) != 0) {
				console_write("xhci: fallback port reset failed\n");
				return;
			}
			udelay(300000);
			slot = -1;
			if (cmd_exec(0, 0, (TRB_ENABLE_SLOT << TRB_TYPE_SHIFT), &slot) != CC_SUCCESS || slot <= 0) {
				console_write("xhci: fallback enable slot failed\n");
				return;
			}
			/* Re-link context to new slot */
			dcbaa[slot] = devctx_phys;
			dev.slot = slot;
			prepare_address_context(devctx, inctx, &dev);
		}
	}

	if (use_two_step) {
		/* Step 3: Update the EP0 context with the actual MaxPacketSize and evaluate the context */
		console_write("xhci: evaluate context with actual MPS0=");
		console_write_dec(actual_mps);
		console_write("...\n");

		/* Evaluate Context only needs EP0 in the Add context (A1).
		 * Per xHCI spec §4.6.7, the Slot context (A0) is not required here.
		 * Clear A0 to avoid confusing strict real-HW controllers. */
		u32 *icc_eval = ctx_at(inctx, 0);
		icc_eval[1] = (1u << 1); /* A1=EP0 only, A0=Slot cleared */

		/* EP0 context (index 2): Update MPS0 and Average TRB Length.
		 * Average TRB Length (DW4 bits[15:0]) should match MPS0 for control EPs. */
		u32 *ep0 = ctx_at(inctx, 2);
		ep0[1] = (ep0[1] & ~(0xFFFFu << 16)) | (actual_mps << 16);
		ep0[4] = actual_mps; /* Average TRB Length = MPS0 */

		int eval_res = cmd_exec(inctx_phys, 0, (TRB_EVAL_CONTEXT << TRB_TYPE_SHIFT) |
		                        ((u32)slot << 24), 0);

		/* MUST restore A0+A1 before Address Device BSR=0: that command requires
		 * both Slot (A0) and EP0 (A1) in the Add context or the controller
		 * returns CC_CONTEXT_STATE_ERROR (cc=5). */
		icc_eval[1] = (1u << 0) | (1u << 1);
		if (eval_res != CC_SUCCESS) {
			console_write("xhci: evaluate context failed res=");
			console_write_dec(eval_res);
			console_write("\n");
			/* We will still try to issue Address Device BSR=0 as best effort */
		}

		/* Step 4: Issue Address Device command with BSR = 0 */
		console_write("xhci: address device (step 2: BSR=0)...\n");
		for (int retry = 0; retry < 5; retry++) {
			addr_res = cmd_exec(inctx_phys, 0, (TRB_ADDRESS_DEV << TRB_TYPE_SHIFT) |
			                    ((u32)slot << 24), 0);
			if (addr_res == CC_SUCCESS) break;

			console_write("xhci: address device BSR=0 failed, retrying... res=");
			console_write_dec(addr_res);
			console_write("\n");
			udelay(100000); // 100 ms recovery delay
		}
	} else {
		/* Fallback: Direct BSR = 0 addressing */
		console_write("xhci: direct addressing (BSR=0)...\n");

		/* EP0 context (index 2): Set default MPS0 in case it was changed */
		u32 *ep0 = ctx_at(inctx, 2);
		ep0[1] = (ep0[1] & ~(0xFFFFu << 16)) | (mps0_for_speed(speed) << 16);

		for (int retry = 0; retry < 5; retry++) {
			addr_res = cmd_exec(inctx_phys, 0, (TRB_ADDRESS_DEV << TRB_TYPE_SHIFT) |
			                    ((u32)slot << 24), 0);
			if (addr_res == CC_SUCCESS) break;

			console_write("xhci: direct address device failed, retrying... res=");
			console_write_dec(addr_res);
			console_write("\n");
			udelay(100000);
		}
	}

	if (addr_res != CC_SUCCESS) {
		u32 ps = rd32(op_base, XHCI_OP_PORTS + (port - 1) * 0x10);
		console_write("xhci: address device failed res=");
		console_write_dec(addr_res);
		console_write(" portsc=0x");
		console_write_hex32(ps);
		console_write("\n");

		/* Disable slot on failure to free resources */
		(void)cmd_exec(0, 0, (TRB_DISABLE_SLOT << TRB_TYPE_SHIFT) | ((u32)slot << 24), 0);
		return;
	}
	console_write("xhci: device addressed\n");

	/* GET_DESCRIPTOR(device, 18 bytes). */
	console_write("xhci: get device descriptor...\n");
	if (ctrl_xfer(&dev, 0x80, 6, (1 << 8), 0, 18) != 0) {
		console_write("xhci: get device descriptor failed\n");
		return;
	}

	u16 vid = (u16)(xfer_buf[8] | (xfer_buf[9] << 8));
	u16 pid = (u16)(xfer_buf[10] | (xfer_buf[11] << 8));
	console_write("xhci: got descriptor vid=0x");
	console_write_hex32(vid);
	console_write(" pid=0x");
	console_write_hex32(pid);
	console_write("\n");

	/* GET_DESCRIPTOR(config, 9) to learn wTotalLength, then full. */
	console_write("xhci: get config descriptor...\n");
	if (ctrl_xfer(&dev, 0x80, 6, (2 << 8), 0, 9) != 0) {
		console_write("xhci: get config descriptor (9 bytes) failed\n");
		return;
	}
	u16 total = (u16)(xfer_buf[2] | (xfer_buf[3] << 8));
	if (total > PAGE_SIZE) total = PAGE_SIZE;
	if (ctrl_xfer(&dev, 0x80, 6, (2 << 8), 0, total) != 0) {
		console_write("xhci: get config descriptor (full) failed\n");
		return;
	}
	console_write("xhci: got config descriptor len=");
	console_write_dec(total);
	console_write("\n");

	/* Parse config. We can scan for interface type. */
	u8 cfg_val = xfer_buf[5];
	u16 i = 0;
	u8 cur_is_msc = 0;
	u8 cur_is_kbd = 0;
	u8 has_msc = 0;
	u8 has_kbd = 0;
	u8 ep_in_addr = 0, ep_out_addr = 0;
	u16 ep_in_mps = 0, ep_out_mps = 0;
	u8 ep_in_burst = 0, ep_out_burst = 0;
	u8 last_ep_is_in = 0;
	u8 kbd_ep_addr = 0, kbd_ep_interval = 0;
	u16 kbd_ep_mps = 0;

	/* Debug: Print config descriptor raw bytes. */
	console_write("xhci: config descriptor raw bytes: ");
	for (u16 idx = 0; idx < total; idx++) {
		console_write_hex32(xfer_buf[idx]);
		console_write(" ");
	}
	console_write("\n");

	while (i + 2 <= total) {
		u8 len = xfer_buf[i];
		u8 type = xfer_buf[i + 1];
		if (len == 0) break;
		if (type == 0x04 && i + 9 <= total) {           /* INTERFACE */
			u8 iclass = xfer_buf[i + 5];
			u8 ialt = xfer_buf[i + 3];
			u8 iproto = xfer_buf[i + 7];
			cur_is_kbd = (iclass == 3 && iproto == 1);
			cur_is_msc = (iclass == 8 && iproto == 0x50 && ialt == 0);
			if (cur_is_kbd) has_kbd = 1;
			if (cur_is_msc) has_msc = 1;
		} else if (type == 0x05 && i + 7 <= total) {     /* ENDPOINT */
			u8 addr = xfer_buf[i + 2];
			u8 attr = xfer_buf[i + 3];
			u16 mps = (u16)(xfer_buf[i + 4] | (xfer_buf[i + 5] << 8));
			u8 interval = xfer_buf[i + 6];

			if (cur_is_kbd) { /* HID */
				if ((addr & 0x80) && (attr & 0x03) == 0x03) { /* int IN */
					kbd_ep_addr = addr;
					kbd_ep_mps = mps;
					kbd_ep_interval = interval;
				}
			} else if (cur_is_msc) { /* Bulk-Only Transport Mass Storage */
				if ((attr & 0x03) == 0x02) { /* Bulk */
					if (addr & 0x80) {
						ep_in_addr = addr;
						ep_in_mps = mps;
						last_ep_is_in = 1;
					} else {
						ep_out_addr = addr;
						ep_out_mps = mps;
						last_ep_is_in = 0;
					}
				}
			}
		} else if (type == 0x30 && len >= 6 && i + 6 <= total) { /* SS Companion */
			u8 max_burst = xfer_buf[i + 2];
			if (cur_is_msc) {
				if (last_ep_is_in) ep_in_burst = max_burst;
				else ep_out_burst = max_burst;
			}
		}
		i += len;
	}

	if (has_kbd && kbd_ep_addr != 0) {
		/* It is a keyboard! */
		kbd_slot = slot;
		kbd_mps = kbd_ep_mps;
		if (kbd_mps < 8) kbd_mps = 8;
		ep0_ring = dev.ep0_ring;
		ep0_ring_phys = dev.ep0_ring_phys;
		ep0_enq = dev.ep0_enq;
		ep0_cycle = dev.ep0_cycle;

		console_write("M37-USB: ok port-reset\n");
		console_write("M37-USB: ok slot-enabled\n");
		console_write("M37-USB: ok device-addressed\n");
		console_write("M37-USB: ok descriptors vid=0x");
		console_write_hex32(vid);
		console_write(" pid=0x");
		console_write_hex32(pid);
		console_write("\n");

		int dci = (kbd_ep_addr & 0x0f) * 2 + 1;
		kbd_ep_dci = dci;
		int_ring = dma_alloc(&int_ring_phys);
		int_buf = dma_alloc(&int_buf_phys);
		int_enq = 0; int_cycle = 1;
		if (!int_ring || !int_buf) return;

		memset(inctx, 0, PAGE_SIZE);
		u32 *icc = ctx_at(inctx, 0);
		icc[1] = (1u << 0) | (1u << dci);        /* slot + this EP */
		u32 *slotc = ctx_at(inctx, 1);
		slotc[0] = ((u32)dci << 27) | (speed << 20);
		slotc[1] = (port << 16);
		u32 *epc = ctx_at(inctx, dci + 1);
		u32 interval_val = xhci_interval_val(speed, kbd_ep_interval);
		epc[0] = (interval_val << 16);
		epc[1] = (EP_TYPE_INT_IN << 3) | ((u32)kbd_ep_mps << 16) | (3u << 1); /* CErr = 3 */
		epc[2] = (u32)((int_ring_phys | 1u) & 0xFFFFFFFF);
		epc[3] = (u32)(int_ring_phys >> 32);
		epc[4] = kbd_ep_mps | ((u32)kbd_ep_mps << 16); /* Average TRB len & Max ESIT payload */

		if (cmd_exec(inctx_phys, 0, (TRB_CONFIG_EP << TRB_TYPE_SHIFT) |
		             ((u32)slot << 24), 0) != CC_SUCCESS) {
			console_write("xhci: configure endpoint failed\n");
			return;
		}

		/* SET_CONFIGURATION. */
		if (ctrl_xfer(&dev, 0x00, 9, cfg_val, 0, 0) != 0) {
			console_write("xhci: set config failed for keyboard\n");
			return;
		}

		udelay(10000); /* 10 ms settle delay */

		/* HID class requests: SET_PROTOCOL(boot=0), SET_IDLE(0). */
		(void)ctrl_xfer(&dev, 0x21, 0x0B, 0, 0, 0);
		(void)ctrl_xfer(&dev, 0x21, 0x0A, 0, 0, 0);

		/* Queue keyboard read. */
		struct trb *n = &int_ring[int_enq];
		n->param = int_buf_phys;
		n->status = kbd_mps;
		n->control = (TRB_NORMAL << TRB_TYPE_SHIFT) | TRB_IOC | int_cycle;
		int_enq++;
		__asm__ volatile("" ::: "memory");
		ring_doorbell((u32)slot, (u32)dci);

		console_write("M37-USB: ok hid-config ep=0x");
		console_write_hex32(kbd_ep_addr);
		console_write("\n");

	} else if (has_msc && ep_in_addr != 0 && ep_out_addr != 0) {
		/* It is a mass storage device! */
		console_write("xhci: mass storage device detected in_addr=0x");
		console_write_hex32(ep_in_addr);
		console_write(" in_mps=");
		console_write_dec(ep_in_mps);
		console_write(" out_addr=0x");
		console_write_hex32(ep_out_addr);
		console_write(" out_mps=");
		console_write_dec(ep_out_mps);
		console_write("\n");

		/* Configure bulk-IN and bulk-OUT endpoints. */
		int in_dci = (ep_in_addr & 0x0f) * 2 + 1;
		int out_dci = (ep_out_addr & 0x0f) * 2;

		msc_dev.slot = slot;
		msc_dev.bulk_in_dci = in_dci;
		msc_dev.bulk_out_dci = out_dci;
		msc_dev.in_ring = dma_alloc(&msc_dev.in_ring_phys);
		msc_dev.in_enq = 0; msc_dev.in_cycle = 1;
		msc_dev.out_ring = dma_alloc(&msc_dev.out_ring_phys);
		msc_dev.out_enq = 0; msc_dev.out_cycle = 1;

		if (!msc_dev.in_ring || !msc_dev.out_ring) {
			console_write("xhci: failed to allocate rings for mass storage\n");
			return;
		}

		memset(inctx, 0, PAGE_SIZE);
		u32 *icc = ctx_at(inctx, 0);
		icc[1] = (1u << 0) | (1u << in_dci) | (1u << out_dci);
		u32 *slotc = ctx_at(inctx, 1);
		int max_dci = in_dci > out_dci ? in_dci : out_dci;
		slotc[0] = ((u32)max_dci << 27) | (speed << 20);
		slotc[1] = (port << 16);

		/* Bulk IN EP Context */
		u32 *ep_in_ctx = ctx_at(inctx, in_dci + 1);
		ep_in_ctx[1] = (EP_TYPE_BULK_IN << 3) | ((u32)ep_in_mps << 16) | ((u32)ep_in_burst << 8) | (3u << 1);
		ep_in_ctx[2] = (u32)((msc_dev.in_ring_phys | 1u) & 0xFFFFFFFF);
		ep_in_ctx[3] = (u32)(msc_dev.in_ring_phys >> 32);
		ep_in_ctx[4] = ep_in_mps;

		/* Bulk OUT EP Context */
		u32 *ep_out_ctx = ctx_at(inctx, out_dci + 1);
		ep_out_ctx[1] = (EP_TYPE_BULK_OUT << 3) | ((u32)ep_out_mps << 16) | ((u32)ep_out_burst << 8) | (3u << 1);
		ep_out_ctx[2] = (u32)((msc_dev.out_ring_phys | 1u) & 0xFFFFFFFF);
		ep_out_ctx[3] = (u32)(msc_dev.out_ring_phys >> 32);
		ep_out_ctx[4] = ep_out_mps;

		if (cmd_exec(inctx_phys, 0, (TRB_CONFIG_EP << TRB_TYPE_SHIFT) |
		             ((u32)slot << 24), 0) != CC_SUCCESS) {
			console_write("xhci: configure endpoint failed for mass storage\n");
			return;
		}

		/* SET_CONFIGURATION. */
		if (ctrl_xfer(&dev, 0x00, 9, cfg_val, 0, 0) != 0) {
			console_write("xhci: set config failed for mass storage\n");
			return;
		}

		udelay(50000); /* 50 ms settle delay */

		/* Initialize SCSI / Block Device */
		if (msc_inquiry(&msc_dev) != 0) {
			console_write("xhci: SCSI Inquiry failed\n");
			return;
		}

		if (msc_read_capacity(&msc_dev) != 0) {
			console_write("xhci: SCSI Read Capacity failed\n");
			return;
		}

		console_write("usb: storage device initialized size=");
		console_write_dec((msc_dev.block_count * msc_dev.block_size) / (1024 * 1024));
		console_write(" MiB\n");

		/* Register Block Device */
		msc_dev.bdev.name = "usb0";
		msc_dev.bdev.block_size = msc_dev.block_size;
		msc_dev.bdev.block_count = msc_dev.block_count;
		msc_dev.bdev.read_blocks = msc_read_blocks;
		msc_dev.bdev.write_blocks = msc_write_blocks;
		msc_dev.bdev.priv = &msc_dev;

		blk_register(&msc_dev.bdev);
	}
}

static void usb_enumerate_ports(void)
{
	for (u32 p = 1; p <= num_ports; p++) {
		u32 portsc = rd32(op_base, XHCI_OP_PORTS + (p - 1) * 0x10);
		console_write("xhci: port ");
		console_write_dec(p);
		console_write(" status=0x");
		console_write_hex32(portsc);
		console_write("\n");
		if (portsc & PORTSC_CCS) {
			u32 speed = (portsc >> PORTSC_SPEED_SHIFT) & PORTSC_SPEED_MASK;
			usb_probe_port(p, speed);
		}
	}
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

	char root_val[64];
	int usb_root_requested = 0;
	if (bootinfo_get_kv("root", root_val, sizeof(root_val))) {
		if (strncmp(root_val, "LABEL=", 6) == 0 ||
		    strncmp(root_val, "UUID=", 5) == 0 ||
		    strstr(root_val, "usb0") != NULL) {
			usb_root_requested = 1;
		}
	}

	int xhci_run_requested = bootinfo_has_flag("b1nix.xhci.run") ||
	                         bootinfo_has_flag("b1nix.xhci.enum") ||
	                         bootinfo_has_flag("b1nix.test=1") ||
	                         usb_root_requested;
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
	console_write("xhci: max_slots=");
	console_write_dec(max_slots);
	console_write("\n");
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

	/* Scratchpads (if required). */
	u32 hcs2 = rd32(cap_base, XHCI_CAP_HCSPARAMS2);
	u32 num_sp = (((hcs2 >> 16) & 0x3e0) | ((hcs2 >> 27) & 0x1f));
	if (num_sp > 0) {
		console_write("xhci: allocating ");
		console_write_dec(num_sp);
		console_write(" scratchpad buffers...\n");
		u64 sp_array_phys;
		u64 *sp_array = dma_alloc(&sp_array_phys);
		if (!sp_array) {
			console_write("xhci: failed to allocate scratchpad array\n");
			return 0;
		}
		for (u32 i = 0; i < num_sp; i++) {
			u64 sp_page_phys;
			void *sp_page = dma_alloc(&sp_page_phys);
			if (!sp_page) {
				console_write("xhci: failed to allocate scratchpad page\n");
				return 0;
			}
			sp_array[i] = sp_page_phys;
		}
		dcbaa[0] = sp_array_phys;
	}

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

	if (xhci_run_requested) {
		console_write("xhci: waiting 1000ms for ports to settle...\n");
		for (int i = 0; i < 1000; i++) {
			if (i % 100 == 0) {
				console_write("xhci: waiting ");
				console_write_dec(1000 - i);
				console_write(" ms...\n");
			}
			udelay(1000);
		}
		console_write("xhci: enumerating ports...\n");
		usb_enumerate_ports();
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

	struct trb e;
	while (evt_poll_hw(&e, 1)) {
		u32 et = (e.control >> TRB_TYPE_SHIFT) & 0x3f;
		if (et == TRB_EVT_TRANSFER) {
			u32 slot = (e.control >> 24) & 0xff;
			u32 dci = (e.control >> 16) & 0x1f;
			if ((int)slot == kbd_slot && (int)dci == kbd_ep_dci) {
				usb_handle_transfer_event(&e);
			} else {
				u64 flags;
				spin_lock_irqsave(&xhci_evt_lock, &flags);
				push_saved_event(&e);
				spin_unlock_irqrestore(&xhci_evt_lock, flags);
			}
		} else {
			u64 flags;
			spin_lock_irqsave(&xhci_evt_lock, &flags);
			push_saved_event(&e);
			spin_unlock_irqrestore(&xhci_evt_lock, flags);
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
