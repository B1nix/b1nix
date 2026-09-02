/*
 * <linux/soundcard.h> — OSS mixer ABI (M79: Audio Stack).
 *
 * The classic OSS SOUND_MIXER_READ_* / SOUND_MIXER_WRITE_* ioctl family,
 * served by the b1nix kernel mixer layer (kernel/dev/mixer.c) through the
 * /dev/dspN inode ioctl hook. Device indices and ioctl numbers match Linux
 * so binaries written against OSS compile and run unchanged.
 */
#ifndef B1NIX_LINUX_SOUNDCARD_H
#define B1NIX_LINUX_SOUNDCARD_H

#ifdef __cplusplus
extern "C" {
#endif

/* Mixer device indices (Linux <linux/soundcard.h> ABI). */
#define SOUND_MIXER_VOLUME   0
#define SOUND_MIXER_BASS     1
#define SOUND_MIXER_TREBLE   2
#define SOUND_MIXER_SYNTH    3
#define SOUND_MIXER_PCM      4
#define SOUND_MIXER_SPEAKER  5
#define SOUND_MIXER_LINE     6
#define SOUND_MIXER_MIC      7
#define SOUND_MIXER_CD       8
#define SOUND_MIXER_IMIX     9
#define SOUND_MIXER_ALTPCM   10
#define SOUND_MIXER_RECLEV   11
#define SOUND_MIXER_IGAIN    12
#define SOUND_MIXER_OGAIN    13
#define SOUND_MIXER_LINE1    14
#define SOUND_MIXER_LINE2    15
#define SOUND_MIXER_LINE3    16
#define SOUND_MIXER_DIGITAL1 17
#define SOUND_MIXER_DIGITAL2 18
#define SOUND_MIXER_DIGITAL3 19
#define SOUND_MIXER_PHONEIN  20
#define SOUND_MIXER_PHONEOUT 21
#define SOUND_MIXER_VIDEO    22
#define SOUND_MIXER_RADIO    23
#define SOUND_MIXER_MONITOR  24
#define SOUND_MIXER_MUTE     30
#define SOUND_MIXER_ENHANCE  31
#define SOUND_MIXER_LAST     31

/* Mask-only query indices (MIXER_READ only). */
#define SOUND_MIXER_DEVMASK    0x10
#define SOUND_MIXER_RECMASK    0x11
#define SOUND_MIXER_RECSRC     0x12
#define SOUND_MIXER_STEREODEVS 0x13

/* OSS ioctls: MIXER_READ(dev) = _IOR('M', dev, int), MIXER_WRITE(dev) =
 * _IOWR('M', dev, int) under the Linux _IOC layout. Volume values are packed
 * as `left | (right << 8)`, 0..100 per channel; MUTE is a 0/1 flag. Writes
 * echo the accepted value back to the caller. */
#define MIXER_READ(dev)   (0x80044D00U | ((dev) & 0xFF))
#define MIXER_WRITE(dev)  (0xC0044D00U | ((dev) & 0xFF))

#define SOUND_MIXER_READ_VOLUME     MIXER_READ(SOUND_MIXER_VOLUME)
#define SOUND_MIXER_WRITE_VOLUME    MIXER_WRITE(SOUND_MIXER_VOLUME)
#define SOUND_MIXER_READ_PCM        MIXER_READ(SOUND_MIXER_PCM)
#define SOUND_MIXER_WRITE_PCM       MIXER_WRITE(SOUND_MIXER_PCM)
#define SOUND_MIXER_READ_MUTE       MIXER_READ(SOUND_MIXER_MUTE)
#define SOUND_MIXER_WRITE_MUTE      MIXER_WRITE(SOUND_MIXER_MUTE)
#define SOUND_MIXER_READ_DEVMASK    MIXER_READ(SOUND_MIXER_DEVMASK)
#define SOUND_MIXER_READ_RECMASK    MIXER_READ(SOUND_MIXER_RECMASK)
#define SOUND_MIXER_READ_RECSRC     MIXER_READ(SOUND_MIXER_RECSRC)
#define SOUND_MIXER_READ_STEREODEVS MIXER_READ(SOUND_MIXER_STEREODEVS)

/* Helpers to pack/unpack per-channel OSS volume values. */
#define VOLUME_LEFT(v)  ((v) & 0xFF)
#define VOLUME_RIGHT(v) (((v) >> 8) & 0xFF)
#define VOLUME_PACK(l, r) ((((r) & 0xFF) << 8) | ((l) & 0xFF))

#ifdef __cplusplus
}
#endif

#endif /* B1NIX_LINUX_SOUNDCARD_H */
