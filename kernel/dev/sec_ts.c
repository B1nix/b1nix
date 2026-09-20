/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Xperia 5 touchscreen: a Samsung sec_ts controller (the S6SY761 family Linux
 * drives with drivers/input/touchscreen/s6sy761.c) at 0x48 on the I2C bus of
 * QUP serial engine 10, fed into /dev/input/event2 as an absolute pointer.
 *
 * Where it is comes from Sony's device-tree overlay (dtbo_a, fragment 87 on
 * qupv3_se10_i2c): reg 0x48, interrupt on TLMM GPIO 122 (active low), panel
 * 1080x2520. The base tree ABL hands over has the bus but not the device.
 *
 * The I2C side is the GENI serial engine in FIFO mode, polled: clocks voted in
 * GCC and checked on before any SE register is touched (an unclocked Qualcomm
 * block does not answer a read at all), pins 9/10 muxed to qup10, the SE's
 * I2C firmware -- loaded by the boot chain -- checked by protocol id. Register
 * layout and sequencing are Linux's (qcom-geni-se.c, i2c-qcom-geni.c).
 *
 * The touch side reports the first finger only, as BTN_LEFT plus ABS_X/ABS_Y
 * scaled to event2's 0..32767: what libinput takes for an absolute pointer,
 * and what a compositor turns into clicks. Multi-touch is not attempted.
 *
 * Started by writing 1 to /proc/sys/kernel/touch rather than at boot, while
 * the bring-up is new: a boot that hangs in here is a phone that hangs every
 * boot, whereas a hang after a manual start is one watchdog reset away from a
 * normal boot and a /proc/last_kmsg that says where it stopped.
 */
#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/input.h>
#include <b1nix/mm.h>
#include <b1nix/sched.h>
#include <b1nix/types.h>
#include <b1nix/arch.h>
#include <stdio.h>
#include <string.h>

#if defined(__aarch64__)

extern int platform_type(void);
#define PLATFORM_SM8150_ID 3

/* GCC: SE10 is QUPv3 wrapper 1, serial engine 2. */
#define GCC_VOTE1          0x5200c
#define GCC_VOTE_S2        (1u << 24)
#define GCC_VOTE_M_AHB     (1u << 20)
#define GCC_VOTE_S_AHB     (1u << 21)
#define GCC_S2_CBCR        0x183a4
#define GCC_M_AHB_CBCR     0x18004
#define GCC_S_AHB_CBCR     0x18008
#define GCC_S2_RCGR        0x183a8
#define CBCR_CLK_OFF       (1u << 31)

/* TLMM: one window from 0x3000000; the north tile holds GPIO 9/10, the south
 * one GPIO 122. */
#define TLMM_NORTH         0x03900000ull
#define TLMM_SOUTH         0x03d00000ull
#define TLMM_CTL(tile, n)  ((tile) + 0x1000ull * (n))
#define TLMM_IO(tile, n)   ((tile) + 0x1000ull * (n) + 4)

#define SE10_BASE          0x00a88000ull
#define TS_ADDR            0x48
#define TS_IRQ_GPIO        122

/* GENI registers. */
#define GENI_FORCE_DEFAULT_REG  0x20
#define GENI_OUTPUT_CTRL        0x24
#define GENI_CGC_CTRL           0x28
#define SE_GENI_STATUS          0x40
#define GENI_SER_M_CLK_CFG      0x48
#define GENI_FW_REVISION_RO     0x68
#define SE_GENI_CLK_SEL         0x7c
#define SE_GENI_BYTE_GRAN       0x254
#define SE_GENI_DMA_MODE_EN     0x258
#define SE_GENI_TX_PACKING_CFG0 0x260
#define SE_GENI_TX_PACKING_CFG1 0x264
#define SE_I2C_TX_TRANS_LEN     0x26c
#define SE_I2C_RX_TRANS_LEN     0x270
#define SE_I2C_SCL_COUNTERS     0x278
#define SE_GENI_RX_PACKING_CFG0 0x284
#define SE_GENI_RX_PACKING_CFG1 0x288
#define SE_GENI_M_CMD0          0x600
#define SE_GENI_M_IRQ_STATUS    0x610
#define SE_GENI_M_IRQ_EN        0x614
#define SE_GENI_M_IRQ_CLEAR     0x618
#define SE_GENI_S_IRQ_CLEAR     0x648
#define SE_GENI_TX_FIFOn        0x700
#define SE_GENI_RX_FIFOn        0x780
#define SE_GENI_RX_FIFO_STATUS  0x804
#define SE_GENI_TX_WATERMARK    0x80c
#define SE_GENI_RX_WATERMARK    0x810
#define SE_GENI_RX_RFR_WATERMARK 0x814
#define SE_GSI_EVENT_EN         0xe18
#define SE_IRQ_EN               0xe1c
#define SE_HW_PARAM_0           0xe24
#define SE_DMA_GENERAL_CFG      0xe30

#define M_CMD_DONE        (1u << 0)
#define M_ERRORS          (0x3eu | (1u << 9) | (1u << 10) | (1u << 11) | (1u << 12) | (1u << 13))
#define M_GP_IRQ_1_NACK   (1u << 10)
#define M_RX_WATERMARK    (1u << 26)
#define M_RX_LAST         (1u << 27)
#define M_TX_WATERMARK    (1u << 30)
#define I2C_WRITE         1u
#define I2C_READ          2u
#define STOP_STRETCH      (1u << 2)

/* sec_ts / S6SY761 */
#define TS_SENSE_ON        0x10
#define TS_FW_INTEGRITY    0x21
#define TS_PANEL_INFO      0x23
#define TS_TOUCH_FUNCTION  0x30
#define TS_DEVICE_ID       0x52
#define TS_BOOT_STATUS     0x55
#define TS_READ_ONE_EVENT  0x60
#define TS_READ_ALL_EVENT  0x61

static u64 g_gcc;
static int g_state; /* 0 not started, 1 running, <0 failed at that step */
static u32 g_max_x = 1080, g_max_y = 2520;
static u32 g_devid;
static u32 g_irq_seen, g_events, g_reads_failed;
static u8 g_last_event[8];

static inline u32 rd(u64 a) { return *(volatile u32 *)(usize)a; }
static inline void wr(u64 a, u32 v) { *(volatile u32 *)(usize)a = v; }
static inline u32 se_rd(u32 off) { return rd(SE10_BASE + off); }
static inline void se_wr(u32 off, u32 v) { wr(SE10_BASE + off, v); }

static void say(const char *s)
{
	extern void ufs_log_panic_flush(void);

	console_write("touch: ");
	console_write(s);
	/* Each step on flash before the next risky one: if the next register
	 * access hangs, the watchdog's reset leaves this line in last_kmsg. */
	ufs_log_panic_flush();
}

static int clocks_on(void)
{
	char line[96];

	if (!g_gcc) {
		g_gcc = (u64)(usize)vmm_map_mmio(fdt_qcom_gcc_base(), 0x1f0000, VMM_WRITABLE);
		if (!g_gcc)
			return -1;
	}
	/* SE clock from TCXO (19.2 MHz), divider bypassed. */
	wr(g_gcc + GCC_S2_RCGR + 4, 0);
	wr(g_gcc + GCC_S2_RCGR, rd(g_gcc + GCC_S2_RCGR) | 1);
	for (int i = 0; i < 1000 && (rd(g_gcc + GCC_S2_RCGR) & 1); i++)
		arch_udelay(10);
	wr(g_gcc + GCC_VOTE1, rd(g_gcc + GCC_VOTE1) | GCC_VOTE_S2 | GCC_VOTE_M_AHB | GCC_VOTE_S_AHB);
	for (int i = 0; i < 1000; i++) {
		/* The slave AHB clock is hardware-gated (HW_CTL, bit 1): it reads
		 * off while idle and runs when the SE is accessed, so only the SE
		 * and master AHB clocks can be checked. */
		if (!(rd(g_gcc + GCC_S2_CBCR) & CBCR_CLK_OFF) &&
		    !(rd(g_gcc + GCC_M_AHB_CBCR) & CBCR_CLK_OFF) &&
		    (!(rd(g_gcc + GCC_S_AHB_CBCR) & CBCR_CLK_OFF) ||
		     (rd(g_gcc + GCC_S_AHB_CBCR) & 2)))
			return 0;
		arch_udelay(10);
	}
	snprintf(line, sizeof(line), "clocks stay off: s2 %x m %x s %x\n",
	         rd(g_gcc + GCC_S2_CBCR), rd(g_gcc + GCC_M_AHB_CBCR),
	         rd(g_gcc + GCC_S_AHB_CBCR));
	say(line);
	return -1;
}

static void pins_on(void)
{
	/* func qup10 is selector 3 on GPIO 9, 4 on GPIO 10; 2 mA, no pull. */
	wr(TLMM_CTL(TLMM_NORTH, 9), 3u << 2);
	wr(TLMM_CTL(TLMM_NORTH, 10), 4u << 2);
}

/* geni_se_config_packing(se, 8, 4, msb_to_lsb=true, tx, rx), unrolled. */
static void se_packing(void)
{
	u32 cfg[4];

	for (int i = 0; i < 4; i++) {
		u32 idx = (u32)i * 8 + 7;

		cfg[i] = (idx << 5) | (1u << 4) | (7u << 1);
	}
	cfg[3] |= 1;
	u32 cfg0 = cfg[0] | (cfg[1] << 10), cfg1 = cfg[2] | (cfg[3] << 10);

	se_wr(SE_GENI_TX_PACKING_CFG0, cfg0);
	se_wr(SE_GENI_TX_PACKING_CFG1, cfg1);
	se_wr(SE_GENI_RX_PACKING_CFG0, cfg0);
	se_wr(SE_GENI_RX_PACKING_CFG1, cfg1);
	se_wr(SE_GENI_BYTE_GRAN, 0);
}

static int se_init(void)
{
	char line[96];
	u32 fw = se_rd(GENI_FW_REVISION_RO);
	u32 proto = (fw >> 8) & 0xff;

	snprintf(line, sizeof(line), "se10 fw rev %x proto %u hw %x\n", fw, proto,
	         se_rd(SE_HW_PARAM_0));
	say(line);
	if (proto != 3)
		return -1; /* not loaded as I2C by the boot chain */

	u32 tx_depth = (se_rd(SE_HW_PARAM_0) >> 16) & 0x3f;

	se_wr(SE_GSI_EVENT_EN, 0);
	se_wr(SE_GENI_M_IRQ_CLEAR, 0xffffffffu);
	se_wr(SE_GENI_S_IRQ_CLEAR, 0xffffffffu);
	se_wr(GENI_CGC_CTRL, se_rd(GENI_CGC_CTRL) | 0x7f);
	se_wr(SE_DMA_GENERAL_CFG, se_rd(SE_DMA_GENERAL_CFG) | 0xf);
	se_wr(GENI_OUTPUT_CTRL, 0x7f);
	se_wr(GENI_FORCE_DEFAULT_REG, 1);
	se_wr(SE_GENI_DMA_MODE_EN, 0);
	se_wr(SE_IRQ_EN, se_rd(SE_IRQ_EN) | 0xf);
	se_wr(SE_GENI_RX_WATERMARK, tx_depth ? tx_depth - 1 : 1);
	se_wr(SE_GENI_RX_RFR_WATERMARK, tx_depth ? tx_depth : 2);
	se_wr(SE_GENI_M_IRQ_EN, M_CMD_DONE | M_ERRORS | M_RX_WATERMARK | M_RX_LAST |
	                        M_TX_WATERMARK);
	se_packing();
	/* 100 kHz from 19.2 MHz: divider 7, t_high 10, t_low 11, t_cycle 26. */
	se_wr(SE_GENI_CLK_SEL, 0);
	se_wr(GENI_SER_M_CLK_CFG, (7u << 4) | 1u);
	se_wr(SE_I2C_SCL_COUNTERS, (10u << 20) | (11u << 10) | 26u);
	return 0;
}

/* Wait for any of `mask` in M_IRQ_STATUS, 50 ms at most. */
static u32 se_wait(u32 mask)
{
	for (int i = 0; i < 5000; i++) {
		u32 st = se_rd(SE_GENI_M_IRQ_STATUS);

		if (st & mask)
			return st;
		arch_udelay(10);
	}
	return 0;
}

/* One I2C message. `stretch`: hold the bus (no STOP) for a following read. */
static int i2c_msg(int read, u8 *buf, u32 len, int stretch)
{
	u32 param = ((u32)TS_ADDR << 9) | (stretch ? STOP_STRETCH : 0);
	u32 done = 0;

	se_wr(SE_GENI_M_IRQ_CLEAR, 0xffffffffu);
	se_wr(read ? SE_I2C_RX_TRANS_LEN : SE_I2C_TX_TRANS_LEN, len);
	__asm__ volatile("dsb sy" ::: "memory");
	se_wr(SE_GENI_M_CMD0, ((read ? I2C_READ : I2C_WRITE) << 27) | param);
	if (!read) {
		se_wr(SE_GENI_TX_WATERMARK, 1);
		while (done < len) {
			u32 st = se_wait(M_TX_WATERMARK | M_CMD_DONE | M_ERRORS);

			if (!st || (st & M_ERRORS))
				goto fail;
			if (!(st & M_TX_WATERMARK))
				break;
			u32 w = 0;

			for (u32 b = 0; b < 4 && done < len; b++)
				w |= (u32)buf[done++] << (8 * b);
			se_wr(SE_GENI_TX_FIFOn, w);
			se_wr(SE_GENI_M_IRQ_CLEAR, M_TX_WATERMARK);
		}
		se_wr(SE_GENI_TX_WATERMARK, 0);
	}
	for (;;) {
		u32 st = se_wait(M_CMD_DONE | M_ERRORS | M_RX_WATERMARK | M_RX_LAST);

		if (!st || (st & M_ERRORS))
			goto fail;
		if (read && (st & (M_RX_WATERMARK | M_RX_LAST))) {
			u32 words = se_rd(SE_GENI_RX_FIFO_STATUS) & 0x1ffffff;

			for (u32 j = 0; j < words; j++) {
				u32 w = se_rd(SE_GENI_RX_FIFOn);

				for (u32 b = 0; b < 4 && done < len; b++)
					buf[done++] = (u8)(w >> (8 * b));
			}
			se_wr(SE_GENI_M_IRQ_CLEAR, M_RX_WATERMARK | M_RX_LAST);
		}
		if (st & M_CMD_DONE)
			break;
	}
	se_wr(SE_GENI_M_IRQ_CLEAR, 0xffffffffu);
	return 0;
fail:
	se_wr(SE_GENI_M_IRQ_CLEAR, 0xffffffffu);
	return -1;
}

static int ts_read(u8 reg, u8 *buf, u32 len)
{
	if (i2c_msg(0, &reg, 1, 1) < 0)
		return -1;
	return i2c_msg(1, buf, len, 0);
}

static int ts_write(const u8 *buf, u32 len)
{
	return i2c_msg(0, (u8 *)buf, len, 0);
}

static int ts_hw_init(void)
{
	char line[128];
	u8 b[11];

	if (ts_read(TS_READ_ONE_EVENT, b, 8) < 0) {
		say("no answer at 0x48 (powered? reset?)\n");
		return -1;
	}
	snprintf(line, sizeof(line), "first event %02x %02x %02x %02x %02x %02x %02x %02x\n",
	         b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
	say(line);
	if (ts_read(TS_BOOT_STATUS, b, 1) < 0)
		return -1;
	u8 boot = b[0];

	if (ts_read(TS_DEVICE_ID, b, 3) < 0)
		return -1;
	g_devid = ((u32)b[0] << 16) | ((u32)b[1] << 8) | b[2];
	if (ts_read(TS_PANEL_INFO, b, 11) == 0 && b[0] | b[1]) {
		g_max_x = ((u32)b[0] << 8) | b[1];
		g_max_y = ((u32)b[2] << 8) | b[3];
	}
	snprintf(line, sizeof(line), "boot status %02x, id %06x, panel %ux%u\n",
	         boot, g_devid, g_max_x, g_max_y);
	say(line);
	if (boot != 0x20)
		return -1; /* stuck in the controller's bootloader */
	const u8 func[3] = { TS_TOUCH_FUNCTION, 0x01, 0x00 };
	const u8 sense[1] = { TS_SENSE_ON };

	if (ts_write(func, 3) < 0 || ts_write(sense, 1) < 0)
		return -1;
	return 0;
}

static int irq_asserted(void)
{
	return !(rd(TLMM_IO(TLMM_SOUTH, TS_IRQ_GPIO)) & 1); /* active low */
}

static void ts_report(const u8 *e, int *down)
{
	u8 tid = (u8)((e[0] & 0x3c) >> 2);
	u8 state = (u8)((e[0] & 0xc0) >> 6);

	if ((e[0] & 0x03) != 0 || tid != 1)
		return; /* not a coordinate, or not the first finger */
	if (state == 3) {
		if (*down) {
			input_event_push(INPUT_DEV_TOUCH, B1NIX_EV_KEY, B1NIX_BTN_LEFT, 0);
			input_event_sync(INPUT_DEV_TOUCH);
			*down = 0;
		}
		return;
	}
	if (state != 1 && state != 2)
		return;
	u32 x = ((u32)e[1] << 4) | ((e[3] & 0xf0) >> 4);
	u32 y = ((u32)e[2] << 4) | (e[3] & 0x0f);

	input_event_push(INPUT_DEV_TOUCH, B1NIX_EV_ABS, B1NIX_ABS_X,
	                 (i32)(x * 32767u / (g_max_x ? g_max_x : 1)));
	input_event_push(INPUT_DEV_TOUCH, B1NIX_EV_ABS, B1NIX_ABS_Y,
	                 (i32)(y * 32767u / (g_max_y ? g_max_y : 1)));
	if (!*down) {
		input_event_push(INPUT_DEV_TOUCH, B1NIX_EV_KEY, B1NIX_BTN_LEFT, 1);
		*down = 1;
	}
	input_event_sync(INPUT_DEV_TOUCH);
}

static void ts_thread(void *arg)
{
	u8 ev[8 * 32];
	int down = 0;

	(void)arg;
	for (;;) {
		if (!irq_asserted()) {
			scheduler_sleep_ticks(1);
			continue;
		}
		g_irq_seen++;
		if (ts_read(TS_READ_ONE_EVENT, ev, 8) < 0) {
			g_reads_failed++;
			scheduler_sleep_ticks(1);
			continue;
		}
		if (!ev[0]) {
			scheduler_sleep_ticks(1);
			continue;
		}
		g_events++;
		memcpy(g_last_event, ev, 8);
		u32 more = ev[7] & 0x3f;

		if (more > 31)
			more = 0;
		if (more && ts_read(TS_READ_ALL_EVENT, ev + 8, more * 8) < 0)
			more = 0;
		for (u32 i = 0; i <= more; i++)
			ts_report(ev + i * 8, &down);
	}
}

int sec_ts_start(void)
{
	if (g_state == 1)
		return 0;
	if (platform_type() != PLATFORM_SM8150_ID) {
		g_state = -1;
		return -ENODEV;
	}
	say("start: clocks\n");
	if (clocks_on() < 0) {
		g_state = -2;
		return -EIO;
	}
	say("start: pins, se10\n");
	pins_on();
	if (se_init() < 0) {
		g_state = -3;
		return -EIO;
	}
	say("start: controller\n");
	if (ts_hw_init() < 0) {
		g_state = -4;
		return -EIO;
	}
	g_state = 1;
	kthread_create("sec-ts", ts_thread, 0);
	say("running\n");
	return 0;
}

int sec_ts_state(void) { return g_state; }

/* For /proc/sys/kernel/touch: what the poller has seen. */
void sec_ts_describe(char *buf, usize len)
{
	snprintf(buf, len, "%d irq_level=%d irq_seen=%u events=%u failed=%u "
	         "last=%02x%02x%02x%02x%02x%02x%02x%02x\n", g_state,
	         g_state == 1 ? irq_asserted() : -1, g_irq_seen, g_events,
	         g_reads_failed, g_last_event[0], g_last_event[1], g_last_event[2],
	         g_last_event[3], g_last_event[4], g_last_event[5], g_last_event[6],
	         g_last_event[7]);
}

#else

void sec_ts_describe(char *buf, usize len) { snprintf(buf, len, "-1\n"); }

int sec_ts_start(void) { return -ENODEV; }
int sec_ts_state(void) { return -1; }

#endif
