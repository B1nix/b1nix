/*
 * The VideoCore mailbox — how an ARM core on a Raspberry Pi asks the firmware
 * for anything the ARM side does not own.
 *
 * On this SoC the GPU boots first and keeps the interesting resources: the
 * board's revision and serial, how much RAM the ARM was given, the on-board
 * Ethernet MAC address, the clocks, and the display. None of that is behind an
 * ARM register — it is behind a message passed through a mailbox, and the
 * property channel (channel 8) is the protocol those messages speak.
 *
 * A message is one buffer: a total size, a request code, a sequence of tags,
 * and an end marker. Each tag carries its own identifier, the size of its value
 * buffer, and the buffer itself, which the firmware overwrites with the answer.
 * The buffer's address goes to the GPU as a bus address, which on a BCM2711 is
 * the physical address seen through the L2-coherent alias at 0xC0000000.
 *
 * This driver is polled and has no interrupt: every one of these calls happens
 * during bring-up, and the firmware answers a property message in microseconds.
 */

#include <b1nix/bcm2835.h>
#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/mm.h>
#include <b1nix/types.h>
#include <string.h>

#if defined(__aarch64__)

/* Offsets from the mailbox block the device tree names, which starts at the
 * ARM's read register rather than at the block's own base. */
#define MBOX_READ      0x00
#define MBOX_STATUS    0x18
#define MBOX_WRITE     0x20

#define MBOX_FULL      (1u << 31)
#define MBOX_EMPTY     (1u << 30)

#define MBOX_CHANNEL_PROPERTY 8

/* The GPU reads the message through the alias that keeps it coherent with the
 * ARM's L2. */
#define VC_BUS_ALIAS   0xC0000000ull

/* Message header and terminator */
#define PROP_REQUEST   0x00000000u
#define PROP_SUCCESS   0x80000000u
#define PROP_TAG_END   0x00000000u
/* A tag's value length word has this bit set in the answer. */
#define PROP_TAG_RESPONSE (1u << 31)

static u64 g_mbox;
static volatile u32 *g_buf;   /* 16-byte aligned message buffer */
static u64 g_buf_phys;

#define MBOX_BUF_WORDS 64

static inline u32 mbox_read_reg(u32 off)
{
	return *(volatile u32 *)(usize)(g_mbox + off);
}

static inline void mbox_write_reg(u32 off, u32 val)
{
	*(volatile u32 *)(usize)(g_mbox + off) = val;
}

#define MBOX_POLL_LIMIT 2000000u

/*
 * Post one message and wait for its answer.
 *
 * The mailbox is shared with other channels, so a message that comes back for
 * somebody else is dropped rather than mistaken for this one's answer — the
 * low four bits of every word name the channel it belongs to.
 */
static int mbox_call(void)
{
	u32 msg = (u32)((g_buf_phys | VC_BUS_ALIAS) & ~0xfull) | MBOX_CHANNEL_PROPERTY;
	u32 i;

	for (i = 0; i < MBOX_POLL_LIMIT; i++) {
		if (!(mbox_read_reg(MBOX_STATUS) & MBOX_FULL))
			break;
		__asm__ volatile("yield");
	}
	if (i == MBOX_POLL_LIMIT)
		return -1;

	__asm__ volatile("dsb sy" ::: "memory");
	mbox_write_reg(MBOX_WRITE, msg);

	for (i = 0; i < MBOX_POLL_LIMIT; i++) {
		if (mbox_read_reg(MBOX_STATUS) & MBOX_EMPTY) {
			__asm__ volatile("yield");
			continue;
		}
		if (mbox_read_reg(MBOX_READ) == msg) {
			__asm__ volatile("dsb sy" ::: "memory");
			return g_buf[1] == PROP_SUCCESS ? 0 : -1;
		}
	}
	return -1;
}

/*
 * Build a message holding a single tag, send it, and hand back the answer.
 *
 * `value` carries the request in and the answer out; `req_bytes` is how much of
 * it the firmware is given and `rsp_bytes` how much of it comes back. The tag's
 * value buffer is the larger of the two, which is the rule the protocol states
 * and the reason a tag that asks nothing and answers eight bytes still needs
 * eight bytes of room.
 */
int bcm2835_property(u32 tag, const u32 *req, u32 req_bytes,
                     u32 *rsp, u32 rsp_bytes)
{
	u32 value_bytes = req_bytes > rsp_bytes ? req_bytes : rsp_bytes;
	u32 value_words = (value_bytes + 3) / 4;
	u32 total_words = 6 + value_words;

	if (!g_mbox || !g_buf)
		return -1;
	if (total_words > MBOX_BUF_WORDS)
		return -1;

	g_buf[0] = total_words * 4;
	g_buf[1] = PROP_REQUEST;
	g_buf[2] = tag;
	g_buf[3] = value_bytes;
	g_buf[4] = req_bytes;   /* request: the size actually supplied */
	for (u32 i = 0; i < value_words; i++)
		g_buf[5 + i] = (req && i * 4 < req_bytes) ? req[i] : 0;
	g_buf[5 + value_words] = PROP_TAG_END;

	if (mbox_call() != 0)
		return -1;
	/* The firmware sets the response bit and replaces the size word with how
	 * much it wrote. A tag it did not recognise comes back with neither. */
	if (!(g_buf[4] & PROP_TAG_RESPONSE))
		return -1;
	if (rsp) {
		u32 got = g_buf[4] & ~PROP_TAG_RESPONSE;

		if (got > rsp_bytes)
			got = rsp_bytes;
		for (u32 i = 0; i * 4 < got; i++)
			rsp[i] = g_buf[5 + i];
	}
	return 0;
}

int bcm2835_board_revision(u32 *revision)
{
	return bcm2835_property(BCM_TAG_GET_BOARD_REVISION, 0, 0, revision, 4);
}

int bcm2835_board_serial(u64 *serial)
{
	u32 v[2] = { 0, 0 };

	if (bcm2835_property(BCM_TAG_GET_BOARD_SERIAL, 0, 0, v, 8) != 0)
		return -1;
	*serial = ((u64)v[1] << 32) | v[0];
	return 0;
}

int bcm2835_arm_memory(u64 *base, u64 *size)
{
	u32 v[2] = { 0, 0 };

	if (bcm2835_property(BCM_TAG_GET_ARM_MEMORY, 0, 0, v, 8) != 0)
		return -1;
	if (base)
		*base = v[0];
	if (size)
		*size = v[1];
	return 0;
}

int bcm2835_mac_address(u8 mac[6])
{
	u32 v[2] = { 0, 0 };

	if (bcm2835_property(BCM_TAG_GET_MAC_ADDRESS, 0, 0, v, 6) != 0)
		return -1;
	memcpy(mac, v, 6);
	return 0;
}

/*
 * Ask the firmware for a framebuffer.
 *
 * Four tags in four messages rather than one: the geometry has to be set
 * before the allocation, and the pitch is only known after it. The firmware
 * hands back a bus address, which becomes a physical one by masking off the
 * alias it was handed through.
 */
int bcm2835_fb_alloc(u32 width, u32 height, u32 depth,
                     u64 *fb_phys, u32 *fb_size, u32 *pitch)
{
	u32 wh[2] = { width, height };
	u32 alloc[2] = { 16, 0 };   /* in: alignment. out: base, size */
	u32 d = depth;
	u32 p = 0;

	if (bcm2835_property(BCM_TAG_SET_PHYS_WH, wh, 8, wh, 8) != 0)
		return -1;
	if (bcm2835_property(BCM_TAG_SET_VIRT_WH, wh, 8, wh, 8) != 0)
		return -1;
	if (bcm2835_property(BCM_TAG_SET_DEPTH, &d, 4, &d, 4) != 0)
		return -1;
	if (bcm2835_property(BCM_TAG_ALLOCATE_BUFFER, alloc, 4, alloc, 8) != 0)
		return -1;
	if (!alloc[0] || !alloc[1])
		return -1;
	if (bcm2835_property(BCM_TAG_GET_PITCH, 0, 0, &p, 4) != 0 || !p)
		return -1;

	if (fb_phys)
		*fb_phys = (u64)alloc[0] & ~VC_BUS_ALIAS;
	if (fb_size)
		*fb_size = alloc[1];
	if (pitch)
		*pitch = p;
	return 0;
}

int bcm2835_mbox_ready(void) { return g_mbox != 0 && g_buf != 0; }

void bcm2835_mbox_init(void)
{
	u64 phys;

	g_mbox = fdt_mbox_base();
	if (!g_mbox)
		return; /* not a Broadcom board */

	/* One page is far more than a property message needs, and it is the
	 * smallest thing the allocator hands out that is guaranteed to satisfy
	 * the protocol's 16-byte alignment. */
	phys = pmm_alloc_frames(1);
	if (!phys) {
		g_mbox = 0;
		return;
	}
	g_buf_phys = phys;
	g_buf = (volatile u32 *)(usize)(DIRECT_MAP_BASE + phys);

	console_write("mbox: VideoCore mailbox at 0x");
	console_write_hex64(g_mbox);
	console_write("\n");
}

/*
 * Test mode: ask the firmware three things it alone knows and check that the
 * answers are answers rather than an untouched buffer. A mailbox that is not
 * working returns the request unchanged, so a revision of zero or a memory
 * size of zero is a failure and not a board that happens to have neither.
 */
void bcm2835_mbox_selftest(void)
{
	u32 revision = 0;
	u64 base = 0, size = 0;
	u8 mac[6] = { 0, 0, 0, 0, 0, 0 };

	if (!bcm2835_mbox_ready())
		return;

	if (bcm2835_board_revision(&revision) == 0 && revision) {
		console_write("M109-RPI: ok board-revision 0x");
		console_write_hex64(revision);
		console_write("\n");
	} else {
		console_write("M109-RPI: FAIL board-revision\n");
	}

	if (bcm2835_arm_memory(&base, &size) == 0 && size) {
		console_write("M109-RPI: ok arm-memory ");
		console_write_dec(size / (1024 * 1024));
		console_write(" MiB\n");
	} else {
		console_write("M109-RPI: FAIL arm-memory\n");
	}

	if (bcm2835_mac_address(mac) == 0 &&
	    (mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5])) {
		console_write("M109-RPI: ok mac-address\n");
	} else {
		console_write("M109-RPI: FAIL mac-address\n");
	}
}

#else  /* !__aarch64__ */

void bcm2835_mbox_init(void) {}
void bcm2835_mbox_selftest(void) {}
int bcm2835_mbox_ready(void) { return 0; }
int bcm2835_fb_alloc(u32 width, u32 height, u32 depth,
                     u64 *fb_phys, u32 *fb_size, u32 *pitch)
{
	(void)width; (void)height; (void)depth;
	(void)fb_phys; (void)fb_size; (void)pitch;
	return -1;
}

#endif
