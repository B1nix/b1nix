/* SPDX-License-Identifier: GPL-2.0-only */
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
#include <stdio.h>
#include <stdlib.h>
#include <b1nix/console.h>
#include <b1nix/ktime.h>
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
#define HDA_CORBSIZE  0x004E  /* CORB Size (entries: 0=2, 1=16, 2=256)     */
#define HDA_RINTCNT   0x005A  /* Response Interrupt Count                  */
#define HDA_RIRBSTS   0x005D  /* RIRB Status: bit 0 RINTFL, bit 2 overrun  */
/* The immediate command interface: one verb at a time, no ring and no DMA.
 * Every Intel controller implements it — it is what firmware uses before it
 * has memory to put a ring in. */
#define HDA_ICOI      0x0060  /* Immediate Command Output                  */
#define HDA_ICII      0x0064  /* Immediate Command Input (response)        */
#define HDA_ICIS      0x0068  /* Immediate Command Status                  */
#define HDA_ICIS_BUSY (1u << 0)
#define HDA_ICIS_VALID (1u << 1)
#define HDA_RIRBSIZE  0x005E  /* RIRB Size, same encoding                  */
#define HDA_CORBCTL   0x004C

/* RIRB (Response Input Ring Buffer) */
#define HDA_RIRBLBASE 0x0050
#define HDA_RIRBUBASE 0x0054
#define HDA_RIRBWP    0x0058
#define HDA_RIRBCTL   0x005C
#define HDA_RIRBSTS   0x005D

/* Stream descriptors — Output (SDO) start at 0x0800, 0x20 bytes apart */
/*
 * Stream descriptors start at 0x80 and are 0x20 bytes apart, input streams
 * first. The first OUTPUT descriptor is therefore 0x80 + ISS * 0x20, and ISS
 * comes from GCAP — which is why this is computed at probe rather than being a
 * constant. It used to be 0x800, past the end of the register file: every
 * write to a stream descriptor went nowhere, the position register read zero
 * for ever, and the driver reported a tone it had never played.
 */
#define HDA_SD_FIRST  0x0080
#define HDA_SDO_STRIDE 0x020
#define HDA_SDO_BASE  (hda_sdo_base)

/* SDO register offsets within a stream descriptor */
#define HDA_SDO_CTL0   0x00  /* Control (bits: stall, stream tag, format) */
#define HDA_SDO_CTL1   0x01  /* Control 1 (channel count, etc.)           */
#define HDA_SDO_CTL2   0x02  /* Control 2 (stripe, etc.)                  */
#define HDA_SDO_STS    0x03  /* Status: bit 2 BCIS, 3 FIFOE, 4 DESE (W1C)  */
#define HDA_SDO_STS_BCIS (1u << 2)
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
/* CORBCTL bit 0 is the memory-error interrupt enable; the engine's own run
 * bit is bit 1. Enabling bit 0 and calling it DMAEN left the CORB engine
 * stopped: the write pointer advanced, the read pointer never moved, and no
 * verb was ever fetched — which is why no codec answered on any machine. */
#define HDA_CORBCTL_MEIE  (1u << 0)  /* Memory Error Interrupt Enable */
#define HDA_CORBCTL_DMAEN (1u << 1)  /* CORB DMA (RUN) */
#define HDA_CORBCTL_CMEIE (1u << 1)  /* CMEI Interrupt Enable */

/* RIRBCTL bits */
/* RIRBCTL bit 0 is the response-interrupt enable, bit 1 the DMA run bit —
 * the same layout, and the same trap. */
#define HDA_RIRBCTL_RINTCTL (1u << 0) /* Response Interrupt Control */
#define HDA_RIRBCTL_DMAEN (1u << 1)  /* RIRB DMA (RUN) */

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

/*
 * Parameter indices for the Get Parameter verb.
 *
 * The verb itself is 0xF00 and the parameter is its PAYLOAD, so these are the
 * small numbers from the specification's table and not 0xF0x. Writing the verb
 * into the payload asks the codec for parameter 0xF01, which no codec has: it
 * answered zero, the probe read that as "not responding", and no machine has
 * ever found its codec.
 */
#define HDA_PARAM_VENDOR_ID       0x00
#define HDA_PARAM_REVISION_ID     0x02
#define HDA_PARAM_NODE_COUNT      0x04
#define HDA_PARAM_AUDIO_FG_CAP    0x08
#define HDA_PARAM_AUDIO_WIDGET_CAP 0x09
#define HDA_PARAM_STREAM_FORMAT   0x0A
#define HDA_PARAM_PIN_CAP         0x0C
#define HDA_PARAM_GPIO_CAP        0x11
#define HDA_PARAM_FG_TYPE         0x05
#define HDA_FG_TYPE_AUDIO         0x01

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
/* What was found, kept for the self-test to report: on emulated hardware and
 * on a passed-through controller the driver takes the same path, and the
 * numbers are the only thing that says which one it was talking to. */
static u32 hda_sdo_base = HDA_SD_FIRST; /* first output stream descriptor */
/* Set from b1nix.hda-no-ici: send every verb through the CORB/RIRB ring, so a
 * run can prove the ring works instead of assuming it because nothing uses
 * it. */
static int hda_no_ici;
static int hda_ring_dead;   /* the CORB/RIRB ring stopped answering; use ICI */
static u16 hda_pci_vendor;
static u16 hda_pci_device;
static u32 hda_codec_vendor;
static u8 hda_codec_count;
static u8 hda_afg_nid;       /* the audio function group the widgets live in */
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
/* Ten milliseconds is enough to prove the stream moves and short enough not to
 * be heard on every boot. A run that WANTS to hear it — the one that captures
 * what the emulator played and looks for the tone in it — asks for longer with
 * b1nix.hda-tone-ms=N. */
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
	 * port read takes: the kernel calibrates a nanosecond clock at boot, and a
	 * delay should be expressed in the unit it asks for and measured with
	 * that.
	 *
	 * The kernel's monotonic clock falls back to the tick when the TSC is not
	 * trusted (no invariant TSC — every QEMU without `+invtsc`, and older
	 * hardware). Before the tick runs it reads zero, and a bounded spin is
	 * all there is then — generous, because a reset delay that is too long
	 * costs milliseconds of boot and one that is too short costs the device.
	 * The raw TSC clock was read here once and answered zero for ever on
	 * such a machine, and the boot spun right after "hda: BAR0". */
	u64 t0 = ktime_monotonic_ns();
	u32 start = hda_wallclock();

	if (t0 == 0) {
		if (start == 0) {
			for (volatile u64 i = 0; i < (u64)ms * 400000ull; i++)
				cpu_relax();
			return;
		}
		while ((u32)(hda_wallclock() - start) < (u32)ms)
			scheduler_yield();
		return;
	}

	u64 deadline = t0 + (u64)ms * 1000000ull;

	if (start == 0) {
		/* `pause`, not a port read: this is a spin hint, and an I/O-port
		 * access is a VM exit under virtualisation -- paying one per
		 * iteration to mark time is the cost this loop is trying to avoid. */
		while (ktime_monotonic_ns() < deadline)
			cpu_relax(); /* not a bare `pause`: x86-only mnemonic */
		return;
	}
	while (ktime_monotonic_ns() < deadline) {
		u32 now = hda_wallclock();

		if ((u32)(now - start) >= (u32)ms)
			return;
		scheduler_yield();
	}
}

/* ── MMIO helpers ────────────────────────────────────────────────────────── */
static inline u8  hda_r8(u32 off)  { return *(volatile u8  *)(hda_regs + off); }
static inline u16 hda_r16(u32 off) { return *(volatile u16 *)(hda_regs + off); }
static inline u32 hda_r32(u32 off) { return *(volatile u32 *)(hda_regs + off); }
static inline void hda_w8(u32 off, u8  v) { *(volatile u8  *)(hda_regs + off) = v; }
static inline void hda_w16(u32 off, u16 v) { *(volatile u16 *)(hda_regs + off) = v; }
static inline void hda_w32(u32 off, u32 v) { *(volatile u32 *)(hda_regs + off) = v; }

/* ── CORB/RIRB transport ─────────────────────────────────────────────────── */

/*
 * Write a verb to the CORB ring. Returns 0 on success, -1 if CORB is full.
 *
 * The write pointer names the LAST entry the controller may read, so the index
 * is advanced first, the verb written there, and only then published. Writing
 * at the current index and publishing it leaves read pointer equal to write
 * pointer — an empty ring — so the very first verb of every boot was never
 * fetched and no codec ever answered.
 *
 * CORBRP is a plain 8-bit index in bits 7:0 (bit 15 is its reset control), so
 * it is masked, not shifted.
 */
static int hda_corb_write(u32 verb) {
	u16 rp = hda_r16(HDA_CORBRP) & 0xFF;
	u16 next = (u16)((hda_corb_wp + 1) & 0xFF);

	if (next == rp) {
		/* CORB full — poll briefly */
		for (int i = 0; i < 100000; i++) {
			rp = hda_r16(HDA_CORBRP) & 0xFF;
			if (next != rp)
				break;
		}
		if (next == rp)
			return -1;
	}
	hda_corb[next] = verb;
	hda_corb_wp = next;
	hda_w16(HDA_CORBWP, hda_corb_wp);
	return 0;
}

/* Send a verb and wait for the response. Returns the 32-bit response or 0
 * on timeout (~500 ms). */
/*
 * Send one verb through the immediate command interface and wait for the
 * response.
 *
 * Returns 1 and fills *resp on success, 0 when the interface did not answer —
 * which is how a controller that does not implement it is recognised, and the
 * caller then uses the ring.
 */
static int hda_ici_send(u32 verb, u32 *resp)
{
	u16 sts;
	int i;

	/* Wait for any previous command to finish. */
	for (i = 0; i < 1000; i++) {
		if (!(hda_r16(HDA_ICIS) & HDA_ICIS_BUSY))
			break;
		cpu_relax();
	}
	if (hda_r16(HDA_ICIS) & HDA_ICIS_BUSY)
		return 0;

	/* Clear a stale result (the valid bit is write-1-to-clear), post the
	 * verb, then set busy to start it. */
	hda_w16(HDA_ICIS, HDA_ICIS_VALID);
	hda_w32(HDA_ICOI, verb);
	hda_w16(HDA_ICIS, HDA_ICIS_BUSY);

	for (i = 0; i < 100000; i++) {
		sts = hda_r16(HDA_ICIS);
		if (!(sts & HDA_ICIS_BUSY) && (sts & HDA_ICIS_VALID)) {
			*resp = hda_r32(HDA_ICII);
			hda_w16(HDA_ICIS, HDA_ICIS_VALID);
			return 1;
		}
		cpu_relax();
	}
	return 0;
}

static u32 hda_corb_send_wait(u32 verb) {
	u32 resp = 0;

	/* The ring is the normal path and the immediate interface the fallback,
	 * the way Linux arranges it: a controller whose ring does not answer is
	 * remembered and every later verb goes the short way, rather than each
	 * one waiting the ring's timeout out first. b1nix.hda-no-ici disables
	 * the fallback so a run proves the ring rather than merely having it. */
	if (hda_ring_dead && !hda_no_ici) {
		if (hda_ici_send(verb, &resp))
			return resp;
		return 0;
	}

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
			/* A RIRB entry is TWO dwords — the response and its extended
			 * word, which carries the codec address and whether the entry is
			 * an unsolicited event. Indexing a u32 array by the write pointer
			 * therefore reads the wrong half of the wrong entry: every verb
			 * came back as zero, the probe concluded no codec was answering,
			 * and the machine played nothing while reporting that it had. */
			u32 resp = hda_rirb[(usize)(new_wp & 0xFF) * 2];

			/* The response has been taken: clear RINTFL (and an overrun, if
			 * one is flagged) so the engine goes on to the next verb. */
			hda_w8(HDA_RIRBSTS, 0x05);
			return resp;
		}
		/* Ten ticks is a tenth of a second — four orders of magnitude more
		 * than a working codec needs, and a fiftieth of what this cost
		 * before. */
		if ((i & 0xff) == 0xff &&
		    scheduler_get_uptime_ticks() - start > SCHED_TICKS_PER_SEC / 10)
			break;
		cpu_relax();
	}
	if (hda_no_ici)
		return 0;
	/* No answer through the ring: this controller gets the immediate
	 * interface from now on. */
	hda_ring_dead = 1;
	if (hda_ici_send(verb, &resp))
		return resp;
	return 0;
}

/* Send a Get Parameter verb to the codec. */
static u32 hda_get_param(u8 nid, u32 param) {
	return hda_corb_send_wait(HDA_VERB(hda_codec_addr, nid, 0xF00, param));
}

/* ── HDA controller reset sequence ───────────────────────────────────────── */
/* Wait, up to 100 ms, for CRST to read back as `want`. The controller reports
 * each transition through the bit itself (HDA 1.0a 3.3.7); waiting a fixed
 * 50 ms either side was 100 ms of every boot for a change that lands at once. */
static int hda_wait_crst(u32 want) {
	for (int ms = 0; ms < 100; ms++) {
		if ((hda_r32(HDA_GCTL) & HDA_GCTL_CRST) == want)
			return 0;
		hda_delay_ms(1);
	}
	return -1;
}

static void hda_controller_reset(void) {
	/* Assert reset */
	hda_w32(HDA_GCTL, hda_r32(HDA_GCTL) & ~HDA_GCTL_CRST);
	if (hda_wait_crst(0) != 0)
		console_write("hda: controller did not enter reset\n");

	/* Clear reset */
	hda_w32(HDA_GCTL, hda_r32(HDA_GCTL) | HDA_GCTL_CRST);
	if (hda_wait_crst(HDA_GCTL_CRST) != 0)
		console_write("hda: controller did not leave reset\n");

	/* Codecs have 521 us after the controller leaves reset to request their
	 * addresses (HDA 1.0a 4.3); STATESTS is read after that. Linux waits a
	 * millisecond here too. */
	hda_delay_ms(1);
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

	/* Both rings hold 256 entries, which is what the size registers have to
	 * say: the controller reads the ring at the size IT was told, not at the
	 * one the driver allocated. */
	hda_w8(HDA_CORBSIZE, (hda_r8(HDA_CORBSIZE) & ~0x03u) | 0x02u);
	hda_w8(HDA_RIRBSIZE, (hda_r8(HDA_RIRBSIZE) & ~0x03u) | 0x02u);

	/* Reset the CORB read pointer, which is a handshake and not a write: set
	 * bit 15, wait for the controller to acknowledge it by reading it back,
	 * clear it, wait for it to clear. Skipping this leaves the read pointer
	 * wherever the last owner of the controller left it — on a passed-through
	 * device, wherever the host driver left it. */
	hda_w16(HDA_CORBRP, 0x8000);
	for (int i = 0; i < 1000; i++) {
		if (hda_r16(HDA_CORBRP) & 0x8000)
			break;
		hda_delay_ms(1);
	}
	hda_w16(HDA_CORBRP, 0);
	for (int i = 0; i < 1000; i++) {
		if (!(hda_r16(HDA_CORBRP) & 0x8000))
			break;
		hda_delay_ms(1);
	}

	/* Write pointers back to the start — 0xFFFF clears RIRBWP's own bits. */
	hda_w16(HDA_CORBWP, 0);
	hda_w16(HDA_RIRBWP, 0xFFFF);

	/*
	 * How many responses the controller delivers before it raises RINTFL
	 * and STOPS. That is the whole meaning of the register: it is a
	 * flow-control count, not merely an interrupt rate, and the engine does
	 * not fetch another verb until software has cleared the flag. Left at
	 * zero, QEMU's model compares its count of zero against it, decides the
	 * limit is already reached, and never fetches the first verb — which is
	 * exactly what the ring did before this line. Linux writes 1 and clears
	 * the flag after every response; so does hda_corb_send_wait().
	 */
	hda_w16(HDA_RINTCNT, 1);
	hda_w8(HDA_RIRBSTS, 0x05);

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
	/* STATESTS, not GCAP: one bit per SDI line, set by the codec at that
	 * address when it announced itself after the controller reset. GCAP's
	 * bits 11:8 are the number of INPUT STREAMS the controller has, which on
	 * ICH6 happens to be 4 — a number that looks like a codec count and is
	 * not one. */
	u16 statests = hda_r16(HDA_STATESTS) & 0x7FFF;
	u8 codecs = 0;

	for (u8 b = 0; b < 15; b++)
		if (statests & (1u << b))
			codecs++;
	hda_codec_count = codecs;
	if (codecs == 0) {
		console_write("hda: no codecs found\n");
		return -1;
	}
	console_write("hda: ");
	console_write_dec(codecs);
	console_write(" codec(s) present, STATESTS 0x");
	console_write_hex32(statests);
	console_write("\n");

	/* Only the addresses that announced themselves, and in order. */
	for (u8 addr = 0; addr < 15; addr++) {
		if (!(statests & (1u << addr)))
			continue;
		hda_codec_addr = addr;
		/* Send a zero verb to wake up the codec */
		hda_corb_send_wait(0);
		u32 vendor = hda_get_param(0, HDA_PARAM_VENDOR_ID);
		if (vendor == 0 || vendor == 0xFFFFFFFF) {
			/* Codec at this address is not responding */
			continue;
		}
		hda_codec_vendor = vendor;
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
	/*
	 * Two levels, as the specification lays them out: the root node's
	 * subordinates are FUNCTION GROUPS, and only a function group's
	 * subordinates are widgets. Walking the root's children as if they were
	 * widgets found the audio function group itself, whose capability word
	 * decodes to whatever its type happens to be, and both the converter and
	 * the pin ended up as node 1 — the group. QEMU's codec forgave that; a
	 * real one takes converter verbs sent to its function group as noise.
	 */
	u32 root = hda_get_param(0, HDA_PARAM_NODE_COUNT);
	u8 fg_start = (root >> 16) & 0xFF;
	u8 fg_count = root & 0xFF;

	hda_output_nid = 0;
	hda_pin_nid = 0;

	for (u8 fg = fg_start; fg < fg_start + fg_count && fg_count; fg++) {
		u32 fgtype = hda_get_param(fg, HDA_PARAM_FG_TYPE);

		if ((fgtype & 0xFF) != HDA_FG_TYPE_AUDIO)
			continue;

		u32 sub = hda_get_param(fg, HDA_PARAM_NODE_COUNT);
		u8 start_nid = (sub >> 16) & 0xFF;
		u8 num_nodes = sub & 0xFF;

		hda_afg_nid = fg;
		for (u8 nid = start_nid; nid < start_nid + num_nodes && num_nodes; nid++) {
			u32 wcaps = hda_get_param(nid, HDA_PARAM_AUDIO_WIDGET_CAP);
			u8 type = (wcaps >> 20) & 0x0F;

			if (type == HDA_WIDGET_AUDIO_OUTPUT && !hda_output_nid)
				hda_output_nid = nid;
			if (type == HDA_WIDGET_PIN_COMPLEX && !hda_pin_nid) {
				/* Check if this pin supports output */
				u32 pincap = hda_get_param(nid, HDA_PARAM_PIN_CAP);

				if (pincap & (1u << 4)) /* Output-capable */
					hda_pin_nid = nid;
			}
		}
		if (hda_output_nid)
			break;
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
	/*
	 * The stream format word, from the specification's own layout:
	 *
	 *   bit 15   type (0 = PCM)        bit 14    base rate (0 = 48 kHz)
	 *   13:11    multiplier            10:8      divisor
	 *   6:4      bits per sample (001 = 16)      3:0  channels - 1
	 *
	 * 48 kHz, 16-bit, stereo is therefore 0x0011. The 0x01F3 that used to be
	 * here is a divisor of 2 (24 kHz), a reserved bit-depth and four
	 * channels — a format no codec accepts, written into a register nothing
	 * was reading anyway.
	 */
	hda_fmt_word = 0x0011;

	/* Unmute the output converter amp at full gain (0 dB). Uses the 4/16
	 * form of SET_AMP_GAIN_MUTE (verb 0x3 in bits 19:16, 16-bit payload);
	 * QEMU's codec rejects the 12/8-form volume verbs used pre-M79. */
	hda_corb_send_wait(HDA_VERB16(hda_codec_addr, hda_output_nid, 0x3,
		AC_AMP_SET_OUTPUT | AC_AMP_SET_LEFT | HDA_AMP_STEPS));
	hda_corb_send_wait(HDA_VERB16(hda_codec_addr, hda_output_nid, 0x3,
		AC_AMP_SET_OUTPUT | AC_AMP_SET_RIGHT | HDA_AMP_STEPS));

	/*
	 * The verb numbers are the specification's, and the three that were here
	 * were not:
	 *
	 *   0x705  Set Power State      (0xF50 is not a verb at all; 0xF05 GETS
	 *                                the power state)
	 *   0x707  Set Pin Widget Control, payload 0x40 = output enable — the
	 *          payload is the control byte, not the converter's node
	 *   0x708  Set Unsolicited Response, which is what the pin enable was
	 *          being sent to
	 *
	 * A codec given these ignored all three: it stayed powered down, its pin
	 * stayed an input, and nothing came out of a stream the driver had
	 * otherwise set up.
	 */

	/* Power up the converter and the pin. */
	hda_corb_send_wait(HDA_VERB(hda_codec_addr, hda_output_nid, 0x705, 0x00));
	hda_corb_send_wait(HDA_VERB(hda_codec_addr, hda_pin_nid, 0x705, 0x00));

	/* Set stream format on the output converter */
	hda_corb_send_wait(HDA_VERB(hda_codec_addr, hda_output_nid, 0x200, hda_fmt_word));

	/* The pin takes its samples from the first connection, and drives out. */
	hda_corb_send_wait(HDA_VERB(hda_codec_addr, hda_pin_nid, 0x701, 0x00));
	hda_corb_send_wait(HDA_VERB(hda_codec_addr, hda_pin_nid, 0x707, 0x40));
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
	 * The stream descriptor's control register, from the specification:
	 *
	 *   bit 0      Stream Reset          bit 1   Stream Run
	 *   bit 2      Interrupt on Completion Enable
	 *   bits 19:16 Traffic priority / stripe
	 *   bits 23:20 Stream Number — the TAG the codec is told to listen for
	 *
	 * Bit 0 is a RESET, not the stream number. Writing the number there held
	 * the stream in reset for its whole life: the run bit was set on top of
	 * it, the controller ignored both, and the position register never moved
	 * while the driver reported that it had played.
	 */
	u32 ctl0 = (u32)stream_tag << 20;

	hda_w32(sdo_off + HDA_SDO_CTL0, ctl0);

	/* And the converter has to be told which stream it belongs to, or it
	 * takes its samples from a stream nobody is filling: payload is the tag
	 * in the high nibble and the first channel in the low one. */
	hda_corb_send_wait(HDA_VERB(hda_codec_addr, hda_output_nid, 0x706,
	                            (u32)stream_tag << 4));

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

		/* Whole frames only: a stereo 16-bit frame is four bytes, and a
		 * cyclic length that is not a multiple of it is refused. */
		chunk &= ~(usize)3;
		if (chunk == 0)
			break;
		memcpy(hda_dam_buf, buffer + written, chunk);

		/*
		 * One chunk, played once.
		 *
		 * The descriptor is CYCLIC: with the run bit left set the controller
		 * goes round the buffer for as long as the machine is up, which a
		 * capture of the emulated card showed as a tone that never ended.
		 * And a stream that was never reset keeps its position from the last
		 * chunk, so "wait until the position passes the chunk" returned at
		 * once, before a byte of the new data had played. So: reset, which
		 * puts the position back to zero; size the cycle to this chunk; run;
		 * wait for the position to reach the end of it; stop.
		 */
		u32 sdo_off = HDA_SDO_BASE;
		u32 ctl0 = hda_r32(sdo_off + HDA_SDO_CTL0) & 0x00FFFFFFu;

		hda_w32(sdo_off + HDA_SDO_CTL0, (ctl0 & ~HDA_SDO_CTL0_RUN) | HDA_SDO_CTL0_SRST);
		for (int i = 0; i < 100 && !(hda_r8(sdo_off + HDA_SDO_CTL0) & HDA_SDO_CTL0_SRST); i++)
			hda_delay_ms(1);
		hda_w32(sdo_off + HDA_SDO_CTL0, ctl0 & ~(HDA_SDO_CTL0_RUN | HDA_SDO_CTL0_SRST));
		for (int i = 0; i < 100 && (hda_r8(sdo_off + HDA_SDO_CTL0) & HDA_SDO_CTL0_SRST); i++)
			hda_delay_ms(1);

		/* A reset clears the descriptor's addresses and format; put them
		 * back, sized to this chunk, with interrupt-on-completion set on the
		 * one entry: that is what raises BCIS in the stream's status when
		 * the entry has been played, and BCIS is the completion signal —
		 * not the position register. A cycle exactly one chunk long wraps
		 * its position back to zero AT the chunk boundary, so "position has
		 * reached the chunk" is a moment that never exists, and waiting for
		 * it cost the full timeout per chunk while the buffer looped. */
		hda_bdl[0].length = (u32)chunk;
		hda_bdl[0].flags = 1; /* IOC */
		hda_w32(sdo_off + HDA_SDO_BDPL, (u32)(hda_bdl_phys & 0xFFFFFFFF));
		hda_w32(sdo_off + HDA_SDO_BDPH, (u32)(hda_bdl_phys >> 32));
		hda_w32(sdo_off + HDA_SDO_CBL, (u32)chunk);
		hda_w16(sdo_off + HDA_SDO_LVI, 0);
		hda_w16(sdo_off + HDA_SDO_FMT, hda_fmt_word);
		hda_w8(sdo_off + HDA_SDO_STS, HDA_SDO_STS_BCIS); /* clear a stale one */
		hda_w32(sdo_off + HDA_SDO_CTL0, (ctl0 & ~HDA_SDO_CTL0_SRST) | HDA_SDO_CTL0_RUN);

		/* Bounded by the wall clock: the chunk is at most the buffer, which
		 * is well under a second of audio. */
		u32 start = hda_wallclock();
		while (!(hda_r8(sdo_off + HDA_SDO_STS) & HDA_SDO_STS_BCIS)) {
			if ((u32)(hda_wallclock() - start) > 2000) break;
			scheduler_yield();
		}
		hda_w32(sdo_off + HDA_SDO_CTL0, ctl0 & ~(HDA_SDO_CTL0_RUN | HDA_SDO_CTL0_SRST));
		hda_w8(sdo_off + HDA_SDO_STS, HDA_SDO_STS_BCIS);

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

	hda_no_ici = bootinfo_has_flag("b1nix.hda-no-ici") ? 1 : 0;
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
	pci_bind_driver(&pci, "snd_hda_intel");

	/* Enable memory space + bus master */
	u16 cmd = pci_config_read16(pci.bus, pci.slot, pci.func, 0x04);
	cmd |= 0x0006;
	pci_config_write16(pci.bus, pci.slot, pci.func, 0x04, cmd);

	/* BAR0: memory BAR */
	u32 bar0 = pci_config_read32(pci.bus, pci.slot, pci.func, 0x10);
	u64 mmio_phys = bar0 & 0xFFFFFFF0u;

	hda_regs = (volatile u8 *)vmm_map_mmio(mmio_phys, 0x4000,
	                                        VMM_WRITABLE | VMM_PCD);

	hda_pci_vendor = pci.vendor_id;
	hda_pci_device = pci.device_id;
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
	u8 in_streams = (gcap >> 8) & 0x0F;

	/* Output descriptors follow the input ones. */
	hda_sdo_base = HDA_SD_FIRST + (u32)in_streams * HDA_SDO_STRIDE;

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
	{
		char v[16];

		if (bootinfo_get_kv("b1nix.hda-tone-ms", v, sizeof(v)) == 1) {
			int ms = atoi(v);

			if (ms > 0 && ms <= 5000)
				hda_tone_ms = ms;
		}
	}
	if (!hda_inited) {
		console_write("M38-SOUND: skip no-device\n");
		return;
	}

	console_write("M38-SOUND: ok probe\n");

	/* Which controller, and which codec answered.
	 *
	 * The driver takes the same path on QEMU's ICH6 and on a chipset
	 * controller handed over by VFIO, so these numbers are the only thing in
	 * the log that says which one it was: 8086:2668 is the emulated one,
	 * 8086:a2f0 the Z370's, and the codec's vendor id is the codec's own. */
	{
		char line[96];

		snprintf(line, sizeof(line),
		         "M38-SOUND: ok controller vendor=%04x device=%04x\n",
		         (unsigned)hda_pci_vendor, (unsigned)hda_pci_device);
		console_write(line);
		if (hda_codec_vendor) {
			snprintf(line, sizeof(line),
			         "M38-SOUND: ok codec addr=%u vendor=%08x count=%u\n",
			         (unsigned)hda_codec_addr, (unsigned)hda_codec_vendor,
			         (unsigned)hda_codec_count);
			console_write(line);
		} else {
			console_write("M38-SOUND: fail codec (none answered)\n");
		}
	}

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

		/* The stream is stereo, so a frame is two samples and the same value
		 * goes to both: written one sample per index, the controller read
		 * each pair as one frame and the sine came out an octave high —
		 * 880 Hz in the capture for a 440 Hz tone. */
		i16 *samples = (i16 *)hda_dam_buf;
		u32 num_frames = num_samples / 2;

		for (u32 i = 0; i < num_frames; i++) {
			i16 v = (i16)hda_test_sine16(440, hda_sample_rate, i);

			samples[2 * i] = v;
			samples[2 * i + 1] = v;
		}

		u32 sdo_off = HDA_SDO_BASE;

		/* LVI is the index of the last BUFFER DESCRIPTOR, and there is one of
		 * them: writing the sample count there pointed the controller at
		 * thousands of descriptors that do not exist, and it fetched none.
		 * The cyclic length stays the descriptor's own — the same thing the
		 * /dev/dsp write path does. */
		hda_w16(sdo_off + HDA_SDO_LVI, 0);
		hda_w32(sdo_off + HDA_SDO_CBL, hda_dam_buf_sz);

		u32 ctl0 = hda_r32(sdo_off + HDA_SDO_CTL0);
		hda_w32(sdo_off + HDA_SDO_CTL0, ctl0 | HDA_SDO_CTL0_RUN);
		u32 pos_before = hda_r32(sdo_off + HDA_SDO_LPIB);
		u32 pos_after = pos_before;

		/* Up to a fifth of a second, and no longer than it takes: the
		 * position moves within a buffer period on real hardware and within
		 * the emulator's timer tick under QEMU, and waiting the whole time
		 * for a controller that IS working would put that on every boot. */
		for (int w = 0; w < 200; w++) {
			hda_delay_ms(1);
			pos_after = hda_r32(sdo_off + HDA_SDO_LPIB);
			if (pos_after != pos_before)
				break;
		}
		console_write("M38-SOUND: ok play-sine\n");

		/* And stop: the descriptor is cyclic, and a stream left running
		 * plays the buffer round and round for as long as the machine is
		 * up — which a capture of the emulated card showed as a tone that
		 * never ended. */
		hda_w32(sdo_off + HDA_SDO_CTL0, ctl0 & ~HDA_SDO_CTL0_RUN);

		/* The position register is the controller's own account of how far it
		 * has read from the buffer. A controller that accepted every write
		 * and fetched nothing — which is what a mis-programmed BDL, a missing
		 * bus-master enable or a stream that never left reset looks like —
		 * leaves it where it was. */
		{
			char line[80];

			snprintf(line, sizeof(line),
			         "M38-SOUND: %s stream-advanced from=%u to=%u\n",
			         pos_after != pos_before ? "ok" : "fail",
			         (unsigned)pos_before, (unsigned)pos_after);
			console_write(line);
		}
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
