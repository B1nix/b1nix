/*
 * Host-side model of the Synopsys DWC3 in device mode, for driving
 * kernel/dev/dwc3_gadget.c without the phone.
 *
 * QEMU has no DWC3 device-mode model, so the gadget driver has only ever run
 * on the Xperia 5, where a mistake costs a hang and a trip through fastboot.
 * This builds the driver unchanged on the host (DWC3_MODEL swaps its register
 * accessors for the ones below) and plays the controller and the USB host
 * against it: endpoint commands, TRBs in "DMA" memory with HWO/LST/CHN/CSP/
 * IOC/link semantics as the databook and Linux's dwc3 use them, the event
 * buffer with its count register and interrupt mask, and a host that
 * enumerates the device and then streams frames through bulk OUT while
 * reading bulk IN.
 *
 * What it proves is the driver's own logic: ring indices, event handling,
 * re-arming, lost-event recovery, no endless loops. It cannot prove anything
 * about the silicon that the model gets wrong.
 *
 *   sh tests/dwc3-model/run.sh
 */
#define DWC3_MODEL 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../kernel/dev/dwc3_gadget.c"

/* Controller features the driver does not use (yet); the model knows them. */
#ifndef DEPCMD_UPDATETRANSFER
#define DEPCMD_UPDATETRANSFER 0x07
#endif
#ifndef DEPCFG_XFER_IN_PROGRESS_EN
#define DEPCFG_XFER_IN_PROGRESS_EN (1u << 9)
#endif
#ifndef TRB_CSP
#define TRB_CSP (1u << 3)
#endif
#ifndef EVT_EP_XFERINPROGRESS
#define EVT_EP_XFERINPROGRESS 2
#endif

#define MODEL_BASE 0x10000000ull
static u32 regs[0x100000 / 4];                 /* the DWC3 + QSCRATCH window */
static u8 dma[0x80000] __attribute__((aligned(4096)));
#define DMA_PHYS 0xf1000000ull

static u8 *dma_ptr(u64 phys)
{
	if (phys < DMA_PHYS || phys >= DMA_PHYS + sizeof(dma)) {
		fprintf(stderr, "FAIL: DMA outside the window: 0x%llx\n",
		        (unsigned long long)phys);
		exit(1);
	}
	return dma + (phys - DMA_PHYS);
}

/* ── Event buffer ── */
static u32 evt_pending, evt_wpos, evt_overflows;
static int drop_next_event;

static void push_event(u32 e)
{
	u32 size = regs[GEVNTSIZ / 4] & 0xfffc;
	u64 phys = regs[GEVNTADRLO / 4] | ((u64)regs[GEVNTADRHI / 4] << 32);

	if (drop_next_event) {		/* a lost event, on purpose */
		drop_next_event = 0;
		return;
	}
	if (evt_pending + 4 > size) {
		evt_overflows++;
		return;
	}
	u8 *p = dma_ptr(phys + evt_wpos);
	p[0] = (u8)e; p[1] = (u8)(e >> 8); p[2] = (u8)(e >> 16); p[3] = (u8)(e >> 24);
	evt_wpos = (evt_wpos + 4) % size;
	evt_pending += 4;
}

static void ep_event(u32 phys, u32 type, u32 status)
{
	push_event((phys << 1) | (type << 6) | (status << 12));
}

static void dev_event(u32 type) { push_event(1 | (type << 8)); }

static int irq_line(void)
{
	return evt_pending && !(regs[GEVNTSIZ / 4] & GEVNTSIZ_INTMASK);
}

/* ── Endpoints ── */
struct mep {
	int active, inprog;
	u32 rsc;
	u64 trb;              /* next TRB the controller looks at */
};
static struct mep mep[6];
static int errors;

static void model_fail(const char *what)
{
	fprintf(stderr, "FAIL: %s\n", what);
	errors++;
}

static void depcmd(u32 phys)
{
	u32 base = DEPCMD_BASE + phys * 0x10;
	u32 cmd = regs[(base + 0x0c) / 4];
	u32 p0 = regs[(base + 0x08) / 4], p1 = regs[(base + 0x04) / 4];
	u32 rsc = 0, status = 0;

	switch (cmd & 0xf) {
	case DEPCMD_SETEPCONFIG:
		mep[phys].inprog = (p1 & DEPCFG_XFER_IN_PROGRESS_EN) != 0;
		break;
	case DEPCMD_STARTTRANSFER:
		if (mep[phys].active) {
			model_fail("STARTTRANSFER on an endpoint with a transfer running");
			status = 1;
			break;
		}
		mep[phys].active = 1;
		mep[phys].trb = ((u64)p0 << 32) | p1;
		mep[phys].rsc = phys + 1;
		rsc = mep[phys].rsc;
		break;
	case DEPCMD_UPDATETRANSFER:
		if (!mep[phys].active || ((cmd >> 16) & 0x7f) != mep[phys].rsc) {
			model_fail("UPDATETRANSFER without a matching transfer");
			status = 1;
		}
		break;
	case DEPCMD_ENDTRANSFER:
		if (mep[phys].active && ((cmd >> 16) & 0x7f) != mep[phys].rsc)
			model_fail("ENDTRANSFER with the wrong resource index");
		mep[phys].active = 0;
		break;
	default:
		break;
	}
	regs[(base + 0x0c) / 4] = (cmd & 0xff & ~DEPCMD_CMDACT) | (status << 12) | (rsc << 16);
}

u32 rd(u64 base, u32 off)
{
	u64 o = base - MODEL_BASE + off;

	if (base < MODEL_BASE || o >= sizeof(regs))
		return *(volatile u32 *)(usize)(base + off);
	switch (o) {
	case GSNPSID:    return 0x5533330au;
	case GHWPARAMS0: return 64u << 8;
	case GHWPARAMS7: return 0x1000u << 16;
	case GEVNTCOUNT: return evt_pending;
	case DSTS:       return (regs[DCTL / 4] & DCTL_RUN_STOP) ? 0 : DSTS_DEVCTRLHLT;
	case DCTL:       return regs[DCTL / 4] & ~DCTL_CSFTRST;
	}
	return regs[o / 4];
}

void wr(u64 base, u32 off, u32 v)
{
	u64 o = base - MODEL_BASE + off;

	if (base < MODEL_BASE || o >= sizeof(regs)) {
		*(volatile u32 *)(usize)(base + off) = v;
		return;
	}
	if (o == GEVNTCOUNT) {
		if (v > evt_pending)
			model_fail("GEVNTCOUNT written with more than is pending");
		else
			evt_pending -= v;
		return;
	}
	regs[o / 4] = v;
	if (o >= DEPCMD_BASE && o < DEPCMD_BASE + 6 * 0x10 && (o & 0xf) == 0x0c &&
	    (v & DEPCMD_CMDACT))
		depcmd((u32)((o - DEPCMD_BASE) / 0x10));
}

static u32 trb_word(u64 trb, int w)
{
	u8 *p = dma_ptr(trb + 4u * (u32)w);
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static void trb_set(u64 trb, int w, u32 v)
{
	u8 *p = dma_ptr(trb + 4u * (u32)w);
	p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}

/* The host sends `len` bytes to OUT endpoint `phys` as one transfer ending in
 * a short packet. 0 = NAKed (no TRB the controller may use). */
static int host_out(u32 phys, const u8 *data, u32 len)
{
	struct mep *e = &mep[phys];

	if (!e->active)
		return 0;
	for (int hops = 0; hops < 4; hops++) {
		u32 ctrl = trb_word(e->trb, 3);

		if (!(ctrl & TRB_HWO))
			return 0;
		if (((ctrl >> 4) & 0x3f) == 6) {        /* link */
			e->trb = trb_word(e->trb, 0) | ((u64)trb_word(e->trb, 1) << 32);
			continue;
		}
		u32 size = trb_word(e->trb, 2) & 0xffffff;
		u64 buf = trb_word(e->trb, 0) | ((u64)trb_word(e->trb, 1) << 32);

		if (len > size) {
			model_fail("OUT frame larger than the TRB it landed in");
			return 0;
		}
		if (len)
			memcpy(dma_ptr(buf), data, len);
		trb_set(e->trb, 2, (trb_word(e->trb, 2) & ~0xffffffu) | (size - len));
		trb_set(e->trb, 3, ctrl & ~TRB_HWO);
		int short_pkt = len < size;
		int ends = (ctrl & TRB_LST) || (short_pkt && !(ctrl & TRB_CSP));
		u64 this_trb = e->trb;

		e->trb += 16;
		if (ends) {
			e->active = 0;
			ep_event(phys, EVT_EP_XFERCOMPLETE, 0);
		} else if ((ctrl & TRB_IOC) || (short_pkt && (ctrl & TRB_ISP_IMI))) {
			if (e->inprog)
				ep_event(phys, EVT_EP_XFERINPROGRESS, 0);
		}
		(void)this_trb;
		return 1;
	}
	model_fail("link TRB loop");
	return 0;
}

/* The host reads IN endpoint `phys`: returns bytes, -1 if nothing armed. */
static int host_in(u32 phys, u8 *out)
{
	struct mep *e = &mep[phys];
	int total = 0;

	if (!e->active)
		return -1;
	for (int n = 0; n < 4; n++) {
		u32 ctrl = trb_word(e->trb, 3);

		if (!(ctrl & TRB_HWO))
			return total ? total : -1;
		u32 size = trb_word(e->trb, 2) & 0xffffff;
		u64 buf = trb_word(e->trb, 0) | ((u64)trb_word(e->trb, 1) << 32);

		if (out && size)
			memcpy(out + total, dma_ptr(buf), size);
		total += (int)size;
		trb_set(e->trb, 2, trb_word(e->trb, 2) & ~0xffffffu);
		trb_set(e->trb, 3, ctrl & ~TRB_HWO);
		e->trb += 16;
		if (!(ctrl & TRB_CHN)) {
			e->active = 0;
			ep_event(phys, EVT_EP_XFERCOMPLETE, 0);
			return total;
		}
	}
	return total;
}

/* ── Kernel stubs ── */
static u32 rx_frames, rx_seq_errors, tx_frames;
static u32 expect_seq;
static int ack_every = 8;

void spin_lock_irqsave(spinlock_t *l, u64 *flags)
{
	if (*l)
		model_fail("spinlock taken twice (would deadlock on the phone)");
	*l = 1;
	*flags = 0;
}
void spin_unlock_irqrestore(spinlock_t *l, u64 flags) { (void)flags; *l = 0; }
void console_write(const char *s) { if (getenv("DWC3_MODEL_VERBOSE")) fputs(s, stdout); }
void console_write_dec(u64 v) { if (getenv("DWC3_MODEL_VERBOSE")) printf("%llu", (unsigned long long)v); }
void console_write_hex64(u64 v) { if (getenv("DWC3_MODEL_VERBOSE")) printf("%llx", (unsigned long long)v); }
int bootinfo_has_flag(const char *f) { (void)f; return 0; }
int bootinfo_get_kv(const char *k, char *buf, usize len) { (void)k; (void)buf; (void)len; return 0; }
u64 fdt_dwc3_base(void) { return 0; }
u64 fdt_qcom_gcc_base(void) { return 0; }
u64 fdt_qcom_hsphy_base(void) { return 0; }
void *vmm_map_mmio(u64 phys, usize len, u32 flags) { (void)phys; (void)len; (void)flags; return 0; }
u64 vmm_direct_map_base(void) { return (u64)(usize)dma - DMA_PHYS; }
void net_set_ip(struct ipv4_addr ip) { (void)ip; }
void net_set_netmask(struct ipv4_addr m) { (void)m; }
void route_configure_interface(struct ipv4_addr a, struct ipv4_addr b, struct ipv4_addr c) { (void)a; (void)b; (void)c; }
int netdev_register(struct netdev *nd) { (void)nd; return 0; }
void irq_unmask(int irq) { (void)irq; }
void arch_udelay(u32 us) { (void)us; }

void ethernet_receive_flags(const u8 *frame, usize len, u32 flags)
{
	(void)flags;
	u32 seq = (u32)frame[14] | ((u32)frame[15] << 8) | ((u32)frame[16] << 16);

	if (seq != expect_seq || len != 60 + (seq % 1400))
		rx_seq_errors++;
	expect_seq = seq + 1;
	rx_frames++;
	/* A TCP receiver answers with ACKs, which go out from inside delivery. */
	if (ack_every && rx_frames % (u32)ack_every == 0) {
		static const u8 hdr[14] = { 0 };
		u8 ack[40] = { 0 };

		if (g.nd.transmit(&g.nd, hdr, ack, sizeof(ack), 0) == 0)
			tx_frames++;
	}
}

/* ── The scenario ── */

/* net_task: woken by the interrupt, or on its idle tick. */
static void net_task_round(void)
{
	if (irq_line()) {
		if (g.nd.irq_ack(&g.nd))
			g.nd.poll(&g.nd);
	} else {
		g.nd.poll(&g.nd);
	}
}

static void run_until_quiet(void)
{
	for (int i = 0; i < 1000 && irq_line(); i++)
		net_task_round();
	net_task_round();
}

static void control(u8 type, u8 req, u16 value, u16 index, u16 wlength)
{
	u8 setup[8] = { type, req, (u8)value, (u8)(value >> 8), (u8)index,
	                (u8)(index >> 8), (u8)wlength, (u8)(wlength >> 8) };
	u8 buf[512];

	if (!host_out(EP0_OUT, setup, 8)) {
		model_fail("SETUP not accepted: no SETUP TRB armed");
		return;
	}
	run_until_quiet();
	if (wlength && (type & 0x80)) {
		if (host_in(EP0_IN, buf) < 0)
			model_fail("no data stage for an IN request");
		run_until_quiet();
		ep_event(EP0_OUT, EVT_EP_XFERNOTREADY, STATUS_CONTROL_STATUS);
		run_until_quiet();
		if (!host_out(EP0_OUT, 0, 0))
			model_fail("status stage (OUT) not armed");
	} else {
		ep_event(EP0_IN, EVT_EP_XFERNOTREADY, STATUS_CONTROL_STATUS);
		run_until_quiet();
		if (host_in(EP0_IN, 0) < 0)
			model_fail("status stage (IN) not armed");
	}
	run_until_quiet();
}

static void frame(u8 *f, u32 seq, u32 *len)
{
	*len = 60 + (seq % 1400);
	memset(f, 0, *len);
	f[12] = 0x08;
	f[14] = (u8)seq; f[15] = (u8)(seq >> 8); f[16] = (u8)(seq >> 16);
}

int main(void)
{
	alarm(20);	/* an endless loop in the driver fails the run */
	srand(1);
	g.base = MODEL_BASE;
	g.qscratch = MODEL_BASE + QSCRATCH_OFFSET;
	if (core_init() != 0) {
		fprintf(stderr, "FAIL: core_init\n");
		return 1;
	}
	g.nd.transmit = gadget_transmit;
	g.nd.poll = gadget_poll;
	g.nd.link_up = gadget_link_up;
	g.nd.irq_ack = gadget_irq_ack;
	wr(g.base, GEVNTSIZ, EVT_BUF_SIZE);

	/* Plug in, enumerate, select the data interface. */
	dev_event(EVT_DEV_RESET);
	dev_event(EVT_DEV_CONNDONE);
	run_until_quiet();
	control(0x00, 0x05, 3, 0, 0);            /* SET_ADDRESS */
	control(0x80, 0x06, 0x0100, 0, 18);      /* GET_DESCRIPTOR device */
	control(0x80, 0x06, 0x0200, 0, 255);     /* GET_DESCRIPTOR config */
	control(0x00, 0x09, 1, 0, 0);            /* SET_CONFIGURATION */
	control(0x01, 0x0b, 1, 1, 0);            /* SET_INTERFACE data alt 1 */
	if (!g.data_alt)
		model_fail("data interface not selected after enumeration");

	/* Stream: the host pushes frames as fast as TRBs allow and reads bulk IN
	 * whenever it likes; net_task runs in between, sometimes late, and now
	 * and then an event is lost outright. */
	const u32 total = 30000;
	u32 sent = 0, naks = 0;
	u8 f[2048], in[4096];

	for (u32 round = 0; sent < total && round < 2000000; round++) {
		int burst = 1 + rand() % 40;

		for (int b = 0; b < burst && sent < total; b++) {
			u32 len;

			frame(f, sent, &len);
			if (!host_out(EP_RX, f, len)) {
				naks++;
				break;
			}
			sent++;
		}
		if (rand() % 3)
			host_in(EP_TX, in);
		if (rand() % 500 == 0)
			drop_next_event = 1;
		if (rand() % 4)
			net_task_round();
	}
	for (int i = 0; i < 100; i++) {
		host_in(EP_TX, in);
		net_task_round();
	}

	printf("sent %u, received %u, out of order %u, naks %u, acks out %u, "
	       "event overflows %u\n", sent, rx_frames, rx_seq_errors, naks,
	       tx_frames, evt_overflows);
	if (sent != total)
		model_fail("the host could not get every frame out");
	if (rx_frames != sent)
		model_fail("frames lost between the wire and the stack");
	if (rx_seq_errors)
		model_fail("frames delivered out of order or with the wrong length");
	if (errors) {
		printf("dwc3-model: %d failure(s)\n", errors);
		return 1;
	}
	printf("dwc3-model: ok\n");
	return 0;
}
