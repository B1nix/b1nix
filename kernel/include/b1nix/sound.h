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
};

/* Register a sound device */
void sound_register(struct sound_device *dev);

/* Find the first registered sound device (or NULL) */
struct sound_device *sound_get_default(void);

/* Initialize HDA driver — called from main.c */
void hda_init(void);

/* Self-test — called from programs.c in test mode */
void hda_selftest(void);

#endif
