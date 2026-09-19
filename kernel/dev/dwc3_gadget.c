/*
 * USB Ethernet gadget on a Synopsys DWC3 — the phone end of a USB cable.
 *
 * A phone's USB port is a DWC3 controller that can act as a device. This
 * driver makes it one: a CDC-ECM network adapter ("usb0") that Linux and
 * macOS hosts drive with their stock class drivers, so plugging the phone into
 * a computer gives it an Ethernet link to that computer and nothing else is
 * needed on the host side but an address.
 *
 * Board: Snapdragon 855 (SM8150), the Xperia 5. Opt-in with b1nix.usb-gadget,
 * because nothing here can be exercised under QEMU — it has no DWC3 device
 * model — and a register read of an unclocked Qualcomm block does not return.
 * Bring-up, in order:
 *   - GCC: power the usb30_prim GDSC, reset the controller and the high-speed
 *     PHY (block resets), enable the controller's clocks;
 *   - the Synopsys "femto" HS PHY at hsphy@88e2000, programmed with the
 *     sequence from Linux's phy-qcom-snps-femto-v2 (the PHY's regulators are
 *     RPMh-controlled and assumed on, as the bootloader leaves them);
 *   - the Qualcomm wrapper (QSCRATCH): UTMI clock as the PIPE clock (the
 *     SuperSpeed PHY is never brought up, so the link is high-speed only) and
 *     a VBUS-valid override, since VBUS detection lives in the PMIC;
 *   - the DWC3 core in device mode, one event buffer, polled from the network
 *     stack's poll hook.
 *
 * The USB side is the smallest thing that enumerates: control endpoint with
 * the standard requests, one configuration with a CDC-ECM control interface
 * (interrupt IN for link notifications) and a data interface whose alternate
 * setting 1 carries a bulk IN/OUT pair. One frame per transfer each way.
 *
 * Addressing: the link is point-to-point, so the address is static —
 * b1nix.usb-ip (default 172.16.42.1/24) — and set when the host selects the
 * data interface. Boot with b1nix.net=off so the DHCP client does not clear it.
 * On the host: `sudo ifconfig <if> 172.16.42.2 netmask 255.255.255.0`.
 *
 * Every step prints to the console, because on this board the console is the
 * panel and it is the only evidence there is.
 */

#include <b1nix/bootinfo.h>
#include <b1nix/bootmark.h>
#include <b1nix/console.h>
#include <b1nix/mm.h>
#include <b1nix/net.h>
#include <b1nix/netdev.h>
#include <b1nix/irq.h>
#include <b1nix/spinlock.h>
#include <b1nix/types.h>
#include <b1nix/arch.h>
#include <string.h>

#if defined(__aarch64__)

/* ── GCC (SM8150) ─────────────────────────────────────────────────────────── */

#define GCC_USB30_PRIM_BCR        0x0f000
#define GCC_USB30_PRIM_GDSCR      0x0f004
#define GCC_QUSB2PHY_PRIM_BCR     0x12000
static const u32 gcc_usb_clocks[] = {
	0x0f010, /* gcc_usb30_prim_master_clk */
	0x0f014, /* gcc_usb30_prim_sleep_clk */
	0x0f018, /* gcc_usb30_prim_mock_utmi_clk */
	0x0f078, /* gcc_cfg_noc_usb3_prim_axi_clk */
	0x0f07c, /* gcc_aggre_usb3_prim_axi_clk */
	0x8c008, /* gcc_usb3_prim_clkref_clk */
};

/* ── Qualcomm wrapper ─────────────────────────────────────────────────────── */

#define QSCRATCH_OFFSET           0xf8800
#define QSCRATCH_GENERAL_CFG      0x08
#define   PIPE_UTMI_CLK_SEL       (1u << 0)
#define   PIPE3_PHYSTATUS_SW      (1u << 3)
#define   PIPE_UTMI_CLK_DIS       (1u << 8)
#define QSCRATCH_HS_PHY_CTRL      0x10
#define   UTMI_OTG_VBUS_VALID     (1u << 20)
#define   SW_SESSVLD_SEL          (1u << 28)
#define QSCRATCH_SS_PHY_CTRL      0x30
#define   LANE0_PWR_PRESENT       (1u << 24)

/* ── Synopsys femto HS PHY ────────────────────────────────────────────────── */

#define PHY_UTMI_CTRL0            0x3c
#define   PHY_SLEEPM              (1u << 0)
#define PHY_UTMI_CTRL5            0x50
#define   PHY_POR                 (1u << 1)
#define PHY_HS_CTRL_COMMON0       0x54
#define   PHY_SIDDQ               (1u << 2)
#define   PHY_FSEL_MASK           (7u << 4)
#define PHY_HS_CTRL_COMMON1       0x58
#define   PHY_VBUSVLDEXTSEL0      (1u << 4)
#define   PHY_PLLBTUNE            (1u << 5)
#define PHY_HS_CTRL_COMMON2       0x5c
#define   PHY_VREGBYPASS          (1u << 0)
#define PHY_HS_CTRL1              0x60
#define   PHY_VBUSVLDEXT0         (1u << 0)
#define PHY_HS_CTRL2              0x64
#define   PHY_SUSPEND_N           (1u << 2)
#define   PHY_SUSPEND_N_SEL       (1u << 3)
#define PHY_CFG0                  0x94
#define   PHY_CMN_CTRL_OVERRIDE_EN (1u << 1)
#define PHY_REFCLK_CTRL           0xa0

/* ── DWC3 ─────────────────────────────────────────────────────────────────── */

#define GCTL          0xc110
#define   GCTL_PRTCAPDIR_MASK  (3u << 12)
#define   GCTL_PRTCAP_DEVICE   (2u << 12)
#define   GCTL_SCALEDOWN_MASK  (3u << 4)
#define GSNPSID       0xc120
#define GHWPARAMS0    0xc140
#define GHWPARAMS7    0xc15c
#define GUSB2PHYCFG   0xc200
#define   GUSB2PHYCFG_SUSPHY   (1u << 6)
#define   GUSB2PHYCFG_ENBLSLPM (1u << 8)
#define GUSB3PIPECTL  0xc2c0
#define   GUSB3PIPECTL_SUSPHY  (1u << 17)
#define GEVNTADRLO    0xc400
#define GEVNTADRHI    0xc404
#define GEVNTSIZ      0xc408
#define   GEVNTSIZ_INTMASK     (1u << 31)
#define GEVNTCOUNT    0xc40c
#define DCFG          0xc700
#define   DCFG_SPEED_MASK      7u
#define   DCFG_HIGHSPEED       0u
#define   DCFG_DEVADDR_MASK    (0x7fu << 3)
#define   DCFG_NUMP_MASK       (0x1fu << 17)
#define   DCFG_IGNSTRMPP       (1u << 23)
#define   DCFG_LPM_CAP         (1u << 22)
#define DCTL          0xc704
#define   DCTL_RUN_STOP        (1u << 31)
#define   DCTL_CSFTRST         (1u << 30)
#define   DCTL_KEEP_CONNECT    (1u << 19)
#define   DCTL_INITU2ENA       (1u << 12)
#define   DCTL_ACCEPTU2ENA     (1u << 11)
#define   DCTL_INITU1ENA       (1u << 10)
#define   DCTL_ACCEPTU1ENA     (1u << 9)
#define DEVTEN        0xc708
#define   DEVTEN_DISCONN       (1u << 0)
#define   DEVTEN_USBRST        (1u << 1)
#define   DEVTEN_CONNDONE      (1u << 2)
#define   DEVTEN_OVERFLOW      (1u << 11)
#define DSTS          0xc70c
#define   DSTS_DEVCTRLHLT      (1u << 22)
#define   DSTS_CONNECTSPD      7u
#define DALEPENA      0xc720
#define DEPCMD_BASE   0xc800

#define DEPCMD_SETEPCONFIG     0x01
#define DEPCMD_SETTRANSFRES    0x02
#define DEPCMD_SETSTALL        0x04
#define DEPCMD_CLEARSTALL      0x05
#define DEPCMD_STARTTRANSFER   0x06
#define DEPCMD_ENDTRANSFER     0x08
#define DEPCMD_DEPSTARTCFG     0x09
#define DEPCMD_CMDIOC          (1u << 8)
#define DEPCMD_CMDACT          (1u << 10)
#define DEPCMD_FORCERM         (1u << 11)

#define DEPCFG_XFER_COMPLETE_EN   (1u << 8)
#define DEPCFG_XFER_NOT_READY_EN  (1u << 10)
#define DEPCFG_ACTION_MODIFY      (2u << 30)

#define TRB_HWO       (1u << 0)
#define TRB_LST       (1u << 1)
#define TRB_CHN       (1u << 2)
#define TRB_ISP_IMI   (1u << 10)
#define TRB_IOC       (1u << 11)
#define TRBCTL_NORMAL        (1u << 4)
#define TRBCTL_SETUP         (2u << 4)
#define TRBCTL_STATUS2       (3u << 4)
#define TRBCTL_STATUS3       (4u << 4)
#define TRBCTL_DATA          (5u << 4)

#define EVT_DEV_DISCONNECT   0
#define EVT_DEV_RESET        1
#define EVT_DEV_CONNDONE     2
#define EVT_EP_XFERCOMPLETE  1
#define EVT_EP_XFERNOTREADY  3
#define STATUS_CONTROL_DATA   1
#define STATUS_CONTROL_STATUS 2

/* Physical endpoints: (number << 1) | IN. */
#define EP0_OUT   0
#define EP0_IN    1
#define EP_NOTIFY 3   /* 0x81 interrupt IN */
#define EP_RX     4   /* 0x02 bulk OUT */
#define EP_TX     5   /* 0x82 bulk IN */

#define EP_TYPE_CONTROL 0
#define EP_TYPE_BULK    2
#define EP_TYPE_INTR    3

#define EVT_BUF_SIZE   4096u
#define PAGE           4096u
#define RX_BUF_LEN     2048u   /* a multiple of every bulk wMaxPacketSize */
#define GADGET_DMA_BASE 0xf1000000ull

enum ep0_state { EP0_SETUP, EP0_DATA, EP0_STATUS };

struct gadget {
	u64 base;           /* DWC3 register block */
	u64 qscratch;
	u64 hsphy;
	u64 gcc;
	u64 evt_phys;
	u8 *evt;
	u32 evt_pos;
	/* One DMA page per endpoint: TRBs at the start, data after them. */
	u64 ep_phys[6];
	u8 *ep_mem[6];
	u32 rsc_idx[6];
	int busy[6];
	enum ep0_state ep0_state;
	int three_stage;
	int ep0_in;         /* data stage direction */
	u16 pending_addr;
	int hs;             /* connected at high speed */
	int configured;
	int data_alt;       /* data interface alternate setting */
	int link_notified;
	/* A received frame waiting to go up the stack, delivered with the lock
	 * dropped; the OUT transfer is re-armed only after delivery. */
	u32 rx_ready;
	int rx_delivering;
	spinlock_t lock;
	struct netdev nd;
};

static struct gadget g;

/* tests/dwc3-model builds this file on the host against a model of the
 * controller, which supplies its own register accessors. */
#ifndef DWC3_MODEL
static inline u32 rd(u64 base, u32 off) { return *(volatile u32 *)(usize)(base + off); }
static inline void wr(u64 base, u32 off, u32 v) { *(volatile u32 *)(usize)(base + off) = v; }
#else
u32 rd(u64 base, u32 off);
void wr(u64 base, u32 off, u32 v);
#endif
static void set_mask(u64 base, u32 off, u32 mask, u32 val)
{
	wr(base, off, (rd(base, off) & ~mask) | (val & mask));
	(void)rd(base, off);
}

static void say(const char *s) { console_write("usb-gadget: "); console_write(s); }
static void say_hex(const char *s, u64 v)
{
	say(s);
	console_write("0x");
	console_write_hex64(v);
	console_write("\n");
}

static void put_le16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void put_le32(u8 *p, u32 v) { put_le16(p, (u16)v); put_le16(p + 2, (u16)(v >> 16)); }
static u16 get_le16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 get_le32(const u8 *p) { return (u32)get_le16(p) | ((u32)get_le16(p + 2) << 16); }

/* ── Descriptors ──────────────────────────────────────────────────────────── */

#define USB_VID 0x0525   /* NetChip: the Linux "Ethernet Gadget" ids */
#define USB_PID 0xa4a1

static const u8 dev_desc[18] = {
	18, 1, 0x00, 0x02, 0x02, 0x00, 0x00, 64,
	USB_VID & 0xff, USB_VID >> 8, USB_PID & 0xff, USB_PID >> 8,
	0x00, 0x01, 1, 2, 3, 1,
};

/* Built per speed: only the bulk wMaxPacketSize differs. */
static usize config_desc(u8 *out, int hs)
{
	static const u8 tmpl[] = {
		/* configuration */
		9, 2, 0, 0, 2, 1, 0, 0xc0, 1,
		/* interface 0: CDC ECM control */
		9, 4, 0, 0, 1, 0x02, 0x06, 0x00, 0,
		5, 0x24, 0x00, 0x10, 0x01,              /* header */
		5, 0x24, 0x06, 0, 1,                    /* union: 0 controls 1 */
		13, 0x24, 0x0f, 4, 0, 0, 0, 0,          /* ethernet: iMACAddress=4 */
		0xea, 0x05, 0, 0, 0,                    /* max segment 1514 */
		7, 5, 0x81, 0x03, 16, 0, 9,             /* notify, interrupt IN */
		/* interface 1 alt 0: no endpoints (link off) */
		9, 4, 1, 0, 0, 0x0a, 0x00, 0x00, 0,
		/* interface 1 alt 1: the data pair */
		9, 4, 1, 1, 2, 0x0a, 0x00, 0x00, 0,
		7, 5, 0x82, 0x02, 0, 2, 0,              /* bulk IN */
		7, 5, 0x02, 0x02, 0, 2, 0,              /* bulk OUT */
	};
	usize n = sizeof(tmpl);

	memcpy(out, tmpl, n);
	put_le16(out + 2, (u16)n);
	u16 maxp = hs ? 512 : 64;
	put_le16(out + n - 7 - 3, maxp);
	put_le16(out + n - 3, maxp);
	if (!hs)
		out[9 + 9 + 5 + 5 + 13 + 6] = 1;        /* FS interval, 1 ms */
	return n;
}

/* MAC addresses: ours, and the one the host's interface will use (announced
 * through iMACAddress). Locally administered, fixed. */
static const u8 dev_mac[6] = { 0x02, 0xb1, 0x1c, 0x00, 0x00, 0x01 };
static const char host_mac_str[] = "02B11C000002";

static usize string_desc(u8 idx, u8 *out)
{
	static const char *const strs[] = {
		0, "b1nix", "b1nix USB Ethernet", "0001", host_mac_str,
	};
	if (idx == 0) {
		out[0] = 4; out[1] = 3; out[2] = 0x09; out[3] = 0x04;
		return 4;
	}
	if (idx >= sizeof(strs) / sizeof(strs[0]))
		return 0;
	usize len = strlen(strs[idx]);
	out[0] = (u8)(2 + len * 2);
	out[1] = 3;
	for (usize i = 0; i < len; i++) {
		out[2 + i * 2] = (u8)strs[idx][i];
		out[3 + i * 2] = 0;
	}
	return out[0];
}

/* ── Endpoint commands and TRBs ───────────────────────────────────────────── */

static int ep_cmd(u32 phys, u32 cmd, u32 p0, u32 p1, u32 p2)
{
	u32 reg = DEPCMD_BASE + phys * 0x10;

	wr(g.base, reg + 0x08, p0);
	wr(g.base, reg + 0x04, p1);
	wr(g.base, reg + 0x00, p2);
	wr(g.base, reg + 0x0c, cmd | DEPCMD_CMDACT);
	for (u32 i = 0; i < 50000; i++) {
		u32 v = rd(g.base, reg + 0x0c);

		if (!(v & DEPCMD_CMDACT)) {
			if ((cmd & 0xf) == DEPCMD_STARTTRANSFER)
				g.rsc_idx[phys] = (v >> 16) & 0x7f;
			return ((v >> 12) & 0xf) ? -1 : 0;
		}
		arch_udelay(20);
	}
	say_hex("endpoint command timed out: ", ((u64)phys << 8) | (cmd & 0xf));
	return -1;
}

static int ep_config(u32 phys, u32 type, u32 maxp, u32 action, u32 interval_m1)
{
	u32 p0 = (type << 1) | ((maxp & 0x7ff) << 3) | action;
	u32 p1 = ((phys & 0x1f) << 25) | (interval_m1 << 16);

	if (phys & 1)
		p0 |= (phys >> 1) << 17;              /* FIFO number */
	if (type == EP_TYPE_CONTROL)
		p1 |= DEPCFG_XFER_COMPLETE_EN | DEPCFG_XFER_NOT_READY_EN;
	else
		p1 |= DEPCFG_XFER_COMPLETE_EN;
	return ep_cmd(phys, DEPCMD_SETEPCONFIG, p0, p1, 0);
}

static int ep_enable(u32 phys, u32 type, u32 maxp, u32 interval_m1)
{
	if (ep_config(phys, type, maxp, 0, interval_m1) != 0)
		return -1;
	if (ep_cmd(phys, DEPCMD_SETTRANSFRES, 1, 0, 0) != 0)
		return -1;
	wr(g.base, DALEPENA, rd(g.base, DALEPENA) | (1u << phys));
	return 0;
}

static void ep_end(u32 phys)
{
	if (!g.busy[phys])
		return;
	ep_cmd(phys, DEPCMD_ENDTRANSFER | DEPCMD_FORCERM | (g.rsc_idx[phys] << 16),
	       0, 0, 0);
	arch_udelay(1000);
	g.busy[phys] = 0;
}

static void ep_disable(u32 phys)
{
	ep_end(phys);
	wr(g.base, DALEPENA, rd(g.base, DALEPENA) & ~(1u << phys));
}

static u8 *ep_data(u32 phys) { return g.ep_mem[phys] + 64; }
static u64 ep_data_phys(u32 phys) { return g.ep_phys[phys] + 64; }

/* Point TRB 0 (and, for a zero-length tail, TRB 1) at this endpoint's data
 * area and start the transfer. */
static int ep_start(u32 phys, u32 len, u32 type, int zlp)
{
	u8 *t = g.ep_mem[phys];

	memset(t, 0, 32);
	put_le32(t, (u32)ep_data_phys(phys));
	put_le32(t + 4, (u32)(ep_data_phys(phys) >> 32));
	put_le32(t + 8, len);
	u32 ctrl = type | TRB_HWO | TRB_ISP_IMI;
	if (zlp) {
		put_le32(t + 12, ctrl | TRB_CHN);
		put_le32(t + 16, (u32)ep_data_phys(phys));
		put_le32(t + 20, (u32)(ep_data_phys(phys) >> 32));
		put_le32(t + 28, type | TRB_HWO | TRB_LST | TRB_IOC);
	} else {
		put_le32(t + 12, ctrl | TRB_LST | TRB_IOC);
	}
	if (ep_cmd(phys, DEPCMD_STARTTRANSFER, (u32)(g.ep_phys[phys] >> 32),
	           (u32)g.ep_phys[phys], 0) != 0)
		return -1;
	g.busy[phys] = 1;
	return 0;
}

/* Has the controller finished the transfer on `phys`? It clears HWO in the
 * TRBs it is done with (both of them, for a zero-length tail). Read instead
 * of trusting the completion event alone: an event lost to a full event
 * buffer left the endpoint "busy" for ever -- "tx dropped, endpoint busy"
 * on every frame after a long UFS write, and the link was dead. */
static int ep_trb_done(u32 phys)
{
	const u8 *t = g.ep_mem[phys];

	return !(get_le32(t + 12) & TRB_HWO) && !(get_le32(t + 28) & TRB_HWO);
}

/* Bytes the last transfer on `phys` moved: requested minus what the TRB says
 * is left. */
static u32 ep_actual(u32 phys, u32 requested)
{
	u32 left = get_le32(g.ep_mem[phys] + 8) & 0xffffff;

	return left <= requested ? requested - left : 0;
}

/* ── Control endpoint ─────────────────────────────────────────────────────── */

static void ep0_setup_start(void)
{
	g.ep0_state = EP0_SETUP;
	ep_start(EP0_OUT, 8, TRBCTL_SETUP, 0);
}

static void ep0_stall(void)
{
	ep_cmd(EP0_OUT, DEPCMD_SETSTALL, 0, 0, 0);
	g.busy[EP0_OUT] = 0;
	g.busy[EP0_IN] = 0;
	ep0_setup_start();
}

/* Queue the data stage of a device-to-host request. */
static void ep0_reply(const u8 *data, usize len, u16 wlength)
{
	if (len > wlength)
		len = wlength;
	memcpy(ep_data(EP0_IN), data, len);
	g.ep0_state = EP0_DATA;
	/* A reply shorter than asked for that ends on a packet boundary has to
	 * be terminated with a zero-length packet. */
	u32 maxp = 64;
	ep_start(EP0_IN, (u32)len, TRBCTL_DATA, len < wlength && (len % maxp) == 0);
}

static void data_alt_set(int alt);

static void ep0_handle_setup(void)
{
	u8 s[8];

	memcpy(s, ep_data(EP0_OUT), 8);
	u8 type = s[0], req = s[1];
	u16 value = get_le16(s + 2), index = get_le16(s + 4), wlength = get_le16(s + 6);
	u8 buf[128];

	g.three_stage = wlength != 0;
	g.ep0_in = (type & 0x80) != 0;

	if ((type & 0x60) == 0) {                   /* standard */
		switch (req) {
		case 0x00:                              /* GET_STATUS */
			memset(buf, 0, 2);
			ep0_reply(buf, 2, wlength);
			return;
		case 0x05:                              /* SET_ADDRESS */
			g.pending_addr = value & 0x7f;
			set_mask(g.base, DCFG, DCFG_DEVADDR_MASK, (u32)g.pending_addr << 3);
			return;                             /* status on XferNotReady */
		case 0x06: {                            /* GET_DESCRIPTOR */
			usize n = 0;

			switch (value >> 8) {
			case 1: memcpy(buf, dev_desc, sizeof(dev_desc)); n = sizeof(dev_desc); break;
			case 2: n = config_desc(buf, g.hs); break;
			case 3: n = string_desc((u8)value, buf); break;
			}
			if (!n) {
				ep0_stall();
				return;
			}
			ep0_reply(buf, n, wlength);
			return;
		}
		case 0x08:                              /* GET_CONFIGURATION */
			buf[0] = (u8)g.configured;
			ep0_reply(buf, 1, wlength);
			return;
		case 0x09:                              /* SET_CONFIGURATION */
			if (value > 1) {
				ep0_stall();
				return;
			}
			data_alt_set(0);
			if (value == 1 && !g.configured) {
				u32 maxp = g.hs ? 512 : 64;

				ep_enable(EP_NOTIFY, EP_TYPE_INTR, 16, g.hs ? 8 : 0);
				ep_enable(EP_RX, EP_TYPE_BULK, maxp, 0);
				ep_enable(EP_TX, EP_TYPE_BULK, maxp, 0);
			} else if (value == 0 && g.configured) {
				ep_disable(EP_NOTIFY);
				ep_disable(EP_RX);
				ep_disable(EP_TX);
			}
			g.configured = value;
			say(value ? "configured by the host\n" : "unconfigured\n");
			return;
		case 0x0a:                              /* GET_INTERFACE */
			buf[0] = (index == 1) ? (u8)g.data_alt : 0;
			ep0_reply(buf, 1, wlength);
			return;
		case 0x0b:                              /* SET_INTERFACE */
			if (index == 1 && value <= 1) {
				data_alt_set(value);
				return;
			}
			if (index == 0 && value == 0)
				return;
			ep0_stall();
			return;
		case 0x01:                              /* CLEAR_FEATURE */
			if ((type & 0x1f) == 2 && value == 0) {  /* ENDPOINT_HALT */
				u32 ep = index & 0x0f, in = (index & 0x80) ? 1 : 0;

				if (ep)
					ep_cmd((ep << 1) | in, DEPCMD_CLEARSTALL, 0, 0, 0);
				return;
			}
			return;
		case 0x03:                              /* SET_FEATURE */
			return;
		}
	} else if ((type & 0x60) == 0x20) {         /* class */
		if (req == 0x43)                        /* SET_ETHERNET_PACKET_FILTER */
			return;
	}
	ep0_stall();
}

static void notify_link(void)
{
	u8 *n = ep_data(EP_NOTIFY);

	/* NETWORK_CONNECTION, wValue 1 = connected, on interface 0. */
	static const u8 msg[8] = { 0xa1, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00 };
	if (g.busy[EP_NOTIFY])
		return;
	memcpy(n, msg, 8);
	if (ep_start(EP_NOTIFY, 8, TRBCTL_NORMAL, 0) == 0)
		g.link_notified = 1;
}

static void rx_start(void)
{
	if (!g.busy[EP_RX] && !g.rx_ready && !g.rx_delivering)
		ep_start(EP_RX, RX_BUF_LEN, TRBCTL_NORMAL, 0);
}

static void data_alt_set(int alt)
{
	if (alt == g.data_alt)
		return;
	g.data_alt = alt;
	if (!alt) {
		ep_end(EP_RX);
		ep_end(EP_TX);
		g.rx_ready = 0;
		g.link_notified = 0;
		return;
	}
	char ipbuf[32];
	struct ipv4_addr ip = { { 172, 16, 42, 1 } };
	if (bootinfo_get_kv("b1nix.usb-ip", ipbuf, sizeof(ipbuf)) && ipbuf[0]) {
		u32 part = 0, oct = 0;
		const char *p = ipbuf;
		u8 v[4];
		for (; *p && part < 4; p++) {
			if (*p == '.') { v[part++] = (u8)oct; oct = 0; }
			else if (*p >= '0' && *p <= '9') oct = oct * 10 + (u32)(*p - '0');
			else break;
		}
		if (part == 3) {
			v[3] = (u8)oct;
			memcpy(ip.bytes, v, 4);
		}
	}
	struct ipv4_addr mask = { { 255, 255, 255, 0 } };
	struct ipv4_addr no_gw = { { 0, 0, 0, 0 } };
	net_set_ip(ip);
	net_set_netmask(mask);
	/* The connected route is what lets replies out; DHCP installs it the
	 * same way. Without it ARP answered and nothing IPv4 ever did. */
	route_configure_interface(ip, mask, no_gw);
	rx_start();
	notify_link();
	say("link up: usb0 ");
	for (int i = 0; i < 4; i++) {
		console_write_dec(ip.bytes[i]);
		console_write(i < 3 ? "." : "/24\n");
	}
}

static void ep0_event(u32 phys, u32 evt, u32 status)
{
	if (evt == EVT_EP_XFERCOMPLETE) {
		g.busy[phys] = 0;
		switch (g.ep0_state) {
		case EP0_SETUP:
			ep0_handle_setup();
			break;
		case EP0_DATA:
			g.ep0_state = EP0_STATUS;           /* waits for XferNotReady */
			break;
		case EP0_STATUS:
			ep0_setup_start();
			break;
		}
		return;
	}
	if (evt == EVT_EP_XFERNOTREADY && (status & 3) == STATUS_CONTROL_STATUS) {
		if (g.busy[phys])
			return;
		g.ep0_state = EP0_STATUS;
		ep_start(phys, 0, g.three_stage ? TRBCTL_STATUS3 : TRBCTL_STATUS2, 0);
	}
}

/* ── Events ───────────────────────────────────────────────────────────────── */

static void on_reset(void)
{
	ep_end(EP_RX);
	ep_end(EP_TX);
	ep_end(EP_NOTIFY);
	set_mask(g.base, DCFG, DCFG_DEVADDR_MASK, 0);
	g.configured = 0;
	g.data_alt = 0;
	g.rx_ready = 0;
	g.link_notified = 0;
	/* A reset can land in the middle of a control transfer; without a SETUP
	 * transfer armed again the next request is never seen. */
	if (g.ep0_state != EP0_SETUP || !g.busy[EP0_OUT]) {
		ep_end(EP0_OUT);
		ep_end(EP0_IN);
		ep0_setup_start();
	}
}

static void on_conndone(void)
{
	u32 spd = rd(g.base, DSTS) & DSTS_CONNECTSPD;

	g.hs = (spd == 0);
	ep_config(EP0_OUT, EP_TYPE_CONTROL, 64, DEPCFG_ACTION_MODIFY, 0);
	ep_config(EP0_IN, EP_TYPE_CONTROL, 64, DEPCFG_ACTION_MODIFY, 0);
	say(g.hs ? "connected at high speed\n" : "connected at full speed\n");
}

static void handle_event(u32 e)
{
	if (e & 1) {
		if (((e >> 1) & 0x7f) != 0)
			return;                             /* not a device event */
		switch ((e >> 8) & 0xf) {
		case EVT_DEV_DISCONNECT:
			say("host disconnected\n");
			on_reset();
			break;
		case EVT_DEV_RESET:
			on_reset();
			break;
		case EVT_DEV_CONNDONE:
			on_conndone();
			break;
		}
		return;
	}
	u32 phys = (e >> 1) & 0x1f, evt = (e >> 6) & 0xf, status = (e >> 12) & 0xf;

	if (phys <= EP0_IN) {
		ep0_event(phys, evt, status);
		return;
	}
	if (evt != EVT_EP_XFERCOMPLETE || phys >= 6)
		return;
	g.busy[phys] = 0;
	if (phys == EP_RX) {
		u32 n = ep_actual(EP_RX, RX_BUF_LEN);

		if (n >= 14 && g.data_alt)
			g.rx_ready = n;
		else if (g.data_alt)
			rx_start();
	}
}

static void process_events(void)
{
	u32 count = rd(g.base, GEVNTCOUNT) & 0xfffc;

	if (!count)
		return;
	for (u32 done = 0; done < count; done += 4) {
		handle_event(get_le32(g.evt + g.evt_pos));
		g.evt_pos = (g.evt_pos + 4) % EVT_BUF_SIZE;
	}
	wr(g.base, GEVNTCOUNT, count);
}

/* ── netdev ───────────────────────────────────────────────────────────────── */

/* The dwc3 core's interrupt, GIC SPI 133 in the Xperia 5
 * device tree. */
#define DWC3_IRQ (32 + 133)

/* Interrupt half of the event buffer, as Linux's dwc3 does it: a non-empty
 * buffer masks itself (GEVNTSIZ.INTMASK) so the level line drops, and
 * net_task -- which net_handle_irq wakes on a 1 -- processes the events and
 * unmasks in gadget_poll. */
static int gadget_irq_ack(struct netdev *nd)
{
	static int said;

	(void)nd;
	if (!(rd(g.base, GEVNTCOUNT) & 0xfffc))
		return 0;
	wr(g.base, GEVNTSIZ, EVT_BUF_SIZE | GEVNTSIZ_INTMASK);
	if (!said) {
		said = 1;
		say("interrupts arrive\n");
	}
	return 1;
}

static void gadget_poll(struct netdev *nd)
{
	u64 flags;

	(void)nd;
	spin_lock_irqsave(&g.lock, &flags);
	/* Events taken, then unmask -- and look again. An event that arrived
	 * while the buffer was masked keeps the line high through the unmask,
	 * and if the GIC has this SPI as edge-triggered (the tree does not say,
	 * and nothing here programs ICFGR) that is no new edge: it sat until the
	 * next idle poll, 100 ms. With the count read back as empty after the
	 * unmask, the next event raises the line from low. */
	do {
		process_events();
		if (g.nd.irq_ack)
			wr(g.base, GEVNTSIZ, EVT_BUF_SIZE);
	} while (g.nd.irq_ack && (rd(g.base, GEVNTCOUNT) & 0xfffc));
	if (g.busy[EP_RX] && !g.rx_ready && !g.rx_delivering && ep_trb_done(EP_RX)) {
		u32 got = ep_actual(EP_RX, RX_BUF_LEN);

		g.busy[EP_RX] = 0;
		if (got >= 14 && g.data_alt)
			g.rx_ready = got;
		else if (g.data_alt)
			rx_start();
	}
	u32 n = g.rx_ready;
	if (n && !g.rx_delivering) {
		g.rx_ready = 0;
		g.rx_delivering = 1;
	} else {
		n = 0;
	}
	spin_unlock_irqrestore(&g.lock, flags);
	if (!n)
		return;
	/* Into RAM before the stack sees it. The DMA window is mapped as Device
	 * memory, and the protocol code reads header fields in place without
	 * regard to alignment — which on Device memory is an alignment fault
	 * (it was, on the host's first packet). */
	static u8 rx_frame[RX_BUF_LEN];
	memcpy(rx_frame, ep_data(EP_RX), n);
	/* Checksums need no second look on this link: every USB data packet
	 * carries its own CRC16. And a macOS host marks the ECM interface
	 * PARTIAL_CSUM and hands over TCP with the checksum left for the
	 * "adapter" to finish. */
	ethernet_receive_flags(rx_frame, n, NET_RX_F_CSUM_OK);
	spin_lock_irqsave(&g.lock, &flags);
	g.rx_delivering = 0;
	if (g.data_alt)
		rx_start();
	spin_unlock_irqrestore(&g.lock, flags);
}

static int gadget_transmit(struct netdev *nd, const u8 hdr[14],
                           const void *payload, usize payload_len, u32 tx_flags)
{
	u64 flags;
	int rc = -1;

	(void)nd;
	(void)tx_flags;
	if (14 + payload_len > PAGE - 64)
		return -1;
	spin_lock_irqsave(&g.lock, &flags);
	/* One frame in flight. Reap a completion that is already waiting before
	 * giving up on this one. */
	for (int i = 0; i < 200 && g.busy[EP_TX]; i++) {
		process_events();
		if (g.busy[EP_TX] && ep_trb_done(EP_TX))
			g.busy[EP_TX] = 0;
		if (g.busy[EP_TX])
			arch_udelay(50);
	}
	static u32 stuck;

	/* A transfer the host never collects holds the endpoint for good; end
	 * it after a run of frames dropped behind it. */
	if (g.busy[EP_TX] && ++stuck >= 100) {
		ep_end(EP_TX);
		stuck = 0;
	} else if (!g.busy[EP_TX]) {
		stuck = 0;
	}
	if (!g.data_alt || g.busy[EP_TX]) {
		static u32 dropped;

		if (dropped++ < 20) {
			console_write(g.data_alt ? "usb-gadget: tx dropped, endpoint busy\n"
			                         : "usb-gadget: tx dropped, link down\n");
		}
	}
	if (g.data_alt && !g.busy[EP_TX]) {
		u8 *d = ep_data(EP_TX);
		u32 len = (u32)(14 + payload_len);
		u32 maxp = g.hs ? 512 : 64;

		memcpy(d, hdr, 14);
		memcpy(d + 14, payload, payload_len);
		rc = ep_start(EP_TX, len, TRBCTL_NORMAL, (len % maxp) == 0);
	}
	spin_unlock_irqrestore(&g.lock, flags);
	return rc;
}

static int gadget_link_up(struct netdev *nd)
{
	(void)nd;
	return g.data_alt ? 1 : 0;
}

/* ── Bring-up ─────────────────────────────────────────────────────────────── */

static int gcc_bring_up(void)
{
	volatile u32 *gdsc = (volatile u32 *)(usize)(g.gcc + GCC_USB30_PRIM_GDSCR);
	volatile u32 *cfg = (volatile u32 *)(usize)(g.gcc + GCC_USB30_PRIM_GDSCR + 4);

	/* Powered is POWER_UP_COMPLETE (bit 16) in the CFG register for this
	 * domain (POLL_CFG_GDSCR), PWR_ON (bit 31) in GDSCR for older ones. */
	say_hex("usb30_prim_gdsc ", ((u64)*cfg << 32) | *gdsc);
	if (*gdsc & 1u) {
		*gdsc &= ~1u;
		for (int i = 0; i < 1500 && !(*gdsc & (1u << 31)) && !(*cfg & (1u << 16)); i++)
			arch_udelay(100);
	}
	if (!(*gdsc & (1u << 31)) && !(*cfg & (1u << 16)))
		say("power-up not reported; trying anyway\n");
	wr(g.gcc, GCC_USB30_PRIM_BCR, 1);
	wr(g.gcc, GCC_QUSB2PHY_PRIM_BCR, 1);
	arch_udelay(10000);
	wr(g.gcc, GCC_QUSB2PHY_PRIM_BCR, 0);
	wr(g.gcc, GCC_USB30_PRIM_BCR, 0);
	arch_udelay(10000);
	for (u32 i = 0; i < sizeof(gcc_usb_clocks) / sizeof(gcc_usb_clocks[0]); i++)
		set_mask(g.gcc, gcc_usb_clocks[i], 1u, 1u);
	arch_udelay(1000);
	say_hex("master clk ", rd(g.gcc, gcc_usb_clocks[0]));
	return 0;
}

static void hsphy_init(void)
{
	u64 b = g.hsphy;

	set_mask(b, PHY_CFG0, PHY_CMN_CTRL_OVERRIDE_EN, PHY_CMN_CTRL_OVERRIDE_EN);
	set_mask(b, PHY_UTMI_CTRL5, PHY_POR, PHY_POR);
	set_mask(b, PHY_HS_CTRL_COMMON0, PHY_FSEL_MASK, 0);
	set_mask(b, PHY_HS_CTRL_COMMON1, PHY_PLLBTUNE, PHY_PLLBTUNE);
	set_mask(b, PHY_REFCLK_CTRL, 0x2, 0x3);
	set_mask(b, PHY_HS_CTRL_COMMON1, PHY_VBUSVLDEXTSEL0, PHY_VBUSVLDEXTSEL0);
	set_mask(b, PHY_HS_CTRL1, PHY_VBUSVLDEXT0, PHY_VBUSVLDEXT0);
	/* The board's tuning override from the vendor tree:
	 * qcom,param-override-seq = <0x43 0x70> (value, register). */
	wr(b, 0x70, 0x43);
	set_mask(b, PHY_HS_CTRL_COMMON2, PHY_VREGBYPASS, PHY_VREGBYPASS);
	set_mask(b, PHY_HS_CTRL2, PHY_SUSPEND_N_SEL | PHY_SUSPEND_N,
	         PHY_SUSPEND_N_SEL | PHY_SUSPEND_N);
	set_mask(b, PHY_UTMI_CTRL0, PHY_SLEEPM, PHY_SLEEPM);
	set_mask(b, PHY_HS_CTRL_COMMON0, PHY_SIDDQ, 0);
	set_mask(b, PHY_UTMI_CTRL5, PHY_POR, 0);
	set_mask(b, PHY_HS_CTRL2, PHY_SUSPEND_N_SEL, 0);
	set_mask(b, PHY_CFG0, PHY_CMN_CTRL_OVERRIDE_EN, 0);
}

static void qscratch_setup(void)
{
	u64 q = g.qscratch;

	/* No SuperSpeed PHY: take the PIPE clock from UTMI. */
	set_mask(q, QSCRATCH_GENERAL_CFG, PIPE_UTMI_CLK_DIS, PIPE_UTMI_CLK_DIS);
	arch_udelay(100);
	set_mask(q, QSCRATCH_GENERAL_CFG, PIPE_UTMI_CLK_SEL | PIPE3_PHYSTATUS_SW,
	         PIPE_UTMI_CLK_SEL | PIPE3_PHYSTATUS_SW);
	arch_udelay(100);
	set_mask(q, QSCRATCH_GENERAL_CFG, PIPE_UTMI_CLK_DIS, 0);
	/* VBUS is sensed by the PMIC, which nothing here talks to. */
	set_mask(q, QSCRATCH_SS_PHY_CTRL, LANE0_PWR_PRESENT, LANE0_PWR_PRESENT);
	set_mask(q, QSCRATCH_HS_PHY_CTRL, UTMI_OTG_VBUS_VALID | SW_SESSVLD_SEL,
	         UTMI_OTG_VBUS_VALID | SW_SESSVLD_SEL);
}

static int core_init(void)
{
	u32 id = rd(g.base, GSNPSID);

	say_hex("GSNPSID ", id);
	if ((id & 0xffff0000u) != 0x55330000u && (id & 0xffff0000u) != 0x33310000u &&
	    (id & 0xffff0000u) != 0x33320000u) {
		say("no DWC3 answers there\n");
		return -1;
	}
	set_mask(g.base, GCTL, GCTL_PRTCAPDIR_MASK | GCTL_SCALEDOWN_MASK,
	         GCTL_PRTCAP_DEVICE);
	wr(g.base, DCTL, DCTL_CSFTRST);
	for (int i = 0; i < 500 && (rd(g.base, DCTL) & DCTL_CSFTRST); i++)
		arch_udelay(1000);
	if (rd(g.base, DCTL) & DCTL_CSFTRST) {
		say("core soft reset did not finish\n");
		return -1;
	}
	/* Linux sets the port direction after the soft reset; say it again. */
	set_mask(g.base, GCTL, GCTL_PRTCAPDIR_MASK, GCTL_PRTCAP_DEVICE);
	set_mask(g.base, GUSB2PHYCFG, GUSB2PHYCFG_SUSPHY | GUSB2PHYCFG_ENBLSLPM, 0);
	set_mask(g.base, GUSB3PIPECTL, GUSB3PIPECTL_SUSPHY, 0);

	/* DMA memory in the window the SM8150 hypervisor lets devices reach
	 * (measured through UFS, see kernel/dev/ufs.c): a DMA into the page
	 * allocator's memory resets the phone. */
	g.evt_phys = GADGET_DMA_BASE;
	for (u32 i = 0; i < 6; i++)
		g.ep_phys[i] = GADGET_DMA_BASE + (u64)(i + 1) * PAGE;
	for (u32 i = 0; i < 6; i++) {
		if (!g.ep_phys[i] || !g.evt_phys) {
			say("no DMA memory\n");
			return -1;
		}
		g.ep_mem[i] = (u8 *)(usize)(g.ep_phys[i] + vmm_direct_map_base());
		memset(g.ep_mem[i], 0, PAGE);
	}
	g.evt = (u8 *)(usize)(g.evt_phys + vmm_direct_map_base());
	memset(g.evt, 0, EVT_BUF_SIZE);
	wr(g.base, GEVNTADRLO, (u32)g.evt_phys);
	wr(g.base, GEVNTADRHI, (u32)(g.evt_phys >> 32));
	wr(g.base, GEVNTSIZ, EVT_BUF_SIZE | GEVNTSIZ_INTMASK);
	wr(g.base, GEVNTCOUNT, rd(g.base, GEVNTCOUNT) & 0xfffc);
	g.evt_pos = 0;

	/* NUMP from the core's own RAM depth, as Linux computes it. */
	u32 ram2 = (rd(g.base, GHWPARAMS7) >> 16) & 0xffff;
	u32 mdwidth = (rd(g.base, GHWPARAMS0) >> 8) & 0xff;
	u32 nump = mdwidth ? ((ram2 * mdwidth / 8) - 24 - 16) / 1024 : 16;
	if (nump > 16)
		nump = 16;
	u32 dcfg = rd(g.base, DCFG);
	dcfg &= ~(DCFG_SPEED_MASK | DCFG_DEVADDR_MASK | DCFG_NUMP_MASK | DCFG_LPM_CAP);
	dcfg |= DCFG_HIGHSPEED | (nump << 17) | DCFG_IGNSTRMPP;
	wr(g.base, DCFG, dcfg);

	if (ep_cmd(EP0_OUT, DEPCMD_DEPSTARTCFG, 0, 0, 0) != 0 ||
	    ep_enable(EP0_OUT, EP_TYPE_CONTROL, 512, 0) != 0 ||
	    ep_enable(EP0_IN, EP_TYPE_CONTROL, 512, 0) != 0) {
		say("control endpoint did not configure\n");
		return -1;
	}
	ep0_setup_start();
	wr(g.base, DEVTEN, DEVTEN_DISCONN | DEVTEN_USBRST | DEVTEN_CONNDONE |
	                   DEVTEN_OVERFLOW);

	u32 dctl = rd(g.base, DCTL);
	dctl &= ~(DCTL_KEEP_CONNECT | DCTL_INITU1ENA | DCTL_ACCEPTU1ENA |
	          DCTL_INITU2ENA | DCTL_ACCEPTU2ENA);
	wr(g.base, DCTL, dctl | DCTL_RUN_STOP);
	for (int i = 0; i < 500 && (rd(g.base, DSTS) & DSTS_DEVCTRLHLT); i++)
		arch_udelay(1000);
	if (rd(g.base, DSTS) & DSTS_DEVCTRLHLT) {
		say_hex("controller stayed halted, DSTS ", rd(g.base, DSTS));
		return -1;
	}
	return 0;
}

/* The apps SMMU (0x15000000) has no stream-match entry for USB (SID 0x140) as
 * the bootloader leaves it, and unmatched streams fault (sCR0.USFCFG), so the
 * controller's first DMA would be refused. Route the stream to the context
 * bank UFS (SID 0x300) already uses — one with translation off — by copying
 * UFS's S2CR into a free stream-map group. Logged with a read-back, because
 * the hypervisor may ignore the write. */
static void smmu_route_usb(void)
{
	const u64 base = 0x15000000ull;
	u32 nsmr = *(volatile u32 *)(usize)(base + 0x20) & 0xff;
	u32 ufs_s2cr = 0xffffffffu;
	int free_idx = -1;

	for (u32 i = 0; i < nsmr; i++) {
		u32 smr = *(volatile u32 *)(usize)(base + 0x800 + i * 4);

		if (!(smr & (1u << 31))) {
			if (free_idx < 0)
				free_idx = (int)i;
			continue;
		}
		if ((smr & 0x7fff) == 0x140) {
			say("smmu: USB stream already routed\n");
			return;
		}
		if ((smr & 0x7fff) == 0x300)
			ufs_s2cr = *(volatile u32 *)(usize)(base + 0xc00 + i * 4);
	}
	if (ufs_s2cr == 0xffffffffu || free_idx < 0) {
		say("smmu: no UFS route to copy or no free group\n");
		return;
	}
	*(volatile u32 *)(usize)(base + 0xc00 + (u32)free_idx * 4) = ufs_s2cr;
	*(volatile u32 *)(usize)(base + 0x800 + (u32)free_idx * 4) = (1u << 31) | 0x140;
	say_hex("smmu: USB routed, smr/s2cr read back ",
	        ((u64)*(volatile u32 *)(usize)(base + 0x800 + (u32)free_idx * 4) << 32) |
	        *(volatile u32 *)(usize)(base + 0xc00 + (u32)free_idx * 4));
}

int dwc3_gadget_probe(void)
{
	if (!bootinfo_has_flag("b1nix.usb-gadget") || !fdt_dwc3_base() ||
	    !fdt_qcom_gcc_base() || !fdt_qcom_hsphy_base())
		return 0;
	memset(&g, 0, sizeof(g));
	g.base = fdt_dwc3_base();
	g.qscratch = g.base + QSCRATCH_OFFSET;
	g.hsphy = fdt_qcom_hsphy_base();
	/* Mapped, not identity: GCC sits inside the unmapped first 2 MiB. */
	g.gcc = (u64)(usize)vmm_map_mmio(fdt_qcom_gcc_base(), 0x1f0000, VMM_WRITABLE);
	if (!g.gcc)
		return 0;
	say_hex("dwc3 at ", g.base);

	/* Panel readout: 3xx is the gadget step reached. */
	BOOTMARK(301);
	if (gcc_bring_up() != 0)
		return 0;
	BOOTMARK(302);
	hsphy_init();
	BOOTMARK(303);
	qscratch_setup();
	BOOTMARK(304);
	smmu_route_usb();
	if (core_init() != 0)
		return 0;
	BOOTMARK(305);

	memcpy(g.nd.mac.bytes, dev_mac, 6);
	memcpy(g.nd.ifname, "usb0", 5);
	g.nd.name = "dwc3-ecm";
	g.nd.transmit = gadget_transmit;
	g.nd.poll = gadget_poll;
	g.nd.link_up = gadget_link_up;
	g.nd.irq = DWC3_IRQ;
	g.nd.irq_ack = gadget_irq_ack;
	netdev_register(&g.nd);
	/* Polling at net_task's idle rate stays the fallback: whether the SPI
	 * reaches this kernel is the hypervisor's to decide. */
	irq_unmask(DWC3_IRQ);
	wr(g.base, GEVNTSIZ, EVT_BUF_SIZE);
	say("running, waiting for a host\n");
	BOOTMARK(309);
	return 1;
}

#else

int dwc3_gadget_probe(void) { return 0; }

#endif
