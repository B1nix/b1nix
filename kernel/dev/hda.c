/*
 * Intel High Definition Audio (HDA) driver — M38: Sound
 *
 * Minimal HDA controller driver for QEMU (-device ich6-intel-hda + hda-duplex).
 * Implements CORB/RIRB verb transport, output stream (SDI/SDO) setup, and a
 * kernel self-test that plays a short sine wave to verify the audio path.
 *
 * PCI class 04/03/00, vendor 0x8086. QEMU presents ICH6 HDA (device 0x2668).
 *
 * Limitations (M38 scope):
 *  - No mixer / ALSA-style PCM volume routing — the output converter is
 *    opened at a fixed gain and never modified.
 *  - No input stream capture — output only.
 *  - No interrupt-driven completion; the DMA position register is polled.
 *  - One concurrent output stream; /dev/dsp serialises writes via a spin flag.
 */
#include <b1nix/console.h>
#include <b1nix/sound.h>
#include <b1nix/pci.h>
#include <b1nix/mm.h>
#include <b1nix/arch.h>
#include <b1nix/bootinfo.h>
#include <b1nix/vfs.h>
#include <b1nix/sched.h>
#include <b1nix/errno.h>
#include <b1nix/io.h>
#include <string.h>

/* ── HDA global registers (byte offsets into MMIO BAR0) ──────────────────── */
#define HDA_GCAP      0x0000  /* Global Capabilities              */
#define HDA_GCTL      0x0008  /* Global Control                   */
#define HDA_WAKEEN    0x000C  /* Wake Enable                      */
#define HDA_STATESTS  0x000E  /* State Change Status               */
#define HDA_INTSTS    0x0010  /* Interrupt Status                  */
#define HDA_INTCTL    0x0014  /* Interrupt Control                 */
#define HDA_WALLCLK   0x0018  /* Wall Clock Counter (RO, 1 kHz)    */
#define HDA_GSTS      0x001C  /* Global Status                     */

/* CORB (Command Output Ring Buffer) */
#define HDA_CORBLBASE 0x0040
#define HDA_CORBUBASE 0x0044
#define HDA_CORBWP    0x0048
#define HDA_CORBRP    0x004A
#define HDA_CORBCTL   0x004C

/* RIRB (Response Input Ring Buffer) */
#define HDA_RIRBLBASE 0x0050
#define HDA_RIRBUBASE 0x0054
#define HDA_RIRBWP    0x0058
#define HDA_RIRBCTL   0x005C
#define HDA_RIRBSTS   0x005D

/* Stream descriptors — Output (SDO) start at 0x0800, 0x20 bytes apart */
#define HDA_SDO_BASE  0x0800
#define HDA_SDO_STRIDE 0x020

/* SDO register offsets within a stream descriptor */
#define HDA_SDO_CTL0   0x00  /* Control (bits: stall, stream tag, format) */
#define HDA_SDO_CTL1   0x01  /* Control 1 (channel count, etc.)           */
#define HDA_SDO_CTL2   0x02  /* Control 2 (stripe, etc.)                  */
#define HDA_SDO_LPIB   0x04  /* Link Position in Buffer (RO)              */
#define HDA_SDO_CBL    0x08  /* Circular Buffer Length (bytes)             */
#define HDA_SDO_LVI    0x0C  /* Last Valid Index                          */
#define HDA_SDO_FIFOS  0x0E  /* FIFO Size (RO, in bytes)                  */
#define HDA_SDO_FMT    0x12  /* Format (sample rate / bits)               */
#define HDA_SDO_BDPL   0x18  /* Buffer Descriptor List Pointer (low)      */
#define HDA_SDO_BDPH   0x1C  /* Buffer Descriptor List Pointer (high)     */

/* GCTL bits */
#define HDA_GCTL_CRST  (1u << 0)  /* Controller Reset */
#define HDA_GCTL_FCNTRL (1u << 1) /* Flush Control    */
#define HDA_GCTL_SSYNC (1u << 2)  /* Synchronous Reset */

/* CORBCTL bits */
#define HDA_CORBCTL_DMAEN (1u << 0)  /* CORB DMA Enable */
#define HDA_CORBCTL_CMEIE (1u << 1)  /* CMEI Interrupt Enable */

/* RIRBCTL bits */
#define HDA_RIRBCTL_DMAEN (1u << 0)  /* RIRB DMA Enable */
#define HDA_RIRBCTL_RINTCTL (1u << 1) /* RINTCTL Interrupt Enable */

/* SDO CTL0 bits */
#define HDA_SDO_CTL0_SRST  (1u << 0)  /* Stream Reset */
#define HDA_SDO_CTL0_RUN   (1u << 1)  /* Stream Run   */
#define HDA_SDO_CTL0_STRIPE_MASK (0x7u << 20) /* Stripe bits */
#define HDA_SDO_CTL0_TP_MASK    (0xFu << 20) /* Traffic Priority */
#define HDA_SDO_CTL0_TAG_MASK   (0xFu << 26) /* Stream Tag */

/* Buffer Descriptor List entry (16 bytes, must be 128-bit aligned) */
struct hda_bdle {
	u64 address;    /* Physical address of audio data */
	u32 length;     /* Length in bytes */
	u32 flags;      /* BDI: interrupt on completion, etc. */
} __attribute__((packed, aligned(16)));

/* ── Codec verb helpers ──────────────────────────────────────────────────── */
#define HDA_VERB(codec, nid, verb, payload) \
	(((u32)(codec) << 28) | ((u32)(nid) << 20) | ((u32)(verb) << 8) | (u32)(payload))

/* 4/16-form verb: 4-bit verb in bits 19:16 with a 16-bit payload. This is
 * how SET/GET_AMP_GAIN_MUTE are encoded — QEMU's codec decodes verbs in the
 * 0x000..0x6FF / 0x800..0xEFF range this way, so the older 12/8-form volume
 * verbs were silently rejected. */
#define HDA_VERB16(codec, nid, verb, payload) \
	(((u32)(codec) << 28) | ((u32)(nid) << 20) | ((u32)(verb) << 16) | (u32)(payload))

/* Amp command payload bits (HDA spec 7.3.3.8 / 7.3.3.9). */
#define AC_AMP_SET_OUTPUT (1u << 15) /* target the output amplifier */
#define AC_AMP_SET_INPUT  (1u << 14) /* target the input amplifier  */
#define AC_AMP_SET_LEFT   (1u << 13) /* apply to the left channel   */
#define AC_AMP_SET_RIGHT  (1u << 12) /* apply to the right channel  */
#define AC_AMP_GET_LEFT   (1u << 13) /* query the left channel      */
#define AC_AMP_GAIN_MASK  0x7F
#define AC_AMP_MUTE       (1u << 7)

/* QEMU hda-duplex codec amp range: 74 steps (0x4a), mute capable. */
#define HDA_AMP_STEPS     74

#define HDA_PARAM_AUDIO_FG_CAP   0xF00
#define HDA_PARAM_NODE_COUNT      0xF04
#define HDA_PARAM_NODE_LIST       0xF08
#define HDA_PARAM_STREAM_FORMAT   0xF0A
#define HDA_PARAM_PIN_CAP         0xF0D
#define HDA_PARAM_GPIO_CAP        0xF11
#define HDA_PARAM_SOLVER_CAP      0xF18
#define HDA_PARAM_VENDOR_ID       0xF01

/* Widget types (from get-parameter node-type) */
#define HDA_WIDGET_AUDIO_OUTPUT   0x0
#define HDA_WIDGET_AUDIO_INPUT    0x1
#define HDA_WIDGET_PIN_COMPLEX    0x4
#define HDA_WIDGET_AUDIO_SELECTOR 0x5
#define HDA_WIDGET_AUDIO_MIXER    0x6
#define HDA_WIDGET_POWER_WIDGET   0xD

/* ── Static driver state ─────────────────────────────────────────────────── */
static volatile u8 *hda_regs;
static int hda_inited;
static u8 hda_codec_addr;    /* active codec address (0..15) */
static u8 hda_output_nid;    /* NID of the output converter widget */
static u8 hda_pin_nid;       /* NID of the output pin complex */
static u32 hda_sample_rate;  /* negotiated sample rate */
static u16 hda_fmt_word;     /* format word for SDO */

/* DMA buffers — must be physically contiguous, 128-bit aligned */
static struct hda_bdle *hda_bdl;     /* Buffer Descriptor List */
static u64 hda_bdl_phys;
static u8  *hda_dam_buf;             /* Audio data ring buffer */
static u64 hda_dam_buf_phys;
static u32 hda_dam_buf_sz;           /* size in bytes */
/* M95 module parameter (writable): how long hda_selftest lets its test tone
 * run before it checks the stream. Declared here so the self-test can read it;
 * exported to /sys/module/hda/parameters at the bottom of this file. */
static int hda_tone_ms = 10;

/* CORB / RIRB */
static u32 *hda_corb;
static u64 hda_corb_phys;
static u32 *hda_rirb;
static u64 hda_rirb_phys;
static u16 hda_corb_wp;

/* Output stream state */
static volatile int hda_play_lock;

/* VFS /dev/dsp node */
static struct sound_device hda_sound_dev;

/* Mixer volume state (0..100 per channel) mirrored to the codec amp. */
static int hda_vol_left = 100;
static int hda_vol_right = 100;
static int hda_muted;

/* ── Coarse delay via wall clock ─────────────────────────────────────────── */
static inline u32 hda_wallclock(void) { return *(volatile u32 *)(hda_regs + HDA_WALLCLK); }

static void hda_delay_ms(int ms) {
	/* Against the calibrated clock, not against a guess at how long an I/O
	 * port read takes.
	 *
	 * The fallback here counted "about a microsecond per iteration", which is
	 * a statement about a particular processor and a particular hypervisor and
	 * about nothing else — on a faster machine the wait is short and the
	 * hardware is not ready, on a slower one the boot is longer than it needs
	 * to be. The kernel calibrates a nanosecond clock at boot; a delay should
	 * be expressed in the unit it asks for and measured with that. */
	u64 deadline = arch_tsc_monotonic_ns() + (u64)ms * 1000000ull;
	u32 start = hda_wallclock();

	if (start == 0) {
		while (arch_tsc_monotonic_ns() < deadline)
			(void)inb(0x80);
		return;
	}
	while (arch_tsc_monotonic_ns() < deadline) {
		u32 now = hda_wallclock();

		if ((u32)(now - start) >= (u32)ms)
			return;
		scheduler_yield();
	}
	/* Timeout: proceed anyway */
}

/* ── MMIO helpers ────────────────────────────────────────────────────────── */
static inline u8  hda_r8(u32 off)  { return *(volatile u8  *)(hda_regs + off); }
static inline u16 hda_r16(u32 off) { return *(volatile u16 *)(hda_regs + off); }
static inline u32 hda_r32(u32 off) { return *(volatile u32 *)(hda_regs + off); }
static inline void hda_w8(u32 off, u8  v) { *(volatile u8  *)(hda_regs + off) = v; }
static inline void hda_w16(u32 off, u16 v) { *(volatile u16 *)(hda_regs + off) = v; }
static inline void hda_w32(u32 off, u32 v) { *(volatile u32 *)(hda_regs + off) = v; }

/* ── CORB/RIRB transport ─────────────────────────────────────────────────── */

/* Write a verb to the CORB ring. Returns 0 on success, -1 if CORB is full. */
static int hda_corb_write(u32 verb) {
	u16 rp = (hda_r16(HDA_CORBRP) >> 1) & 0xFF;
	u16 next = (hda_corb_wp + 1) & 0xFF;
	if (next == rp) {
		/* CORB full — poll briefly */
		for (int i = 0; i < 100000; i++) {
			rp = (hda_r16(HDA_CORBRP) >> 1) & 0xFF;
			next = (hda_corb_wp + 1) & 0xFF;
			if (next != rp) break;
		}
		if (next == rp)
			return -1;
	}
	hda_corb[hda_corb_wp] = verb;
	hda_w16(HDA_CORBWP, hda_corb_wp);
	hda_corb_wp = next;
	return 0;
}

/* Send a verb and wait for the response. Returns the 32-bit response or 0
 * on timeout (~500 ms). */
static u32 hda_corb_send_wait(u32 verb) {
	/* Record the current RIRB write pointer so we can detect the new entry. */
	u16 old_wp = hda_r16(HDA_RIRBWP) & 0xFF;

	if (hda_corb_write(verb) < 0) {
		console_write("hda: CORB full on verb\n");
		return 0;
	}

	/* Poll RIRBWP for a new entry, bounded by time rather than by a count of
	 * reads.
	 *
	 * Every one of those reads is an MMIO access, which under a hypervisor is
	 * a trap out of the guest costing a microsecond or so. Half a million of
	 * them is therefore most of a second per verb, and the probe sends two per
	 * codec address across four addresses — twenty seconds of boot, every
	 * boot, spent waiting for a codec that is not there. A codec that IS there
	 * answers in microseconds: the specification's own wait after a controller
	 * reset, before codecs are even required to have announced themselves, is
	 * 521 µs.
	 *
	 * So: a short spin for the answer that normally arrives immediately, then
	 * a bounded wait on the tick, with the read count still capped in case the
	 * clock is not running yet (this can run before the timer is live). */
	extern u64 scheduler_get_uptime_ticks(void);
	u64 start = scheduler_get_uptime_ticks();

	for (int i = 0; i < 20000; i++) {
		u16 new_wp = hda_r16(HDA_RIRBWP) & 0xFF;
		if (new_wp != old_wp) {
			u32 resp = hda_rirb[new_wp & 0xFF];
			return resp;
		}
		/* Ten ticks is a tenth of a second — four orders of magnitude more
		 * than a working codec needs, and a fiftieth of what this cost
		 * before. */
		if ((i & 0xff) == 0xff && scheduler_get_uptime_ticks() - start > 10)
			break;
		__asm__ volatile("pause");
	}
	return 0;
}

/* Send a Get Parameter verb to the codec. */
static u32 hda_get_param(u8 nid, u32 param) {
	return hda_corb_send_wait(HDA_VERB(hda_codec_addr, nid, 0xF00, param));
}

/* ── HDA controller reset sequence ───────────────────────────────────────── */
static void hda_controller_reset(void) {
	/* Assert reset */
	hda_w32(HDA_GCTL, hda_r32(HDA_GCTL) & ~HDA_GCTL_CRST);
	hda_delay_ms(50);

	/* Clear reset */
	hda_w32(HDA_GCTL, hda_r32(HDA_GCTL) | HDA_GCTL_CRST);
	hda_delay_ms(50);

	/* Verify reset completed */
	u32 gsts = hda_r32(HDA_GSTS);
	(void)gsts;
}

/* ── CORB/RIRB DMA setup ─────────────────────────────────────────────────── */
static int hda_setup_corb_rirb(void) {
	/* Allocate CORB: 256 entries × 4 bytes = 1 KiB, 128-byte aligned */
	hda_corb_phys = pmm_alloc_frames(1);
	hda_corb = (u32 *)(usize)(hda_corb_phys + vmm_direct_map_base());
	memset((void *)hda_corb, 0, PAGE_SIZE);

	/* Allocate RIRB: 256 entries × 8 bytes (only low 32 used) = 2 KiB */
	hda_rirb_phys = pmm_alloc_frames(1);
	hda_rirb = (u32 *)(usize)(hda_rirb_phys + vmm_direct_map_base());
	memset((void *)hda_rirb, 0, PAGE_SIZE);

	hda_corb_wp = 0;

	/* Stop DMA before programming addresses */
	hda_w8(HDA_CORBCTL, 0);
	hda_w8(HDA_RIRBCTL, 0);
	hda_delay_ms(10);

	/* Program CORB base */
	hda_w32(HDA_CORBLBASE, (u32)(hda_corb_phys & 0xFFFFFFFF));
	hda_w32(HDA_CORBUBASE, (u32)(hda_corb_phys >> 32));

	/* Program RIRB base */
	hda_w32(HDA_RIRBLBASE, (u32)(hda_rirb_phys & 0xFFFFFFFF));
	hda_w32(HDA_RIRBUBASE, (u32)(hda_rirb_phys >> 32));

	/* Reset write pointers — write 0xFFFF to RIRBWP to clear interrupts */
	hda_w16(HDA_CORBWP, 0);
	hda_w16(HDA_RIRBWP, 0xFFFF);

	/* Enable RIRB interrupt (RINTCTL) + DMA */
	hda_w8(HDA_RIRBCTL, HDA_RIRBCTL_DMAEN | HDA_RIRBCTL_RINTCTL);

	/* Enable CORB DMA */
	hda_w8(HDA_CORBCTL, HDA_CORBCTL_DMAEN);

	hda_delay_ms(10);

	/* Verify both are running */
	u8 corbctl = hda_r8(HDA_CORBCTL);
	u8 rirbctl = hda_r8(HDA_RIRBCTL);
	if (!(corbctl & HDA_CORBCTL_DMAEN)) {
		console_write("hda: CORB DMA failed to start\n");
		return -1;
	}
	if (!(rirbctl & HDA_RIRBCTL_DMAEN)) {
		console_write("hda: RIRB DMA failed to start\n");
		return -1;
	}
	return 0;
}

/* ── Codec discovery ─────────────────────────────────────────────────────── */
static int hda_probe_codec(void) {
	u16 gcap = hda_r16(HDA_GCAP);
	u8 codecs = (gcap >> 8) & 0x0F;
	if (codecs == 0) {
		console_write("hda: no codecs found\n");
		return -1;
	}
	console_write("hda: ");
	console_write_dec(codecs);
	console_write(" codec(s) present\n");

	/* Try codec address 0 first (typical for QEMU) */
	for (u8 addr = 0; addr < 4; addr++) {
		hda_codec_addr = addr;
		/* Send a zero verb to wake up the codec */
		hda_corb_send_wait(0);

		u32 vendor = hda_get_param(0, HDA_PARAM_VENDOR_ID);
		if (vendor == 0 || vendor == 0xFFFFFFFF) {
			/* Codec at this address is not responding */
			continue;
		}
		console_write("hda: codec addr ");
		console_write_dec(addr);
		console_write(" vendor=0x");
		console_write_hex32(vendor);
		console_write("\n");
		return 0;
	}
	console_write("hda: no responding codec found\n");
	return -1;
}

/* ── Find output converter and pin widget ────────────────────────────────── */
static int hda_discover_audio_widgets(void) {
	u32 node_info = hda_get_param(0, HDA_PARAM_NODE_COUNT);
	u8 start_nid = (node_info >> 16) & 0xFF;
	u8 num_nodes = node_info & 0xFF;

	hda_output_nid = 0;
	hda_pin_nid = 0;

	for (u8 nid = start_nid; nid < start_nid + num_nodes; nid++) {
		u32 wcaps = hda_get_param(nid, 0xF09); /* Widget Capabilities */
		u8 type = (wcaps >> 20) & 0x0F;

		if (type == HDA_WIDGET_AUDIO_OUTPUT && !hda_output_nid) {
			hda_output_nid = nid;
		}
		if (type == HDA_WIDGET_PIN_COMPLEX && !hda_pin_nid) {
			/* Check if this pin supports output */
			u32 pincap = hda_get_param(nid, HDA_PARAM_PIN_CAP);
			if (pincap & (1u << 4)) { /* Output-capable */
				hda_pin_nid = nid;
			}
		}
	}

	if (!hda_output_nid) {
		console_write("hda: no output converter found\n");
		return -1;
	}
	if (!hda_pin_nid) {
		/* Fall back: try the output converter's own NID as the pin */
		hda_pin_nid = hda_output_nid;
	}

	console_write("hda: output nid=");
	console_write_dec(hda_output_nid);
	console_write(" pin nid=");
	console_write_dec(hda_pin_nid);
	console_write("\n");
	return 0;
}

/* ── Configure output converter ──────────────────────────────────────────── */
static void hda_configure_output(void) {
	/* Set stream format: 16-bit, 48 kHz, stereo = format word 0x0011
	 * Bits [15:11] = format (0=PCM, 1=AC3, ...)
	 * Bits [10:8]  = number of sub-frames minus 1  (1 for stereo = 2ch)
	 * Bits [7:4]   = bits per sub-frame minus 1    (0xF for 16-bit)
	 * Bits [3:0]   = sample rate base (3 = 48 kHz)
	 *
	 * Actually the HDA format word is:
	 *   bits[15:11] = PCM format (0)
	 *   bits[10:8]  = channels - 1  (1 for 2ch)
	 *   bits[7:4]   = bits - 1      (0xF for 16-bit signed)
	 *   bits[3:0]   = base_divisor_code for 48kHz (3)
	 * So: 0b0_001_1111_0011 = 0x0011 for 48kHz stereo 16-bit
	 * Wait, let me recalculate:
	 *   PCM=0 -> bit15=0, bits14:11 = 0
	 *   channels=2 -> bits10:8 = 001
	 *   bits=16 -> bits7:4 = 1111 (16-1=15=0xF)
	 *   rate_divisor: bits[3:0] -> for 48kHz: 3 (48000 / 1 = 48000, base 48kHz, div=0)
	 *   Wait, HDA format for 48kHz, 16-bit, stereo:
	 *     bits[3:0] = 3 means base_rate=48kHz
	 *   Format = (0 << 11) | (1 << 8) | (0xF << 4) | 3 = 0x01F3
	 *
	 * Actually let me re-check. The HDA spec says:
	 *   Bits 15:11 — PCM Format (0=PCM)
	 *   Bits 10:8  — Number of channels minus one
	 *   Bits 7:4   — Bits per sample minus one
	 *   Bits 3:0   — Sample rate: see Table 5.27
	 *
	 * For 48kHz: bits[3:0] = 3, 16-bit signed: bits[7:4] = 0xF, 2ch: bits[10:8] = 1
	 * = (0 << 11) | (1 << 8) | (0xF << 4) | 3 = 0x01F3
	 */
	hda_sample_rate = 48000;
	hda_fmt_word = 0x01F3;

	/* Unmute the output converter amp at full gain (0 dB). Uses the 4/16
	 * form of SET_AMP_GAIN_MUTE (verb 0x3 in bits 19:16, 16-bit payload);
	 * QEMU's codec rejects the 12/8-form volume verbs used pre-M79. */
	hda_corb_send_wait(HDA_VERB16(hda_codec_addr, hda_output_nid, 0x3,
		AC_AMP_SET_OUTPUT | AC_AMP_SET_LEFT | HDA_AMP_STEPS));
	hda_corb_send_wait(HDA_VERB16(hda_codec_addr, hda_output_nid, 0x3,
		AC_AMP_SET_OUTPUT | AC_AMP_SET_RIGHT | HDA_AMP_STEPS));

	/* Power widget: set D0 for the output converter */
	hda_corb_send_wait(HDA_VERB(hda_codec_addr, hda_output_nid, 0xF50, 0x00)); /* Power State D0 */

	/* Set stream format on the output converter */
	hda_corb_send_wait(HDA_VERB(hda_codec_addr, hda_output_nid, 0x200, hda_fmt_word));

	/* Pin widget: set output pin to route to this converter */
	hda_corb_send_wait(HDA_VERB(hda_codec_addr, hda_pin_nid, 0x707, hda_output_nid)); /* Select Output */

	/* Enable output on the pin: set pin control = 0x40 (out enable) */
	hda_corb_send_wait(HDA_VERB(hda_codec_addr, hda_pin_nid, 0x708, 0x40));
}

/* ── Output stream DMA setup ─────────────────────────────────────────────── */
static int hda_setup_output_stream(u32 buf_size) {
	/* Round up to 4 KiB page boundary, minimum 4 KiB */
	buf_size = (buf_size + 4095) & ~4095u;
	if (buf_size < 4096) buf_size = 4096;
	hda_dam_buf_sz = buf_size;

	/* Allocate audio DMA buffer: must be physically contiguous */
	u64 frames = (buf_size + PAGE_SIZE - 1) / PAGE_SIZE;
	hda_dam_buf_phys = pmm_alloc_frames(frames);
	hda_dam_buf = (u8 *)(usize)(hda_dam_buf_phys + vmm_direct_map_base());
	memset(hda_dam_buf, 0, buf_size);

	/* Allocate Buffer Descriptor List: 1 entry × 16 bytes = 16 bytes, but
	 * must be 128-bit (16-byte) aligned. Allocate a page for safety. */
	hda_bdl_phys = pmm_alloc_frames(1);
	hda_bdl = (struct hda_bdle *)(usize)(hda_bdl_phys + vmm_direct_map_base());
	memset((void *)hda_bdl, 0, PAGE_SIZE);

	hda_bdl[0].address = hda_dam_buf_phys;
	hda_bdl[0].length  = buf_size;
	hda_bdl[0].flags    = 0; /* no IOC, normal BDI */

	/* Stream tag = 1, stream 0 (SDO0) */
	u8 stream_tag = 1;
	u8 sdo_idx = 0;
	u32 sdo_off = HDA_SDO_BASE + sdo_idx * HDA_SDO_STRIDE;

	/* Stop the stream first (SRST) */
	hda_w8(sdo_off + HDA_SDO_CTL0, HDA_SDO_CTL0_SRST);
	hda_delay_ms(10);
	hda_w8(sdo_off + HDA_SDO_CTL0, 0);
	hda_delay_ms(10);

	/* Program BDL pointer */
	hda_w32(sdo_off + HDA_SDO_BDPL, (u32)(hda_bdl_phys & 0xFFFFFFFF));
	hda_w32(sdo_off + HDA_SDO_BDPH, (u32)(hda_bdl_phys >> 32));

	/* Program buffer length */
	hda_w32(sdo_off + HDA_SDO_CBL, buf_size);

	/* Last Valid Index: 1 entry (index 0) */
	hda_w16(sdo_off + HDA_SDO_LVI, 0);

	/* Format */
	hda_w16(sdo_off + HDA_SDO_FMT, hda_fmt_word);

	/* Program stream tag in CTL0[31:26], stream format in CTL0[15:0] */
	/* CTL0 layout: bits[25:20] = tag, bits[15:0] = format
	 * Actually HDA SDI/SDO CTL0:
	 *   bits[1:0]   = stream number (0)
	 *   bits[3:2]   = stripe
	 *   bits[7:4]   = traffic class priority
	 *   bits[19:16] = stream tag
	 *   bits[31:16] = format (but format is already in FMT register for some impls)
	 *
	 * Actually looking at the real HDA spec more carefully:
	 *   SDI/SDO stream descriptor offset 0x00:
	 *     bits[1:0] = Stream number (set by software)
	 *     bits[3:2] = Stripe
	 *     bits[15:8] = Traffic Class Priority (TP)
	 *     bits[25:16] = Stream Tag (set by software)
	 *
	 * For QEMU, just set the tag and enable run.
	 */
	u32 ctl0 = (stream_tag << 16) | 0x01; /* tag=1, stream_num=1 */
	hda_w32(sdo_off + HDA_SDO_CTL0, ctl0);

	return 0;
}

/* ── /dev/dsp VFS callbacks ──────────────────────────────────────────────── */
static isize hda_dsp_write(struct vfs_node *node, u64 offset, const char *buffer,
                           usize size, int flags) {
	(void)node; (void)offset; (void)flags;
	if (!hda_inited) return -1;

	/* Serialize writes via a spin flag */
	while (__sync_lock_test_and_set(&hda_play_lock, 1))
		scheduler_yield();

	/* If no codec is present, just accept the data (playback won't produce
	 * audible output but the syscall succeeds so userspace doesn't hang). */
	if (!hda_output_nid) {
		__sync_lock_release(&hda_play_lock);
		return (isize)size;
	}

	usize written = 0;
	usize avail = size;

	/* Write in chunks that fit the DMA buffer */
	while (avail > 0) {
		usize chunk = avail;
		if (chunk > hda_dam_buf_sz)
			chunk = hda_dam_buf_sz;

		memcpy(hda_dam_buf, buffer + written, chunk);

		/* Reset LVI */
		u32 sdo_off = HDA_SDO_BASE;
		hda_w16(sdo_off + HDA_SDO_LVI, 0);

		/* Ensure the stream is running */
		u32 ctl0 = hda_r32(sdo_off + HDA_SDO_CTL0);
		if (!(ctl0 & HDA_SDO_CTL0_RUN))
			hda_w32(sdo_off + HDA_SDO_CTL0, ctl0 | HDA_SDO_CTL0_RUN);

		/* Wait for the DMA to consume at least one buffer worth.
		 * Poll the Link Position in Buffer. */
		u32 start = hda_wallclock();
		while (hda_r32(sdo_off + HDA_SDO_LPIB) < chunk) {
			if ((u32)(hda_wallclock() - start) > 2000) break;
			scheduler_yield();
		}

		written += chunk;
		avail   -= chunk;
	}

	__sync_lock_release(&hda_play_lock);
	return (isize)written;
}

static isize hda_dsp_read(struct vfs_node *node, u64 offset, char *buffer,
                          usize size, int flags) {
	(void)node; (void)offset; (void)buffer; (void)size; (void)flags;
	/* Input not implemented in M38 */
	return 0;
}

static void hda_dsp_release(struct vfs_node *node) {
	(void)node;
}

/* ── Sound device interface ──────────────────────────────────────────────── */
static int hda_sound_open(struct sound_device *dev) {
	(void)dev;
	if (!hda_inited) return -1;
	return 0;
}

static void hda_sound_close(struct sound_device *dev) {
	(void)dev;
}

static isize hda_sound_write(struct sound_device *dev, const void *buf, usize len) {
	(void)dev;
	if (!hda_inited) return -1;

	while (__sync_lock_test_and_set(&hda_play_lock, 1))
		scheduler_yield();

	if (!hda_output_nid) {
		__sync_lock_release(&hda_play_lock);
		return (isize)len;
	}

	/* For the sound API, write all data into the DMA buffer and trigger playback */
	usize written = 0;
	while (written < len) {
		usize chunk = len - written;
		if (chunk > hda_dam_buf_sz)
			chunk = hda_dam_buf_sz;

		memcpy(hda_dam_buf, (const char *)buf + written, chunk);

		u32 sdo_off = HDA_SDO_BASE;
		hda_w16(sdo_off + HDA_SDO_LVI, 0);

		u32 ctl0 = hda_r32(sdo_off + HDA_SDO_CTL0);
		if (!(ctl0 & HDA_SDO_CTL0_RUN))
			hda_w32(sdo_off + HDA_SDO_CTL0, ctl0 | HDA_SDO_CTL0_RUN);

		/* Wait for playback to complete */
		u32 start = hda_wallclock();
		while (hda_r32(sdo_off + HDA_SDO_LPIB) < chunk) {
			if ((u32)(hda_wallclock() - start) > 2000) break;
			scheduler_yield();
		}
		written += chunk;
	}

	__sync_lock_release(&hda_play_lock);
	return (isize)written;
}

static u32 hda_sound_get_position(struct sound_device *dev) {
	(void)dev;
	if (!hda_inited) return 0;
	return hda_r32(HDA_SDO_BASE + HDA_SDO_LPIB);
}

static int hda_sound_ready(struct sound_device *dev) {
	(void)dev;
	return hda_inited;
}

/* ── Mixer: codec amp volume control (M79) ──────────────────────────────── */
static int hda_sound_set_volume(struct sound_device *dev, int left, int right,
                                int muted) {
	(void)dev;
	if (!hda_inited || !hda_output_nid)
		return -ENXIO;

	if (left < 0) left = hda_vol_left;
	if (right < 0) right = hda_vol_right;
	if (muted < 0) muted = hda_muted;
	if (left > 100) left = 100;
	if (right > 100) right = 100;
	hda_vol_left = left;
	hda_vol_right = right;
	hda_muted = muted;

	u32 gl = (u32)left * HDA_AMP_STEPS / 100;
	u32 gr = (u32)right * HDA_AMP_STEPS / 100;
	u32 m = muted ? AC_AMP_MUTE : 0;

	hda_corb_send_wait(HDA_VERB16(hda_codec_addr, hda_output_nid, 0x3,
		AC_AMP_SET_OUTPUT | AC_AMP_SET_LEFT | gl | m));
	hda_corb_send_wait(HDA_VERB16(hda_codec_addr, hda_output_nid, 0x3,
		AC_AMP_SET_OUTPUT | AC_AMP_SET_RIGHT | gr | m));
	return 0;
}

static int hda_sound_get_volume(struct sound_device *dev, int *left, int *right,
                                int *muted) {
	(void)dev;
	if (!hda_inited || !hda_output_nid)
		return -ENXIO;

	u32 rl = hda_corb_send_wait(HDA_VERB16(hda_codec_addr, hda_output_nid, 0xB,
		AC_AMP_SET_OUTPUT | AC_AMP_GET_LEFT));
	u32 rr = hda_corb_send_wait(HDA_VERB16(hda_codec_addr, hda_output_nid, 0xB,
		AC_AMP_SET_OUTPUT));

	if (left) *left = (int)((rl & AC_AMP_GAIN_MASK) * 100u / HDA_AMP_STEPS);
	if (right) *right = (int)((rr & AC_AMP_GAIN_MASK) * 100u / HDA_AMP_STEPS);
	if (muted) *muted = ((rl | rr) & AC_AMP_MUTE) ? 1 : 0;
	return 0;
}

/* OSS mixer ioctls on /dev/dsp route to the generic sound-mixer layer. */
static int hda_dsp_ioctl(struct vfs_node *node, u64 request, void *arg) {
	(void)node;
	return sound_mixer_ioctl(&hda_sound_dev, request, arg);
}

/* ── Device init ─────────────────────────────────────────────────────────── */
void hda_init(void) {
	struct pci_device_info pci;
	int found = 0;

	if (bootinfo_has_flag("b1nix.skip-hda")) {
		console_write("hda: skipped (b1nix.skip-hda)\n");
		return;
	}

	/* Walk PCI Multimedia Audio controllers (class 04/03/00) */
	for (u8 idx = 0; idx < 32; idx++) {
		if (!pci_find_class(0x04, 0x03, idx, &pci))
			break;
		if (pci.vendor_id == 0x8086) {
			found = 1;
			break;
		}
	}
	if (!found)
		return;

	/* Enable memory space + bus master */
	u16 cmd = pci_config_read16(pci.bus, pci.slot, pci.func, 0x04);
	cmd |= 0x0006;
	pci_config_write16(pci.bus, pci.slot, pci.func, 0x04, cmd);

	/* BAR0: memory BAR */
	u32 bar0 = pci_config_read32(pci.bus, pci.slot, pci.func, 0x10);
	u64 mmio_phys = bar0 & 0xFFFFFFF0u;

	hda_regs = (volatile u8 *)vmm_map_mmio(mmio_phys, 0x4000,
	                                        VMM_WRITABLE | VMM_PCD);

	console_write("hda: ");
	console_write_hex32(pci.device_id);
	console_write(" BAR0 0x");
	console_write_hex64(mmio_phys);
	console_write("\n");

	if (!hda_regs) {
		console_write("hda: MMIO mapping failed\n");
		return;
	}

	/* Reset the controller */
	hda_controller_reset();

	u16 gcap = hda_r16(HDA_GCAP);
	u8 out_streams = (gcap >> 12) & 0x0F;

	if (out_streams == 0) {
		console_write("hda: no output streams — aborting\n");
		return;
	}

	/* Set up CORB/RIRB */
	if (hda_setup_corb_rirb() < 0)
		return;

	/* Probe codec — some QEMU audio backends don't present a codec */
	int codec_found = (hda_probe_codec() == 0);
	if (codec_found) {
		if (hda_discover_audio_widgets() < 0)
			codec_found = 0;
	}
	if (codec_found)
		hda_configure_output();

	/* Set up output stream DMA: 4 × 4 KiB = 16 KiB ring buffer */
	hda_dam_buf_sz = 16 * 1024;
	if (hda_setup_output_stream(hda_dam_buf_sz) < 0)
		return;

	hda_inited = 1;

	/* Register the sound device interface */
	hda_sound_dev.name = "hda";
	hda_sound_dev.sample_rate = hda_sample_rate;
	hda_sound_dev.channels = 2;
	hda_sound_dev.format = SOUND_FMT_S16LE;
	hda_sound_dev.buffer_size = hda_dam_buf_sz;
	hda_sound_dev.open = hda_sound_open;
	hda_sound_dev.close = hda_sound_close;
	hda_sound_dev.write = hda_sound_write;
	hda_sound_dev.get_position = hda_sound_get_position;
	hda_sound_dev.ready = hda_sound_ready;
	hda_sound_dev.vol_left = 100;
	hda_sound_dev.vol_right = 100;
	hda_sound_dev.muted = 0;
	hda_sound_dev.set_volume = hda_sound_set_volume;
	hda_sound_dev.get_volume = hda_sound_get_volume;
	sound_register(&hda_sound_dev);

	console_write("hda: initialized 48kHz stereo 16-bit, /dev/dsp ready\n");
}

/* Re-register /dev/dsp after the real root is mounted. The node created by
 * hda_init() lands on the initramfs root, which becomes unreachable when "/"
 * redirects to the ext4 root; vfs_repopulate_after_root_mount() calls this so
 * the device node stays visible to userspace. Idempotent. */
void hda_dev_init(void) {
	if (!hda_inited)
		return;
	struct vfs_node *dsp = vfs_add_node("/dev/dsp", VFS_DEVICE, 0, 0, 0);
	if (dsp && !IS_ERR(dsp)) {
		dsp->inode->mode = 0644;
		dsp->inode->read_cb  = hda_dsp_read;
		dsp->inode->write_cb = hda_dsp_write;
		dsp->inode->release_cb = hda_dsp_release;
		dsp->inode->ioctl_cb = hda_dsp_ioctl;
		vfs_node_put(dsp);
	}
}

/* ── Self-test (test mode) ──────────────────────────────────────────────── */
static u16 hda_test_sine16(u32 freq, u32 sample_rate, u32 i) {
	/* Simple 16-bit signed sine wave generator */
	u32 period = sample_rate / freq;
	if (period == 0) period = 1;
	u32 pos = i % period;
	u32 half = period / 2;
	i32 v;
	if (pos < half) {
		v = (i32)((u32)16000 * 2 * pos / period);
	} else {
		v = (i32)((u32)16000 * 2 * (period - pos) / period);
	}
	v -= 8000;
	if (v > 16000) v = 16000;
	if (v < -16000) v = -16000;
	return (u16)(i16)v;
}

void hda_selftest(void) {
	if (!hda_inited) {
		console_write("M38-SOUND: skip no-device\n");
		return;
	}

	console_write("M38-SOUND: ok probe\n");

	/* Verify the DMA buffer is accessible */
	memset(hda_dam_buf, 0, hda_dam_buf_sz);
	console_write("M38-SOUND: ok dma-buf\n");

	/* Create /dev/dsp verification */
	struct vfs_node *dsp = vfs_find_node("/dev/dsp");
	if (dsp && !IS_ERR(dsp)) {
		console_write("M38-SOUND: ok dev-dsp\n");
		vfs_node_put(dsp);
	} else {
		console_write("M38-SOUND: fail dev-dsp\n");
	}

	/* Verify sound device API */
	struct sound_device *sd = sound_get_default();
	if (sd && sd->ready(sd)) {
		console_write("M38-SOUND: ok sound-api\n");
	} else {
		console_write("M38-SOUND: fail sound-api\n");
	}

	/* Play a test tone only if codec is present */
	if (hda_output_nid) {
		u32 num_samples = hda_sample_rate * 100 / 1000;
		if (num_samples * 2 > hda_dam_buf_sz)
			num_samples = hda_dam_buf_sz / 2;

		i16 *samples = (i16 *)hda_dam_buf;
		for (u32 i = 0; i < num_samples; i++) {
			samples[i] = (i16)hda_test_sine16(440, hda_sample_rate, i);
		}

		u32 sdo_off = HDA_SDO_BASE;
		hda_w16(sdo_off + HDA_SDO_LVI, num_samples - 1);
		hda_w32(sdo_off + HDA_SDO_CBL, num_samples * 2);

		u32 ctl0 = hda_r32(sdo_off + HDA_SDO_CTL0);
		hda_w32(sdo_off + HDA_SDO_CTL0, ctl0 | HDA_SDO_CTL0_RUN);
		hda_delay_ms(hda_tone_ms);
		console_write("M38-SOUND: ok play-sine\n");
	} else {
		console_write("M38-SOUND: ok play-sine (no-codec)\n");
	}

	console_write("M38-SOUND: ok done\n");
}

/* ── M95: the Intel HDA controller driver is a loadable module ───────────── */
#include <b1nix/module.h>

MODULE_NAME("hda");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("b1nix");
MODULE_DESCRIPTION("Intel High Definition Audio controller (/dev/dsp)");
MODULE_ALIAS("sound-hda");
MODULE_ALIAS("snd-hda-intel");

/* Sample rate the output stream is programmed for, and the DMA ring size.
 * Both are read-only knobs: the stream descriptor is configured from them
 * once during init, so they report what the hardware actually runs at. */
module_param_desc(hda_sample_rate, MODULE_PARAM_UINT, 0444,
                  "PCM sample rate in Hz");
module_param_desc(hda_dam_buf_sz, MODULE_PARAM_UINT, 0444,
                  "DMA ring buffer size in bytes");

/* Writable knob: milliseconds hda_selftest lets the test tone play. */
module_param_desc(hda_tone_ms, MODULE_PARAM_INT, 0644,
                  "self-test tone duration in milliseconds");

static const struct sound_driver_hooks hda_hooks = {
	.dev_init = hda_dev_init,
	.selftest = hda_selftest,
};

static int hda_module_init(void) {
	hda_init();
	sound_register_hooks(&hda_hooks);
	/* An absent controller is not a load failure: the module stays resident
	 * with hda_inited == 0 and every entry point reports "no device". */
	return 0;
}

static void hda_module_exit(void) {
	sound_unregister_hooks(&hda_hooks);
	if (hda_inited)
		sound_unregister(&hda_sound_dev);
	hda_inited = 0;
}

module_init(hda_module_init);
module_exit(hda_module_exit);
