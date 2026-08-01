#ifndef B1NIX_SOUND_H
#define B1NIX_SOUND_H

#include <b1nix/types.h>

/* PCM sample formats */
enum sound_fmt {
	SOUND_FMT_S16LE = 0, /* signed 16-bit little-endian (WAV default) */
	SOUND_FMT_S16BE,     /* signed 16-bit big-endian */
	SOUND_FMT_U8,        /* unsigned 8-bit */
	SOUND_FMT_S32LE,     /* signed 32-bit little-endian */
};

/* Sound device operations */
struct sound_device {
	const char *name;
	u32 sample_rate;
	u32 channels;
	enum sound_fmt format;
	u32 buffer_size;  /* DMA ring buffer size in bytes */

	/* Open the device for playback */
	int (*open)(struct sound_device *dev);
	/* Close the device */
	void (*close)(struct sound_device *dev);
	/* Write PCM samples, returns bytes written or negative errno */
	isize (*write)(struct sound_device *dev, const void *buf, usize len);
	/* Get current playback position in bytes */
	u32 (*get_position)(struct sound_device *dev);
	/* Check if the device is present and initialized */
	int (*ready)(struct sound_device *dev);

	/* Mixer state and optional hardware volume hooks (M79). Volume is
	 * 0..100 per channel, muted is 0/1. set_volume treats a -1 argument
	 * as "keep current" so a mute-only or gain-only call is possible.
	 * Drivers without a hardware volume leave the ops NULL and the
	 * generic mixer layer tracks the software state in vol_left/right. */
	int vol_left;
	int vol_right;
	int muted;
	int (*set_volume)(struct sound_device *dev, int left, int right, int muted);
	int (*get_volume)(struct sound_device *dev, int *left, int *right, int *muted);
};

/* Register a sound device */
void sound_register(struct sound_device *dev);

/* Find the first registered sound device (or NULL) */
struct sound_device *sound_get_default(void);

/* Generic OSS-style mixer ioctl dispatcher (M79). `dev` may be NULL to use
 * the default registered device; the caller supplies the user pointer as
 * the `arg` of the ioctl syscall. Returns 0 or a negative errno. */
int sound_mixer_ioctl(struct sound_device *dev, u64 request, void *arg);

/* Initialize HDA driver — called from main.c */
void hda_init(void);

/* Re-register /dev/dsp after the real root is mounted (see vfs.c) */
void hda_dev_init(void);

/* Self-test — called from main.c in test mode */
void hda_selftest(void);

/* Initialize AC'97 driver — called from main.c */
void ac97_init(void);

/* Re-register /dev/dsp1 after the real root is mounted (see vfs.c) */
void ac97_dev_init(void);

/* Self-test — called from main.c in test mode */
void ac97_selftest(void);

#endif
